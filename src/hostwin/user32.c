/* LEGOLAND portable build -- the USER32 host shim (scope PORT-B): the window,
 * the message queue fed from canvas events, the message pump, the rect maths,
 * and the main-loop yield the whole port hangs off.
 *
 * The yield (docs/lanes/scope-port-b.md §1). The game's frame is produced
 * inside synchronous spin loops it does not own a one-frame function for, so
 * the host yields on its behalf, through ASYNCIFY:
 *   - PeekMessageA yields once when the queue goes empty. The pump
 *     (ProcessSystemEvents, input.c 0x00480050) is `while (PeekMessageA(...
 *     PM_NOREMOVE)) { ... }`, so "queue empty" happens exactly once per pass
 *     -- one yield per frame, at the game's own idle point.
 *   - WaitMessage yields 16 ms, which is what the game asks for: the pump
 *     idles in it while the window is inactive (g_game->flags bit 0).
 *   - winmm.c's timeGetTime yields when 4 ms has passed, which is what makes
 *     FlipPrimary's `while (timeGetTime() - g_flip_time < 0x1c) ;` cheap.
 * Nothing may call into wasm while a yield is unwound, so the JS side only
 * enqueues events; ll_host_drain_events pulls them on the wasm side.
 *
 * Rect maths (IntersectRect / OffsetRect / PtInRect) is REAL, not a stub: the
 * renderer's clipping is built on it (gpu.c's SetClipping / RenderBlock /
 * RenderSprite, surface.c's lock path). Win32 semantics: rects are half-open,
 * and the game's own rects are inclusive, which is why surface.c builds
 * {0,0,w-1,h-1} before intersecting -- that asymmetry lives in the game and
 * must not be "fixed" here.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ll_host.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* portable/src/browser/ll_canvas.js */
extern void ll_js_display_open(int w, int h);
extern void ll_js_present16(const void* pixels, int w, int h, int pitch);
extern int  ll_js_event_next(int* out4);
extern void ll_js_set_cursor(int visible);
#else
/* Native build: the shim compiles and links, there is just no canvas. */
static void ll_js_display_open(int w, int h) { (void)w; (void)h; }
static void ll_js_present16(const void* p, int w, int h, int pitch)
{ (void)p; (void)w; (void)h; (void)pitch; }
static int  ll_js_event_next(int* out4) { (void)out4; return 0; }
static void ll_js_set_cursor(int visible) { (void)visible; }
#endif

/* ---- Win32 constants the shim needs ------------------------------------- */
#define WM_DESTROY       0x0002
#define WM_SETFOCUS      0x0007
#define WM_KILLFOCUS     0x0008
#define WM_CLOSE         0x0010
#define WM_QUIT          0x0012
#define WM_ACTIVATEAPP   0x001c
#define WM_CANCELMODE    0x001f
#define WM_KEYDOWN       0x0100
#define WM_KEYUP         0x0101
#define WM_CHAR          0x0102
#define WM_MOUSEMOVE     0x0200
#define WM_LBUTTONDOWN   0x0201
#define WM_LBUTTONUP     0x0202
#define WM_RBUTTONDOWN   0x0204
#define WM_RBUTTONUP     0x0205
#define WM_MBUTTONDOWN   0x0207
#define WM_MBUTTONUP     0x0208

#define GWL_HINSTANCE    (-6)
#define VK_SHIFT         0x10
#define VK_CONTROL       0x11
#define VK_CAPITAL       0x14

/* ---- the MSG the game declares (portable/hostwin/include/windows.h) ----- */
typedef struct LLMsg {
    void*         hwnd;
    unsigned int  message;
    unsigned int  wParam;
    long          lParam;
    unsigned long time;
    LLPoint       pt;
} LLMsg;

/* ---- window / class state ----------------------------------------------- */
/* WNDCLASSEXA, 0x30 bytes (screen.c:1168): lpfnWndProc at +0x08, hInstance at
 * +0x14. Only those two matter to the shim. */
typedef struct LLWndClassEx {
    unsigned int cbSize;
    unsigned int style;
    void*        lpfnWndProc;      /* +0x08 */
    int          cbClsExtra;
    int          cbWndExtra;
    void*        hInstance;        /* +0x14 */
    void*        hIcon;
    void*        hCursor;
    void*        hbrBackground;
    const char*  lpszMenuName;
    const char*  lpszClassName;
    void*        hIconSm;
} LLWndClassEx;

