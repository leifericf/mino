/*
 * ws.c -- RFC 6455 websocket frame codec and handshake accept-key.
 *
 * Three prims sit under MINO_CAP_WEBSOCKET and back the mino.ws client
 * (ADR 41): ws-encode-frame builds one wire frame from a plain map,
 * ws-decode-frames consumes an accumulated byte buffer and returns the
 * complete messages plus the unconsumed tail, and ws-accept-key
 * computes the Sec-WebSocket-Accept echo the handshake verifies.
 *
 * ws-decode-frames eats untrusted network bytes, so it is the security
 * surface: every length field is bounded before a payload is realized
 * (a hostile 64-bit length degrades to a :codec/limit throw, never an
 * allocation), the RFC 6455 role rules (clients mask, servers do not)
 * are enforced, reserved bits and reserved opcodes are rejected,
 * control frames may not be fragmented or carry extended lengths, close
 * codes and text payloads are validated, and fragmented messages
 * reassemble natively under the same cap. The decoder consumes each
 * byte once: :rest is the suffix from the first incomplete frame or
 * still-open fragment run, so feeding (rest ++ more) resumes exactly
 * where the previous call stopped.
 *
 * The caller (mino.ws) sources every client mask and the handshake
 * nonce from secure-rand-bytes; this codec applies exactly the mask it
 * is handed and never draws randomness itself.
 */

#include "prim/internal.h"
#include "mino.h"
/* The generated bundled-source header is one string literal whose
 * concatenated length exceeds ANSI-C's guaranteed 4095, the same as the
 * headers install_stdlib.c includes under this pragma. */
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Woverlength-strings"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "lib_mino_ws.h"
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

/* bearssl's public headers probe BR_DOXYGEN_IGNORE with #if; scoped
 * silence, the treatment digest.c and tls.c give the same headers. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#endif
#include "bearssl_hash.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <stdint.h>
#include <string.h>

/* Default reassembly and per-frame cap: the 16 MiB HTTP body cap, so a
 * websocket message defaults to the same bound the http parser uses. */
#define WS_DEFAULT_MAX_PAYLOAD (16u * 1024u * 1024u)

/* RFC 6455 opcodes. */
#define WS_OP_CONTINUATION 0x0
#define WS_OP_TEXT         0x1
#define WS_OP_BINARY       0x2
#define WS_OP_CLOSE        0x8
#define WS_OP_PING         0x9
#define WS_OP_PONG         0xA

/* The GUID RFC 6455 appends to the client key before hashing. */
static const char WS_ACCEPT_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static const char k_ws_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Base64-encode n bytes into out (out holds ((n+2)/3)*4 bytes). Returns
 * the number of characters written. */
static size_t ws_base64(const unsigned char *src, size_t n, char *out)
{
    size_t i, o = 0, rem;
    for (i = 0; i + 3 <= n; i += 3) {
        uint32_t v = ((uint32_t)src[i] << 16)
                   | ((uint32_t)src[i + 1] << 8)
                   | (uint32_t)src[i + 2];
        out[o++] = k_ws_b64[(v >> 18) & 63u];
        out[o++] = k_ws_b64[(v >> 12) & 63u];
        out[o++] = k_ws_b64[(v >> 6) & 63u];
        out[o++] = k_ws_b64[v & 63u];
    }
    rem = n - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)src[i] << 16;
        out[o++] = k_ws_b64[(v >> 18) & 63u];
        out[o++] = k_ws_b64[(v >> 12) & 63u];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8);
        out[o++] = k_ws_b64[(v >> 18) & 63u];
        out[o++] = k_ws_b64[(v >> 12) & 63u];
        out[o++] = k_ws_b64[(v >> 6) & 63u];
        out[o++] = '=';
    }
    return o;
}

/* RFC 6455 text validity: strict UTF-8 over the whole message. Rejects
 * overlong forms, surrogates, and out-of-range code points. Returns 1
 * when every byte in [s, s+n) forms a well-formed sequence. */
static int ws_utf8_valid(const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c  = s[i];
        unsigned char lo = 0x80, hi = 0xBF;
        size_t        need, k;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
        } else if (c == 0xE0) {
            need = 2; lo = 0xA0;
        } else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) {
            need = 2;
        } else if (c == 0xED) {
            need = 2; hi = 0x9F;
        } else if (c == 0xF0) {
            need = 3; lo = 0x90;
        } else if (c >= 0xF1 && c <= 0xF3) {
            need = 3;
        } else if (c == 0xF4) {
            need = 3; hi = 0x8F;
        } else {
            return 0;
        }
        if (i + need >= n) return 0;
        if (s[i + 1] < lo || s[i + 1] > hi) return 0;
        for (k = 2; k <= need; k++) {
            if (s[i + k] < 0x80 || s[i + k] > 0xBF) return 0;
        }
        i += need + 1;
    }
    return 1;
}

