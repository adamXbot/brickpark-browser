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

/* Threads: the one thread the game starts (MusicThread) runs on an Emscripten
 * fiber in the browser -- its waits swap back to the main stack and SetEvent
 * resumes it -- and inline to completion everywhere else. kernel32.c's threads
 * note has both. */
HANDLE CreateThread(SECURITY_ATTRIBUTES* sa, DWORD stack, void* start, void* param,
                    DWORD flags, DWORD* id);
DWORD  ResumeThread(HANDLE h);
DWORD  SuspendThread(HANDLE h);
BOOL   TerminateThread(HANDLE h, DWORD exit_code);
/* 1 while a thread's routine is running (on its fiber, or inline). */
int    ll_host_in_thread(void);

/* Time. GetTickCount and QueryPerformanceCounter come from the same
 * monotonic clock (emscripten_get_now under emcc, clock_gettime elsewhere). */
DWORD GetTickCount(void);
BOOL  QueryPerformanceCounter(long long* counter);
BOOL  QueryPerformanceFrequency(long long* frequency);
void  GetSystemTimeAsFileTime(FILETIME* ft);
BOOL  FileTimeToLocalFileTime(const FILETIME* in, FILETIME* out);
BOOL  FileTimeToDosDateTime(const FILETIME* ft, WORD* date, WORD* time);

/* Sleep yields to the host. The port has no second thread to yield to, so
 * the body is a no-op plus the hook below: the browser page (PORT-B) sets
 * ll_host_sleep_hook = ll_host_yield, PORT-B's ASYNCIFY yield (emscripten_sleep).
 * PORT-B's rule: under the browser host Sleep MUST yield, never no-op, or
 * RunGame's music wait hangs the tab. Headless leaves the hook NULL. */
extern void (*ll_host_sleep_hook)(unsigned int ms);
void Sleep(DWORD ms);

/* The shim's one path entry point (PORT-A2). Turns a path the game wrote for
 * a 1999 Windows install into one that exists here:
 *   - "D:\x" -> "$LL_CD_DIR/x", backslashes -> slashes (the old ll_host_path);
 *   - then, unless `create`, each component is matched case-insensitively
 *     against the directory that holds it ("LEGOLAND.ICM" -> `Legoland.icm`);
 *   - and if a DIRECTORY component does not exist at all, the last component
 *     of the path is looked for in the deepest directory that did match, which
 *     is what makes the flattened `gamedata/main` work: the game asks for
 *     ".\strings\stab.str" and the file is `stab.str` in the game directory.
 * `create` (CREATE_NEW / CREATE_ALWAYS / OPEN_ALWAYS, or an O_CREAT open)
 * takes the name as written -- a file being made must not be renamed.
 * msvcrt.c routes _open/fopen/_chdir/_findfirst through this too. */
void ll_host_resolve_path(char* out, unsigned int cap, const char* in, int create);

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
/* DDRAW / USER32 / GDI32 / DINPUT / WINMM / DSOUND -- PORT-B                 */
/* (portable/src/hostwin/{ddraw,user32,gdi32,dinput,winmm,dsound}.c)         */
/* ======================================================================== */
/* The host core below (ll_host_*) is PORT-B's: the browser page sets
 * `ll_host_sleep_hook = ll_host_yield;` before WinMain so KERNEL32's Sleep
 * yields through it; headless (node, no canvas) leaves the hook NULL. */
/* ---- shared scalar types ------------------------------------------------ */
/* ILP32 on the run targets (wasm32 / i386); `long` is 4 bytes there, which is
 * what the game's structs assume. */
typedef void*          LL_HANDLE;
typedef unsigned long  LL_DWORD;
typedef unsigned int   LL_UINT;
typedef long           LL_HRESULT;

typedef struct LLRect { long left, top, right, bottom; } LLRect;
typedef struct LLPoint { long x, y; } LLPoint;

/* ---- host core (PORT-B, portable/src/hostwin/user32.c) ------------------ */

/* THE main-loop primitive. See docs/lanes/scope-port-b.md §1: the game's frame
 * is produced inside synchronous spin loops, so the host yields on its behalf.
 * Under Emscripten this is emscripten_sleep(ms) (ASYNCIFY unwinds the whole C
 * stack, returns to the browser event loop and rewinds on the next tick);
 * elsewhere it is a no-op. ms == 0 means "give the browser one turn".
 *
 * Nothing may call back into wasm while a yield is unwound: canvas event
 * handlers only enqueue, and timeSetEvent callbacks are dispatched from the
 * message pump, never from a JS timer. */
