/* LEGOLAND portable build -- KERNEL32, ADVAPI32 and VERSION for real.
 *
 * These are the host calls the startup spine and the loaders make, with
 * working bodies rather than the traps gen_link.py generates for everything
 * the shim does not define yet (it stops generating a trap for a symbol as
 * soon as an object in the build defines it, which is why this file is
 * compiled into legoland_core -- see portable/cmake/headless.cmake).
 *
 * The shape of the port this assumes, and why each decision is safe:
 *
 *   - **Single-threaded.** The original runs a MIDI/streaming helper thread
 *     (musicthread.c, resaudio2.c) that the port does not start: CreateThread
 *     refuses, so the game keeps its work on the main thread. Mutexes are
 *     therefore always free and a wait on anything returns immediately, which
 *     is exactly what startup.c's one-instance check wants
 *     (`WaitForSingleObject(mutex, 0) == WAIT_TIMEOUT` means "already
 *     running", so returning WAIT_OBJECT_0 means "we are the first").
 *   - **Files are POSIX files.** Under node with `-sNODERAWFS=1` the
 *     emscripten FS calls go straight to the real filesystem, so gamedata/ is
 *     read in place with no packaging step; the CRT layer in msvcrt.c already
 *     works this way. Win32 paths use backslashes, which ll_host_path()
 *     normalises.
 *   - **Time is one monotonic clock**, emscripten_get_now() under emcc and
 *     clock_gettime(CLOCK_MONOTONIC) otherwise, so GetTickCount and
 *     QueryPerformanceCounter cannot disagree.
 *   - **Sleep does not block.** See ll_host_sleep_hook below: the main-loop
 *     strategy is PORT-B's call (docs/SCOPE_PORT_WAVE.md), so Sleep is a
 *     no-op with a hook PORT-B points at its yield.
 *
 * Ownership: PORT-A (docs/SCOPE_PORT_WAVE.md). Declarations:
 * portable/hostwin/include/ll_host.h.
 */
#include "ll_host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>          /* strcasecmp, for the case-insensitive lookup */
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* ---- tracing -------------------------------------------------------------
 * `LL_HOST_TRACE=1` makes every interesting host call print one line. That is
 * how the ordered list of what the startup spine asks the host for was
 * recorded (docs/lanes/scope-port-a.md), and it is the first thing to turn on
 * when the game stops somewhere unexplained. GetTickCount and the other
 * per-frame calls are deliberately not traced: they would bury everything
 * else. */

static int ll_trace_on = -1;

static int ll_tracing(void)
{
    if (ll_trace_on < 0) {
        const char* e = getenv("LL_HOST_TRACE");
        ll_trace_on = e && *e && *e != '0';
    }
    return ll_trace_on;
}

#define LL_TRACE(...)                                   \
    do {                                                \
        if (ll_tracing()) {                             \
            fprintf(stderr, "HOST " __VA_ARGS__);        \
            fputc('\n', stderr);                        \
            fflush(stderr);                             \
        }                                               \
    } while (0)

/* ---- paths --------------------------------------------------------------- */

/* The game's resource volumes live on a CD, and once it has found that CD it
 * builds absolute paths from the drive letter ("D:\Legoland.res"). $LL_CD_DIR
 * points the shim at a directory to present as that drive -- gamedata/disc in
 * this checkout, which holds Legoland.res, Graphics1.res and Graphics2.res.
 * Unset, there is no CD and the game takes its local-path branch. */
#define LL_CD_LETTER 'D'

static const char* ll_cd_dir(void)
{
    const char* d = getenv("LL_CD_DIR");
    return (d && *d) ? d : 0;
}

static int ll_is_cd_root(const char* p)
{
    return p && (p[0] == LL_CD_LETTER || p[0] == LL_CD_LETTER + 32) && p[1] == ':';
}

static void ll_host_path(char* out, unsigned int cap, const char* in)
{
    const char*  cd = ll_cd_dir();
    unsigned int i = 0;
    if (!in) {
        out[0] = 0;
        return;
    }
    if (cd && ll_is_cd_root(in)) {           /* "D:\x" -> "<LL_CD_DIR>/x" */
        while (cd[i] && i + 1 < cap) {
            out[i] = cd[i];
            i++;
        }
        in += 2;
        if (*in == '\\' || *in == '/')
            in++;
        if (i + 1 < cap)
            out[i++] = '/';
    }
    for (; i + 1 < cap && *in; in++, i++)
        out[i] = *in == '\\' ? '/' : *in;
    out[i] = 0;
}

