/* LEGOLAND portable build -- the host ABI: every Win32 entry point the shim
 * in portable/src/hostwin/ implements, grouped by the DLL the original
 * imported it from.
 *
 * This header is for the SHIM sources, not for the game: the game's sources
 * declare the imports they use themselves, with VC6 prototypes
 * (`__declspec(dllimport) ... __stdcall`). The shim's definitions have to
 * agree with those declarations at the ABI level, which off x86 means:
 *
 *   - `__stdcall` is ignored, so it is omitted here;
 *   - on wasm32 and i386, `int`, `long`, `unsigned long`, every pointer and
 *     every handle are the same 4-byte machine type, which is why the game
 *     declaring `int CreateFileA(...)` in one file and `void* CreateFileA(...)`
 *     in another is not a problem for either of them. On wasm the signature
 *     (i32 count and result) must still match exactly, or wasm-ld replaces
 *     the call with a trapping stub -- see portable/tools/gen_link.py.
 *
 * Ownership (docs/SCOPE_PORT_WAVE.md): PORT-A created this header and owns
 * the KERNEL32 / ADVAPI32 / VERSION block; PORT-B adds the DDRAW, USER32,
 * GDI32, DINPUT, WINMM and DSOUND blocks as its shims grow.
 */
#ifndef LL_HOST_H
#define LL_HOST_H

#include "windows.h"          /* HWND, MSG, POINT, UINT, DWORD, BOOL, ... */

/* ---- the Win32 types the shim needs beyond the windows.h slice ---------- */
typedef void*          HANDLE;
typedef void*          HMODULE;
typedef void*          HINSTANCE;
typedef void*          HGLOBAL;
typedef void*          HLOCAL;
typedef char*          LPSTR;
typedef const char*    LPCSTR;
typedef unsigned short WORD;
typedef unsigned short WCHAR;
typedef long           LONG;
typedef unsigned long  ULONG;
typedef unsigned int   LL_SIZE_T;          /* Win32 SIZE_T on a 32-bit host */

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME;

typedef struct _SECURITY_ATTRIBUTES {
    DWORD nLength;
    void* lpSecurityDescriptor;
    BOOL  bInheritHandle;
} SECURITY_ATTRIBUTES;

typedef struct _SYSTEM_INFO {             /* 36 bytes, as exceptlog.c reads */
    DWORD dwOemId;
    DWORD dwPageSize;
    void* lpMinimumApplicationAddress;
    void* lpMaximumApplicationAddress;
    DWORD dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD  wProcessorLevel;
    WORD  wProcessorRevision;
} SYSTEM_INFO;

typedef struct _MEMORYSTATUS {            /* 32 bytes */
    DWORD dwLength;
    DWORD dwMemoryLoad;
    DWORD dwTotalPhys;
    DWORD dwAvailPhys;
    DWORD dwTotalPageFile;
    DWORD dwAvailPageFile;
    DWORD dwTotalVirtual;
    DWORD dwAvailVirtual;
} MEMORYSTATUS;

typedef struct _MEMORY_BASIC_INFORMATION {  /* 28 bytes */
    void* BaseAddress;
    void* AllocationBase;
    DWORD AllocationProtect;
    DWORD RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION;

/* Win32 constants the shim and the game agree on. */
#define LL_INVALID_HANDLE_VALUE ((HANDLE)(__INTPTR_TYPE__)-1)
#define LL_WAIT_OBJECT_0        0x00000000ul
#define LL_WAIT_TIMEOUT         0x00000102ul
#define LL_WAIT_FAILED          0xfffffffful
#define LL_INVALID_SET_FILE_POINTER 0xfffffffful

/* ======================================================================== */
/* KERNEL32 -- portable/src/hostwin/kernel32.c (PORT-A)                      */
/* ======================================================================== */

/* Single-threaded waitable objects: a mutex is always free, an event keeps a
 * signalled flag, and a wait on anything returns at once. The game's only
 * real use of them is the one-instance mutex in startup.c and the music
 * thread's handshake, which never runs in the port. */
HANDLE CreateMutexA(SECURITY_ATTRIBUTES* sa, BOOL initial_owner, LPCSTR name);
HANDLE CreateEventA(SECURITY_ATTRIBUTES* sa, BOOL manual_reset, BOOL initial, LPCSTR name);
BOOL   SetEvent(HANDLE h);
BOOL   ResetEvent(HANDLE h);
DWORD  WaitForSingleObject(HANDLE h, DWORD ms);
DWORD  WaitForMultipleObjects(DWORD count, HANDLE* handles, BOOL wait_all, DWORD ms);
BOOL   CloseHandle(HANDLE h);

/* Threads: the port is single-threaded. CreateThread refuses (returns 0) so
 * the game takes its "no helper thread" path, and the three controls are
 * no-ops. */
HANDLE CreateThread(SECURITY_ATTRIBUTES* sa, DWORD stack, void* start, void* param,
                    DWORD flags, DWORD* id);
DWORD  ResumeThread(HANDLE h);
DWORD  SuspendThread(HANDLE h);
BOOL   TerminateThread(HANDLE h, DWORD exit_code);

/* Time. GetTickCount and QueryPerformanceCounter come from the same
 * monotonic clock (emscripten_get_now under emcc, clock_gettime elsewhere). */
DWORD GetTickCount(void);
BOOL  QueryPerformanceCounter(long long* counter);
BOOL  QueryPerformanceFrequency(long long* frequency);
void  GetSystemTimeAsFileTime(FILETIME* ft);
BOOL  FileTimeToLocalFileTime(const FILETIME* in, FILETIME* out);
BOOL  FileTimeToDosDateTime(const FILETIME* ft, WORD* date, WORD* time);

/* Sleep yields to the host. The port has no second thread to yield to, so
 * the body is a no-op plus the hook below: PORT-B sets ll_host_yield to its
 * main-loop yield (emscripten_sleep under ASYNCIFY, or nothing if the canvas
 * host drives the game from emscripten_set_main_loop instead). Keep the hook
 * NULL-checked; nothing in the startup spine needs it. */
extern void (*ll_host_yield)(unsigned int ms);
void Sleep(DWORD ms);

/* Files. Thin wrappers over POSIX (open/read/write/lseek), which under node
 * with -sNODERAWFS=1 is the real filesystem, so gamedata/ is read in place.
 * Paths may use backslashes; they are normalised. */
HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, SECURITY_ATTRIBUTES* sa,
                   DWORD disposition, DWORD flags, HANDLE template_file);