/* Byte view from a string or bytes value; 0 on any other type. */
static int ws_bytes_view(const mino_val *v, const unsigned char **data,
                         size_t *len)
{
    if (v == NULL) return 0;
    if (mino_type_of(v) == MINO_STRING) {
        *data = (const unsigned char *)v->as.s.data;
        *len  = v->as.s.len;
        return 1;
    }
    if (mino_is_bytes(v)) {
        *data = mino_bytes_data(v);
        *len  = mino_bytes_len(v);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* ws-accept-key                                                    */
/* ---------------------------------------------------------------- */

/* (ws-accept-key key) -- SHA-1 of the key string concatenated with the
 * RFC 6455 GUID, base64-encoded. The key is a header token, so its
 * bytes are taken as-is (no trimming); a non-string throws. */
static mino_val *prim_ws_accept_key(mino_state *S, mino_val *args,
                                    mino_env *env)
{
    mino_val       *v;
    const char     *key;
    size_t          key_len;
    br_sha1_context cc;
    unsigned char   digest[br_sha1_SIZE];
    char            out[28]; /* ceil(20/3)*4 = 28 */
    size_t          out_len;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "ws-accept-key requires one argument");
    }
    v = args->as.cons.car;
    if (mino_type_of(v) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "ws-accept-key: key must be a string");
    }
    key     = v->as.s.data;
    key_len = v->as.s.len;

    br_sha1_init(&cc);
    br_sha1_update(&cc, key, key_len);
    br_sha1_update(&cc, WS_ACCEPT_GUID, sizeof(WS_ACCEPT_GUID) - 1);
    br_sha1_out(&cc, digest);

    out_len = ws_base64(digest, sizeof digest, out);
    return mino_string_n(S, out, out_len);
}

/* ---------------------------------------------------------------- */
/* ws-encode-frame                                                  */
/* ---------------------------------------------------------------- */

/* Map an :opcode keyword to its wire nibble, or -1 for an unknown one. */
static int ws_opcode_for(const mino_val *kw)
{
    const char *name;
    if (kw == NULL || mino_type_of(kw) != MINO_KEYWORD) return -1;
    name = kw->as.s.data;
    if (name == NULL) return -1;
    if (strcmp(name, "continuation") == 0) return WS_OP_CONTINUATION;
    if (strcmp(name, "text")         == 0) return WS_OP_TEXT;
    if (strcmp(name, "binary")       == 0) return WS_OP_BINARY;
    if (strcmp(name, "close")        == 0) return WS_OP_CLOSE;
    if (strcmp(name, "ping")         == 0) return WS_OP_PING;
    if (strcmp(name, "pong")         == 0) return WS_OP_PONG;
    return -1;
}

static int ws_is_control(int opcode)
{
    return (opcode & 0x8) != 0;
}

/* (ws-encode-frame frame) -- one wire frame from a plain map. Keys:
 * :opcode (required keyword), :payload (string or bytes; a close frame
 * uses :code and :reason instead), :fin? (default true), :mask (four
 * wire bytes; present emits a masked client frame, absent an unmasked
 * server frame). Returns the frame as bytes. */