void ll_host_yield(unsigned int ms);
/* Yield only if at least min_gap_ms has passed since the last yield. This is
 * what bounds every wall-clock spin in the game: winmm.c's timeGetTime calls
 * it with 4. */
void ll_host_yield_throttled(unsigned int min_gap_ms);

/* ---- PORT-B8: the heartbeat (the wedge detector) -------------------------
 *
 * When the game stops reaching a yield the page's main thread stops running
 * JS entirely: llFrameHash, llStats and every javascript_tool call time out,
 * so the ONLY channel left out of a wedged tab is stderr, which reaches the
 * devtools console over CDP without the page's main thread doing anything.
 *
 * ll_host_beat is one call at the top of every host entry point that a spin
 * loop could plausibly sit on. It costs an increment, and once every
 * LL_BEAT_MS of wall clock it prints one line naming the last entry point and
 * the per-family counts since the previous line. Reading the console tail of a
 * wedged tab then answers the only question that matters:
 *
 *   the heartbeat keeps ticking  -> the loop IS calling the host and the host
 *                                   is not yielding: a shim bug, fixable here.
 *   the heartbeat stops dead     -> the loop calls nothing: it is in the game,
 *                                   and the last beat line names the host call
 *                                   it made immediately before.
 *
 * Off unless $LL_HOST_BEAT is set (the page's ?beat=1), so a normal run pays
 * one predictable branch per host call. */
void ll_host_beat(const char* who);
/* Non-zero when $LL_HOST_BEAT is set; the entry points test it inline rather
 * than paying a call. */
int ll_host_beating(void);

/* Open the canvas at w x h and set the 16-bpp mode the game asked for. Called
 * by ddraw.c's SetDisplayMode; idempotent. */
void ll_host_display_open(int w, int h);
/* Push one 16-bpp (RGB565) frame to the canvas, then yield so the browser
 * composites it. Called by ddraw.c when the primary surface changes. */
void ll_host_present16(const void* pixels, int w, int h, int pitch);

/* Drain the browser's input queue into the shim's keyboard array, mouse
 * deltas and Win32 message queue. Safe and cheap to call repeatedly; called
 * from the message pump and from DirectInput's GetDeviceState. */
void ll_host_drain_events(void);
/* Post a Win32 message into the shim's queue (used by the event translation
 * and by PostQuitMessage). */
void ll_host_post_message(unsigned int msg, unsigned int wparam, long lparam);

/* The window the shim created, or 0 before InitScreen. */
void* ll_host_hwnd(void);
/* The display extent ddraw.c settled on (640x480 until SetDisplayMode). */
void ll_host_display_size(int* w, int* h);

/* DirectInput state, owned by dinput.c and fed by ll_host_drain_events. */
unsigned char* ll_host_key_state(void);                 /* 256 DIK_* bytes */
void ll_host_key_set(int dik, int down);
void ll_host_mouse_move(int dx, int dy);
void ll_host_mouse_wheel(int dz);
void ll_host_mouse_button(int button, int down);

/* Run every timeSetEvent callback whose period has elapsed. winmm.c; called
 * once per message-pump pass. */
void ll_host_pump_timers(void);

/* LL_HOST_TRACE, for the DirectX half of the shim. kernel32.c has its own copy
 * of this switch (PORT-A); this is the same env var read independently, so
 * ddraw.c / user32.c / gdi32.c / dinput.c can trace without a change in a file
 * this lane does not own. Trace ONE-OFF events only -- object creation, mode
 * changes, the first present -- never per-frame calls. PORT-B2; user32.c. */
int  ll_host_tracing(void);
void ll_host_trace(const char* fmt, ...);

/* The last MessageBoxA the shim answered, for the page's status line: the text,
 * the caption, and the button id it answered with. PORT-B2; user32.c. */
const char* ll_host_last_messagebox(void);
int         ll_host_last_messagebox_answer(void);

/* ---- DDRAW (portable/src/hostwin/ddraw.c) ------------------------------- */
/* The one exported entry; everything else is reached through the C vtables it
 * hands back. Returns DD_OK (0) and a fully populated IDirectDraw. */