BOOL   ReadFile(HANDLE h, void* buf, DWORD n, DWORD* got, void* overlapped);
BOOL   WriteFile(HANDLE h, const void* buf, DWORD n, DWORD* written, void* overlapped);
DWORD  SetFilePointer(HANDLE h, LONG offset, LONG* offset_high, DWORD method);
DWORD  GetFileSize(HANDLE h, DWORD* size_high);
BOOL   GetFileTime(HANDLE h, FILETIME* created, FILETIME* accessed, FILETIME* written);
DWORD  GetCurrentDirectoryA(DWORD size, LPSTR buf);
BOOL   SetCurrentDirectoryA(LPCSTR path);
DWORD  GetModuleFileNameA(HMODULE module, LPSTR buf, DWORD size);
LPSTR  GetCommandLineA(void);
/* The host program tells the shim what GetCommandLineA should answer; the
 * game itself is handed its switches as WinMain's argument. */
void   ll_host_set_command_line(const char* cmdline);

/* The game enumerates the CD with these (sysmisc2.c). The port reports one
 * fixed drive and no volume label, which makes the CD search fail and the
 * game fall back to its local path. */
DWORD GetLogicalDrives(void);
UINT  GetDriveTypeA(LPCSTR root);
BOOL  GetVolumeInformationA(LPCSTR root, LPSTR name, DWORD name_size, DWORD* serial,
                            DWORD* max_component, DWORD* flags, LPSTR fs_name,
                            DWORD fs_name_size);
BOOL  DeviceIoControl(HANDLE h, DWORD code, void* in, DWORD in_size,
                      void* out, DWORD out_size, DWORD* returned, void* overlapped);

/* Memory: the Global and Local pairs are malloc with a moveable-handle shape
 * the game never actually relies on (it locks immediately). */
HGLOBAL GlobalAlloc(UINT flags, DWORD bytes);
void*   GlobalLock(HGLOBAL h);
BOOL    GlobalUnlock(HGLOBAL h);
HGLOBAL GlobalFree(HGLOBAL h);
void    GlobalMemoryStatus(MEMORYSTATUS* status);
HLOCAL  LocalAlloc(UINT flags, UINT bytes);
HLOCAL  LocalFree(HLOCAL h);
LL_SIZE_T VirtualQuery(const void* address, MEMORY_BASIC_INFORMATION* info, LL_SIZE_T length);
BOOL    IsBadReadPtr(const void* p, UINT n);

/* Modules: no DLL loading in the port (the AVI and DirectMusic paths that
 * use it are stubs), so LoadLibrary fails and the game skips them. */
HMODULE LoadLibraryA(LPCSTR name);
HMODULE LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags);
BOOL    FreeLibrary(HMODULE h);

/* Misc. */
DWORD GetLastError(void);
void  SetLastError(DWORD err);
void  OutputDebugStringA(LPCSTR s);
void  GetSystemInfo(SYSTEM_INFO* info);
BOOL  GetComputerNameA(LPSTR buf, DWORD* size);
int   MulDiv(int number, int numerator, int denominator);
int   MultiByteToWideChar(UINT code_page, DWORD flags, LPCSTR mb, int mb_chars,
                          WCHAR* wide, int wide_chars);
LPSTR lstrcpyA(LPSTR dst, LPCSTR src);
int   lstrlenA(LPCSTR s);

/* ======================================================================== */
/* ADVAPI32 -- kernel32.c (PORT-A)                                           */
/* ======================================================================== */
BOOL GetUserNameA(LPSTR buf, DWORD* size);

/* ======================================================================== */
/* VERSION -- kernel32.c (PORT-A)                                            */
/* ======================================================================== */
/* A fixed version block: gameframe.c's ReadExeVersionString asks for the
 * size, reads the block, then VerQueryValues the ProductVersion string out
 * of it into g_exe_version (which only ever reaches the crash log). */
DWORD GetFileVersionInfoSizeA(LPCSTR file, DWORD* handle);
BOOL  GetFileVersionInfoA(LPCSTR file, DWORD handle, DWORD len, void* data);
BOOL  VerQueryValueA(const void* block, LPCSTR sub_block, void** buffer, UINT* len);

/* ======================================================================== */
/* USER32 / GDI32 / DDRAW / DINPUT / WINMM / DSOUND -- PORT-B                 */
/* ======================================================================== */
/* PORT-B's shims declare themselves here. The message-pump entry points the
 * game sources include directly are in windows.h, which this header pulls in.
 * Until then gen_link.py generates a trapping stub for each of them, so a
 * run stops with `TRAP DDRAW DirectDrawCreate from gpu.c` rather than
 * misbehaving. */

#endif /* LL_HOST_H */