static mino_val *prim_ws_encode_frame(mino_state *S, mino_val *args,
                                      mino_env *env)
{
    mino_val            *frame, *opv, *finv, *maskv;
    int                  opcode, fin, masked = 0;
    const unsigned char *payload = NULL;
    size_t               payload_len = 0;
    const unsigned char *mask = NULL;
    unsigned char        close_body[125];
    unsigned char       *wire;
    size_t               header_len, total, i;
    mino_val            *result;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "ws-encode-frame requires one argument");
    }
    frame = args->as.cons.car;
    if (mino_type_of(frame) != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "ws-encode-frame: argument must be a map");
    }

    opv    = map_get_val(frame, mino_keyword(S, "opcode"));
    opcode = ws_opcode_for(opv);
    if (opcode < 0) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "ws-encode-frame: :opcode must be one of "
                                     ":text :binary :ping :pong :close "
                                     ":continuation");
    }

    finv = map_get_val(frame, mino_keyword(S, "fin?"));
    if (finv == NULL || mino_type_of(finv) == MINO_NIL) {
        fin = 1;
    } else if (mino_type_of(finv) == MINO_BOOL) {
        fin = mino_val_bool_get(finv) ? 1 : 0;
    } else {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "ws-encode-frame: :fin? must be a boolean");
    }

    if (ws_is_control(opcode) && !fin) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "ws-encode-frame: a control frame is "
                                     "never fragmented");
    }

    /* Close frames carry a code and reason, not a raw payload. Assemble
     * the two-byte code plus reason bytes into a stack buffer capped at
     * the 125-byte control limit. */
    if (opcode == WS_OP_CLOSE) {
        mino_val *codev, *reasonv;
        long long code = -1;
        const unsigned char *reason = NULL;
        size_t reason_len = 0;
        codev = map_get_val(frame, mino_keyword(S, "code"));
        if (codev != NULL && mino_type_of(codev) != MINO_NIL) {
            if (!as_long(codev, &code) || code < 0 || code > 0xFFFF) {
                return prim_throw_classified(S, "eval/contract", "MCT001",
                                             "ws-encode-frame: :code must be "
                                             "an integer in 0..65535");
            }
        }
        reasonv = map_get_val(frame, mino_keyword(S, "reason"));
        if (reasonv != NULL && mino_type_of(reasonv) != MINO_NIL) {
            if (!ws_bytes_view(reasonv, &reason, &reason_len)) {
                return prim_throw_classified(S, "eval/type", "MTY001",
                                             "ws-encode-frame: :reason must be "
                                             "a string or bytes value");
            }
        }
        if (code < 0 && reason_len > 0) {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                                         "ws-encode-frame: a close :reason "
                                         "requires a :code");
        }
        if (code < 0) {
            payload_len = 0;
        } else {
            if (reason_len > sizeof(close_body) - 2) {
                return prim_throw_classified(S, "eval/contract", "MCT001",
                                             "ws-encode-frame: close frame "
                                             "body exceeds 125 bytes");
            }
            close_body[0] = (unsigned char)((code >> 8) & 0xFF);
            close_body[1] = (unsigned char)(code & 0xFF);
            if (reason_len > 0) memcpy(close_body + 2, reason, reason_len);
            payload     = close_body;
            payload_len = reason_len + 2;
        }
    } else {
        mino_val *pv = map_get_val(frame, mino_keyword(S, "payload"));
        if (pv != NULL && mino_type_of(pv) != MINO_NIL) {
            if (!ws_bytes_view(pv, &payload, &payload_len)) {
                return prim_throw_classified(S, "eval/type", "MTY001",
                                             "ws-encode-frame: :payload must "
                                             "be a string or bytes value");
            }
        }
    }

    if (ws_is_control(opcode) && payload_len > 125) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "ws-encode-frame: a control payload "
                                     "never exceeds 125 bytes");
    }

    maskv = map_get_val(frame, mino_keyword(S, "mask"));
    if (maskv != NULL && mino_type_of(maskv) != MINO_NIL) {
        size_t mask_len;
        if (!mino_is_bytes(maskv)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "ws-encode-frame: :mask must be a "
                                         "bytes value");
        }
        mask     = mino_bytes_data(maskv);
        mask_len = mino_bytes_len(maskv);
        if (mask_len != 4) {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                                         "ws-encode-frame: :mask must be "
                                         "exactly four bytes");
        }
        masked = 1;
    }

    /* Header size: two base bytes, extended length (0/2/8), mask (0/4). */
    if (payload_len <= 125) {
        header_len = 2;
    } else if (payload_len <= 0xFFFF) {
        header_len = 4;
    } else {
        header_len = 10;
    }
    if (masked) header_len += 4;

    /* Guard the total against SIZE_MAX before the allocation. */
    if (payload_len > SIZE_MAX - header_len) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
                                     "ws-encode-frame: frame too large");
    }
    total = header_len + payload_len;

    wire = (unsigned char *)malloc(total > 0 ? total : 1);
    if (wire == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "ws-encode-frame: out of memory");
    }

    wire[0] = (unsigned char)((fin ? 0x80 : 0x00) | (opcode & 0x0F));
    if (payload_len <= 125) {
        wire[1] = (unsigned char)payload_len;
        i = 2;
    } else if (payload_len <= 0xFFFF) {
        wire[1] = 126;
        wire[2] = (unsigned char)((payload_len >> 8) & 0xFF);
        wire[3] = (unsigned char)(payload_len & 0xFF);
        i = 4;
    } else {
        uint64_t n = (uint64_t)payload_len;
        wire[1] = 127;
        wire[2] = (unsigned char)((n >> 56) & 0xFF);
        wire[3] = (unsigned char)((n >> 48) & 0xFF);
        wire[4] = (unsigned char)((n >> 40) & 0xFF);
        wire[5] = (unsigned char)((n >> 32) & 0xFF);
        wire[6] = (unsigned char)((n >> 24) & 0xFF);
        wire[7] = (unsigned char)((n >> 16) & 0xFF);
        wire[8] = (unsigned char)((n >> 8) & 0xFF);
        wire[9] = (unsigned char)(n & 0xFF);
        i = 10;
    }

    if (masked) {
        wire[1] |= 0x80;
        memcpy(wire + i, mask, 4);
        i += 4;
        for (size_t j = 0; j < payload_len; j++) {
            wire[i + j] = (unsigned char)(payload[j] ^ mask[j & 3]);
        }
    } else if (payload_len > 0) {
        memcpy(wire + i, payload, payload_len);
    }

    result = mino_bytes(S, wire, total);
    free(wire);
    return result;
}