/* ---- resolving a path the game wrote for a 1999 Windows install ----------
 *
 * Two things are wrong with those paths on any host that is not FAT/NTFS, and
 * both are fatal rather than cosmetic:
 *
 * 1. CASE. The game asks for "LEGOLAND.ICM" and the file is `Legoland.icm`;
 *    it asks for "Stab.str" and gets `stab.str`. APFS is case-insensitive by
 *    default so this works on this Mac by luck, and it fails on Linux CI, in
 *    the browser's preloaded MEMFS (which is case-SENSITIVE whatever the
 *    machine that packaged it), and under NODERAWFS on ext4.
 *
 * 2. LAYOUT. The paths carry the install's directories -- ".\strings\stab.str",
 *    ".\3ddata\%s", ".\graphics\%s", ".\volumes\%s.res", ".\CompSprite\%s",
 *    ".\dlls\%s" -- and the asset tree in `gamedata/main` is FLAT: all 331
 *    files in one directory, `stab.str` among them. The first casualty is the
 *    string table, and it is not a soft failure: LoadStrings (narration2.c
 *    0x00498d00) reproduces the original's `if (!f) exit(1)`, so the whole
 *    program vanished with status 1 and no message, right after the volumes
 *    mounted.
 *
 * So: try the path as written; then match each component case-insensitively
 * against the directory that holds it; then, if a DIRECTORY component cannot
 * be matched at all, look for the remaining name in the deepest directory that
 * did match. That last step is what makes a flattened install work, and it can
 * only ever find a file the game asked for by name.
 *
 * Read-only lookups only. A path that is being created must not be rewritten,
 * so callers pass `create` for those and get the plain normalisation.
 */
static int ll_dir_find(const char* dir, const char* name, char* out, size_t cap)
{
    DIR*           d;
    struct dirent* e;
    int            found = 0;
    d = opendir(dir[0] ? dir : ".");
    if (!d)
        return 0;
    while ((e = readdir(d)) != 0) {
        if (strcasecmp(e->d_name, name) == 0) {
            if (strlen(e->d_name) + 1 <= cap) {
                strcpy(out, e->d_name);
                found = 1;
            }
            break;
        }
    }
    closedir(d);
    return found;
}

/* `built` (the resolved prefix so far, "" meaning the working directory) plus
 * one component, into `out`. Returns 0 if it would not fit. */
static int ll_join(char* out, size_t cap, const char* built, const char* name)
{
    size_t n = strlen(built);
    size_t m = strlen(name);
    int    sep = n && built[n - 1] != '/';
    if (n + (size_t)sep + m + 1 > cap)
        return 0;
    memcpy(out, built, n);
    if (sep)
        out[n++] = '/';
    memcpy(out + n, name, m + 1);
    return 1;
}

/* `path` is normalised and rewritten in place. Returns 1 if it now names
 * something that exists. */
static int ll_resolve_inplace(char* path, size_t cap)
{
    char        built[1024];
    char        cand[1024];
    char        comp[256];
    char        match[256];
    struct stat st;
    const char* p = path;

    if (stat(path, &st) == 0)
        return 1;

    built[0] = 0;
    if (*p == '/') {                       /* keep an absolute root */
        strcpy(built, "/");
        p++;
    }
    while (*p) {
        const char* slash = strchr(p, '/');
        size_t      len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0 || (len == 1 && p[0] == '.')) {     /* "//", "./", "." */
            if (!slash)
                break;
            p = slash + 1;
            continue;
        }
        if (len >= sizeof comp)
            return 0;
        memcpy(comp, p, len);
        comp[len] = 0;

        if (ll_join(cand, sizeof cand, built, comp) && stat(cand, &st) == 0) {
            strcpy(built, cand);           /* exact */
        } else if (ll_dir_find(built, comp, match, sizeof match) &&
                   ll_join(cand, sizeof cand, built, match)) {
            strcpy(built, cand);           /* same name, different case */
        } else if (slash) {
            /* A missing DIRECTORY component: the install was flattened. Look
             * for the LAST component of the whole path right here, which can
             * only ever find a file the game asked for by name. */
            const char* last = strrchr(p, '/');
            last = last ? last + 1 : p;
            if (!ll_dir_find(built, last, match, sizeof match) ||
                !ll_join(cand, sizeof cand, built, match) ||
                strlen(cand) + 1 > cap)
                return 0;
            strcpy(path, cand);
            return stat(path, &st) == 0;
        } else {
            return 0;
        }
        if (!slash)
            break;
        p = slash + 1;
    }
    if (strlen(built) + 1 > cap)
        return 0;
    strcpy(path, built);
    return stat(path, &st) == 0;
}

/* The shim's one path entry point; msvcrt.c uses it too (ll_host.h). */
void ll_host_resolve_path(char* out, unsigned int cap, const char* in, int create)
{
    ll_host_path(out, cap, in);
    if (create || !out[0])
        return;
    ll_resolve_inplace(out, cap);
}

