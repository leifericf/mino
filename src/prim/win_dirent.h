/*
 * win_dirent.h -- minimal POSIX directory + stat surface for MSVC.
 *
 * fs.c and io.c walk directories with opendir/readdir/closedir and
 * stat files for type and mtime. MSVC's CRT ships <sys/stat.h>
 * (struct stat, st_mode, st_mtime) but no <dirent.h>, so this header
 * supplies just the dirent slice those two files use, backed by the
 * Win32 FindFirstFile family, plus the S_ISDIR macro MSVC omits.
 * GCC/Clang/mingw keep the system <dirent.h>; this file is compiled
 * only under _MSC_VER (both call sites include it from an #else arm).
 *
 * Only the names fs.c / io.c reference are provided: opendir, readdir,
 * closedir, struct dirent::d_name, DIR, and S_ISDIR. It is not a
 * general dirent implementation.
 */
#ifndef PRIM_WIN_DIRENT_H
#define PRIM_WIN_DIRENT_H

#include <sys/stat.h>
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#ifndef S_ISDIR
#  define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

struct dirent {
    char d_name[MAX_PATH];
};

typedef struct DIR {
    HANDLE           handle;   /* INVALID_HANDLE_VALUE once exhausted */
    WIN32_FIND_DATAA find;
    int              pending;  /* 1 while `find` holds an unread entry */
    struct dirent    entry;
} DIR;

static DIR *opendir(const char *name)
{
    DIR   *d;
    size_t n = strlen(name);
    char   pat[MAX_PATH];

    /* Build the "<name>\*" search pattern FindFirstFile expects. */
    if (n + 3 > sizeof(pat)) return NULL;
    memcpy(pat, name, n);
    if (n > 0 && pat[n - 1] != '\\' && pat[n - 1] != '/') pat[n++] = '\\';
    pat[n++] = '*';
    pat[n]   = '\0';

    d = (DIR *)malloc(sizeof(*d));
    if (d == NULL) return NULL;
    d->handle = FindFirstFileA(pat, &d->find);
    if (d->handle == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->pending = 1;
    return d;
}

static struct dirent *readdir(DIR *d)
{
    if (!d->pending) {
        if (!FindNextFileA(d->handle, &d->find)) return NULL;
    }
    d->pending = 0;
    strncpy(d->entry.d_name, d->find.cFileName, MAX_PATH - 1);
    d->entry.d_name[MAX_PATH - 1] = '\0';
    return &d->entry;
}

static int closedir(DIR *d)
{
    if (d == NULL) return -1;
    if (d->handle != INVALID_HANDLE_VALUE) FindClose(d->handle);
    free(d);
    return 0;
}

#endif /* PRIM_WIN_DIRENT_H */