typedef long (*LLWndProc)(void* hwnd, unsigned int msg, unsigned int wp, long lp);

static LLWndProc g_wndproc;
static void*     g_class_inst;
static int       g_hwnd_token = 0x4c4c4800;   /* 'LLH\0': a non-null cookie */
static void*     g_hwnd;
static void*     g_desktop_token = (void*)0x4c4c4401; /* GetDesktopWindow */
static int       g_cursor_shown = 1;

void* ll_host_hwnd(void) { return g_hwnd; }

/* ---- the message queue -------------------------------------------------- */
#define LL_MSGQ 256
static LLMsg       g_queue[LL_MSGQ];
static int         g_q_head, g_q_tail;
static unsigned int g_msg_seq;

void ll_host_post_message(unsigned int msg, unsigned int wp, long lp)
{
    int next = (g_q_tail + 1) % LL_MSGQ;
    if (next == g_q_head)
        return;                       /* full: drop, like a flooded queue */
    g_queue[g_q_tail].hwnd = g_hwnd;
    g_queue[g_q_tail].message = msg;
    g_queue[g_q_tail].wParam = wp;
    g_queue[g_q_tail].lParam = lp;
    g_queue[g_q_tail].time = ++g_msg_seq;
    g_queue[g_q_tail].pt.x = 0;
    g_queue[g_q_tail].pt.y = 0;
    g_q_tail = next;
}

static int queue_peek(LLMsg* out, int remove)
{
    if (g_q_head == g_q_tail)
        return 0;
    if (out)
        *out = g_queue[g_q_head];
    if (remove)
        g_q_head = (g_q_head + 1) % LL_MSGQ;
    return 1;
}

/* ---- the yield ---------------------------------------------------------- */
static double g_last_yield_ms;

void ll_host_yield(unsigned int ms)
{
#ifdef __EMSCRIPTEN__
    g_last_yield_ms = emscripten_get_now();
    emscripten_sleep(ms);
    g_last_yield_ms = emscripten_get_now();
#else
    (void)ms;
#endif
}

/* Yield only if at least min_gap_ms of wall clock has passed since the last
 * yield. winmm.c's timeGetTime calls this, which bounds how long any spin loop
 * in the game can hold the browser's event loop. */
void ll_host_yield_throttled(unsigned int min_gap_ms)
{
#ifdef __EMSCRIPTEN__
    if (emscripten_get_now() - g_last_yield_ms >= (double)min_gap_ms)
        ll_host_yield(0);
#else
    (void)min_gap_ms;
#endif
}

/* ---- canvas -> game event translation ----------------------------------- */
/* Event records from ll_canvas.js: {type, a, b, c}. */
#define LLEV_KEYDOWN   1      /* a = DIK scan code, b = VK, c = ascii or 0 */
#define LLEV_KEYUP     2
#define LLEV_MOUSEMOVE 3      /* a = dx, b = dy */
#define LLEV_MOUSEDOWN 4      /* a = 0 left, 1 right, 2 middle */
#define LLEV_MOUSEUP   5
#define LLEV_WHEEL     6      /* a = delta in DirectInput Z units */
#define LLEV_FOCUS     7
#define LLEV_BLUR      8
#define LLEV_CLOSE     9

static int g_vk_down[256];

