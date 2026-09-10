/* LEGOLAND portable build -- the host-shim self test (scope PORT-B).
 *
 * Why this exists. `legoland_browser` cannot link until PORT-A's
 * `gen_link.py --ilp32` fix lands (gen/globals.c does not compile on wasm32
 * yet: 12 `redeclaration with a different type` errors -- PORT-A deliverable 1).
 * Meanwhile the shim itself is testable on its own, so this drives it through
 * EXACTLY the sequence the game's startup spine drives it through, with the
 * same vtable-offset structs the game declares, and paints something the eye
 * can check.
 *
 * The sequence, and where each step comes from:
 *   DirectDrawCreate                      gpu.c 0x00463700 InitHostSystemGPU
 *   QueryInterface(IID_IDirectDraw2)      gpu.c:421
 *   GetCaps(dwSize 0x17c, 0x17c)          gpu.c:428
 *   RegisterClassExA / CreateWindowExA    screen.c 0x00463870 InitScreen
 *   SetCooperativeLevel(hwnd, 0x11)       screen.c:1383
 *   SetDisplayMode(w, h, 16, 0, 0)        sysmisc.c 0x00463ef0
 *   GetDisplayMode -> classify depth      sysmisc.c:323 (must give 2 = RGB565)
 *   CreateSurface primary  caps 0x4200    screen.c:1398
 *   CreateSurface back     caps 0x800     screen.c:1408
 *   CreateClipper + SetClipList           screen.c:1420
 *   DirectInputCreateA + 2 devices        input2.c 0x00473870
 *   Lock/draw/Unlock then primary Blt     surface.c + sysmisc.c FlipPrimary
 *
 * Then it loops: pump messages the way ProcessSystemEvents does, read the two
 * DirectInput devices the way ScanKeyboard/ScanMouse do, move a cursor from the
 * mouse deltas, and present. If the page shows the gradient, the pixel path is
 * right; if the white cursor box follows the pointer and the bars light up on
 * key presses, the input path is right.
 */
#include <stdio.h>
#include <string.h>

#include "ll_host.h"

/* ---- the game's own views of the host objects ---------------------------- */
typedef struct WinRect { long left, top, right, bottom; } WinRect;

typedef struct DDSurfaceDesc {
    unsigned long dwSize, dwFlags, dwHeight, dwWidth;
    long          lPitch;
    unsigned long dwBackBufferCount, dwMipMapCount, dwAlphaBitDepth, dwReserved;
    void*         lpSurface;
    char          pad28[0x48 - 0x28];
    struct { unsigned long dwSize, dwFlags, dwFourCC, dwRGBBitCount,
                           dwRBitMask, dwGBitMask, dwBBitMask, dwAlpha; } ddpf; /* +0x48 */
    unsigned long dwCaps;                                                       /* +0x68 */
} DDSurfaceDesc;                                                                /* 0x6c */

typedef struct DDBltFx { unsigned long dwSize; char pad[0x50 - 4];
                         unsigned long dwFillColor; char pad2[0x64 - 0x54]; } DDBltFx;

typedef struct RgnData { unsigned long dwSize, iType, nCount, nRgnSize;
                         WinRect rcBound, rect; } RgnData;

typedef struct DDSurface DDSurface;
typedef struct DDClipper DDClipper;
typedef struct DDraw DDraw;

/* Exactly the slot offsets gpu.c / surface.c / screen.c / blitmisc.c use. */
typedef struct DDSurfaceVtbl {
    char pad00[0x08];
    long (*Release)(DDSurface*);                                        /* 0x08 */
    char pad0c[0x14 - 0x0c];
    long (*Blt)(DDSurface*, WinRect*, DDSurface*, WinRect*, unsigned long, DDBltFx*); /* 0x14 */
    char pad18[0x58 - 0x18];
    long (*GetSurfaceDesc)(DDSurface*, DDSurfaceDesc*);                 /* 0x58 */
    char pad5c[0x64 - 0x5c];
    long (*Lock)(DDSurface*, WinRect*, DDSurfaceDesc*, unsigned long, void*); /* 0x64 */
    char pad68[0x6c - 0x68];
    long (*Restore)(DDSurface*);                                        /* 0x6c */
    long (*SetClipper)(DDSurface*, DDClipper*);                         /* 0x70 */
    char pad74[0x80 - 0x74];
    long (*Unlock)(DDSurface*, void*);                                  /* 0x80 */
} DDSurfaceVtbl;
struct DDSurface { DDSurfaceVtbl* vtbl; };

