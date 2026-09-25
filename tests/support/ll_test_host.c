/* PORT-C test support: the host calls the asset loaders need, nothing more.
 *
 * ====================== READ THIS BEFORE MERGING =========================
 * This file is a STOPGAP owned by PORT-C and it is meant to disappear.
 * PORT-A owns the real shim (`src/hostwin/kernel32.c`, brief
 * deliverable 3, plus the CRT-range wrappers docs/DEVELOPMENT.md's census
 * files as "game-fn"). cmake/tests.cmake compiles this file ONLY
 * while that file does not exist:
 *
 *     if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/kernel32.c")
 *       target_sources(legoland_core PRIVATE tests/support/ll_test_host.c)
 *     endif()
 *
 * so the moment PORT-A lands, this stops being compiled and the tests run
 * against the real shim. If PORT-A's shim turns out NOT to define some of the
 * CRT-range wrappers below, the integrator should move those few into it
 * rather than re-enabling this file.
 * =========================================================================
 *
 * WHY IT IS ATTACHED TO legoland_core AND NOT TO THE TEST EXECUTABLE
 * gen_link.py decides what to trap by asking which symbols the objects under
 * CMakeFiles/legoland_core.dir define (linkreport.collect_objects with
 * only_game=False), while it collects what is REFERENCED only from the
 * decomp/LEGOLAND sources. So a file compiled into legoland_core that is not
 * under LEGOLAND/ suppresses the trap for every symbol it defines and adds
 * nothing to the work list -- exactly the mechanism src/hostwin/msvcrt.c
 * already uses. Defining these in the test executable instead would collide
 * with the trap that gen_link would still emit for them.
 *
 * FINDINGS FOR THE INTEGRATOR (also in docs/lanes/scope-port-c.md):
 *  1. `RES_LowSeek` and `RES_LowRead` (LEGOLAND/res.c, LEGOLAND/memdb.c,
 *     LEGOLAND/sweep4.c) are declared __declspec(dllimport) against the IAT
 *     slots [0x4ab104] and [0x4ab264], which are SetFilePointer and ReadFile.
 *     linkreport.py files them under "Unclassified (83)" because the names are
 *     local, so gen_link traps them instead of routing them to the kernel32
 *     shim. They are an ALIAS PAIR, not missing work; gen_link's alias pass or
 *     win32_imports.txt should learn about them.
 *  2. The twelve "CRT-range wrappers filed as game-fn" are all trivial libc
 *     forwards; their addresses say so (0x0049e4ff = malloc, 0x0049e4d0 =
 *     free, 0x0049e573 = sprintf, 0x0049e4b2 = rand, 0x004a020e = calloc,
 *     0x004aab90 = _stricmp). Four of them are the SAME address under four
 *     names (RES_FreeFile / HeapFree_w / MemFree / ReleaseAnimInstance all at
 *     0x0049e4d0), so they are an alias family too.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

extern int stricmp(const char*, const char*);   /* src/hostwin/portable.c */

/* -------------------------------------------------------------------------
 * The CRT-range wrappers (docs/DEVELOPMENT.md's census calls these 12
 * "game-fn"; they are libc under another name).
 * ------------------------------------------------------------------------- */
void* MemAlloc(unsigned int size)            { return malloc(size); }
void  MemFree(void* p)                       { free(p); }
void* HeapAlloc_w(unsigned int size)         { return malloc(size); }
void  HeapFree_w(void* p)                    { free(p); }
void  RES_FreeFile(void* p)                  { free(p); }
void  ReleaseAnimInstance(void* p)           { free(p); }
void* CRT_calloc(unsigned int n, unsigned int sz) { return calloc(n, sz); }
int   rand_w(void)                           { return rand(); }
int   NameCompare(const char* a, const char* b)   { return stricmp(a, b); }
void  DebugPrint(const char* msg)            { fputs(msg, stderr); }

void Format(char* dst, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsprintf(dst, fmt, ap);
    va_end(ap);
}

int sprintf_w(char* dst, const char* fmt, int v)
{
    return sprintf(dst, fmt, v);
}

/* -------------------------------------------------------------------------
 * The KERNEL32 slice RES_OpenVolume / RES_ReadFile need. Handles are POSIX
 * file descriptors: the game keeps them in an `int` (RVol +0x1c), compares
 * against -1 and never passes them anywhere but these calls.
 * ------------------------------------------------------------------------- */
#define LL_GENERIC_READ  0x80000000u
#define LL_OPEN_EXISTING 3
#define LL_CREATE_ALWAYS 2