void ll_host_drain_events(void)
{
    int ev[4];
    while (ll_js_event_next(ev)) {
        switch (ev[0]) {
        case LLEV_KEYDOWN:
            ll_host_key_set(ev[1], 1);
            if (ev[2] > 0 && ev[2] < 256)
                g_vk_down[ev[2]] = 1;
            ll_host_post_message(WM_KEYDOWN, (unsigned)ev[2], 0);
            /* The only WM_CHAR the game acts on is backspace (input2.c's
             * LegoLandWindowProc: `if ((char)wp == 8)`), so that is the only
             * one synthesised. Typed text reaches the game through the
             * DirectInput key array (GetTypedChar, input2.c), not WM_CHAR. */
            if (ev[3] == 8)
                ll_host_post_message(WM_CHAR, 8, 0);
            break;
        case LLEV_KEYUP:
            ll_host_key_set(ev[1], 0);
            if (ev[2] > 0 && ev[2] < 256)
                g_vk_down[ev[2]] = 0;
            ll_host_post_message(WM_KEYUP, (unsigned)ev[2], 0);
            break;
        case LLEV_MOUSEMOVE:
            ll_host_mouse_move(ev[1], ev[2]);
            break;
        case LLEV_MOUSEDOWN:
            ll_host_mouse_button(ev[1], 1);
            break;
        case LLEV_MOUSEUP:
            ll_host_mouse_button(ev[1], 0);
            break;
        case LLEV_WHEEL:
            ll_host_mouse_wheel(ev[1]);
            break;
        case LLEV_FOCUS:
            ll_host_post_message(WM_SETFOCUS, 0, 0);
            break;
        case LLEV_BLUR:
            ll_host_post_message(WM_KILLFOCUS, 0, 0);
            break;
        case LLEV_CLOSE:
            ll_host_post_message(WM_CLOSE, 0, 0);
            break;
        default:
            break;
        }
    }
}

/* ---- display ------------------------------------------------------------ */
void ll_host_display_open(int w, int h)
{
    static int open_w, open_h;
    if (w == open_w && h == open_h)
        return;
    open_w = w;
    open_h = h;
    ll_js_display_open(w, h);
}

void ll_host_present16(const void* pixels, int w, int h, int pitch)
{
    ll_js_present16(pixels, w, h, pitch);
    /* The browser only composites once control returns to it. */
    ll_host_yield(0);
}

/* =========================================================================
 * rect maths -- real, Win32 semantics (half-open rects)
 * ========================================================================= */

int IntersectRect(LLRect* dst, const LLRect* a, const LLRect* b)
{
    long l, t, r, btm;
    if (!dst || !a || !b)
        return 0;
    l = a->left   > b->left   ? a->left   : b->left;
    t = a->top    > b->top    ? a->top    : b->top;
    r = a->right  < b->right  ? a->right  : b->right;
    btm = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (l >= r || t >= btm) {
        dst->left = dst->top = dst->right = dst->bottom = 0;
        return 0;
    }
    dst->left = l; dst->top = t; dst->right = r; dst->bottom = btm;
    return 1;
}

int OffsetRect(LLRect* rc, int dx, int dy)
{
    if (!rc) return 0;
    rc->left += dx; rc->right += dx;
    rc->top += dy;  rc->bottom += dy;
    return 1;
}

int PtInRect(const LLRect* rc, LLPoint pt)
{
    if (!rc) return 0;
    return pt.x >= rc->left && pt.x < rc->right &&
           pt.y >= rc->top  && pt.y < rc->bottom;
}

/* AdjustWindowRect: there is no window frame on a canvas, so the client rect
 * is the window rect. InitScreen's windowed arm uses the result only to size
 * CreateWindowEx, so leaving it alone gives a canvas exactly w x h. */
int AdjustWindowRect(LLRect* rc, unsigned long style, int menu)
{
    (void)rc; (void)style; (void)menu;
    return 1;
}

/* =========================================================================
 * window and class
 * ========================================================================= */

unsigned short RegisterClassExA(const void* wcex)
{
    const LLWndClassEx* wc = (const LLWndClassEx*)wcex;
    if (!wc)
        return 0;
    g_wndproc = (LLWndProc)wc->lpfnWndProc;
    g_class_inst = wc->hInstance;
    return 1;   /* a non-zero ATOM */
}

void* CreateWindowExA(unsigned long ex, const char* cls, const char* name,
                      unsigned long style, int x, int y, int w, int h,
                      void* parent, void* menu, void* inst, void* param)
{
    (void)ex; (void)cls; (void)style; (void)x; (void)y;
    (void)parent; (void)menu; (void)param;
    g_hwnd = &g_hwnd_token;
    if (inst)
        g_class_inst = inst;
    if (w > 0 && h > 0)
        ll_host_display_open(w, h);
    printf("[hostwin] window \"%s\" %dx%d\n", name ? name : "", w, h);
    return g_hwnd;
}

int DestroyWindow(void* hwnd)
{
    if (hwnd == g_hwnd)
        g_hwnd = 0;
    return 1;
}

