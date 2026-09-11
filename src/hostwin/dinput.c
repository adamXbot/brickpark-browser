/* LEGOLAND portable build -- the DirectInput host shim (scope PORT-B).
 *
 * This one has to WORK, not degrade: InitSession (startup.c 0x0047f880) aborts
 * the whole session when InitInputSystem returns 0, and InitInputSystem
 * (input2.c 0x00473870) requires both CreateKeyboardDevice (0x004738b0) and
 * CreateMouseDevice (0x00473970) to succeed. So the shim reports attached
 * devices and real state.
 *
 * Vtable slots, all from input.c:18-33 / sysmisc.c:143-177 (the two files
 * declare identical layouts, which is the DirectX 3 IDirectInputDevice ABI):
 *   IDirectInputDeviceA   0x00 QueryInterface  0x04 AddRef       0x08 Release
 *                         0x0c GetCapabilities 0x10 EnumObjects  0x14 GetProperty
 *                         0x18 SetProperty     0x1c Acquire      0x20 Unacquire
 *                         0x24 GetDeviceState  0x28 GetDeviceData
 *                         0x2c SetDataFormat   0x30 SetEventNotification
 *                         0x34 SetCooperativeLevel
 *   IDirectInputA         0x00 QueryInterface  0x04 AddRef       0x08 Release
 *                         0x0c CreateDevice    0x10 EnumDevices  0x14 GetDeviceStatus
 *                         (0x18 RunControlPanel, 0x1c Initialize -- unused)
 *
 * What the game reads back:
 *   keyboard  GetDeviceState(256, g_key_state) -- 256 DIK_* bytes, high bit
 *             set while down (input.c's ScanKeyboard 0x00473930). The DIK codes
 *             are PS/2 set-1 scan codes; ll_canvas.js maps KeyboardEvent.code
 *             to them.
 *   mouse     GetDeviceState(16, &g_mouse_state) into DIMOUSESTATE
 *             {long lX, lY, lZ; unsigned char rgbButtons[4];} (input.c:41).
 *             RELATIVE axes: the shim accumulates deltas since the last read
 *             and zeroes them here, which is what DirectInput's relative mode
 *             does.
 *   wheel     CreateMouseDevice queries DIPROP_GRANULARITY of lZ and keeps it
 *             in g_wheel_granularity; ScanMouse then compares lZ against
 *             +/- that. Reporting 120 (the Windows default) and emitting +/-120
 *             per wheel notch from JS is what makes one notch one scroll.
 *   caps      GetCapabilities must report dwFlags != 0: CreateMouseDevice's
 *             test is `((caps.dwFlags == 0) & 1) == 0`, i.e. "flags are
 *             non-zero" (sysmisc.c:245). DIDC_ATTACHED (1) is set.
 *
 * The browser only reports ABSOLUTE pointer positions, so ll_canvas.js
 * differences them. The game then applies its own acceleration
 * (UpdateControllerFromMouseData doubles the delta up to twice, input.c:~250),
 * so the game cursor moves faster than the real pointer. Known divergence,
 * recorded in docs/lanes/scope-port-b.md §7.
 *
 * ---------------------------------------------------------------------------
 * PORT-B6: THE PRESS LATCH -- why an EVENT source feeding a POLLED API needs
 * one, and why the front end ignored every click until it had one.
 *
 * DirectInput's GetDeviceState is a POLL: it reports the state at the instant
 * it is called, and a press that begins and ends between two polls never
 * happened. The browser is an EVENT source: mousedown and mouseup arrive as
 * two records in a queue, and `ll_host_drain_events` (user32.c) empties the
 * WHOLE queue -- from inside this function -- before the state is copied out.
 * So a click whose down and up landed in the same drain collapsed to "up" and
 * the game read `rgbButtons[0] == 0`, every time.
 *
 * MEASURED, before the fix, in a real tab with ?trace=1 (a click on the
 * PLAYER DETAILS slot-1 button, confirmed to reach the canvas):
 *
 *     HOST DINPUT mouse state dx=-60 dy=-53 dz=0 buttons=000
 *     HOST DINPUT mouse state dx=0   dy=127 dz=0 buttons=000
 *                                                        ^^^ never once 1
 *
 * The cursor moved -- motion is accumulated, so it cannot be lost this way --
 * and the frame hash before and after the click was bit-identical
 * (0x17207e04). That is the whole of PORT-B4 §5's "input arrives from the host
 * but the front end ignores it": the button half never arrived at all.
 *
 * The latch holds a press until a state read has actually REPORTED it, and
 * then until the frame that read it has been presented:
 *
 *   press   down = 1, seen = 0
 *   release seen and its frame already presented -> release now (the normal
 *           path for a human click, which spans several frames: no added
 *           latency, no behaviour change)
 *           otherwise -> remember it (`pending_up`) and stay down
 *   poll    report `down`, then: first sight of a press records the frame
 *           counter; a later poll with a release pending applies it once the
 *           frame counter has MOVED ON.
 *
 * Why the frame counter and not just "one poll": ScanMouse's GetDeviceState is
 * not the only poll in a frame -- `ll_host_drain_events` also runs from
 * PeekMessageA and WaitMessage, and the game may read the device more than once
 * between two ReadGameButtons ticks. `ReadGameButtons` (bighelp.c 0x00452460)
 * computes its press and release edges from `g_controller->buttons ^
 * g_input.prev_buttons` ONCE per tick, so the press has to survive to the end
 * of the frame it was first seen in, not merely to the end of one poll.
 *
 * The wall-clock escape (LL_LATCH_MAX_MS) is for the loops that poll input
 * WITHOUT presenting -- PlayMovie's button-release spin (uimisc2.c:766) and
 * RunMovie's abort test are both `do { ProcessSystemEvents(); ReadGameButtons();
 * } while (buttons)`. Without it a latched press in one of those never lifts and
 * the loop never exits. 250 ms is longer than any frame and shorter than a
 * deliberate press.
 *
 * Keys get the same treatment for the same reason, and it matters more there:
 * name entry on the PLAYER DETAILS screen reads the DIK array (input2.c's
 * GetTypedChar), so a synthesised keystroke that is not held across a frame
 * types nothing.
 */