static void ll_path(char* out, unsigned int cap, const char* in)
{
    unsigned int i;
    for (i = 0; i + 1 < cap && in[i]; i++)
        out[i] = in[i] == '\\' ? '/' : in[i];
    out[i] = 0;
}

int CreateFileA(const char* name, unsigned int access, unsigned int share,
                void* sa, unsigned int disp, unsigned int flags, void* tmpl)
{
    char p[1024];
    int  oflags;
    (void)share; (void)sa; (void)flags; (void)tmpl;
    ll_path(p, sizeof p, name);
    if (access & 0x40000000u)            /* GENERIC_WRITE */
        oflags = (access & LL_GENERIC_READ) ? O_RDWR : O_WRONLY;
    else
        oflags = O_RDONLY;
    if (disp == LL_CREATE_ALWAYS)
        oflags |= O_CREAT | O_TRUNC;
    return open(p, oflags, 0644);
}

int CloseHandle(int h) { return close(h) == 0; }

int GetFileSize(int h, unsigned int* hi)
{
    struct stat st;
    if (hi)
        *hi = 0;
    if (fstat(h, &st) != 0)
        return -1;
    return (int)st.st_size;
}

/* FILE_BEGIN 0 / FILE_CURRENT 1 / FILE_END 2; `hi` is always 0 in the game. */
int SetFilePointer(int h, int off, int* hi, unsigned int method)
{
    off_t r;
    (void)hi;
    r = lseek(h, off, method == 1 ? SEEK_CUR : method == 2 ? SEEK_END : SEEK_SET);
    return (int)r;
}

int ReadFile(int h, void* buf, unsigned int n, unsigned int* got, void* ov)
{
    long r;
    (void)ov;
    r = (long)read(h, buf, n);
    if (got)
        *got = r < 0 ? 0u : (unsigned int)r;
    return r >= 0;
}

int WriteFile(int h, const void* buf, unsigned int n, unsigned int* put, void* ov)
{
    long r;
    (void)ov;
    r = (long)write(h, buf, n);
    if (put)
        *put = r < 0 ? 0u : (unsigned int)r;
    return r >= 0;
}

void OutputDebugStringA(const char* s) { fputs(s, stderr); }

/* -------------------------------------------------------------------------
 * The CD presence check. RES_EnsureMounted (LEGOLAND/sysmisc.c 0x004515e0)
 * loops until RES_FindVolumeOnResPath (sysmisc2.c 0x004510e0) reports a CDFS
 * volume called "LEGOLAND", nagging with a modal retry box in between.
 * Headless that is an infinite loop through two traps, so EVERY RES_OpenFile
 * call depends on this answer. PORT-A's and PORT-B's hosts need it too; it is
 * finding 3 in docs/lanes/scope-port-c.md.
 * ------------------------------------------------------------------------- */
int GetVolumeInformationA(const char* root, char* volname, unsigned int volcap,
                          unsigned long* serial, unsigned long* maxcomp,
                          unsigned long* fsflags, char* fsname,
                          unsigned int fscap)
{
    (void)root;
    if (volname && volcap >= 9)
        strcpy(volname, "LEGOLAND");
    if (fsname && fscap >= 5)
        strcpy(fsname, "CDFS");
    if (serial)
        *serial = 0x4c45474f;
    if (maxcomp)
        *maxcomp = 255;
    if (fsflags)
        *fsflags = 0;
    return 1;
}

/* GetLogicalDrives / GetDriveTypeA drive the sibling prober
 * (RES_FindVolumeOnAnyDrive 0x00450f30), the arm RES_EnsureMounted takes when
 * its `volume` argument is non-zero. LEGOLAND/res.c passes 0 so the shipped
 * path is the one above; these keep the other arm from trapping. */
#define LL_DRIVE_CDROM 5
unsigned long GetLogicalDrives(void) { return 1uL << ('D' - 'A'); }
unsigned int GetDriveTypeA(const char* root)
{
    return (root && (root[0] == 'D' || root[0] == 'd')) ? LL_DRIVE_CDROM : 0;
}

/* Finding 1 above: these two ARE SetFilePointer and ReadFile, reached through
 * the same IAT slots under local names. __stdcall is ignored off x86. */
int RES_LowSeek(int h, int off, int a, int b)
{
    (void)a; (void)b;
    return SetFilePointer(h, off, 0, 0);
}

int RES_LowRead(int h, void* buf, int n, int* got, int ov)
{
    (void)ov;
    return ReadFile(h, buf, (unsigned int)n, (unsigned int*)got, 0);
}