/* ---- handles ------------------------------------------------------------- */
/* One table for every waitable or closable object. A HANDLE is the table
 * index plus one, so it is never 0 and never -1 (INVALID_HANDLE_VALUE), both
 * of which the game tests for. */

enum { LL_H_NONE = 0, LL_H_FILE, LL_H_EVENT, LL_H_MUTEX, LL_H_THREAD };

typedef struct LLHandle {
    int kind;
    int fd;            /* LL_H_FILE */
    int signalled;     /* LL_H_EVENT */
    int manual;        /* LL_H_EVENT: manual-reset */
} LLHandle;

#define LL_MAX_HANDLES 256
static LLHandle g_handles[LL_MAX_HANDLES];
static DWORD    g_last_error;

static LLHandle* ll_handle(HANDLE h)
{
    unsigned int i = (unsigned int)(__UINTPTR_TYPE__)h;
    if (i == 0 || i > LL_MAX_HANDLES)
        return 0;
    return g_handles[i - 1].kind == LL_H_NONE ? 0 : &g_handles[i - 1];
}

static HANDLE ll_handle_new(int kind)
{
    int i;
    for (i = 0; i < LL_MAX_HANDLES; i++) {
        if (g_handles[i].kind == LL_H_NONE) {
            memset(&g_handles[i], 0, sizeof g_handles[i]);
            g_handles[i].kind = kind;
            g_handles[i].fd = -1;
            return (HANDLE)(__UINTPTR_TYPE__)(i + 1);
        }
    }
    return 0;
}

/* ---- waitable objects ---------------------------------------------------- */

HANDLE CreateMutexA(SECURITY_ATTRIBUTES* sa, BOOL initial_owner, LPCSTR name)
{
    LL_TRACE("CreateMutexA(\"%s\")", name ? name : "");
    (void)sa;
    (void)initial_owner;
    (void)name;
    return ll_handle_new(LL_H_MUTEX);
}

HANDLE CreateEventA(SECURITY_ATTRIBUTES* sa, BOOL manual_reset, BOOL initial, LPCSTR name)
{
    LL_TRACE("CreateEventA(manual=%d, initial=%d)", manual_reset, initial);
    HANDLE    h = ll_handle_new(LL_H_EVENT);
    LLHandle* e = ll_handle(h);
    (void)sa;
    (void)name;
    if (e) {
        e->manual = manual_reset;
        e->signalled = initial;
    }
    return h;
}

BOOL SetEvent(HANDLE h)
{
    LLHandle* e = ll_handle(h);
    if (!e)
        return 0;
    e->signalled = 1;
    return 1;
}

BOOL ResetEvent(HANDLE h)
{
    LLHandle* e = ll_handle(h);
    if (!e)
        return 0;
    e->signalled = 0;
    return 1;
}

/* Nothing else runs, so a wait can only succeed (there is no other thread to
 * wait for) -- and succeeding is what the game's checks want. An auto-reset
 * event is consumed by the wait, as on Win32. */
DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    LL_TRACE("WaitForSingleObject(%u, %u)", (unsigned)(__UINTPTR_TYPE__)h, (unsigned)ms);
    LLHandle* o = ll_handle(h);
    (void)ms;
    if (!o)
        return LL_WAIT_FAILED;
    if (o->kind == LL_H_EVENT && !o->manual)
        o->signalled = 0;
    return LL_WAIT_OBJECT_0;
}

DWORD WaitForMultipleObjects(DWORD count, HANDLE* handles, BOOL wait_all, DWORD ms)
{
    LL_TRACE("WaitForMultipleObjects(%u)", (unsigned)count);
    (void)ms;
    if (!count || !handles)
        return LL_WAIT_FAILED;
    if (!wait_all) {
        DWORD i;
        for (i = 0; i < count; i++) {
            LLHandle* o = ll_handle(handles[i]);
            if (o && o->kind == LL_H_EVENT && o->signalled) {
                if (!o->manual)
                    o->signalled = 0;
                return LL_WAIT_OBJECT_0 + i;
            }
        }
    }
    return LL_WAIT_OBJECT_0;
}

BOOL CloseHandle(HANDLE h)
{
    LL_TRACE("CloseHandle(%u)", (unsigned)(__UINTPTR_TYPE__)h);
    LLHandle* o = ll_handle(h);
    if (!o)
        return 0;
    if (o->kind == LL_H_FILE && o->fd >= 0)
        close(o->fd);
    o->kind = LL_H_NONE;
    o->fd = -1;
    return 1;
}