#include <string.h>

#include "ll_host.h"

#define DI_OK                 0L
#define DIERR_INVALIDPARAM    ((long)0x80070057)
#define DIERR_UNSUPPORTED     ((long)0x80004001)

#define LL_DEV_KEYBOARD 1
#define LL_DEV_MOUSE    2

/* ---- the press latch ---------------------------------------------------- */
/* See the header comment. One of these per key and per mouse button; `down` is
 * the byte a poll copies out and everything else is bookkeeping. */
#define LL_LATCH_MAX_MS 250u

typedef struct LLLatch {
    unsigned char down;         /* 0x80 while a poll should report it pressed */
    unsigned char seen;         /* a poll has reported this press at least once */
    unsigned char pending_up;   /* a release arrived that could not be applied */
    unsigned int  frame;        /* frames presented when `seen` was set */
    unsigned int  ms;           /* timeGetTime() when the press arrived */
} LLLatch;

/* Frames presented (ddraw.c) and the millisecond clock (winmm.c). Both are
 * already part of the host ABI; nothing new is needed for the latch. */
static unsigned int latch_frame(void) { return (unsigned int)ll_host_frames_presented(); }

static void latch_press(LLLatch* l)
{
    l->down = 0x80;
    l->seen = 0;
    l->pending_up = 0;
    l->frame = latch_frame();
    l->ms = timeGetTime();
}

static void latch_release(LLLatch* l)
{
    /* The ordinary case: a human press that several polls have already seen,
     * in a frame that has been presented. Released immediately -- the latch
     * adds nothing to the path a real mouse takes. */
    if (l->seen && latch_frame() != l->frame) {
        l->down = 0;
        l->pending_up = 0;
        return;
    }
    l->pending_up = 1;
}

