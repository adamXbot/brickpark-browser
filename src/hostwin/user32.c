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
static int       g_cursor_shown = 0;   /* Win32's display count: >= 0 is visible */

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

/* ---- tracing (PORT-B2) --------------------------------------------------
 * kernel32.c has had LL_HOST_TRACE since PORT-A, but it is a static there and
 * the DirectX half of the shim had no tracing at all -- so a run that got past
 * the loaders and then stopped produced a trace that ended at the last ReadFile
 * and said nothing about whether DirectDrawCreate, the window or the first
 * surface was ever reached. That is exactly the gap between "the loader works"
 * and "the page draws", which is this lane's whole subject, so the same switch
 * now drives ddraw.c, user32.c, gdi32.c and dinput.c too.
 *
 * The env var is read independently rather than by calling into kernel32.c:
 * that file belongs to PORT-A2 and this must not need a change there. Per-frame
 * calls (Lock, Blt, timeGetTime, PeekMessageA) are deliberately NOT traced --
 * they would bury everything else; ddraw.c traces the FIRST present only. */
static int g_trace = -1;

int ll_host_tracing(void)
{
    if (g_trace < 0) {
        const char* e = getenv("LL_HOST_TRACE");
        g_trace = e && *e && *e != '0';
    }
    return g_trace;
}