/* ---------------------------------------------------------------- */
/* ws-decode-frames                                                 */
/* ---------------------------------------------------------------- */

/* One decoded frame ready to become a map. Payloads for text/binary/
 * ping/pong point into an owned buffer (unmasked, reassembled); close
 * frames carry a parsed code and reason instead. */
typedef struct {
    int            opcode;      /* the message opcode (text/binary), or control */
    unsigned char *payload;     /* owned; NULL for an empty payload */
    size_t         payload_len;
    int            is_text;     /* text messages decode to strings */
    /* close-frame fields */
    int            has_code;
    long long      code;
} ws_frame_out;

/* The decoder's running state over one call: the frames it has completed
 * and the open fragmented message it is still assembling. */
typedef struct {
    ws_frame_out *frames;
    size_t        count;
    size_t        cap;
    /* open fragment run */
    int            frag_open;   /* a message with fin?=false is in progress */
    int            frag_opcode; /* the first fragment's opcode */
    unsigned char *frag_buf;    /* owned accumulator */
    size_t         frag_len;
    size_t         frag_cap;
    size_t         run_mark;    /* frame count when the open run started; a
                                 * control frame emitted behind the run
                                 * sits past it, and rolls back if the run
                                 * does not complete this call so its bytes
                                 * (still in :rest) are decoded exactly once */
} ws_decode_state;

static void ws_state_free(ws_decode_state *st)
{
    size_t i;
    for (i = 0; i < st->count; i++) free(st->frames[i].payload);
    free(st->frames);
    free(st->frag_buf);
    st->frames    = NULL;
    st->count     = 0;
    st->frag_buf  = NULL;
}

/* Append one completed frame record. Returns 0 on success, -1 on OOM
 * (the caller frees `owned` on failure). */
static int ws_push_frame(ws_decode_state *st, int opcode,
                         unsigned char *owned, size_t len, int is_text,
                         int has_code, long long code)
{
    if (st->count == st->cap) {
        size_t nc = st->cap ? st->cap * 2 : 4;
        ws_frame_out *nf = (ws_frame_out *)realloc(st->frames,
                                                   nc * sizeof(*nf));
        if (nf == NULL) return -1;
        st->frames = nf;
        st->cap    = nc;
    }
    st->frames[st->count].opcode      = opcode;
    st->frames[st->count].payload     = owned;
    st->frames[st->count].payload_len = len;
    st->frames[st->count].is_text     = is_text;
    st->frames[st->count].has_code    = has_code;
    st->frames[st->count].code        = code;
    st->count++;
    return 0;
}

/* Decode outcome codes. */
enum { WS_OK = 0, WS_NEED_MORE = 1, WS_CORRUPT = 2, WS_LIMIT = 3, WS_OOM = 4 };

/* RFC 6455 7.4.1: valid close codes are 1000..1015 minus the reserved
 * 1004/1005/1006, or the private range 3000..4999. */
static int ws_close_code_valid(long long code)
{
    if (code >= 3000 && code <= 4999) return 1;
    if (code < 1000 || code > 1015) return 0;
    if (code == 1004 || code == 1005 || code == 1006) return 0;
    return 1;
}

