/* LEGOLAND portable build -- the MSVC CRT file/directory calls the game uses
 * (see portable/hostwin/include/io.h and direct.h), on POSIX. Paths may use
 * backslashes; they are normalised here. Flag values are MSVC's. */
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

static void ll_path(char* out, size_t cap, const char* in)
{
    size_t i;
    for (i = 0; i + 1 < cap && in[i]; i++)
        out[i] = in[i] == '\\' ? '/' : in[i];
    out[i] = 0;
}

int _open(const char* path, int oflag, ...)
{
    char p[1024];
    int  flags = 0;
    int  mode = 0644;
    ll_path(p, sizeof p, path);
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

int _mkdir(const char* path) { char p[1024]; ll_path(p, sizeof p, path); return mkdir(p, 0755); }
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

static int ll_find_step(struct ll_find* f, struct ll_finddata* out)
{
    struct dirent* e;
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
    struct ll_find* f = (struct ll_find*)calloc(1, sizeof *f);
    char            p[1024];
    char*           slash;
    if (!f)
        return -1;
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
    return ll_find_step((struct ll_find*)(intptr_t)handle, (struct ll_finddata*)out);
}

int _findclose(long handle)
{
    struct ll_find* f = (struct ll_find*)(intptr_t)handle;
    if (!f)
        return -1;
    closedir(f->dir);
    free(f);
    return 0;
}

/* ---- misc CRT ------------------------------------------------------------ */
unsigned int _msize(void* block)
{
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