long DirectDrawCreate(void* guid, void** out, void* outer);

/* How many frames have reached the canvas. */
int ll_host_frames_presented(void);

/* Resolve an HDC that IDirectDrawSurface::GetDC handed out (ddraw.c returns the
 * surface pointer itself) to the surface's 16-bpp pixels. Returns 0 when the
 * handle is not one of ddraw.c's surfaces -- a memory DC from
 * CreateCompatibleDC, say -- in which case there is nothing to draw into and
 * the GDI call becomes the no-op it was before. PORT-B2; the vtable pointer is
 * the identity check, so a stray cookie cannot be mistaken for a surface. */
int ll_host_surface_pixels(void* hdc, unsigned short** bits,
                           int* w, int* h, int* pitch);

/* ---- USER32 (portable/src/hostwin/user32.c) ----------------------------- */
/* Rect maths: real implementations, Win32 semantics (exclusive right/bottom).
 * These are load-bearing for the renderer's clipping, not stubs. */
int   IntersectRect(LLRect* dst, const LLRect* a, const LLRect* b);
int   OffsetRect(LLRect* rc, int dx, int dy);
int   PtInRect(const LLRect* rc, LLPoint pt);
int   AdjustWindowRect(LLRect* rc, unsigned long style, int menu);

/* Window and class: one window, one class, the wndproc remembered so
 * DispatchMessageA can call it. */
unsigned short RegisterClassExA(const void* wcex);
void* CreateWindowExA(unsigned long ex, const char* cls, const char* name,
                      unsigned long style, int x, int y, int w, int h,
                      void* parent, void* menu, void* inst, void* param);
int   DestroyWindow(void* hwnd);
void* GetDesktopWindow(void);
int   ShowWindow(void* hwnd, int cmd);
long  GetWindowLongA(void* hwnd, int index);
int   GetClientRect(void* hwnd, LLRect* rc);
int   ClientToScreen(void* hwnd, LLPoint* pt);
int   GetSystemMetrics(int index);
int   SystemParametersInfoA(unsigned int action, unsigned int param,
                            void* data, unsigned int flags);

/* The message pump. PeekMessageA is where the per-frame yield happens.
 * PeekMessageA / GetMessageA / TranslateMessage / DispatchMessageA / WaitMessage
 * / SetCursor are declared by windows.h (included above) with the game's own
 * MSG / HWND types; user32.c defines them with those types. */
void  PostQuitMessage(int code);
long  DefWindowProcA(void* hwnd, unsigned int msg, unsigned int wp, long lp);
long  SendMessageA(void* hwnd, unsigned int msg, unsigned int wp, long lp);

/* Cursor and keyboard state. */
void* SetCursor(void* cursor);
int   ShowCursor(int show);
void* LoadCursorA(void* inst, const char* name);
void* LoadIconA(void* inst, const char* name);
short GetKeyState(int vk);

/* MessageBoxA goes to the console and returns IDOK (1); dialogs fail. */
int   MessageBoxA(void* hwnd, const char* text, const char* caption,
                  unsigned int type);
int   DialogBoxParamA(void* inst, const char* tmpl, void* parent,
                      void* proc, long param);
int   EndDialog(void* dlg, int result);
void* GetDlgItem(void* dlg, int id);

/* Formatting: real, over vsnprintf. */
int   wsprintfA(char* out, const char* fmt, ...);
int   wvsprintfA(char* out, const char* fmt, void* args);

/* GDI-ish USER32 drawing: no-ops that report success. */
int   FillRect(void* hdc, const LLRect* rc, void* brush);
int   DrawTextA(void* hdc, const char* text, int len, LLRect* rc,
                unsigned int format);

/* ---- Web Audio (portable/src/hostwin/ll_audio.c, PORT-B11) -------------- */
/* The back end behind PORT-B4's silent IDirectSound. dsound.c keeps every
 * DirectSound semantic; this layer knows only PCM blocks, gains and time, and
 * on a non-Emscripten toolchain it is a set of counters so the shim still
 * builds and links natively. See ll_audio.c's header for the two playback
 * modes and the autoplay policy. */

