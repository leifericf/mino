/*
 * path_buf.c — fixed-capacity caller-owned buffer for filesystem paths.
 */
#include "path_buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void path_buf_init(path_buf_t *pb)
{
    pb->data[0] = '\0';
    pb->len     = 0;
}

int path_buf_set(path_buf_t *pb, const char *s)
{
    size_t n = strlen(s);
    if (n >= PATH_BUF_CAP) {
        memcpy(pb->data, s, PATH_BUF_CAP - 1);
        pb->data[PATH_BUF_CAP - 1] = '\0';
        pb->len = PATH_BUF_CAP - 1;
        return 1;
    }
    memcpy(pb->data, s, n + 1);
    pb->len = n;
    return 0;
}

int path_buf_append(path_buf_t *pb, const char *s)
{
    size_t n     = strlen(s);
    size_t avail = PATH_BUF_CAP - pb->len - 1;
    if (n > avail) {
        memcpy(pb->data + pb->len, s, avail);
        pb->len += avail;
        pb->data[pb->len] = '\0';
        return 1;
    }
    memcpy(pb->data + pb->len, s, n + 1);
    pb->len += n;
    return 0;
}

int path_buf_format(path_buf_t *pb, const char *fmt, ...)
{
    va_list ap;
    int     n;
    va_start(ap, fmt);
    n = vsnprintf(pb->data, PATH_BUF_CAP, fmt, ap);
    va_end(ap);
    if (n < 0) {
        pb->data[0] = '\0';
        pb->len     = 0;
        return 1;
    }
    if ((size_t)n >= PATH_BUF_CAP) {
        pb->len = PATH_BUF_CAP - 1;
        return 1;
    }
    pb->len = (size_t)n;
    return 0;
}