/* Decode as many frames as the buffer holds. On WS_NEED_MORE, *consumed
 * marks the prefix that fully decoded into completed messages; the tail
 * from there (including any open fragment run's raw bytes) is the
 * leftover the caller keeps. */
static int ws_decode_all(ws_decode_state *st, const unsigned char *buf,
                         size_t n, int role_is_server, size_t max_payload,
                         size_t *consumed)
{
    size_t pos = 0;
    /* Bytes fully consumed into completed messages. An open fragment run
     * rewinds this to the run's first byte so its bytes stay in :rest. */
    size_t safe = 0;
    size_t frag_start = 0;

    while (pos < n) {
        unsigned char b0, b1;
        int fin, opcode, masked;
        size_t hdr = pos;
        uint64_t plen;
        unsigned char mask[4];
        size_t need;
        size_t this_frame_start = pos;

        if (n - pos < 2) break;                 /* need the base header */
        b0 = buf[pos];
        b1 = buf[pos + 1];

        if (b0 & 0x70) { *consumed = safe; return WS_CORRUPT; } /* RSV set */
        fin    = (b0 & 0x80) != 0;
        opcode = b0 & 0x0F;
        masked = (b1 & 0x80) != 0;

        /* Reserved opcodes: 0x3-0x7 (data) and 0xB-0xF (control). */
        if ((opcode >= 0x3 && opcode <= 0x7)
            || (opcode >= 0xB && opcode <= 0xF)) {
            *consumed = safe; return WS_CORRUPT;
        }
        /* Role rule: clients mask every frame, servers never do. */
        if (role_is_server && !masked) { *consumed = safe; return WS_CORRUPT; }
        if (!role_is_server && masked) { *consumed = safe; return WS_CORRUPT; }

        plen = b1 & 0x7F;
        pos += 2;

        if (ws_is_control(opcode)) {
            if (!fin) { *consumed = safe; return WS_CORRUPT; } /* no frag */
            if (plen > 125) { *consumed = safe; return WS_CORRUPT; } /* no ext len */
        }

        if (plen == 126) {
            if (n - pos < 2) { pos = hdr; break; }
            plen = ((uint64_t)buf[pos] << 8) | (uint64_t)buf[pos + 1];
            pos += 2;
        } else if (plen == 127) {
            int k;
            if (n - pos < 8) { pos = hdr; break; }
            plen = 0;
            for (k = 0; k < 8; k++) {
                plen = (plen << 8) | (uint64_t)buf[pos + k];
            }
            pos += 8;
            /* RFC 6455 5.2: the top bit of a 64-bit length MUST be 0. */
            if (plen & 0x8000000000000000ULL) {
                *consumed = safe; return WS_CORRUPT;
            }
        }

        /* Bound the declared length before realizing anything. On a
         * SIZE_MAX platform this also guards the pointer math below. */
        if (plen > (uint64_t)max_payload) { *consumed = safe; return WS_LIMIT; }
        if (plen > (uint64_t)(SIZE_MAX - pos)) {
            *consumed = safe; return WS_LIMIT;
        }

        if (masked) {
            if (n - pos < 4) { pos = hdr; break; }
            memcpy(mask, buf + pos, 4);
            pos += 4;
        }

        need = (size_t)plen;
        if (n - pos < need) { pos = hdr; break; } /* payload not all here */

        /* The frame is fully present at [hdr, pos+need). Unmask into a
         * scratch pointer view; text/binary payloads may join an open
         * fragment run. */
        {
            const unsigned char *raw = buf + pos;
            (void)this_frame_start;

            if (opcode == WS_OP_CONTINUATION) {
                size_t total;
                if (!st->frag_open) { *consumed = safe; return WS_CORRUPT; }
                total = st->frag_len + need;
                if (total > max_payload) { *consumed = safe; return WS_LIMIT; }
                if (need > 0) {
                    unsigned char *nb = (unsigned char *)realloc(st->frag_buf,
                                                                 total);
                    if (nb == NULL) { *consumed = safe; return WS_OOM; }
                    st->frag_buf = nb;
                    if (masked) {
                        size_t j;
                        for (j = 0; j < need; j++) {
                            st->frag_buf[st->frag_len + j] =
                                (unsigned char)(raw[j] ^ mask[j & 3]);
                        }
                    } else {
                        memcpy(st->frag_buf + st->frag_len, raw, need);
                    }
                    st->frag_len = total;
                }
                pos += need;
                if (fin) {
                    int is_text = (st->frag_opcode == WS_OP_TEXT);
                    if (is_text
                        && !ws_utf8_valid(st->frag_buf, st->frag_len)) {
                        *consumed = safe; return WS_CORRUPT;
                    }
                    if (ws_push_frame(st, st->frag_opcode, st->frag_buf,
                                      st->frag_len, is_text, 0, 0) != 0) {
                        *consumed = safe; return WS_OOM;
                    }
                    st->frag_buf  = NULL;
                    st->frag_len  = 0;
                    st->frag_open = 0;
                    safe = pos;           /* the whole run is now complete */
                }
                continue;
            }

            if (!ws_is_control(opcode) && !fin) {
                /* Opening fragment of a new message. */
                if (st->frag_open) { *consumed = safe; return WS_CORRUPT; }
                if (opcode != WS_OP_TEXT && opcode != WS_OP_BINARY) {
                    *consumed = safe; return WS_CORRUPT;
                }
                if ((uint64_t)need > (uint64_t)max_payload) {
                    *consumed = safe; return WS_LIMIT;
                }
                st->frag_buf = (unsigned char *)malloc(need > 0 ? need : 1);
                if (st->frag_buf == NULL) { *consumed = safe; return WS_OOM; }
                if (masked) {
                    size_t j;
                    for (j = 0; j < need; j++) {
                        st->frag_buf[j] = (unsigned char)(raw[j] ^ mask[j & 3]);
                    }
                } else if (need > 0) {
                    memcpy(st->frag_buf, raw, need);
                }
                st->frag_len    = need;
                st->frag_open   = 1;
                st->frag_opcode = opcode;
                st->run_mark    = st->count;
                frag_start      = this_frame_start;
                (void)frag_start;
                pos += need;
                continue;
            }

            /* A control frame, or a whole unfragmented data message. */
            if (!ws_is_control(opcode) && st->frag_open
                && opcode != WS_OP_CONTINUATION) {
                /* A new data frame while a message is still open is a
                 * protocol violation (interleaved data). */
                *consumed = safe; return WS_CORRUPT;
            }

            {
                unsigned char *owned = NULL;
                int is_text = 0, has_code = 0;
                long long code = 0;
                size_t body_len = need;

                if (need > 0) {
                    owned = (unsigned char *)malloc(need);
                    if (owned == NULL) { *consumed = safe; return WS_OOM; }
                    if (masked) {
                        size_t j;
                        for (j = 0; j < need; j++) {
                            owned[j] = (unsigned char)(raw[j] ^ mask[j & 3]);
                        }
                    } else {
                        memcpy(owned, raw, need);
                    }
                }

                if (opcode == WS_OP_CLOSE) {
                    if (need == 1) {           /* a lone code byte is invalid */
                        free(owned);
                        *consumed = safe; return WS_CORRUPT;
                    }
                    if (need >= 2) {
                        code = ((long long)owned[0] << 8) | (long long)owned[1];
                        has_code = 1;
                        if (!ws_close_code_valid(code)) {
                            free(owned);
                            *consumed = safe; return WS_CORRUPT;
                        }
                        if (need > 2
                            && !ws_utf8_valid(owned + 2, need - 2)) {
                            free(owned);
                            *consumed = safe; return WS_CORRUPT;
                        }
                    }
                } else if (opcode == WS_OP_TEXT) {
                    is_text = 1;
                    if (!ws_utf8_valid(owned, body_len)) {
                        free(owned);
                        *consumed = safe; return WS_CORRUPT;
                    }
                } else if (opcode == WS_OP_PING || opcode == WS_OP_PONG
                           || opcode == WS_OP_BINARY) {
                    /* payload stays raw bytes */
                } else {
                    free(owned);
                    *consumed = safe; return WS_CORRUPT;
                }

                if (ws_push_frame(st, opcode, owned, body_len, is_text,
                                  has_code, code) != 0) {
                    free(owned);
                    *consumed = safe; return WS_OOM;
                }
                pos += need;
                /* A control frame behind an open fragment run does not
                 * advance `safe` past the run's start: the run's raw
                 * bytes must stay in :rest until it completes. */
                if (!st->frag_open) safe = pos;
            }
        }
        (void)hdr;
    }

    /* An open fragment run at the end of the buffer keeps every byte of
     * the run in :rest, including any control frames that arrived behind
     * it: roll those emitted control frames back so the next feed decodes
     * them exactly once, in order, once the run completes. */
    if (st->frag_open) {
        while (st->count > st->run_mark) {
            st->count--;
            free(st->frames[st->count].payload);
            st->frames[st->count].payload = NULL;
        }
    }

    *consumed = st->frag_open ? frag_start : pos;
    return st->frag_open || pos < n ? WS_NEED_MORE : WS_OK;
}

