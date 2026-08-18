/*
 * net.c -- TCP socket capability: install hook and (in the socket
 * unit) net-connect / net-read / net-read-all / net-write / net-close.
 *
 * Winsock is initialised lazily, once per process, on the first
 * socket call: a WSAStartup failure there can be reported as a
 * script-visible error, which a void install hook cannot do.
 * WSACleanup is intentionally never called; the init is held for the
 * process lifetime and the OS reclaims sockets at exit. POSIX needs
 * no equivalent.
 *
 * Trust model.
 *
 * Like fs.c, the socket primitives take whatever host / port /
 * payload the script author hands them. The embedder is inside the
 * trust boundary; the script author *is* the trust boundary. Argument
 * shapes are validated; destinations are not policed. An embedder
 * that wants to forbid network access refuses to install
 * MINO_CAP_NET (it is not in MINO_CAP_DEFAULT).
 */

#define _POSIX_C_SOURCE 200809L

#include "prim/internal.h"
#include "mino.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/types.h>
#endif

#include <stdlib.h>

#ifdef _WIN32
/* Process-lifetime winsock init; see the file-top comment. */
static int net_winsock_started = 0;

static int net_winsock_init(void)
{
    WSADATA data;
    if (net_winsock_started) return 0;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return -1;
    net_winsock_started = 1;
    return 0;
}
#endif

/* ---- install ---- */

static const mino_prim_def k_prims_net[] = {
    {NULL, NULL, NULL},
};

#define K_PRIMS_NET_COUNT 0

void mino_install_net(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_net, K_PRIMS_NET_COUNT,
                                       "net");
    S->caps_installed |= MINO_CAP_NET;
}
