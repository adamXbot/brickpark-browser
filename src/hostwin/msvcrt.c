/* LEGOLAND portable build -- the MSVC CRT file/directory calls the game uses
 * (see portable/hostwin/include/io.h and direct.h), on POSIX. Paths may use
 * backslashes and the install's directory names; they are resolved by
 * ll_host_resolve_path (kernel32.c). Flag values are MSVC's. */
#include "ll_host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

#define MS_O_WRONLY 0x0001
#define MS_O_RDWR   0x0002
#define MS_O_APPEND 0x0008
#define MS_O_CREAT  0x0100
#define MS_O_TRUNC  0x0200
#define MS_O_EXCL   0x0400
#define MS_A_SUBDIR 0x10

/* Every path the CRT layer takes goes through the shim's resolver
 * (kernel32.c, PORT-A2): backslashes, the emulated CD drive, a
 * case-insensitive component match, and the flattened-install fallback that
 * finds ".\strings\stab.str" as `stab.str`. `create` means the name is being
 * made and must be taken as written. */
static void ll_path2(char* out, size_t cap, const char* in, int create)
{
    ll_host_resolve_path(out, (unsigned int)cap, in, create);
}

static void ll_path(char* out, size_t cap, const char* in)
{
    ll_path2(out, cap, in, 0);
}

int _open(const char* path, int oflag, ...)
{
    char p[1024];
    int  flags = 0;
    int  mode = 0644;
    ll_path2(p, sizeof p, path, (oflag & MS_O_CREAT) != 0);
    if (oflag & MS_O_RDWR) flags |= O_RDWR;
    else if (oflag & MS_O_WRONLY) flags |= O_WRONLY;
    else flags |= O_RDONLY;
    if (oflag & MS_O_APPEND) flags |= O_APPEND;
    if (oflag & MS_O_CREAT) {
        va_list ap;
        va_start(ap, oflag);
        (void)va_arg(ap, int);        /* MSVC permission bits: ignored */
        va_end(ap);
        flags |= O_CREAT;
    }
    if (oflag & MS_O_TRUNC) flags |= O_TRUNC;
    if (oflag & MS_O_EXCL) flags |= O_EXCL;
    return open(p, flags, mode);
}

/* ---- fopen -----------------------------------------------------------------
 * The game opens most of its loose assets with the stdio `fopen`, not with
 * `_open` or `CreateFileA`, and it hands it Windows paths:
 *
 *     fopen(".\\strings\\stab.str", "r")     narration2.c LoadStrings
 *
 * On POSIX a backslash is an ordinary filename character, so that call cannot
 * succeed even on a case-insensitive filesystem with the file sitting right
 * there -- and LoadStrings reproduces the original's `if (!f) exit(1)`, so the
 * whole program disappeared with status 1 the moment the resource volumes
 * finished mounting. Defining `fopen` here overrides libc's for everything in
 * this build (wasm-ld prefers an object's definition to an archive member's,
 * so musl's is never pulled in) and routes it through the same resolver as
 * everything else. The body must NOT call fopen -- it opens the fd itself and
 * hands it to fdopen, which is a different symbol.
 *
 * Mode strings the game uses: "r", "rb", "w", "wb", "a", "ab", with an
 * optional "+"; anything else falls through to read-only, which is what the
 * CRT would have done with a mode it did not understand. */