void ll_host_trace(const char* fmt, ...)
{
    va_list ap;
    if (!ll_host_tracing())
        return;
    fputs("HOST ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ---- PORT-B8: the heartbeat --------------------------------------------- */
/* See ll_host.h. One line every LL_BEAT_MS naming the most recent host entry
 * point and how many host calls have happened since the previous line, printed
 * to stderr so it survives a main thread that has stopped running JS. */
/* $LL_HOST_BEAT is the period in ms; "1" means the 500 ms default. */
#define LL_BEAT_DEFAULT_MS 500.0

/* The ring. A beat line every LL_BEAT_MS is at best 500 ms stale, and at
 * 360,000 host calls a second that is 180,000 calls of slack -- useless for
 * "what was the last thing the game asked for". What IS precise is the
 * sequence of DISTINCT entry points: the front end cycles through a few dozen
 * per frame, so 32 of them with their repeat counts is several frames of exact
 * history, and the last line printed before a wedge carries it. */
#define LL_BEAT_RING 32

static int         g_beat = -1;
static double      g_beat_ms = LL_BEAT_DEFAULT_MS;
static const char* g_beat_who = "(none)";
static unsigned    g_beat_calls;
static unsigned    g_beat_total;
static double      g_beat_last_ms;
static double      g_beat_last_yield_ms;   /* copy, so the line can report the gap */

static const char* g_beat_ring[LL_BEAT_RING];
static unsigned    g_beat_ring_n[LL_BEAT_RING];
static int         g_beat_ring_head = -1;  /* index of the newest entry */

int ll_host_beating(void)
{
    if (g_beat < 0) {
        const char* e = getenv("LL_HOST_BEAT");
        g_beat = e && *e && *e != '0';
        if (g_beat) {
            double ms = atof(e);
            if (ms > 1.0)
                g_beat_ms = ms;
        }
    }
    return g_beat;
}

/* Newest last, "name*count", so one line reads left to right as history.
 * Called on every wrap of the ring and from the timed line. */
static void ll_beat_dump_ring(void)
{
    int i;
    fputs("BEAT   ring:", stderr);
    for (i = 0; i < LL_BEAT_RING; i++) {
        int k = (g_beat_ring_head + 1 + i) % LL_BEAT_RING;
        if (!g_beat_ring[k])
            continue;
        fprintf(stderr, " %s*%u", g_beat_ring[k], g_beat_ring_n[k]);
    }
    fputc('\n', stderr);
}

void ll_host_beat(const char* who)
{
    double now;
    if (!ll_host_beating())
        return;
    /* Compared by POINTER: every caller passes a string literal, so this is
     * one compare and it never allocates. */
    if (g_beat_ring_head < 0 || g_beat_ring[g_beat_ring_head] != who) {
        g_beat_ring_head = (g_beat_ring_head + 1) % LL_BEAT_RING;
        /* Dumped on every WRAP, not only on the timed line. The timed line is
         * up to LL_BEAT_MS stale, which at 33 fps is a dozen frames of
         * transitions -- so it can never carry the last calls before a wedge.
         * Wrapping is counted in transitions, not in time, so the last line a
         * wedged tab prints always ends within 32 distinct host calls of the
         * loop it never left. */
        if (g_beat_ring_head == 0)
            ll_beat_dump_ring();
        g_beat_ring[g_beat_ring_head] = who;
        g_beat_ring_n[g_beat_ring_head] = 0;
    }
    g_beat_ring_n[g_beat_ring_head]++;
    g_beat_who = who;
    g_beat_calls++;
    g_beat_total++;
#ifdef __EMSCRIPTEN__
    now = emscripten_get_now();
#else
    now = 0.0;
#endif
    if (now - g_beat_last_ms < g_beat_ms)
        return;
    fprintf(stderr, "BEAT t=%.0f last=%s calls=%u total=%u since_yield=%.0fms\n",
            now, who, g_beat_calls, g_beat_total, now - g_beat_last_yield_ms);
    ll_beat_dump_ring();
    fflush(stderr);
    g_beat_last_ms = now;
    g_beat_calls = 0;
}

/* ---- the yield ---------------------------------------------------------- */
static double g_last_yield_ms;

void ll_host_yield(unsigned int ms)
{
#ifdef __EMSCRIPTEN__
    g_last_yield_ms = emscripten_get_now();
    emscripten_sleep(ms);
    g_last_yield_ms = emscripten_get_now();
    g_beat_last_yield_ms = g_last_yield_ms;
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
    ll_host_trace("RegisterClassExA(\"%s\") wndproc %p",
                  wc->lpszClassName ? wc->lpszClassName : "", wc->lpfnWndProc);
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
    ll_host_trace("CreateWindowExA(\"%s\") %dx%d", name ? name : "", w, h);
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

/* SPI_GETMOUSE (3) MUST fill its buffer. ResetController (gameframe.c:311,
 * 0x004589a0) does
 *
 *     SystemParametersInfoA(3, 0, mouse, 0);
 *     c->accel_t1 = mouse[0]; c->accel_t2 = mouse[1]; c->accel = mouse[2];
 *
 * on an UNINITIALISED three-int stack array and never checks the result, so the
 * old "report success and touch nothing" left the game's mouse acceleration
 * reading stack garbage -- with `accel` garbage-non-zero and the thresholds
 * garbage-small, UpdateControllerFromMouseData (input.c:238) multiplies every
 * delta by 4 and the cursor is uncontrollable. PORT-B2.
 *
 * The values reported are {6, 10, 0}: Windows' own default thresholds, and
 * acceleration OFF. Acceleration off is a real Windows configuration ("enhance
 * pointer precision" unchecked), and choosing it also closes the mouse
 * divergence PORT-B recorded in its §7: the browser reports absolute pointer
 * positions which ll_canvas.js differences into DirectInput deltas, so with the
 * game's own 2x/4x stages disabled the game cursor tracks the real pointer 1:1
 * instead of accelerating away from it. No game edit needed. */
int SystemParametersInfoA(unsigned int action, unsigned int param,
                          void* data, unsigned int flags)
{
    (void)param; (void)flags;
    if (action == 3 && data) {          /* SPI_GETMOUSE */
        int* m = (int*)data;
        m[0] = 6;                      /* xThreshold  (Windows default) */
        m[1] = 10;                     /* yThreshold  (Windows default) */
        m[2] = 0;                      /* acceleration off */
        return 1;
    }
    if (action == 4 && data) {         /* SPI_SETMOUSE: accepted, ignored */
        return 1;
    }
    return 1;
}

/* =========================================================================
 * the pump
 * ========================================================================= */

static int g_peek_calls;

BOOL PeekMessageA(MSG* msgp, HWND hwnd, UINT lo, UINT hi,
                 UINT flags)
{
    LLMsg* msg = (LLMsg*)msgp;
    (void)hwnd; (void)lo; (void)hi;

    ll_host_drain_events();
    if (ll_host_beating()) ll_host_beat("user32.PeekMessageA");
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

BOOL GetMessageA(MSG* msgp, HWND hwnd, UINT lo, UINT hi)
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
BOOL TranslateMessage(const MSG* msg) { (void)msg; return 0; }

long DispatchMessageA(const MSG* msgp)
{
    if (ll_host_beating()) ll_host_beat("user32.DispatchMessageA");
    const LLMsg* msg = (const LLMsg*)msgp;
    if (!msg)
        return 0;
    if (g_wndproc)
        return g_wndproc(msg->hwnd, msg->message, msg->wParam, msg->lParam);
    return 0;
}

int WaitMessage(void)
{
    if (ll_host_beating()) ll_host_beat("user32.WaitMessage");
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

/* Win32's display count starts at 0 with a mouse installed and the cursor is
 * visible while the count is >= 0, so InitScreen's ShowCursor(0) (screen.c:1392,
 * taken when g_screencfg->cursor == 0) must take it to -1 and hide it. PORT-B
 * started the count at 1, which made that call a no-op. PORT-B2. */
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

/* MessageBoxA has to ANSWER, not just print (PORT-B2).
 *
 * There is nobody to click a button in this port, and one of the game's two
 * modal sites is a LOOP that only exits on a specific answer:
 *
 *   RES_EnsureMounted (sysmisc.c:445, 0x004515e0)
 *       while (!RES_FindVolumeOnAnyDrive("LEGOLAND")) {
 *           ... WNDENV_Minimise();
 *           if (MessageBoxA(hwnd, "Please insert the LEGOLAND CD-ROM ...",
 *                           "CD Missing", 0x50015) == IDCANCEL) return 0;
 *       }
 *
 * 0x50015 is MB_RETRYCANCEL|MB_ICONHAND|MB_SETFOREGROUND|MB_TOPMOST, and
 * IDCANCEL is 2. Returning IDOK (1) forever, as PORT-B did, means the `while`
 * re-probes and asks again with nothing in between that yields -- a hard hang of
 * the tab if the CD probe ever fails. (It does not fail today: kernel32.c
 * presents $LL_CD_DIR as a CDFS volume named LEGOLAND. It will fail the moment
 * that changes, and the failure mode must not be a wedged page.)
 *
 * So the answer is derived from the button set in the low nibble of `type`, and
 * it is always the answer that DOES NOT ask again:
 *
 *   MB_OK (0)                 -> IDOK       1
 *   MB_OKCANCEL (1)           -> IDCANCEL   2
 *   MB_ABORTRETRYIGNORE (2)   -> IDIGNORE   5
 *   MB_YESNOCANCEL (3)        -> IDNO       7
 *   MB_YESNO (4)              -> IDNO       7
 *   MB_RETRYCANCEL (5)        -> IDCANCEL   2
 *   MB_CANCELTRYCONTINUE (6)  -> IDCONTINUE 11
 *
 * The text goes to the console AND to the page (ll_js_messagebox puts it in the
 * status bar, so a modal that flashes past is still readable), and the shim
 * yields for long enough that a person watching sees it before the game moves
 * on. The delay shortens after the first few boxes so a repeated modal cannot
 * turn into minutes of waiting. */
#define LL_MB_PAUSE_MS       1200u
#define LL_MB_PAUSE_SHORT_MS  150u
#define LL_MB_PAUSE_COUNT       3

#ifdef __EMSCRIPTEN__
extern void ll_js_messagebox(const char* text, const char* caption, int answer);
#else
static void ll_js_messagebox(const char* text, const char* caption, int answer)
{ (void)text; (void)caption; (void)answer; }
#endif

static char g_mb_text[512];
static int  g_mb_answer;
static int  g_mb_count;

const char* ll_host_last_messagebox(void) { return g_mb_text; }
int         ll_host_last_messagebox_answer(void) { return g_mb_answer; }

int MessageBoxA(void* hwnd, const char* text, const char* caption,
                unsigned int type)
{
    static const char* const kName[12] = {
        "0", "IDOK", "IDCANCEL", "IDABORT", "IDRETRY", "IDIGNORE", "IDYES",
        "IDNO", "IDCLOSE", "IDHELP", "IDTRYAGAIN", "IDCONTINUE"
    };
    int answer;
    (void)hwnd;

    switch (type & 0x0fu) {
    case 1:  answer = 2;  break;   /* MB_OKCANCEL          -> IDCANCEL   */
    case 2:  answer = 5;  break;   /* MB_ABORTRETRYIGNORE  -> IDIGNORE   */
    case 3:  answer = 7;  break;   /* MB_YESNOCANCEL       -> IDNO       */
    case 4:  answer = 7;  break;   /* MB_YESNO             -> IDNO       */
    case 5:  answer = 2;  break;   /* MB_RETRYCANCEL       -> IDCANCEL   */
    case 6:  answer = 11; break;   /* MB_CANCELTRYCONTINUE -> IDCONTINUE */
    default: answer = 1;  break;   /* MB_OK                -> IDOK       */
    }

    snprintf(g_mb_text, sizeof(g_mb_text), "%s: %s",
             caption ? caption : "LEGOLAND", text ? text : "");
    g_mb_answer = answer;
    fprintf(stderr, "[MessageBox] %s  (type 0x%x, auto-answered %s)\n",
            g_mb_text, type,
            answer >= 0 && answer < 12 ? kName[answer] : "?");
    fflush(stderr);
    ll_js_messagebox(text ? text : "", caption ? caption : "LEGOLAND", answer);

    /* Let the page paint the text, and let the browser breathe: a modal is
     * exactly the place a game expects to have given up the CPU. */
    ll_host_yield(g_mb_count++ < LL_MB_PAUSE_COUNT ? LL_MB_PAUSE_MS
                                                  : LL_MB_PAUSE_SHORT_MS);
    return answer;
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

/* =========================================================================
 * GDI drawing that lives in USER32 (PORT-B2)
 * ========================================================================= */

/* bubblecache.c:439 fills the speech-bubble background with a solid brush
 * before drawing the bubble's text into it, so this one is on screen. */
int FillRect(void* hdc, const LLRect* rc, void* brush)
{
    LLGdiStats* g = ll_host_gdi_stats();
    LLFontTarget t;
    unsigned short c;
    unsigned long cr;
    long y, x;

    g->fill_calls++;
    if (!rc || !ll_host_dc_target(hdc, &t)) {
        g->fill_no_target++;
        return 1;
    }
    cr = ll_host_brush_colour(brush);
    c = ll_font_colorref_to_565(cr);
    /* PORT-B13: the whole of P4-3 is in these five fields. An unresolved brush
     * resolves to BLACK, which is indistinguishable on the canvas from a fill
     * the colour key missed -- so record the handle, what it resolved to, and
     * the pixel that went down, and let llGdi() tell the two apart. */
    g->fill_last_brush = (unsigned int)(size_t)brush;
    g->fill_last_colorref = cr;
    g->fill_last_565 = c;
    g->fill_last_rect[0] = (int)rc->left;
    g->fill_last_rect[1] = (int)rc->top;
    g->fill_last_rect[2] = (int)rc->right;
    g->fill_last_rect[3] = (int)rc->bottom;
    g->fill_last_surf_w = t.w;
    g->fill_last_surf_h = t.h;
    for (y = rc->top; y < rc->bottom; y++) {
        unsigned short* row;
        if (y < t.clip.top || y >= t.clip.bottom)
            continue;
        row = (unsigned short*)((char*)t.bits + y * t.pitch);
        for (x = rc->left; x < rc->right; x++)
            if (x >= t.clip.left && x < t.clip.right)
                row[x] = c;
    }
    return 1;
}

/* DrawTextA is BOTH halves of the game's text handling, which is why it has to
 * be real even more than TextOutA does:
 *
 *   - it draws every wrapped and centred label (text.c's PrintCent 0x00454e60,
 *     PrintCentOpaque, PrintLimitedText, PrintCentColref; frontend2.c:522;
 *     movie.c:529; fpui4.c:562);
 *   - and it MEASURES. Eleven call sites pass DT_CALCRECT (0x400) and lay out
 *     around the rect that comes back: frontend2.c:521 centres a button caption
 *     on the measured box, bighelp.c:316 and bubblecache.c:500 size a speech
 *     bubble to hold the text, misc3.c:1105 measures a wrapped paragraph.
 *
 * The old stub returned 1 and left the rect alone, which told the game every
 * block of text was one pixel tall. ll_font.c does both halves from one set of
 * metrics, so what is measured is what is drawn. */
int DrawTextA(void* hdc, const char* text, int len, LLRect* rc,
              unsigned int format)
{
    LLFontTarget  t;
    LLFontMetrics m;
    int have_target;

    if (ll_host_beating()) ll_host_beat("user32.DrawTextA");
    ll_host_gdi_stats()->drawtext_calls++;
    if (!rc || !text)
        return 0;
    ll_host_dc_font(hdc, &m);
    have_target = ll_host_dc_target(hdc, &t);
    return ll_font_draw_text(have_target ? &t : 0, &m, rc, text, len, format,
                             ll_host_dc_fg(hdc), ll_host_dc_opaque(hdc),
                             ll_host_dc_bg(hdc));
}