/* Called from a device poll AFTER the state has been copied out, so the poll
 * that first reports a press always reports it. */
static void latch_polled(LLLatch* l)
{
    if (!l->down)
        return;
    if (!l->seen) {
        l->seen = 1;
        l->frame = latch_frame();
        return;
    }
    if (l->pending_up &&
        (latch_frame() != l->frame || timeGetTime() - l->ms > LL_LATCH_MAX_MS)) {
        l->down = 0;
        l->pending_up = 0;
    }
}

/* ---- the state the browser feeds --------------------------------------- */
static unsigned char g_keys[256];
static LLLatch       g_key_latch[256];
static int  g_mouse_dx, g_mouse_dy, g_mouse_dz;
static unsigned char g_mouse_buttons[4];
static LLLatch       g_btn_latch[4];

unsigned char* ll_host_key_state(void) { return g_keys; }

void ll_host_key_set(int dik, int down)
{
    if (dik <= 0 || dik >= 256)
        return;
    /* PORT-B7: the DELIVERY half of PORT-B6's poll line. The poll line says a
     * key was reported; this one says a key arrived from the browser at all, so
     * "the game ignored my keystroke" splits into three answerable states --
     * no key_set line (the DOM event never reached ll_canvas.js's handler, or
     * dikOf returned 0), a key_set line with no poll line (the game is not
     * calling ScanKeyboard), or both (the game has it and is ignoring it).
     * Two lines per keystroke, nothing while nothing is pressed. */
    ll_host_trace("DINPUT key_set DIK 0x%02x %s", (unsigned)dik,
                  down ? "down" : "up");
    if (down)
        latch_press(&g_key_latch[dik]);
    else
        latch_release(&g_key_latch[dik]);
    g_keys[dik] = g_key_latch[dik].down;
}

void ll_host_mouse_move(int dx, int dy) { g_mouse_dx += dx; g_mouse_dy += dy; }
void ll_host_mouse_wheel(int dz) { g_mouse_dz += dz; }

void ll_host_mouse_button(int button, int down)
{
    if (button < 0 || button >= 4)
        return;
    if (down)
        latch_press(&g_btn_latch[button]);
    else
        latch_release(&g_btn_latch[button]);
    g_mouse_buttons[button] = g_btn_latch[button].down;
}

/* ---- objects ----------------------------------------------------------- */
typedef long (*ll_slot)(void);

typedef struct LLDIDevice {
    const void* vtbl;
    int kind;
    int refs;
    int acquired;
} LLDIDevice;

typedef struct LLDInput {
    const void* vtbl;
    int refs;
} LLDInput;

/* DIMOUSESTATE as input.c declares it (16 bytes on ILP32). */
typedef struct LLMouseState {
    long lX, lY, lZ;
    unsigned char rgbButtons[4];
} LLMouseState;

/* DIPROPHEADER / DIPROPDWORD (sysmisc.c:180-190). */
typedef struct LLPropHeader {
    unsigned long dwSize, dwHeaderSize, dwObj, dwHow;
} LLPropHeader;
typedef struct LLPropDword {
    LLPropHeader  diph;
    unsigned long dwData;
} LLPropDword;

/* DIDEVCAPS, DirectX 3 size 0x2c (sysmisc.c:193). */
typedef struct LLDevCaps {
    unsigned long dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs;
    unsigned long pad18[5];
} LLDevCaps;

#define DIPROP_GRANULARITY_ID  3u     /* MAKEDIPROP(3) */
#define DIMOFS_Z               8u

/* ---- IDirectInputDeviceA ----------------------------------------------- */

static long dev_QueryInterface(LLDIDevice* d, const void* iid, void** out)
{ (void)iid; if (!out) return DIERR_INVALIDPARAM; d->refs++; *out = d; return DI_OK; }
static long dev_AddRef(LLDIDevice* d) { return ++d->refs; }
static long dev_Release(LLDIDevice* d) { if (d->refs > 0) d->refs--; return d->refs; }