/* 1 once an AudioContext exists. The first call creates it. */
int  ll_audio_enabled(void);
/* 0 none, 1 suspended (waiting for a user gesture), 2 running. */
int  ll_audio_state(void);
/* 1 when the page was opened with `?sound=test`. dsound.c then runs one sample
 * through the game's own load-and-play sequence at DirectSoundCreate time, so
 * the whole path can be proved without depending on the game reaching a
 * PlayInstanceOfSample. */
int  ll_audio_selftest_requested(void);

/* One voice per IDirectSoundBuffer: a gain and a panner that outlive the source
 * nodes started on them. 0 means "no audio", and every call below tolerates it. */
int  ll_audio_voice_new(void);
void ll_audio_voice_free(int voice);

/* DirectSound units straight through: volume and pan in hundredths of a dB,
 * `rate_mul` the ratio of SetFrequency's rate to the format's own. */
void ll_audio_set_levels(int voice, int volume_cb, int pan_cb);
void ll_audio_set_rate(int voice, double rate_mul);
void ll_audio_stop(int voice);

/* A buffer written in full before it is played (every sound effect). */
int  ll_audio_play_static(int voice, const void* pcm, unsigned int bytes,
                          unsigned int rate, int channels, int bits,
                          unsigned int offset, int looping,
                          int volume_cb, int pan_cb, double rate_mul);

/* A buffer rewritten while it plays (the narration ring, the AVI audio track).
 * `cursor` is the play position the game is filling against, so everything
 * behind it is safe to read; returns the number of chunks scheduled. */
int  ll_audio_feed_stream(int voice, const void* pcm, unsigned int bytes,
                          unsigned int cursor, unsigned int chunk,
                          unsigned int rate, int channels, int bits,
                          double rate_mul);

/* The music stream (the DirectMusic lane): planar float chunks at the
 * context's own rate, scheduled back to back on one gain. `lead` is how many
 * seconds are already scheduled ahead of the context's clock, -1 when nothing
 * can play (no context, or still suspended by the autoplay policy). */
double ll_audio_music_rate(void);
int    ll_audio_music_open(void);
double ll_audio_music_lead(void);
int    ll_audio_music_push(const float* left, const float* right, int frames, double rate);
void   ll_audio_music_gain(double gain);
void   ll_audio_music_stop(void);

/* ---- the TrueType face (portable/src/hostwin/ll_ttf.c, PORT-B11) -------- */
/* The game SHIPS its typeface: gamedata/main/Lego.TTF, handed to
 * AddFontResourceA by gpu.c's InitHostSystemGPU before anything draws. With the
 * face loaded, ll_font.c measures and draws through these entry points and the
 * game's own layout lands where it landed on Windows; without it (the native
 * and node builds have no mounted gamedata) ll_ttf_available() is 0 and
 * PORT-B10's bitmap face is used unchanged. See ll_ttf.c's header for the
 * lfHeight -> pixel mapping and the evidence that it is right. */
typedef struct LLTtfMetrics {
    int    ready;        /* 0 when there is no face: use the bitmap one */
    double scale;        /* device pixels per font design unit */
    int    cell_h;       /* tmHeight: what the LOGFONT's lfHeight asked for */
    int    ascent;       /* tmAscent: the baseline, measured from the cell top */
    int    descent;      /* tmDescent */
    int    ppem;         /* the em in pixels, for the trace */
    int    embolden;     /* GDI's synthetic bold: 1 extra column of ink and advance */
} LLTtfMetrics;

/* 1 once a face is loaded. The first call searches for the file; later calls are
 * free, and a failed search is not retried. */
int  ll_ttf_available(void);
/* Load an explicit file or an in-memory sfnt. ll_ttf_load_memory TAKES OWNERSHIP
 * of the block (it is freed when another face replaces it). */
int  ll_ttf_load_file(const char* path);
int  ll_ttf_load_memory(void* data, unsigned long size);

void ll_ttf_metrics(int lf_height, int weight, LLTtfMetrics* m);
int  ll_ttf_char_advance(const LLTtfMetrics* m, unsigned char ch);
/* The glyph's 8-bit coverage, `*w` by `*h`, with `*bx` pixels from the pen to
 * its left column and `*by` pixels from the BASELINE to its top row (negative
 * above the baseline). NULL for a blank glyph. The pointer is into a cache and
 * is valid until the next ll_ttf_glyph call. */
