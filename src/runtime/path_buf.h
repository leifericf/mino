/*
 * path_buf.h — fixed-capacity caller-owned buffer for filesystem paths.
 *
 * Centralizes the 4096-byte static buffer pattern that existed across
 * the file-I/O primitives. The buffer is plain inline storage, so
 * declaring a path_buf_t never allocates; mutators report truncation
 * with a non-zero return value and leave the buffer NUL-terminated at
 * a safe length so the caller can either retry, surface the error, or
 * keep operating on the truncated prefix.
 *
 * Not binary-safe: assumes NUL-terminated input.
 */
#ifndef MINO_PATH_BUF_H
#define MINO_PATH_BUF_H

#include <stddef.h>

#define PATH_BUF_CAP 4096

typedef struct {
    char   data[PATH_BUF_CAP];
    size_t len;
} path_buf_t;

/* Reset the buffer to the empty string. */
void path_buf_init(path_buf_t *pb);

/* Set the buffer contents to `s`. Returns 0 on success, 1 on
 * truncation (buffer left NUL-terminated at PATH_BUF_CAP-1). */
int path_buf_set(path_buf_t *pb, const char *s);

/* Append `s` to the current contents. Returns 0 on success, 1 on
 * truncation. */
int path_buf_append(path_buf_t *pb, const char *s);

/* snprintf into the buffer, replacing current contents. Returns 0 on
 * success, 1 on truncation. */
int path_buf_format(path_buf_t *pb, const char *fmt, ...);

#endif /* MINO_PATH_BUF_H */