/* ---- threads ------------------------------------------------------------- */
/* The port is single-threaded: refusing here makes the game keep the work on
 * the main thread instead of handing it to a helper that would never run. */

HANDLE CreateThread(SECURITY_ATTRIBUTES* sa, DWORD stack, void* start, void* param,
                    DWORD flags, DWORD* id)
{
    LL_TRACE("CreateThread: refused, the port is single-threaded");
    (void)sa;
    (void)stack;
    (void)start;
    (void)param;
    (void)flags;
    if (id)
        *id = 0;
    g_last_error = 120;          /* ERROR_CALL_NOT_IMPLEMENTED */
    return 0;
}

DWORD ResumeThread(HANDLE h)  { (void)h; return 0; }
DWORD SuspendThread(HANDLE h) { (void)h; return 0; }

BOOL TerminateThread(HANDLE h, DWORD exit_code)
{
    (void)h;
    (void)exit_code;
    return 1;
}

/* ---- time ---------------------------------------------------------------- */

static double ll_now_ms(void)
{
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

DWORD GetTickCount(void)
{
    return (DWORD)(unsigned int)(unsigned long long)ll_now_ms();
}

/* One microsecond ticks: a frequency the game's 32-bit arithmetic survives
 * (movie2.c divides by it). */
BOOL QueryPerformanceCounter(long long* counter)
{
    if (!counter)
        return 0;
    *counter = (long long)(ll_now_ms() * 1000.0);
    return 1;
}

BOOL QueryPerformanceFrequency(long long* frequency)
{
    if (!frequency)
        return 0;
    *frequency = 1000000;
    return 1;
}

/* Win32 FILETIME: 100 ns ticks since 1601-01-01. */
#define LL_FT_EPOCH_DELTA 116444736000000000ll

void GetSystemTimeAsFileTime(FILETIME* ft)
{
    unsigned long long t;
    if (!ft)
        return;
    t = (unsigned long long)((long long)time(0) * 10000000ll + LL_FT_EPOCH_DELTA);
    ft->dwLowDateTime = (DWORD)(t & 0xffffffffu);
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

BOOL FileTimeToLocalFileTime(const FILETIME* in, FILETIME* out)
{
    if (!in || !out)
        return 0;
    *out = *in;                  /* the port reports UTC as local time */
    return 1;
}

BOOL FileTimeToDosDateTime(const FILETIME* ft, WORD* date, WORD* time_out)
{
    unsigned long long t;
    time_t             unix_time;
    struct tm          tmv;
    if (!ft || !date || !time_out)
        return 0;
    t = ((unsigned long long)ft->dwHighDateTime << 32) | (unsigned int)ft->dwLowDateTime;
    unix_time = (time_t)((t - LL_FT_EPOCH_DELTA) / 10000000ull);
    gmtime_r(&unix_time, &tmv);
    *date = (WORD)((((tmv.tm_year + 1900 - 1980) & 0x7f) << 9) |
                   (((tmv.tm_mon + 1) & 0xf) << 5) | (tmv.tm_mday & 0x1f));
    *time_out = (WORD)(((tmv.tm_hour & 0x1f) << 11) | ((tmv.tm_min & 0x3f) << 5) |
                       ((tmv.tm_sec / 2) & 0x1f));
    return 1;
}

/* ---- Sleep and the main-loop hook --------------------------------------- */
/* PORT-B owns the main-loop strategy (docs/SCOPE_PORT_WAVE.md PORT-B.1). Its
 * notes did not exist when this was written, so Sleep is a NO-OP: nothing in
 * the startup spine sleeps to wait for another thread, and a blocking sleep
 * in a browser would freeze the page. The hook is the seam -- PORT-B sets
 *
 *     ll_host_sleep_hook = ll_host_yield;   // PORT-B: emscripten_sleep(ms) under ASYNCIFY
 *
 * once it decides, and gamemain.c's Sleep(...) in the frame loop starts
 * yielding to the host with no change here. */
void (*ll_host_sleep_hook)(unsigned int ms);

void Sleep(DWORD ms)
{
    LL_TRACE("Sleep(%u)%s", (unsigned)ms, ll_host_sleep_hook ? "" : ": no-op");
    if (ll_host_sleep_hook)
        ll_host_sleep_hook((unsigned int)ms);
}

/* ---- files --------------------------------------------------------------- */
/* Win32 CreateFileA flags the game uses: GENERIC_READ 0x80000000,
 * GENERIC_WRITE 0x40000000, CREATE_ALWAYS 2, OPEN_EXISTING 3. */

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, SECURITY_ATTRIBUTES* sa,
                   DWORD disposition, DWORD flags, HANDLE template_file)
{
    LL_TRACE("CreateFileA(\"%s\", access=0x%08x, disp=%u)", name ? name : "", (unsigned)access, (unsigned)disposition);
    char      path[1024];
    int       oflag;
    int       fd;
    HANDLE    h;
    LLHandle* f;
    (void)share;
    (void)sa;
    (void)flags;
    (void)template_file;

    /* CREATE_NEW/CREATE_ALWAYS/OPEN_ALWAYS may be making the file, so the name
     * must be taken as written; everything else gets the tolerant lookup. */
    ll_host_resolve_path(path, sizeof path, name,
                         disposition == 1 || disposition == 2 || disposition == 4);
    if ((access & 0x40000000u) && (access & 0x80000000u))
        oflag = O_RDWR;
    else if (access & 0x40000000u)
        oflag = O_WRONLY;
    else
        oflag = O_RDONLY;
    if (disposition == 1 || disposition == 2)          /* CREATE_NEW/ALWAYS */
        oflag |= O_CREAT | O_TRUNC;
    else if (disposition == 4)                         /* OPEN_ALWAYS */
        oflag |= O_CREAT;
    if (disposition == 1)
        oflag |= O_EXCL;

    fd = open(path, oflag, 0644);
    if (fd < 0) {
        g_last_error = 2;                              /* ERROR_FILE_NOT_FOUND */
        return LL_INVALID_HANDLE_VALUE;
    }
    h = ll_handle_new(LL_H_FILE);
    f = ll_handle(h);
    if (!f) {
        close(fd);
        g_last_error = 4;                              /* ERROR_TOO_MANY_OPEN_FILES */
        return LL_INVALID_HANDLE_VALUE;
    }
    f->fd = fd;
    return h;
}

BOOL ReadFile(HANDLE h, void* buf, DWORD n, DWORD* got, void* overlapped)
{
    LL_TRACE("ReadFile(%u, %u bytes)", (unsigned)(__UINTPTR_TYPE__)h, (unsigned)n);
    LLHandle* f = ll_handle(h);
    long      r;
    (void)overlapped;
    if (got)
        *got = 0;
    if (!f || f->kind != LL_H_FILE)
        return 0;
    r = (long)read(f->fd, buf, (size_t)n);
    if (r < 0)
        return 0;
    if (got)
        *got = (DWORD)r;
    return 1;
}

BOOL WriteFile(HANDLE h, const void* buf, DWORD n, DWORD* written, void* overlapped)
{
    LL_TRACE("WriteFile(%u, %u bytes)", (unsigned)(__UINTPTR_TYPE__)h, (unsigned)n);
    LLHandle* f = ll_handle(h);
    long      r;
    (void)overlapped;
    if (written)
        *written = 0;
    if (!f || f->kind != LL_H_FILE)
        return 0;
    r = (long)write(f->fd, buf, (size_t)n);
    if (r < 0)
        return 0;
    if (written)
        *written = (DWORD)r;
    return 1;
}

DWORD SetFilePointer(HANDLE h, LONG offset, LONG* offset_high, DWORD method)
{
    LL_TRACE("SetFilePointer(%u, %ld, method=%u)", (unsigned)(__UINTPTR_TYPE__)h, (long)offset, (unsigned)method);
    LLHandle* f = ll_handle(h);
    off_t     pos;
    int       whence = method == 1 ? SEEK_CUR : method == 2 ? SEEK_END : SEEK_SET;
    if (!f || f->kind != LL_H_FILE)
        return LL_INVALID_SET_FILE_POINTER;
    pos = lseek(f->fd, (off_t)offset, whence);
    if (pos < 0)
        return LL_INVALID_SET_FILE_POINTER;
    if (offset_high)
        *offset_high = 0;
    return (DWORD)pos;
}

/* res.c and memdb.c import SetFilePointer / ReadFile under their own names
 * (`RES_LowSeek` [0x4ab104], `RES_LowRead` [0x4ab264] -- the import thunks'
 * addresses, which gen_link cannot map to a KERNEL32 name). Forward them here
 * so the loaders never trap (scope PORT-C finding 4). */
int RES_LowSeek(int h, int off, int a, int b)
{
    return (int)SetFilePointer((HANDLE)(__UINTPTR_TYPE__)h, (LONG)off,
                               (LONG*)(__UINTPTR_TYPE__)a, (DWORD)b);
}
int RES_LowRead(int h, void* buf, int n, int* got, int ov)
{
    return (int)ReadFile((HANDLE)(__UINTPTR_TYPE__)h, buf, (DWORD)n,
                         (DWORD*)got, (void*)(__UINTPTR_TYPE__)ov);
}

DWORD GetFileSize(HANDLE h, DWORD* size_high)
{
    LL_TRACE("GetFileSize(%u)", (unsigned)(__UINTPTR_TYPE__)h);
    LLHandle*   f = ll_handle(h);
    struct stat st;
    if (size_high)
        *size_high = 0;
    if (!f || f->kind != LL_H_FILE || fstat(f->fd, &st) != 0)
        return 0xffffffffu;
    return (DWORD)st.st_size;
}

BOOL GetFileTime(HANDLE h, FILETIME* created, FILETIME* accessed, FILETIME* written)
{
    LLHandle*   f = ll_handle(h);
    struct stat st;
    if (!f || f->kind != LL_H_FILE || fstat(f->fd, &st) != 0)
        return 0;
    {
        unsigned long long t = (unsigned long long)((long long)st.st_mtime * 10000000ll +
                                                    LL_FT_EPOCH_DELTA);
        FILETIME           ft;
        ft.dwLowDateTime = (DWORD)(t & 0xffffffffu);
        ft.dwHighDateTime = (DWORD)(t >> 32);
        if (created)
            *created = ft;
        if (accessed)
            *accessed = ft;
        if (written)
            *written = ft;
    }
    return 1;
}

DWORD GetCurrentDirectoryA(DWORD size, LPSTR buf)
{
    char cwd[1024];
    unsigned int n;
    if (!getcwd(cwd, sizeof cwd))
        return 0;
    n = (unsigned int)strlen(cwd);
    if (!buf || size <= n)
        return n + 1;
    memcpy(buf, cwd, n + 1);
    return n;
}

BOOL SetCurrentDirectoryA(LPCSTR path)
{
    LL_TRACE("SetCurrentDirectoryA(\"%s\")", path ? path : "");
    char p[1024];
    ll_host_resolve_path(p, sizeof p, path, 0);
    return chdir(p) == 0;
}

/* The game only ever _splitpath's this to find its own directory. */
DWORD GetModuleFileNameA(HMODULE module, LPSTR buf, DWORD size)
{
    LL_TRACE("GetModuleFileNameA");
    char cwd[1024];
    char full[1100];
    unsigned int n;
    (void)module;
    if (!buf || size == 0)
        return 0;
    if (!getcwd(cwd, sizeof cwd))
        strcpy(cwd, ".");
    snprintf(full, sizeof full, "%s/legoland.exe", cwd);
    n = (unsigned int)strlen(full);
    if (n >= size)
        n = size - 1;
    memcpy(buf, full, n);
    buf[n] = 0;
    return n;
}

/* The headless and browser hosts pass the switches to WinMain themselves;
 * this is here for code that asks the system instead. */
static const char* g_command_line = "legoland.exe";

void ll_host_set_command_line(const char* cmdline)
{
    if (cmdline)
        g_command_line = cmdline;
}

LPSTR GetCommandLineA(void)
{
    return (LPSTR)g_command_line;
}

/* ---- drives -------------------------------------------------------------- */
/* sysmisc2.c walks the drive letters looking for the CD by volume label. The
 * port reports one fixed drive and no label, so that search fails and the
 * game falls back to its local resource path -- the data layout under
 * gamedata/ is the installed one, not the disc. */

DWORD GetLogicalDrives(void)
{
    LL_TRACE("GetLogicalDrives");
    return ll_cd_dir() ? 0x4 | (1u << (LL_CD_LETTER - 'A')) : 0x4;
}

UINT GetDriveTypeA(LPCSTR root)
{
    UINT type = (ll_cd_dir() && ll_is_cd_root(root)) ? 5 : 3;  /* CDROM : FIXED */
    LL_TRACE("GetDriveTypeA(\"%s\") = %u", root ? root : "", type);
    return type;
}

/* The label the game looks for is kVolLegoland, "LEGOLAND", and it insists on
 * CDFS as well (sysmisc2.c RES_FindVolumeOnAnyDrive); answering both is what
 * makes RES_EnsureMounted stop asking for the disc and set g_res_path to the
 * drive root. */
BOOL GetVolumeInformationA(LPCSTR root, LPSTR name, DWORD name_size, DWORD* serial,
                           DWORD* max_component, DWORD* flags, LPSTR fs_name,
                           DWORD fs_name_size)
{
    int cd = ll_cd_dir() && ll_is_cd_root(root);
    LL_TRACE("GetVolumeInformationA(\"%s\"): %s", root ? root : "",
             cd ? "LEGOLAND on CDFS" : "no label");
    if (name && name_size) {
        strncpy(name, cd ? "LEGOLAND" : "", name_size - 1);
        name[name_size - 1] = 0;
    }
    if (serial)
        *serial = cd ? 0x1e60a4d0u : 0;
    if (max_component)
        *max_component = 255;
    if (flags)
        *flags = 0;
    if (fs_name && fs_name_size) {
        strncpy(fs_name, cd ? "CDFS" : "", fs_name_size - 1);
        fs_name[fs_name_size - 1] = 0;
    }
    return cd;
}

BOOL DeviceIoControl(HANDLE h, DWORD code, void* in, DWORD in_size,
                     void* out, DWORD out_size, DWORD* returned, void* overlapped)
{
    LL_TRACE("DeviceIoControl: refused");
    (void)h; (void)code; (void)in; (void)in_size; (void)out; (void)out_size;
    (void)overlapped;
    if (returned)
        *returned = 0;
    return 0;                    /* no raw CD reads in the port */
}

/* ---- memory -------------------------------------------------------------- */
/* certificate.c allocates a moveable block, locks it, fills it and unlocks
 * it. malloc plus an identity lock is all that needs. */

HGLOBAL GlobalAlloc(UINT flags, DWORD bytes)
{
    LL_TRACE("GlobalAlloc(%u bytes)", (unsigned)bytes);
    void* p = malloc(bytes ? (size_t)bytes : 1);
    if (p && (flags & 0x40))     /* GMEM_ZEROINIT */
        memset(p, 0, (size_t)bytes);
    return (HGLOBAL)p;
}

void*   GlobalLock(HGLOBAL h)   { return (void*)h; }
BOOL    GlobalUnlock(HGLOBAL h) { (void)h; return 1; }

HGLOBAL GlobalFree(HGLOBAL h)
{
    free((void*)h);
    return 0;
}

HLOCAL LocalAlloc(UINT flags, UINT bytes)
{
    return (HLOCAL)GlobalAlloc(flags, (DWORD)bytes);
}

HLOCAL LocalFree(HLOCAL h)
{
    free((void*)h);
    return 0;
}

void GlobalMemoryStatus(MEMORYSTATUS* status)
{
    LL_TRACE("GlobalMemoryStatus");
    if (!status)
        return;
    memset(status, 0, sizeof *status);
    status->dwLength = sizeof *status;
    status->dwMemoryLoad = 50;
    status->dwTotalPhys = 256u * 1024u * 1024u;
    status->dwAvailPhys = 128u * 1024u * 1024u;
    status->dwTotalPageFile = 256u * 1024u * 1024u;
    status->dwAvailPageFile = 128u * 1024u * 1024u;
    status->dwTotalVirtual = 512u * 1024u * 1024u;
    status->dwAvailVirtual = 256u * 1024u * 1024u;
}

/* Only the crash reporter calls this, to describe the faulting address. */
LL_SIZE_T VirtualQuery(const void* address, MEMORY_BASIC_INFORMATION* info, LL_SIZE_T length)
{
    LL_TRACE("VirtualQuery");
    if (!info || length < sizeof *info)
        return 0;
    memset(info, 0, sizeof *info);
    info->BaseAddress = (void*)address;
    info->AllocationBase = (void*)address;
    info->AllocationProtect = 0x04;        /* PAGE_READWRITE */
    info->RegionSize = 0x1000;
    info->State = 0x1000;                  /* MEM_COMMIT */
    info->Protect = 0x04;
    info->Type = 0x20000;                  /* MEM_PRIVATE */
    return sizeof *info;
}

/* A wasm linear-memory read cannot fault the way a Win32 one can; the
 * blitters only ask to decide whether to skip a source rectangle. */
BOOL IsBadReadPtr(const void* p, UINT n)
{
    (void)n;
    return p == 0;
}

/* ---- modules ------------------------------------------------------------- */
/* No dynamic loading: the AVI playback and DirectMusic paths that ask for a
 * DLL are stubs, and failing here is the branch the game already handles. */

HMODULE LoadLibraryA(LPCSTR name)
{
    LL_TRACE("LoadLibraryA(\"%s\"): refused", name ? name : "");
    (void)name;
    g_last_error = 126;          /* ERROR_MOD_NOT_FOUND */
    return 0;
}

HMODULE LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
{
    LL_TRACE("LoadLibraryExA(\"%s\"): refused", name ? name : "");
    (void)name;
    (void)file;
    (void)flags;
    g_last_error = 126;
    return 0;
}

BOOL FreeLibrary(HMODULE h)
{
    (void)h;
    return 1;
}

/* ---- misc ---------------------------------------------------------------- */

DWORD GetLastError(void)          { return g_last_error; }
void  SetLastError(DWORD err)     { g_last_error = err; }

void OutputDebugStringA(LPCSTR s)
{
    if (!s)
        return;
    fputs(s, stderr);
}

void GetSystemInfo(SYSTEM_INFO* info)
{
    LL_TRACE("GetSystemInfo");
    if (!info)
        return;
    memset(info, 0, sizeof *info);
    info->dwPageSize = 65536;                     /* a wasm page */
    info->lpMinimumApplicationAddress = (void*)0x10000;
    info->lpMaximumApplicationAddress = (void*)0x7fff0000;
    info->dwActiveProcessorMask = 1;
    info->dwNumberOfProcessors = 1;
    info->dwProcessorType = 586;                  /* PROCESSOR_INTEL_PENTIUM */
    info->dwAllocationGranularity = 65536;
    info->wProcessorLevel = 5;
    return;
}

static BOOL ll_copy_name(LPSTR buf, DWORD* size, const char* value)
{
    unsigned int n = (unsigned int)strlen(value);
    if (!buf || !size)
        return 0;
    if (*size <= n) {
        *size = n + 1;
        return 0;
    }
    memcpy(buf, value, n + 1);
    *size = n;
    return 1;
}

BOOL GetComputerNameA(LPSTR buf, DWORD* size)
{
    return ll_copy_name(buf, size, "LEGOLAND-PORTABLE");
}

BOOL GetUserNameA(LPSTR buf, DWORD* size)          /* ADVAPI32 */
{
    return ll_copy_name(buf, size, "player");
}

int MulDiv(int number, int numerator, int denominator)
{
    long long n;
    if (denominator == 0)
        return -1;
    n = (long long)number * (long long)numerator;
    /* Win32 rounds to nearest, away from zero. */
    if ((n < 0) != (denominator < 0))
        return (int)((n - denominator / 2) / denominator);
    return (int)((n + denominator / 2) / denominator);
}

/* The game asks for UTF-16 copies of ASCII file names (the DirectMusic and
 * AVI paths). One byte in, one wide character out is right for its data. */
int MultiByteToWideChar(UINT code_page, DWORD flags, LPCSTR mb, int mb_chars,
                        WCHAR* wide, int wide_chars)
{
    int n;
    int i;
    (void)code_page;
    (void)flags;
    if (!mb)
        return 0;
    n = mb_chars < 0 ? (int)strlen(mb) + 1 : mb_chars;
    if (!wide || wide_chars == 0)
        return n;
    if (n > wide_chars)
        n = wide_chars;
    for (i = 0; i < n; i++)
        wide[i] = (WCHAR)(unsigned char)mb[i];
    return n;
}

LPSTR lstrcpyA(LPSTR dst, LPCSTR src)
{
    if (!dst || !src)
        return dst;
    strcpy(dst, src);
    return dst;
}

int lstrlenA(LPCSTR s)
{
    return s ? (int)strlen(s) : 0;
}

/* ---- VERSION ------------------------------------------------------------- */
/* gameframe.c's ReadExeVersionString does exactly this:
 *
 *     len = GetFileVersionInfoSizeA(exe, &handle);
 *     buf = malloc(len);
 *     GetFileVersionInfoA(exe, handle, len, buf);
 *     VerQueryValueA(buf, "\\StringFileInfo\\...\\ProductVersion", &str, &len);
 *     strcpy(out, str);
 *
 * so the block only has to survive that sequence and yield a string. It is a
 * fixed block here -- a real VS_VERSIONINFO parse would mean reading the
 * resource section of a PE the port does not load -- and the value reaches
 * nothing but the crash log's header line. */

#define LL_VERSION_STRING "1.0.0.0 (LEGOLAND portable)"

DWORD GetFileVersionInfoSizeA(LPCSTR file, DWORD* handle)
{
    LL_TRACE("GetFileVersionInfoSizeA(\"%s\")", file ? file : "");
    (void)file;
    if (handle)
        *handle = 0;
    return (DWORD)sizeof(LL_VERSION_STRING);
}

BOOL GetFileVersionInfoA(LPCSTR file, DWORD handle, DWORD len, void* data)
{
    LL_TRACE("GetFileVersionInfoA");
    (void)file;
    (void)handle;
    if (!data || len < sizeof(LL_VERSION_STRING))
        return 0;
    memcpy(data, LL_VERSION_STRING, sizeof(LL_VERSION_STRING));
    return 1;
}

BOOL VerQueryValueA(const void* block, LPCSTR sub_block, void** buffer, UINT* len)
{
    LL_TRACE("VerQueryValueA(\"%s\")", sub_block ? sub_block : "");
    (void)sub_block;              /* one value in the block: the version string */
    if (!block || !buffer)
        return 0;
    *buffer = (void*)block;
    if (len)
        *len = (UINT)sizeof(LL_VERSION_STRING);
    return 1;
}