/* Build the {:frames [..] :rest bytes} result map from the decode
 * state and the leftover byte range. */
static mino_val *ws_build_result(mino_state *S, ws_decode_state *st,
                                 const unsigned char *buf, size_t n,
                                 size_t consumed)
{
    mino_val **items = NULL;
    mino_val  *frames_vec, *rest_val, *result;
    mino_val  *rkeys[2], *rvals[2];
    size_t     i;

    if (st->count > 0) {
        items = (mino_val **)malloc(st->count * sizeof(*items));
        if (items == NULL) {
            return prim_throw_classified(S, "internal", "MIN001",
                                         "ws-decode-frames: out of memory");
        }
    }

    for (i = 0; i < st->count; i++) {
        ws_frame_out *f = &st->frames[i];
        mino_val *m;
        const char *opname;
        int is_control = ws_is_control(f->opcode);

        switch (f->opcode) {
        case WS_OP_TEXT:   opname = "text";   break;
        case WS_OP_BINARY: opname = "binary"; break;
        case WS_OP_CLOSE:  opname = "close";  break;
        case WS_OP_PING:   opname = "ping";   break;
        case WS_OP_PONG:   opname = "pong";   break;
        default:           opname = "binary"; break;
        }

        if (f->opcode == WS_OP_CLOSE) {
            mino_val *ck[4], *cv[4];
            ck[0] = mino_keyword(S, "opcode");
            cv[0] = mino_keyword(S, "close");
            ck[1] = mino_keyword(S, "fin?");
            cv[1] = mino_true(S);
            ck[2] = mino_keyword(S, "code");
            cv[2] = f->has_code ? mino_int(S, f->code) : mino_nil(S);
            ck[3] = mino_keyword(S, "reason");
            cv[3] = (f->payload_len > 2)
                ? mino_string_n(S, (const char *)f->payload + 2,
                                f->payload_len - 2)
                : mino_string_n(S, "", 0);
            m = mino_map(S, ck, cv, 4);
        } else {
            mino_val *ck[3], *cv[3];
            (void)is_control;
            ck[0] = mino_keyword(S, "opcode");
            cv[0] = mino_keyword(S, opname);
            ck[1] = mino_keyword(S, "fin?");
            cv[1] = mino_true(S);
            ck[2] = mino_keyword(S, "payload");
            if (f->is_text) {
                cv[2] = mino_string_n(S, (const char *)f->payload,
                                      f->payload_len);
            } else {
                cv[2] = mino_bytes(S, f->payload, f->payload_len);
            }
            m = mino_map(S, ck, cv, 3);
        }
        items[i] = m;
    }

    frames_vec = mino_vector(S, items, st->count);
    free(items);

    /* buf may be NULL for an empty input; adding an offset to NULL is UB
     * even when the offset is zero, so only advance a live pointer. */
    rest_val = mino_bytes(S, (n - consumed > 0) ? buf + consumed : NULL,
                          n - consumed);

    rkeys[0] = mino_keyword(S, "frames");
    rvals[0] = frames_vec;
    rkeys[1] = mino_keyword(S, "rest");
    rvals[1] = rest_val;
    result = mino_map(S, rkeys, rvals, 2);
    return result;
}

