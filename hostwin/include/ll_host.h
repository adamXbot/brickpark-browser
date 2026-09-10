/* LEGOLAND portable build -- the host ABI: every Win32 / DirectX entry point
 * the shims under portable/src/hostwin/ implement, grouped by DLL.
 *
 * Ownership (docs/SCOPE_PORT_WAVE.md): PORT-A owns this file; PORT-B appends
 * its declarations. PORT-A had not created it when PORT-B reached it, so
 * PORT-B created it with the DDRAW / USER32 / GDI32 / DINPUT / WINMM / DSOUND
 * half and left the KERNEL32 / ADVAPI32 / VERSION section empty for PORT-A.
 * Whoever merges second folds the two halves together -- the two sections do
 * not overlap.
 *
 * Signatures are the Win32 ones. __stdcall is ignored off x86, so it is not
 * spelled here: the game's own `__declspec(dllimport) ... __stdcall` prototypes
 * and these definitions agree on everything that matters on wasm32 (argument
 * count and types). Nothing here is __declspec(dllimport): these are ordinary
 * definitions in the same link.
 *
 * Every entry point either works or returns a DOCUMENTED failure code. None
 * may trap: a trap here is a crash in the middle of the game's startup spine,
 * which is exactly what the port is trying to get past.
 */
#ifndef LL_HOSTWIN_LL_HOST_H
#define LL_HOSTWIN_LL_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

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

/* ---- DDRAW (portable/src/hostwin/ddraw.c) ------------------------------- */
/* The one exported entry; everything else is reached through the C vtables it
 * hands back. Returns DD_OK (0) and a fully populated IDirectDraw. */
long DirectDrawCreate(void* guid, void** out, void* outer);

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

/* The message pump. PeekMessageA is where the per-frame yield happens. */
int   PeekMessageA(void* msg, void* hwnd, unsigned int lo, unsigned int hi,
                   unsigned int flags);
int   GetMessageA(void* msg, void* hwnd, unsigned int lo, unsigned int hi);
int   TranslateMessage(const void* msg);
long  DispatchMessageA(const void* msg);
int   WaitMessage(void);
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

/* ---- GDI32 (portable/src/hostwin/gdi32.c) ------------------------------- */
/* Handle factories hand back distinct non-null cookies; the drawing calls are
 * no-ops that report success; printing reports failure so the print path
 * aborts at its first check. Nothing here traps. */
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
/* Returns DSERR_NODRIVER (0x88780078) so the game runs silent. The game
 * tolerates it: InitSoundSampleSystem (audio4.c 0x00492130) clears g_dsound
 * and g_samples_ready and returns 0, InitSoundSystem propagates the 0, and
 * RunGame (gamemain.c 0x00459520) ignores the result. Every later sample call
 * is guarded by g_samples_ready. */
long  DirectSoundCreate(void* guid, void** out, void* outer);

/* ---- KERNEL32 / ADVAPI32 / VERSION (PORT-A, kernel32.c) ---------------- */
/* PORT-A's half of this header. One contract from PORT-B's §1 that PORT-A
 * must honour: `Sleep(ms)` MUST yield (emscripten_sleep), never be a no-op --
 * RunGame's music wait is `while (g_music_disabled == 0) { PeekMessageA(...);
 * Sleep(100); }` and a no-op Sleep turns that into a hung tab. (The wait is
 * skipped entirely when the command line carries `-nomusic`, because
 * InitMusicSystem then sets g_music_disabled itself -- sysstubs.c 0x00495a10.) */

#ifdef __cplusplus
}
#endif
#endif /* LL_HOSTWIN_LL_HOST_H */