const unsigned char* ll_ttf_glyph(const LLTtfMetrics* m, unsigned char ch,
                                  int* w, int* h, int* bx, int* by);
/* Coverage is BLENDED by default. 0 thresholds it at 50% for the bilevel look
 * GDI would have had; the metrics are identical either way. */
void ll_ttf_set_antialias(int on);
int  ll_ttf_antialias(void);
/* upem / glyph count / usWinAscent / usWinDescent / usWeightClass, for llFont().
 * Returns 0 when no face is loaded. */
int  ll_ttf_face_info(int* upem, int* glyphs, int* win_asc, int* win_desc,
                      int* weight);
/* Publish one LOGFONT's answer to the page's `llFont()` hook. Called from
 * CreateFontIndirectA; a no-op off Emscripten. */
void ll_ttf_report_font(int lf_height, int lf_weight, const LLTtfMetrics* m);

/* ---- the bitmap font (portable/src/hostwin/ll_font.c, PORT-B2) ---------- */
/* GDI text is the game's only text: text.c's Print* routines borrow a DC from
 * the DirectDraw draw surface and let GDI draw into it, and eleven DrawTextA
 * call sites MEASURE with DT_CALCRECT and lay out around the answer. So one
 * engine does both halves and they agree by construction. See ll_font.c's
 * header for the face, the metrics and what they are derived from. */
typedef struct LLFontMetrics {
    int cell_h;     /* the LOGFONT's lfHeight: a cell height */
    int weight;     /* the LOGFONT's lfWeight */
    int bold;       /* weight >= 600 */
    int gw, gh;     /* the ink box one glyph is scaled into */
    int advance;    /* NOMINAL pen movement: the widest glyph. The face is
                     * proportional -- every glyph advances by its own ink
                     * width -- so nothing lays text out with this; it is the
                     * one number a caller with no character in hand can use.
                     * ll_font_text_width is the real measure. (PORT-B10) */
    int line_h;     /* baseline-to-baseline, == cell_h */
    int ascent;
    /* PORT-B11: when `ttf.ready` the measuring and drawing come from the game's
     * own Lego.TTF through ll_ttf.c and the fields above are the TrueType
     * face's; `gw`/`gh`/`advance` are then only the trace's nominal figures. */
    LLTtfMetrics ttf;
} LLFontMetrics;

/* Where text goes: a 16-bpp surface plus the clip box already intersected from
 * the DC's clip region and the surface bounds. Half-open, like every Win32
 * rect. */
typedef struct LLFontTarget {
    unsigned short* bits;
    int             w, h, pitch;
    LLRect          clip;
} LLFontTarget;

void ll_font_metrics(int cell_h, int weight, LLFontMetrics* m);
void ll_font_default_metrics(LLFontMetrics* m);
int  ll_font_text_width(const LLFontMetrics* m, const char* s, int n);

/* TextOutA's engine: (x, y) is the top-left of the cell (TA_TOP|TA_LEFT). */
void ll_font_text_out(const LLFontTarget* t, const LLFontMetrics* m,
                      int x, int y, const char* s, int n,
                      unsigned short fg, int opaque, unsigned short bg);

/* DrawTextA's engine. Honours DT_CENTER/RIGHT/VCENTER/BOTTOM/WORDBREAK/
 * SINGLELINE/NOCLIP/CALCRECT, returns the text height, and under DT_CALCRECT
 * writes the measured rect back instead of drawing. `t` may be NULL when only
 * measuring. */
int  ll_font_draw_text(const LLFontTarget* t, const LLFontMetrics* m,
                       LLRect* rc, const char* s, int n, unsigned int format,
                       unsigned short fg, int opaque, unsigned short bg);

unsigned short ll_font_colorref_to_565(unsigned long colorref);


/* ---- GDI32 (portable/src/hostwin/gdi32.c) ------------------------------- */
/* Handle factories hand back distinct non-null cookies; the drawing calls are
 * no-ops that report success; printing reports failure so the print path
 * aborts at its first check. Nothing here traps. */

/* Reset a DC's attributes to GDI's defaults (black text on a white opaque
 * background, no clip region, the 20-pixel font). IDirectDrawSurface::GetDC
 * hands out a FRESH DC every call, so ddraw.c calls this from its GetDC --
 * without it PrintLimitedText's SetTextColor, which it never restores, would
 * leak into the next Print. gdi32.c. */
