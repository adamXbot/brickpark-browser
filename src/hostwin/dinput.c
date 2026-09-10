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
 */
#include <string.h>

#include "ll_host.h"

#define DI_OK                 0L
#define DIERR_INVALIDPARAM    ((long)0x80070057)
#define DIERR_UNSUPPORTED     ((long)0x80004001)

#define LL_DEV_KEYBOARD 1
#define LL_DEV_MOUSE    2

/* ---- the state the browser feeds --------------------------------------- */
static unsigned char g_keys[256];
static int  g_mouse_dx, g_mouse_dy, g_mouse_dz;
static unsigned char g_mouse_buttons[4];

unsigned char* ll_host_key_state(void) { return g_keys; }

void ll_host_key_set(int dik, int down)
{
    if (dik > 0 && dik < 256)
        g_keys[dik] = down ? 0x80 : 0x00;
}

void ll_host_mouse_move(int dx, int dy) { g_mouse_dx += dx; g_mouse_dy += dy; }
void ll_host_mouse_wheel(int dz) { g_mouse_dz += dz; }

void ll_host_mouse_button(int button, int down)
{
    if (button >= 0 && button < 4)
        g_mouse_buttons[button] = down ? 0x80 : 0x00;
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
    if (!data)
        return DIERR_INVALIDPARAM;

    /* Pull whatever the browser queued since the last call. */
    ll_host_drain_events();

    if (d->kind == LL_DEV_KEYBOARD) {
        if (size > sizeof(g_keys))
            size = sizeof(g_keys);
        memcpy(data, g_keys, (size_t)size);
        return DI_OK;
    }

    if (size >= sizeof(LLMouseState)) {
        LLMouseState* ms = (LLMouseState*)data;
        ms->lX = g_mouse_dx;
        ms->lY = g_mouse_dy;
        ms->lZ = g_mouse_dz;
        memcpy(ms->rgbButtons, g_mouse_buttons, 4);
        g_mouse_dx = g_mouse_dy = g_mouse_dz = 0;   /* relative axes: consumed */
        return DI_OK;
    }
    return DIERR_INVALIDPARAM;
}

/* Buffered data is never requested (the game is state-based). */
static long dev_GetDeviceData(LLDIDevice* d, unsigned long size, void* data,
                              unsigned long* count, unsigned long flags)
{
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
