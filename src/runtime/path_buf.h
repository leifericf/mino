/*
 * path_buf.h -- capacity constant for fixed-size filesystem path buffers.
 *
 * The buffer pattern: `char buf[PATH_BUF_CAP]` on the stack or in a
 * struct, filled with snprintf / memcpy. Not binary-safe; assumes
 * NUL-terminated input.
 */
#ifndef MINO_PATH_BUF_H
#define MINO_PATH_BUF_H

#define PATH_BUF_CAP 4096

#endif /* MINO_PATH_BUF_H */