typedef struct DDClipperVtbl {
    char pad00[0x1c];
    long (*SetClipList)(DDClipper*, RgnData*, unsigned long);            /* 0x1c */
    long (*SetHWnd)(DDClipper*, unsigned long, void*);                   /* 0x20 */
} DDClipperVtbl;
struct DDClipper { DDClipperVtbl* vtbl; };

typedef struct DDrawVtbl {
    long (*QueryInterface)(DDraw*, const void*, void**);                /* 0x00 */
    char pad04[0x08 - 0x04];
    long (*Release)(DDraw*);                                            /* 0x08 */
    char pad0c[0x10 - 0x0c];
    long (*CreateClipper)(DDraw*, unsigned long, DDClipper**, void*);   /* 0x10 */
    char pad14[0x18 - 0x14];
    long (*CreateSurface)(DDraw*, DDSurfaceDesc*, DDSurface**, void*);  /* 0x18 */
    char pad1c[0x2c - 0x1c];
    long (*GetCaps)(DDraw*, void*, void*);                              /* 0x2c */
    long (*GetDisplayMode)(DDraw*, DDSurfaceDesc*);                     /* 0x30 */
    char pad34[0x50 - 0x34];
    long (*SetCooperativeLevel)(DDraw*, void*, unsigned long);          /* 0x50 */
    long (*SetDisplayMode)(DDraw*, unsigned long, unsigned long,
                           unsigned long, unsigned long, unsigned long);/* 0x54 */
} DDrawVtbl;
struct DDraw { DDrawVtbl* vtbl; };

/* DirectInput, the shape input.c:18 declares. */
typedef struct DIDevice DIDevice;
typedef struct DIDeviceVtbl {
    char pad00[0x1c];
    long (*Acquire)(DIDevice*);                                         /* 0x1c */
    char pad20[0x24 - 0x20];
    long (*GetDeviceState)(DIDevice*, unsigned long, void*);            /* 0x24 */
    char pad28[0x34 - 0x28];
    long (*SetCooperativeLevel)(DIDevice*, void*, unsigned long);       /* 0x34 */
} DIDeviceVtbl;
struct DIDevice { DIDeviceVtbl* lpVtbl; };

typedef struct DInput DInput;
typedef struct DInputVtbl {
    char pad00[0x0c];
    long (*CreateDevice)(DInput*, const void*, DIDevice**, void*);      /* 0x0c */
} DInputVtbl;
struct DInput { DInputVtbl* lpVtbl; };

typedef struct MouseState { int lX, lY, lZ; unsigned char rgbButtons[4]; } MouseState;

/* DIK codes input2.c's key map uses; a handful for the visible test. */
#define DIK_ESCAPE 0x01
#define DIK_SPACE  0x39
#define DIK_LEFT   0xcb
#define DIK_RIGHT  0xcd
#define DIK_UP     0xc8
#define DIK_DOWN   0xd0

/* IID_IDirectDraw2 and the two device GUIDs, as the real headers define them. */
static const unsigned char IID_IDirectDraw2[16] = {
    0x18, 0x2c, 0x1b, 0xb6, 0xea, 0x4b, 0xce, 0x11,
    0x91, 0x41, 0x00, 0xaa, 0x00, 0x55, 0x75, 0x9c
};
static const unsigned char GUID_SysKeyboard[16] = {
    0x61, 0x2b, 0x1d, 0x6f, 0xa0, 0xd5, 0xcf, 0x11,
    0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00
};
static const unsigned char GUID_SysMouse[16] = {
    0x60, 0x2b, 0x1d, 0x6f, 0xa0, 0xd5, 0xcf, 0x11,
    0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00
};

#define DDBLT_WAIT           0x01000000u
#define DDBLT_COLORFILL      0x00000400u
#define DDLOCK_WRITEONLY_WAIT 0x00000810u    /* DDLOCK_WAIT | DDLOCK_WRITEONLY */