void* GetDesktopWindow(void) { return g_desktop_token; }

/* The fullscreen arm of InitScreen calls this in a loop while the game record's
 * +0x1c bit 0 is set (the "suspended" flag); returning 1 is enough. */
int ShowWindow(void* hwnd, int cmd) { (void)hwnd; (void)cmd; return 1; }

/* InitInputSystem takes its HINSTANCE from here (input2.c:266) and passes it
 * straight to DirectInputCreateA without a check, so it must be non-null. */
long GetWindowLongA(void* hwnd, int index)
{
    (void)hwnd;
    if (index == GWL_HINSTANCE)
        return (long)(size_t)(g_class_inst ? g_class_inst : (void*)&g_hwnd_token);
    return 0;
}

/* FlipPrimary (sysmisc.c 0x004661d0) does GetClientRect then ClientToScreen on
 * the rect's first two longs and offsets its {0,0,640,480} destination by the
 * result. Reporting a client origin of (0,0) keeps that destination where the
 * canvas is. */
int GetClientRect(void* hwnd, LLRect* rc)
{
    int w, h;
    (void)hwnd;
    if (!rc) return 0;
    ll_host_display_size(&w, &h);
    rc->left = 0; rc->top = 0; rc->right = w; rc->bottom = h;
    return 1;
}

int ClientToScreen(void* hwnd, LLPoint* pt) { (void)hwnd; (void)pt; return 1; }

int GetSystemMetrics(int index)
{
    int w, h;
    ll_host_display_size(&w, &h);
    switch (index) {
    case 0:  return w;    /* SM_CXSCREEN */
    case 1:  return h;    /* SM_CYSCREEN */
    case 4:  return 0;    /* SM_CYCAPTION: no frame on a canvas */
    case 7:  return 0;    /* SM_CXFIXEDFRAME */
    case 8:  return 0;    /* SM_CYFIXEDFRAME */
    case 32: return 0;    /* SM_CXSIZEFRAME */
    case 33: return 0;    /* SM_CYSIZEFRAME */
    case 43: return 3;    /* SM_CMOUSEBUTTONS */
    default: return 0;
    }
}

/* Screen-saver / mouse-parameter queries: report success and touch nothing. */
int SystemParametersInfoA(unsigned int action, unsigned int param,
                          void* data, unsigned int flags)
{
    (void)action; (void)param; (void)data; (void)flags;
    return 1;
}

/* =========================================================================
 * the pump
 * ========================================================================= */

static int g_peek_calls;

int PeekMessageA(void* msgp, void* hwnd, unsigned int lo, unsigned int hi,
                 unsigned int flags)
{
    LLMsg* msg = (LLMsg*)msgp;
    (void)hwnd; (void)lo; (void)hi;

    ll_host_drain_events();
    ll_host_pump_timers();

    /* Belt and braces: a flood of messages must not starve the browser. */
    if (++g_peek_calls >= 64) {
        g_peek_calls = 0;
        ll_host_yield(0);
    }

    if (queue_peek(msg, (flags & 1) != 0))
        return 1;

    /* The queue going empty is the game's own idle point, once per
     * ProcessSystemEvents pass: this is the per-frame yield. */
    g_peek_calls = 0;
    ll_host_yield(0);
    return 0;
}

int GetMessageA(void* msgp, void* hwnd, unsigned int lo, unsigned int hi)
{
    LLMsg* msg = (LLMsg*)msgp;
    (void)hwnd; (void)lo; (void)hi;
    for (;;) {
        ll_host_drain_events();
        if (queue_peek(msg, 1))
            return msg->message == WM_QUIT ? 0 : 1;
        /* Specified to block. ProcessSystemEvents only calls it after a
         * successful PeekMessageA, so this loop is not normally entered. */
        ll_host_yield(16);
    }
}

/* No keyboard-layout translation to do: the only WM_CHAR the game reads is
 * backspace, and the event translation synthesises that directly (see
 * ll_host_drain_events). */
int TranslateMessage(const void* msg) { (void)msg; return 0; }

long DispatchMessageA(const void* msgp)
{
    const LLMsg* msg = (const LLMsg*)msgp;
    if (!msg)
        return 0;
    if (g_wndproc)
        return g_wndproc(msg->hwnd, msg->message, msg->wParam, msg->lParam);
    return 0;
}