void ll_host_dc_reset(void* hdc);

/* Resolve a DC to a drawing target: the surface behind it plus the DC's current
 * clip box. Returns 0 when the DC is not over a surface. gdi32.c. */
int  ll_host_dc_target(void* hdc, LLFontTarget* t);

/* The DC's selected font's metrics, its text colour, its background colour and
 * whether the background is opaque (SetBkMode(OPAQUE) == 2). gdi32.c. */
void ll_host_dc_font(void* hdc, LLFontMetrics* m);
unsigned short ll_host_dc_fg(void* hdc);
unsigned short ll_host_dc_bg(void* hdc);
int  ll_host_dc_opaque(void* hdc);
unsigned long  ll_host_brush_colour(void* brush);

/* ---- PORT-B13: the GDI/blit witness the page reads as llGdi() -------------
 *
 * P4-3 (three text boxes fill solid black) is a question about FOUR numbers
 * that no screenshot can answer: what colour the game asked the brush for, what
 * 565 value the fill actually wrote, what colour key the sprite's surface
 * carries, and whether the blit that put it on screen asked for the key at all.
 * The game's cached-text sprites (bubblecache.c DrawCachedTextSprite) fill the
 * whole cell with GetNearestColor(ink) and then SET THAT COLOUR AS THE SPRITE'S
 * COLOUR KEY, so the fill is MEANT to be invisible -- which means a black box
 * is either a fill in the wrong colour or a key that missed it, and the two
 * look identical on the canvas. One struct, written by gdi32.c, user32.c and
 * ddraw.c; `ll_host_gdi_stats()` hands the page its address. Reads only. */
typedef struct LLGdiStats {
    int  words;                    /* sizeof(LLGdiStats)/4 -- the page's check
                                    * that it is decoding the layout it knows */
    int  objs_live;                /* GDI objects in the table right now */
    int  objs_high;                /* high-water mark of that */
    int  objs_exhausted;           /* obj_new calls with NO free slot left */
    int  objs_by_class[8];         /* live objects per LL_OBJ_* class */
    int  dc_evictions;             /* a DC slot reused for a different HDC */
    int  bk_transparent;           /* SetBkMode(TRANSPARENT) calls */
    int  bk_opaque;                /* SetBkMode(OPAQUE) calls */
    int  textout_calls;
    int  drawtext_calls;
    int  fill_calls;               /* FillRect entered */
    int  fill_no_target;           /* ...over a DC with no pixels behind it */
    int  fill_no_brush;            /* ...with a brush that did NOT resolve:
                                    * ll_host_brush_colour fell back to BLACK */
    unsigned int  fill_last_brush;       /* the handle as the game passed it */
    unsigned long fill_last_colorref;    /* what it resolved to, 0x00bbggrr */
    int  fill_last_565;                  /* the pixel actually written */
    int  fill_last_rect[4];
    int  fill_last_surf_w, fill_last_surf_h;
    int  ck_sets;                  /* IDirectDrawSurface::SetColorKey calls */
    unsigned long ck_last_low;     /* the key the last one set */
    int  blt_keysrc;               /* Blt calls that asked for DDBLT_KEYSRC */
    int  blt_keysrc_unset;         /* ...on a source with NO key set: opaque */
    int  blt_plain;                /* Blt calls that did not ask for the key */
    int  objs_capacity;            /* slots the table has grown to */
} LLGdiStats;

/* The live counters. Never null, and cheap: the drawing paths call it once per
 * FillRect/DrawTextA/Blt, so it does NOT walk the object table. */
LLGdiStats* ll_host_gdi_stats(void);

/* Refresh objs_live / objs_high / objs_by_class / objs_capacity by walking the
 * table. The page calls this (through main.c's ll_gdi_stats) before reading;
 * nothing on a drawing path does. */
void ll_host_gdi_census(void);

int   AddFontResourceA(const char* file);
int   RemoveFontResourceA(const char* file);
void* CreateFontIndirectA(const void* logfont);
void* CreateCompatibleDC(void* hdc);
void* CreateDCA(const char* driver, const char* device, const char* output,
                const void* devmode);
int   DeleteDC(void* hdc);
void* CreateDIBitmap(void* hdc, const void* hdr, unsigned long init,
                     const void* bits, const void* info, unsigned int usage);