/* dwFlags MUST be non-zero: see the header comment. */
static long dev_GetCapabilities(LLDIDevice* d, LLDevCaps* caps)
{
    if (!caps) return DIERR_INVALIDPARAM;
    caps->dwFlags = 0x00000001;                       /* DIDC_ATTACHED */
    caps->dwDevType = d->kind == LL_DEV_MOUSE ? 0x02 : 0x03;  /* MOUSE / KEYBOARD */
    caps->dwAxes = d->kind == LL_DEV_MOUSE ? 3 : 0;
    caps->dwButtons = d->kind == LL_DEV_MOUSE ? 3 : 256;
    caps->dwPOVs = 0;
    return DI_OK;
}

static long dev_EnumObjects(LLDIDevice* d, void* cb, void* ref, unsigned long flags)
{ (void)d; (void)cb; (void)ref; (void)flags; return DI_OK; }

/* The only property the game reads is the wheel's granularity. */
static long dev_GetProperty(LLDIDevice* d, const void* guid, LLPropDword* prop)
{
    (void)d;
    if (!prop) return DIERR_INVALIDPARAM;
    if ((unsigned long)(size_t)guid == DIPROP_GRANULARITY_ID &&
        prop->diph.dwObj == DIMOFS_Z) {
        prop->dwData = 120;          /* one wheel notch, the Windows default */
        return DI_OK;
    }
    return DIERR_UNSUPPORTED;
}

static long dev_SetProperty(LLDIDevice* d, const void* guid, const void* prop)
{ (void)d; (void)guid; (void)prop; return DI_OK; }

static long dev_Acquire(LLDIDevice* d) { d->acquired = 1; return DI_OK; }
static long dev_Unacquire(LLDIDevice* d) { d->acquired = 0; return DI_OK; }

/* ScanKeyboard / ScanMouse retry until this returns 0, so it must succeed. */
static long dev_GetDeviceState(LLDIDevice* d, unsigned long size, void* data)
{
    if (ll_host_beating()) ll_host_beat("dinput.GetDeviceState");
    if (!data)
        return DIERR_INVALIDPARAM;

    /* Pull whatever the browser queued since the last call. */
    ll_host_drain_events();

    if (d->kind == LL_DEV_KEYBOARD) {
        int i;
        /* PORT-B7: once, the first time ScanKeyboard polls -- the size it asks
         * for and the ADDRESS it asks for it at. The address is the cheap test
         * for the split-record class of defect that has stopped this port three
         * times already (PORT-A5's GameInput, PORT-B6's BlitCtx and CurProfile):
         * `g_key_state` is a 256-byte array the game hands to DirectInput, and
         * if the closure ever emits it as two objects the writer's base and the
         * reader's base differ. Compare it against the page's
         * `Module._ll_dbg_addr(0)` (main.c) -- equal means one object, and the
         * keyboard is not where a "the game ignores keys" report should look. */
        static int first_poll;
        if (!first_poll) {
            first_poll = 1;
            ll_host_trace("DINPUT keyboard poll #1: %lu bytes into 0x%08x",
                          (unsigned long)size, (unsigned)(size_t)data);
        }
        if (size > sizeof(g_keys))
            size = sizeof(g_keys);
        memcpy(data, g_keys, (size_t)size);
        /* AFTER the copy: this poll has now reported whatever is held, so a
         * release that arrived too early may be applied for the next one. 255
         * trivial tests once a frame. */
        for (i = 1; i < 256; i++)
            if (g_key_latch[i].down) {
                /* PORT-B6: the keyboard's half of PORT-B4's mouse trace line --
                 * the one that says whether a key that "did nothing" was never
                 * delivered or was delivered and ignored. One line per key per
                 * poll while it is held, and nothing at all while it is not. */
                ll_host_trace("DINPUT key state DIK 0x%02x down (poll reports it)",
                              (unsigned)i);
                latch_polled(&g_key_latch[i]);
                g_keys[i] = g_key_latch[i].down;
            }
        return DI_OK;
    }

    if (size >= sizeof(LLMouseState)) {
        LLMouseState* ms = (LLMouseState*)data;
        ms->lX = g_mouse_dx;
        ms->lY = g_mouse_dy;
        ms->lZ = g_mouse_dz;
        memcpy(ms->rgbButtons, g_mouse_buttons, 4);
        /* PORT-B4: trace only the calls that carry something, which is a few
         * per gesture rather than one per frame. This is the line that says
         * whether a click that "did nothing" was never delivered or was
         * delivered and ignored -- the question that cost this lane an hour,
         * because the browser event queue draining to empty proves only that
         * SOMETHING consumed it. */
        if (g_mouse_dx || g_mouse_dy || g_mouse_dz || g_mouse_buttons[0]
            || g_mouse_buttons[1] || g_mouse_buttons[2])
            ll_host_trace("DINPUT mouse state dx=%d dy=%d dz=%d buttons=%d%d%d",
                          g_mouse_dx, g_mouse_dy, g_mouse_dz,
                          g_mouse_buttons[0] ? 1 : 0, g_mouse_buttons[1] ? 1 : 0,
                          g_mouse_buttons[2] ? 1 : 0);
        g_mouse_dx = g_mouse_dy = g_mouse_dz = 0;   /* relative axes: consumed */
        /* AFTER the copy, for the same reason as the keyboard branch. */
        {
            int b;
            for (b = 0; b < 4; b++)
                if (g_btn_latch[b].down) {
                    latch_polled(&g_btn_latch[b]);
                    g_mouse_buttons[b] = g_btn_latch[b].down;
                }
        }
        return DI_OK;
    }
    return DIERR_INVALIDPARAM;
}