FILE* fopen(const char* path, const char* mode)
{
    char p[1024];
    int  flags;
    int  fd;
    int  update = mode && strchr(mode, '+') != 0;
    char m = mode ? mode[0] : 'r';
    FILE* f;

    if (m == 'w')
        flags = (update ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
    else if (m == 'a')
        flags = (update ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
    else
        flags = update ? O_RDWR : O_RDONLY;

    ll_host_resolve_path(p, sizeof p, path, (flags & O_CREAT) != 0);
    fd = open(p, flags, 0644);
    if (fd < 0)
        return 0;
    f = fdopen(fd, mode ? mode : "r");
    if (!f)
        close(fd);
    return f;
}

/* ---- the failure-value audit (PORT-A5) -------------------------------------
 * The CRT has TWO failure conventions and this file mixes two implementations
 * of them, so every entry point was checked for the asymmetry that broke
 * `_findclose`: a call made WITH the failure value the previous call returned.
 *
 *   fd family (`_open`, `_close`, `_read`, `_write`, `_lseek`, `_tell`,
 *   `_filelength`): failure is -1 and the fd goes straight to POSIX, which
 *   rejects -1 with EBADF. audio4.c:152/227 `_close(g_speech_fd)` on a failed
 *   `_open` is therefore already safe, and behaves as it does on Windows.
 *   Nothing to fix.
 *
 *   find family (`_findfirst`/`_findnext`/`_findclose`): failure is -1 and the
 *   handle is OURS -- a pointer cast to long. -1 is non-null, so `if (!f)`
 *   misses it; see ll_bad_find. FIXED here, all three entry points.
 *
 *   pointer family (`fopen`, `_getcwd`, `_strupr`, `_msize`): failure is NULL.
 *   `fopen` already returns 0 on any error. `_msize(NULL)` and `_strupr(NULL)`
 *   are invalid-parameter cases on MSVC (no crash, a defined return) and were
 *   an unguarded dereference here, so they are guarded below.
 *
 *   `_stat`/`_fstat`/`_access`/`_unlink`/`_chmod` are NOT defined here and no
 *   game source calls them -- a grep of LEGOLAND/*.c for the whole CRT surface
 *   finds only `_filelength` (2), `_getcwd` (4), `_msize` (4), `_splitpath`
 *   (15), `_strupr` (3), `_tell` (7) beyond the families above. If one is ever
 *   added, note that MSVC's `_stat` returns -1 and leaves the buffer alone
 *   while POSIX `stat` is the same shape, so it needs no special care; the
 *   handle-shaped calls are the dangerous ones. */
int  _close(int fd) { return close(fd); }
int  _read(int fd, void* buf, unsigned int n) { return (int)read(fd, buf, n); }
int  _write(int fd, const void* buf, unsigned int n) { return (int)write(fd, buf, n); }
long _lseek(int fd, long off, int origin) { return (long)lseek(fd, off, origin); }
long _tell(int fd) { return (long)lseek(fd, 0, SEEK_CUR); }

long _filelength(int fd)
{
    struct stat st;
    return fstat(fd, &st) == 0 ? (long)st.st_size : -1;
}

int _mkdir(const char* path) { char p[1024]; ll_path2(p, sizeof p, path, 1); return mkdir(p, 0755); }
int _chdir(const char* path) { char p[1024]; ll_path(p, sizeof p, path); return chdir(p); }
char* _getcwd(char* buf, int size) { return getcwd(buf, (size_t)size); }

/* ---- _findfirst / _findnext / _findclose --------------------------------- */
struct ll_find {
    DIR* dir;
    char dirpath[1024];
    char pattern[256];
};

struct ll_finddata {          /* mirrors struct _finddata_t in hostwin/io.h */
    unsigned int  attrib;
    long          time_create;
    long          time_access;
    long          time_write;
    unsigned long size;
    char          name[260];
};

/* The CRT's find handle is an OPAQUE long whose FAILURE value is -1, not 0.
 * This layer hands back a `struct ll_find*` cast to long, so every entry point
 * that takes a handle has to reject -1 as well as 0 before it dereferences:
 * `(struct ll_find*)(intptr_t)-1` is a perfectly non-null pointer to
 * 0xffffffff. `ll_bad_find` is that test, in one place.
 *
 * This is not a hypothetical: profiles.c's `Goto_ProfileDir` (0x00491360)
 * calls `_findclose(h)` UNCONDITIONALLY, as shipped -- on Windows that is a
 * documented -1/EINVAL return, here it was a wild `closedir` that took the
 * whole module down with `memory access out of bounds` on the first front-end
 * frame whenever no `profiles` directory existed. */
static int ll_bad_find(long handle)
{
    return handle == 0 || handle == -1;
}

static int ll_find_step(struct ll_find* f, struct ll_finddata* out)
{
    struct dirent* e;
    if (!f || !f->dir || !out) {
        errno = EINVAL;
        return -1;
    }
    while ((e = readdir(f->dir)) != 0) {
        struct stat st;
        char full[1300];
        if (fnmatch(f->pattern, e->d_name, 0) != 0)
            continue;
        snprintf(full, sizeof full, "%s/%s", f->dirpath, e->d_name);
        memset(out, 0, sizeof *out);
        if (stat(full, &st) == 0) {
            out->attrib = S_ISDIR(st.st_mode) ? MS_A_SUBDIR : 0;
            out->size = (unsigned long)st.st_size;
            out->time_write = (long)st.st_mtime;
            out->time_access = (long)st.st_atime;
            out->time_create = (long)st.st_ctime;
        }
        strncpy(out->name, e->d_name, sizeof out->name - 1);
        return 0;
    }
    return -1;
}

long _findfirst(const char* spec, void* out)
{
    struct ll_find* f;
    char            p[1024];
    char*           slash;
    if (!spec || !out) {              /* MSVC: invalid parameter -> -1/EINVAL */
        errno = EINVAL;
        return -1;
    }
    f = (struct ll_find*)calloc(1, sizeof *f);
    if (!f) {
        errno = ENOMEM;
        return -1;
    }
    ll_path(p, sizeof p, spec);
    slash = strrchr(p, '/');
    if (slash) {
        *slash = 0;
        strncpy(f->dirpath, p, sizeof f->dirpath - 1);
        strncpy(f->pattern, slash + 1, sizeof f->pattern - 1);
    } else {
        strcpy(f->dirpath, ".");
        strncpy(f->pattern, p, sizeof f->pattern - 1);
    }
    if (strcmp(f->pattern, "*.*") == 0)      /* DOS "everything" */
        strcpy(f->pattern, "*");
    f->dir = opendir(f->dirpath);
    if (!f->dir || ll_find_step(f, (struct ll_finddata*)out) != 0) {
        if (f->dir)
            closedir(f->dir);
        free(f);
        errno = ENOENT;
        return -1;
    }
    return (long)(intptr_t)f;
}

int _findnext(long handle, void* out)
{
    if (ll_bad_find(handle)) {        /* a failed _findfirst handed back -1 */
        errno = EINVAL;
        return -1;
    }
    return ll_find_step((struct ll_find*)(intptr_t)handle, (struct ll_finddata*)out);
}

int _findclose(long handle)
{
    struct ll_find* f;
    if (ll_bad_find(handle)) {        /* Goto_ProfileDir closes unconditionally */
        errno = EINVAL;
        return -1;
    }
    f = (struct ll_find*)(intptr_t)handle;
    if (f->dir)
        closedir(f->dir);
    free(f);
    return 0;
}

/* ---- misc CRT ------------------------------------------------------------ */
unsigned int _msize(void* block)
{
    if (!block)                       /* MSVC: invalid parameter, not a crash */
        return 0;
#ifdef __APPLE__
    return (unsigned int)malloc_size(block);
#else
    return (unsigned int)malloc_usable_size(block);
#endif
}

extern int stricmp(const char*, const char*);
int _stricmp(const char* a, const char* b) { return stricmp(a, b); }

char* _strupr(char* s)
{
    char* p;
    if (!s)
        return 0;
    for (p = s; *p; p++)
        if (*p >= 'a' && *p <= 'z')
            *p -= 'a' - 'A';
    return s;
}

void _splitpath(const char* path, char* drive, char* dir, char* fname, char* ext)
{
    const char* p = path;
    const char* last_sep = 0;
    const char* dot = 0;
    const char* q;
    if (!path) {                      /* every out buffer still gets emptied */
        if (drive) drive[0] = 0;
        if (dir) dir[0] = 0;
        if (fname) fname[0] = 0;
        if (ext) ext[0] = 0;
        return;
    }
    if (drive) drive[0] = 0;
    if (p[0] && p[1] == ':') {
        if (drive) { drive[0] = p[0]; drive[1] = ':'; drive[2] = 0; }
        p += 2;
    }
    for (q = p; *q; q++)
        if (*q == '\\' || *q == '/')
            last_sep = q;
    q = last_sep ? last_sep + 1 : p;
    if (dir) {
        size_t n = last_sep ? (size_t)(last_sep + 1 - p) : 0;
        memcpy(dir, p, n);
        dir[n] = 0;
    }
    dot = strrchr(q, '.');
    if (fname) {
        size_t n = dot ? (size_t)(dot - q) : strlen(q);
        memcpy(fname, q, n);
        fname[n] = 0;
    }
    if (ext)
        strcpy(ext, dot ? dot : "");
}