void* CreatePen(int style, int width, unsigned long colour);
void* CreateSolidBrush(unsigned long colour);
void* CreateRectRgn(int l, int t, int r, int b);
void* CreateRectRgnIndirect(const LLRect* rc);
void* GetStockObject(int index);
void* SelectObject(void* hdc, void* obj);
int   DeleteObject(void* obj);
int   GetDeviceCaps(void* hdc, int index);
unsigned long GetNearestColor(void* hdc, unsigned long colour);
unsigned long SetBkColor(void* hdc, unsigned long colour);
unsigned long SetTextColor(void* hdc, unsigned long colour);
int   SetBkMode(void* hdc, int mode);
unsigned int SetTextAlign(void* hdc, unsigned int align);
int   TextOutA(void* hdc, int x, int y, const char* text, int len);
int   MoveToEx(void* hdc, int x, int y, LLPoint* prev);
int   LineTo(void* hdc, int x, int y);
int   StretchDIBits(void* hdc, int xd, int yd, int wd, int hd,
                    int xs, int ys, int ws, int hs, const void* bits,
                    const void* info, unsigned int usage, unsigned long rop);
/* WINSPOOL.DRV, in gdi32.c with the rest of the print path (PORT-B6): the last
 * generated host trap in the closure. Reports zero printers, which is where
 * certificate.c's SaveScreenshotBmp stops. */
int   EnumPrintersA(unsigned long flags, char* name, unsigned long level,
                    void* buf, unsigned long buflen, unsigned long* needed,
                    unsigned long* returned);
int   StartDocA(void* hdc, const void* docinfo);
int   StartPage(void* hdc);
int   EndPage(void* hdc);
int   EndDoc(void* hdc);

/* ---- DINPUT (portable/src/hostwin/dinput.c) ----------------------------- */
long  DirectInputCreateA(void* inst, unsigned long version, void** out,
                         void* outer);

/* ---- WINMM (portable/src/hostwin/winmm.c) ------------------------------- */
/* timeGetTime is real and is the second yield point (§1): it yields whenever
 * 4 ms has passed since the last yield, which bounds every wall-clock spin in
 * the game. The MIDI calls return MMSYSERR_NOTSUPPORTED (8). */
unsigned int timeGetTime(void);
unsigned int timeSetEvent(unsigned int delay, unsigned int resolution,
                          void* callback, unsigned long user,
                          unsigned int flags);
unsigned int timeKillEvent(unsigned int id);
unsigned int timeBeginPeriod(unsigned int period);
unsigned int timeEndPeriod(unsigned int period);
unsigned int midiOutOpen(void** out, unsigned int device, unsigned long cb,
                         unsigned long inst, unsigned long flags);
unsigned int midiOutClose(void* handle);
unsigned int midiOutShortMsg(void* handle, unsigned long msg);
unsigned int midiOutReset(void* handle);

/* ---- DSOUND (portable/src/hostwin/dsound.c) ---------------------------- */
/* PORT-B4: this SUCCEEDS now, returning a silent IDirectSound with the full
 * IDirectSoundBuffer vtable and a play cursor driven by wall clock.
 *
 * It used to return DSERR_NODRIVER on the reasoning that every sample entry
 * point is guarded by g_samples_ready -- true, but InitSoundSystem
 * (lifecycle.c 0x004964f0) is not one: `if (!ok) return ok;` skips
 * InitMusicSystem, so nothing ever sets g_music_disabled and RunGame
 * (gamemain.c 0x00459520) spins on it for ever. A failing sample system makes
 * -nomusic unreachable. dsound.c's header comment has the whole chain, the
 * swept list of call sites, and why the cursor has to move (two loops in the
 * game spin on it). */
long  DirectSoundCreate(void* guid, void** out, void* outer);
/* The volume last set on a buffer (hundredths of a dB): the music slider, as
 * UpdateSoundVols applies it to DirectMusic's port buffer. */
long  ll_dsound_buffer_volume(void* buffer);