/* Buffered data is never requested (the game is state-based). */
static long dev_GetDeviceData(LLDIDevice* d, unsigned long size, void* data,
                              unsigned long* count, unsigned long flags)
{
    if (ll_host_beating()) ll_host_beat("dinput.GetDeviceData");
    (void)d; (void)size; (void)data; (void)flags;
    if (count) *count = 0;
    return DI_OK;
}

static long dev_SetDataFormat(LLDIDevice* d, const void* fmt)
{ (void)d; (void)fmt; return DI_OK; }
static long dev_SetEventNotification(LLDIDevice* d, void* ev)
{ (void)d; (void)ev; return DI_OK; }
static long dev_SetCooperativeLevel(LLDIDevice* d, void* hwnd, unsigned long flags)
{ (void)d; (void)hwnd; (void)flags; return DI_OK; }

static const ll_slot g_device_vtbl[] = {
    (ll_slot)dev_QueryInterface,        /* 0x00 */
    (ll_slot)dev_AddRef,                /* 0x04 */
    (ll_slot)dev_Release,               /* 0x08 */
    (ll_slot)dev_GetCapabilities,       /* 0x0c */
    (ll_slot)dev_EnumObjects,           /* 0x10 */
    (ll_slot)dev_GetProperty,           /* 0x14 */
    (ll_slot)dev_SetProperty,           /* 0x18 */
    (ll_slot)dev_Acquire,               /* 0x1c */
    (ll_slot)dev_Unacquire,             /* 0x20 */
    (ll_slot)dev_GetDeviceState,        /* 0x24 */
    (ll_slot)dev_GetDeviceData,         /* 0x28 */
    (ll_slot)dev_SetDataFormat,         /* 0x2c */
    (ll_slot)dev_SetEventNotification,  /* 0x30 */
    (ll_slot)dev_SetCooperativeLevel    /* 0x34 */
};

/* ---- IDirectInputA ----------------------------------------------------- */

static LLDIDevice g_keyboard_dev;
static LLDIDevice g_mouse_dev;
static LLDInput   g_dinput_obj;