/* (ws-decode-frames bytes {:role :client|:server :max-payload n}) --
 * decode complete messages from an accumulated buffer. */
static mino_val *prim_ws_decode_frames(mino_state *S, mino_val *args,
                                       mino_env *env)
{
    mino_val            *bufv, *opts, *rolev, *maxv;
    const unsigned char *buf;
    size_t               n;
    const char          *role;
    int                  role_is_server;
    size_t               max_payload = WS_DEFAULT_MAX_PAYLOAD;
    ws_decode_state      st;
    size_t               consumed = 0;
    int                  rc;
    mino_val            *result;
    (void)env;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "ws-decode-frames requires bytes and "
                                     "an opts map");
    }
    bufv = args->as.cons.car;
    opts = args->as.cons.cdr->as.cons.car;

    if (!mino_is_bytes(bufv) && mino_type_of(bufv) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "ws-decode-frames: first argument must "
                                     "be bytes");
    }
    {
        const unsigned char *v;
        size_t vl;
        ws_bytes_view(bufv, &v, &vl);
        buf = v;
        n   = vl;
    }

    if (mino_type_of(opts) != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "ws-decode-frames: opts must be a map");
    }
    rolev = map_get_val(opts, mino_keyword(S, "role"));
    if (rolev == NULL || mino_type_of(rolev) != MINO_KEYWORD) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "ws-decode-frames: :role must be :client "
                                     "or :server");
    }
    role = rolev->as.s.data;
    if (role != NULL && strcmp(role, "server") == 0) {
        role_is_server = 1;
    } else if (role != NULL && strcmp(role, "client") == 0) {
        role_is_server = 0;
    } else {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "ws-decode-frames: :role must be :client "
                                     "or :server");
    }

    maxv = map_get_val(opts, mino_keyword(S, "max-payload"));
    if (maxv != NULL && mino_type_of(maxv) != MINO_NIL) {
        long long m;
        if (!as_long(maxv, &m) || m < 0) {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                                         "ws-decode-frames: :max-payload must "
                                         "be a non-negative integer");
        }
        max_payload = (size_t)m;
    }

    memset(&st, 0, sizeof st);
    rc = ws_decode_all(&st, buf, n, role_is_server, max_payload, &consumed);

    if (rc == WS_CORRUPT) {
        ws_state_free(&st);
        return prim_throw_classified(S, "codec/corrupt", "MWS001",
                                     "ws-decode-frames: malformed websocket "
                                     "frame");
    }
    if (rc == WS_LIMIT) {
        ws_state_free(&st);
        return prim_throw_classified(S, "codec/limit", "MWS002",
                                     "ws-decode-frames: frame or message "
                                     "exceeds the payload cap");
    }
    if (rc == WS_OOM) {
        ws_state_free(&st);
        return prim_throw_classified(S, "internal", "MIN001",
                                     "ws-decode-frames: out of memory");
    }

    result = ws_build_result(S, &st, buf, n, consumed);
    ws_state_free(&st);
    return result;
}