int WaitMessage(void)
{
    ll_host_drain_events();
    ll_host_pump_timers();
    ll_host_yield(16);            /* one animation frame, then look again */
    return 1;
}

void PostQuitMessage(int code) { ll_host_post_message(WM_QUIT, (unsigned)code, 0); }

/* The game's own window procedure handles WM_CLOSE / WM_CHAR / focus and sends
 * everything else here; there is nothing default to do. */
long DefWindowProcA(void* hwnd, unsigned int msg, unsigned int wp, long lp)
{
    (void)hwnd; (void)msg; (void)wp; (void)lp;
    return 0;
}

long SendMessageA(void* hwnd, unsigned int msg, unsigned int wp, long lp)
{
    if (g_wndproc)
        return g_wndproc(hwnd, msg, wp, lp);
    return 0;
}

/* =========================================================================
 * cursor and key state
 * ========================================================================= */

void* SetCursor(void* cursor)
{
    /* ProcessSystemEvents calls SetCursor(0) after every dispatched message to
     * keep the system cursor hidden; the game draws its own pointer sprite. */
    static void* last = (void*)-1;
    if (cursor != last) {
        last = cursor;
        ll_js_set_cursor(cursor != 0);
    }
    return 0;
}

int ShowCursor(int show)
{
    g_cursor_shown += show ? 1 : -1;
    ll_js_set_cursor(g_cursor_shown >= 0);
    return g_cursor_shown;
}

void* LoadCursorA(void* inst, const char* name)
{
    (void)inst; (void)name;
    return (void*)0x4c4c4302;      /* an opaque non-null HCURSOR */
}

void* LoadIconA(void* inst, const char* name)
{
    (void)inst; (void)name;
    return (void*)0x4c4c4303;      /* an opaque non-null HICON */
}

/* input2.c reads VK_CAPITAL (and the shift keys) through this; the high bit is
 * "down", bit 0 is the toggle state. Only "down" is tracked. */
short GetKeyState(int vk)
{
    if (vk < 0 || vk > 255)
        return 0;
    return (short)(g_vk_down[vk] ? (short)0x8000 : 0);
}

/* =========================================================================
 * message boxes, dialogs, formatting, GDI-ish drawing
 * ========================================================================= */

/* InitSession reports every fatal startup failure through MessageBoxA and then
 * returns; sending it to the console keeps those messages visible. IDOK. */
int MessageBoxA(void* hwnd, const char* text, const char* caption,
                unsigned int type)
{
    (void)hwnd; (void)type;
    fprintf(stderr, "[MessageBox] %s: %s\n",
            caption ? caption : "LEGOLAND", text ? text : "");
    return 1;   /* IDOK */
}

/* No dialog system. -1 is Win32's "could not create the dialog"; every caller
 * treats a non-positive result as "the dialog did not run". */
int DialogBoxParamA(void* inst, const char* tmpl, void* parent, void* proc,
                    long param)
{ (void)inst; (void)tmpl; (void)parent; (void)proc; (void)param; return -1; }
int EndDialog(void* dlg, int result) { (void)dlg; (void)result; return 0; }
void* GetDlgItem(void* dlg, int id) { (void)dlg; (void)id; return 0; }

int wvsprintfA(char* out, const char* fmt, void* args)
{
    /* Win32's wvsprintfA takes a va_list; on every ABI this port targets that
     * is what the caller passes through. */
    va_list* ap = (va_list*)args;
    if (!out || !fmt)
        return 0;
    return vsprintf(out, fmt, *ap);
}

int wsprintfA(char* out, const char* fmt, ...)
{
    int n;
    va_list ap;
    if (!out || !fmt)
        return 0;
    va_start(ap, fmt);
    n = vsprintf(out, fmt, ap);
    va_end(ap);
    return n;
}

/* GDI drawing through USER32: no-ops that report success. See gdi32.c. */
int FillRect(void* hdc, const LLRect* rc, void* brush)
{ (void)hdc; (void)rc; (void)brush; return 1; }

int DrawTextA(void* hdc, const char* text, int len, LLRect* rc,
              unsigned int format)
{ (void)hdc; (void)text; (void)len; (void)format; return rc ? 1 : 0; }