static long di_QueryInterface(LLDInput* d, const void* iid, void** out)
{ (void)iid; if (!out) return DIERR_INVALIDPARAM; d->refs++; *out = d; return DI_OK; }
static long di_AddRef(LLDInput* d) { return ++d->refs; }
static long di_Release(LLDInput* d) { if (d->refs > 0) d->refs--; return d->refs; }

/* Which device is being asked for: GUID_SysKeyboard and GUID_SysMouse differ
 * only in their first dword (…2b61 keyboard, …2b60 mouse). The GUIDs are
 * dinput.lib data linked into the exe, so gen_link.py rebuilds them from the
 * image and the comparison is against the real bytes. If the first dword is
 * neither (a zero-filled closure, say), fall back to call order: the game
 * always creates the keyboard first (input2.c's InitInputSystem). */
static int device_kind(const void* guid)
{
    static int nth;
    unsigned long first = guid ? *(const unsigned long*)guid : 0;
    if (first == 0x6f1d2b61ul) return LL_DEV_KEYBOARD;
    if (first == 0x6f1d2b60ul) return LL_DEV_MOUSE;
    return (nth++ == 0) ? LL_DEV_KEYBOARD : LL_DEV_MOUSE;
}

static long di_CreateDevice(LLDInput* d, const void* guid, LLDIDevice** out,
                            void* outer)
{
    LLDIDevice* dev;
    (void)d; (void)outer;
    if (!out)
        return DIERR_INVALIDPARAM;
    dev = device_kind(guid) == LL_DEV_KEYBOARD ? &g_keyboard_dev : &g_mouse_dev;
    dev->vtbl = g_device_vtbl;
    dev->kind = dev == &g_keyboard_dev ? LL_DEV_KEYBOARD : LL_DEV_MOUSE;
    dev->refs++;
    ll_host_trace("DirectInput CreateDevice(%s)",
                  dev->kind == LL_DEV_KEYBOARD ? "GUID_SysKeyboard" : "GUID_SysMouse");
    *out = dev;
    return DI_OK;
}

static long di_EnumDevices(LLDInput* d, unsigned long type, void* cb,
                           void* ref, unsigned long flags)
{ (void)d; (void)type; (void)cb; (void)ref; (void)flags; return DI_OK; }

/* CreateMouseDevice's last act is `return GetDeviceStatus(...) == 0;`. */
static long di_GetDeviceStatus(LLDInput* d, const void* guid)
{ (void)d; (void)guid; return DI_OK; }

static long di_RunControlPanel(LLDInput* d, void* owner, unsigned long flags)
{ (void)d; (void)owner; (void)flags; return DI_OK; }
static long di_Initialize(LLDInput* d, void* inst, unsigned long ver)
{ (void)d; (void)inst; (void)ver; return DI_OK; }

static const ll_slot g_dinput_vtbl[] = {
    (ll_slot)di_QueryInterface,   /* 0x00 */
    (ll_slot)di_AddRef,           /* 0x04 */
    (ll_slot)di_Release,          /* 0x08 */
    (ll_slot)di_CreateDevice,     /* 0x0c */
    (ll_slot)di_EnumDevices,      /* 0x10 */
    (ll_slot)di_GetDeviceStatus,  /* 0x14 */
    (ll_slot)di_RunControlPanel,  /* 0x18 */
    (ll_slot)di_Initialize        /* 0x1c */
};

/* The one exported DINPUT entry (the game's static dinput.lib thunk at
 * 0x0049d320). InitInputSystem does not check the result. */
long DirectInputCreateA(void* inst, unsigned long version, void** out,
                        void* outer)
{
    (void)inst; (void)version; (void)outer;
    if (!out)
        return DIERR_INVALIDPARAM;
    g_dinput_obj.vtbl = g_dinput_vtbl;
    g_dinput_obj.refs++;
    *out = &g_dinput_obj;
    return DI_OK;
}