static unsigned short rgb565(int r, int g, int b)
{
    return (unsigned short)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

int main(void)
{
    DDraw*     dd1 = 0;
    DDraw*     dd2 = 0;
    DDSurface* primary = 0;
    DDSurface* back = 0;
    DDClipper* clipper = 0;
    DInput*    di = 0;
    DIDevice*  kbd = 0;
    DIDevice*  mouse = 0;
    DDSurfaceDesc ddsd;
    DDSurfaceDesc lock;
    RgnData    rgn;
    unsigned char caps_drv[0x17c], caps_hel[0x17c];
    unsigned char keys[256];
    MouseState ms;
    int  depth, w = 640, h = 480;
    int  cx = 320, cy = 240;
    long frame = 0;
    long hr;

    printf("[shimtest] DirectDrawCreate\n");
    if (DirectDrawCreate(0, (void**)&dd1, 0) != 0) { printf("FAIL create\n"); return 1; }
    if (dd1->vtbl->QueryInterface(dd1, IID_IDirectDraw2, (void**)&dd2) != 0) {
        printf("FAIL QueryInterface(IID_IDirectDraw2)\n"); return 1;
    }
    *(unsigned long*)caps_drv = 0x17c;
    *(unsigned long*)caps_hel = 0x17c;
    if (dd2->vtbl->GetCaps(dd2, caps_drv, caps_hel) != 0) { printf("FAIL GetCaps\n"); return 1; }

    /* The window, the way InitScreen builds it. */
    {
        struct { unsigned int cbSize, style; void* proc; int a, b; void* inst;
                 void* i1; void* c1; void* bg; const char* menu;
                 const char* cls; void* i2; } wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.proc = 0;                       /* no wndproc: nothing to dispatch to */
        wc.inst = (void*)0x00400000;
        wc.cls = "LEGOLANDMAIN";
        RegisterClassExA(&wc);
        CreateWindowExA(8, "LEGOLANDMAIN", "LEGOLAND shim test", 0x90000000,
                        0, 0, w, h, GetDesktopWindow(), 0, (void*)0x00400000, 0);
    }
    if (dd2->vtbl->SetCooperativeLevel(dd2, ll_host_hwnd(), 0x11) != 0) {
        printf("FAIL SetCooperativeLevel\n"); return 1;
    }

    /* SetScreenDisplayMode, including the depth classification it performs. */
    if (dd2->vtbl->SetDisplayMode(dd2, (unsigned long)w, (unsigned long)h, 16, 0, 0) != 0) {
        printf("FAIL SetDisplayMode 16bpp\n"); return 1;
    }
    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    dd2->vtbl->GetDisplayMode(dd2, &ddsd);
    if (ddsd.ddpf.dwRGBBitCount != 16) { printf("FAIL bit count %lu\n", ddsd.ddpf.dwRGBBitCount); return 1; }
    depth = (ddsd.ddpf.dwGBitMask == 0x7e0) + 1;
    printf("[shimtest] mode %lux%lu %lubpp, g_screen_depth would be %d (2 = RGB565)\n",
           ddsd.dwWidth, ddsd.dwHeight, ddsd.ddpf.dwRGBBitCount, depth);
    if (depth != 2) { printf("FAIL expected depth 2\n"); return 1; }

    /* primary (caps 0x4200, no size) and back buffer (caps 0x800, w x h). */
    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = 1;
    ddsd.dwCaps = 0x4200;
    if (dd2->vtbl->CreateSurface(dd2, &ddsd, &primary, 0) != 0) { printf("FAIL primary\n"); return 1; }

    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = 7;
    ddsd.dwCaps = 0x800;
    ddsd.dwWidth = (unsigned long)w;
    ddsd.dwHeight = (unsigned long)h;
    if (dd2->vtbl->CreateSurface(dd2, &ddsd, &back, 0) != 0) { printf("FAIL back\n"); return 1; }

    /* The one-rectangle clip list InitScreen hands to the clipper. */
    dd2->vtbl->CreateClipper(dd2, 0, &clipper, 0);
    memset(&rgn, 0, sizeof(rgn));
    rgn.dwSize = 0x20; rgn.iType = 1; rgn.nCount = 1; rgn.nRgnSize = 0x10;
    rgn.rcBound.right = w; rgn.rcBound.bottom = h;
    rgn.rect = rgn.rcBound;
    if (clipper)
        clipper->vtbl->SetClipList(clipper, &rgn, 0);

    /* DirectInput, the way InitInputSystem does. */
    DirectInputCreateA((void*)0x00400000, 0x300, (void**)&di, 0);
    if (!di) { printf("FAIL DirectInputCreateA\n"); return 1; }
    if (di->lpVtbl->CreateDevice(di, GUID_SysKeyboard, &kbd, 0) != 0) { printf("FAIL keyboard\n"); return 1; }
    if (di->lpVtbl->CreateDevice(di, GUID_SysMouse, &mouse, 0) != 0) { printf("FAIL mouse\n"); return 1; }
    kbd->lpVtbl->SetCooperativeLevel(kbd, ll_host_hwnd(), 5);
    mouse->lpVtbl->SetCooperativeLevel(mouse, ll_host_hwnd(), 5);
    kbd->lpVtbl->Acquire(kbd);
    mouse->lpVtbl->Acquire(mouse);

    printf("[shimtest] all host objects built; entering the game-shaped loop\n");
    fflush(stdout);

    for (;;) {
        int x, y;

        /* --- the pump, the shape ProcessSystemEvents uses ----------------- */
        {
            struct { void* hwnd; unsigned int msg, wp; long lp;
                     unsigned long t; long px, py; } msg;
            while (PeekMessageA(&msg, ll_host_hwnd(), 0, 0, 0 /* PM_NOREMOVE */)) {
                if (!GetMessageA(&msg, ll_host_hwnd(), 0, 0))
                    goto done;
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
                SetCursor(0);
            }
        }

        /* --- the two devices, the shape ScanKeyboard / ScanMouse use ------- */
        while (kbd->lpVtbl->GetDeviceState(kbd, 256, keys) != 0)
            kbd->lpVtbl->Acquire(kbd);
        while (mouse->lpVtbl->GetDeviceState(mouse, 16, &ms) != 0)
            mouse->lpVtbl->Acquire(mouse);
        cx += ms.lX; cy += ms.lY;
        if (cx < 0) cx = 0; if (cx >= w) cx = w - 1;
        if (cy < 0) cy = 0; if (cy >= h) cy = h - 1;
        if (keys[DIK_ESCAPE] & 0x80)
            break;

        /* --- draw into the LOCKED back surface, as the game does ---------- */
        memset(&lock, 0, sizeof(lock));
        lock.dwSize = 0x6c;
        hr = back->vtbl->Lock(back, 0, &lock, DDLOCK_WRITEONLY_WAIT, 0);
        if (hr != 0) { printf("FAIL Lock %08lx\n", (unsigned long)hr); return 1; }
        for (y = 0; y < (int)lock.dwHeight; y++) {
            unsigned short* row = (unsigned short*)((char*)lock.lpSurface + (long)y * lock.lPitch);
            for (x = 0; x < (int)lock.dwWidth; x++)
                row[x] = rgb565(x * 255 / (int)lock.dwWidth,
                                y * 255 / (int)lock.dwHeight,
                                (int)((frame * 2) & 0xff));
        }
        /* key bars: space, then left/right/up */
        {
            static const int diks[4] = { DIK_SPACE, DIK_LEFT, DIK_RIGHT, DIK_UP };
            int i;
            for (i = 0; i < 4; i++) {
                if (!(keys[diks[i]] & 0x80))
                    continue;
                for (y = 8; y < 40; y++) {
                    unsigned short* row = (unsigned short*)((char*)lock.lpSurface + (long)y * lock.lPitch);
                    for (x = 8 + i * 40; x < 40 + i * 40; x++)
                        row[x] = rgb565(255, 255, 0);
                }
            }
            if (keys[DIK_DOWN] & 0x80) {
                for (y = 8; y < 40; y++) {
                    unsigned short* row = (unsigned short*)((char*)lock.lpSurface + (long)y * lock.lPitch);
                    for (x = 168; x < 200; x++)
                        row[x] = rgb565(0, 255, 255);
                }
            }
        }
        /* the cursor box, plus a red fill while a mouse button is down */
        for (y = cy - 5; y <= cy + 5; y++) {
            unsigned short* row;
            if (y < 0 || y >= (int)lock.dwHeight) continue;
            row = (unsigned short*)((char*)lock.lpSurface + (long)y * lock.lPitch);
            for (x = cx - 5; x <= cx + 5; x++) {
                if (x < 0 || x >= (int)lock.dwWidth) continue;
                row[x] = (ms.rgbButtons[0] & 0x80) ? rgb565(255, 0, 0) : rgb565(255, 255, 255);
            }
        }
        back->vtbl->Unlock(back, lock.lpSurface);

        /* --- present, the shape FlipPrimary uses -------------------------- */
        {
            WinRect dst;
            GetClientRect(ll_host_hwnd(), (LLRect*)&dst);
            dst.right = dst.left + w;
            dst.bottom = dst.top + h;
            hr = primary->vtbl->Blt(primary, &dst, back, 0, DDBLT_WAIT, 0);
            if (hr != 0) { printf("FAIL Blt %08lx\n", (unsigned long)hr); return 1; }
        }

        if ((++frame % 60) == 0) {
            printf("[shimtest] frame %ld, cursor %d,%d, timeGetTime %u\n",
                   frame, cx, cy, timeGetTime());
            fflush(stdout);
        }
    }
done:
    printf("[shimtest] stopped after %ld frames\n", frame);
    fflush(stdout);
    return 0;
}