/* ---- ole32 (portable/src/hostwin/dsound.c) ------------------------------ */
/* The program's only two COM imports, both DirectMusic's, both called only by
 * MusicThread (musicthread.c 0x00492db0). CoInitialize reports success (its
 * result is discarded at the one call site). CoCreateInstance hands out
 * ll_dmusic.c's performance, loader and composer when DirectMusic is available,
 * and otherwise reports REGDB_E_CLASSNOTREG, which takes the thread straight to
 * its `shutdown:` rung -- the designed no-DirectMusic path, and the one that
 * sets g_music_disabled. */
long  CoInitialize(void* reserved);
long  CoCreateInstance(const void* clsid, void* outer, unsigned long context,
                       const void* iid, void** out);

/* ---- DirectMusic (portable/src/hostwin/ll_dmusic.c) ---------------------- */
/* REGDB_E_CLASSNOTREG unless Web Audio, the imusic data and a DLS collection
 * are all there (and the page did not say ?music=0). */
long  ll_dmusic_create(const void* clsid, const void* iid, void** out);
/* Render music until enough is scheduled ahead. ll_host_yield calls it on the
 * main stack before every yield; it does nothing inside a thread's fiber. */
void  ll_dmusic_pump(void);

/* ---- AVIFIL32 (portable/src/hostwin/avifil32.c) -------------------------- */
/* Video for Windows' AVIFile API: the FMV player (movie.c, movie2.c) and the
 * on-screen advisor (advisor.c, screens3.c). There is no Indeo 5 decoder in
 * this port, so AVIFileOpenA reports AVIERR_FILEOPEN (0x8004406F) and every
 * other entry point reports cleanly out of a handle it never follows. Both
 * callers are built for exactly that: PlayMovie returns without entering the
 * player, and the advisor's one draw site is behind `if (g_vidanim)`, which
 * stays null. The full reasoning, the rejected alternative (a synthetic
 * zero-length stream) and the game-side null-deref the shim has to survive are
 * in the header of avifil32.c. PORT-B6. */
void          AVIFileInit(void);
void          AVIFileExit(void);
long          AVIFileOpenA(void** ppfile, const char* name, unsigned int mode,
                           const void* handler);
long          AVIFileInfoA(void* pfile, void* pfi, long size);
long          AVIFileGetStream(void* pfile, void** ppstream, unsigned long fcc,
                               long lParam);
unsigned long AVIFileRelease(void* pfile);
long          AVIStreamInfoA(void* pavi, void* psi, long size);
unsigned long AVIStreamAddRef(void* pavi);
unsigned long AVIStreamRelease(void* pavi);
void*         AVIStreamGetFrameOpen(void* pavi, const void* wanted);
void*         AVIStreamGetFrame(void* pgf, long pos);
long          AVIStreamGetFrameClose(void* pgf);
long          AVIStreamStart(void* pavi);
long          AVIStreamLength(void* pavi);
long          AVIStreamRead(void* pavi, long start, long samples, void* buf,
                            long buflen, long* bytes, long* nsamples);
long          AVIStreamReadFormat(void* pavi, long pos, void* fmt, long* size);

/* ---- MSACM32 (portable/src/hostwin/msacm32.c) --------------------------- */
/* The Audio Compression Manager. NOT a stub: data2.c's CreateSampleFromWAV
 * runs EVERY sample in the archives through resaudio2.c's ConvertWAVToPCM and
 * drops the sample when it fails, so refusing everything would break the
 * sample loader rather than silence it. PCM -> 16-bit PCM is implemented for
 * real (including the 8-bit unsigned -> 16-bit signed widening); ADPCM is
 * refused with ACMERR_NOTPOSSIBLE (512), which is what a real ACM returns with
 * no driver and which every caller handles. acmStreamSize writes its out
 * parameter even on failure -- audio4.c:212 mallocs it without checking
 * anything. See the header of msacm32.c. PORT-B6. */
int acmStreamOpen(void** phas, void* hdrv, void* srcfmt, void* dstfmt,
                  void* wfltr, unsigned long callback, unsigned long inst,
                  unsigned long flags);
int acmStreamSize(void* has, unsigned long srclen, unsigned long* pdwOutput,
                  unsigned long flags);
int acmStreamPrepareHeader(void* has, void* phdr, unsigned long flags);
int acmStreamUnprepareHeader(void* has, void* phdr, unsigned long flags);
int acmStreamConvert(void* has, void* phdr, unsigned long flags);
int acmStreamClose(void* has, unsigned long flags);

#endif /* LL_HOST_H */