/* ---------------------------------------------------------------- */
/* install                                                          */
/* ---------------------------------------------------------------- */

const mino_prim_def k_prims_ws[] = {
    {"ws-encode-frame", prim_ws_encode_frame,
     "Encodes one RFC 6455 websocket frame from a plain map and returns "
     "the wire bytes. Keys: :opcode (:text :binary :ping :pong :close "
     ":continuation), :payload (a string or bytes value; a close frame "
     "uses :code and :reason instead), :fin? (default true), :mask (four "
     "bytes; present emits a masked client frame, absent an unmasked "
     "server frame). The caller sources the mask from secure-rand-bytes. "
     "A control payload must not exceed 125 bytes and a control frame is "
     "never fragmented."},
    {"ws-decode-frames", prim_ws_decode_frames,
     "Decodes complete websocket messages from an accumulated buffer and "
     "returns {:frames [..] :rest bytes}, where :rest is the suffix from "
     "the first incomplete frame or still-open fragment run, so feeding "
     "the rest plus more bytes resumes exactly where the last call "
     "stopped. Opts: :role :client (requires unmasked server frames) or "
     ":server (requires masked client frames), and :max-payload (default "
     "16 MiB, the cap on each message including reassembly). Fragmented "
     "messages reassemble natively; a :text payload decodes to a UTF-8 "
     "validated string, other data payloads to bytes, and a :close to "
     ":code and :reason. Protocol violations throw :codec/corrupt and "
     "cap breaches throw :codec/limit, both before the payload is "
     "realized."},
    {"ws-accept-key", prim_ws_accept_key,
     "Computes the Sec-WebSocket-Accept value from a Sec-WebSocket-Key "
     "string: the SHA-1 of the key concatenated with the RFC 6455 GUID, "
     "base64-encoded. Used to verify the server's handshake response."},
};

const size_t k_prims_ws_count = sizeof(k_prims_ws) / sizeof(k_prims_ws[0]);

/* websocket rides its own MINO_CAP_WEBSOCKET bit (in MINO_CAP_DEFAULT).
 * The bit is set here as well as by the capability dispatch loop so a
 * direct mino_install_websocket sets it too, matching the random /
 * signal / digest install shape. */
void mino_install_websocket(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_ws, k_prims_ws_count,
                                       "websocket");
    mino_register_bundled_lib(S, "mino.ws", lib_mino_ws_src);
    S->caps_installed |= MINO_CAP_WEBSOCKET;
}
