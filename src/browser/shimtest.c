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
#include <stdlib.h>
#include <string.h>

#include "ll_host.h"

/* portable/src/browser/ll_canvas.js -- the two counters a headless run reports.
 * Declared here rather than in ll_host.h because the native build of the shim
 * has no JS library and defines its own static no-ops for the ll_js_* family. */
extern int ll_js_frames(void);
extern int ll_js_checksum(void);

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

/* IDirectDrawSurface's two DC slots, the way text.c:41 declares them. The GDI
 * text pass below is text.c's Print / PrintCent shape exactly: GetDC(+0x44),
 * SetBkMode, SelectObject(region), SelectObject(font), TextOutA / DrawTextA,
 * restore in reverse, ReleaseDC(+0x68), DeleteObject(region). PORT-B2. */
typedef struct DCSlots {
    char pad00[0x44];
    long (*GetDC)(DDSurface*, void**);                                  /* 0x44 */
    char pad48[0x68 - 0x48];
    long (*ReleaseDC)(DDSurface*, void*);                               /* 0x68 */
} DCSlots;

/* LOGFONTA, the 0x3c-byte record screen.c:1183 declares. */
typedef struct TestLogFont {
    long          lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    unsigned char lfItalic, lfUnderline, lfStrikeOut, lfCharSet;
    unsigned char lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
    char          lfFaceName[32];
} TestLogFont;

static void* make_font(int height, int weight)
{
    TestLogFont lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = height;
    lf.lfWeight = weight;
    lf.lfCharSet = 1;
    lf.lfQuality = 2;
    strcpy(lf.lfFaceName, "Lego");
    return CreateFontIndirectA(&lf);
}

/* Draw the four fonts InitScreen creates, through the same host calls the game
 * uses, into the surface the DC comes from. If the page shows this text, GDI
 * text works; if it also shows the measured box around "DT_CALCRECT", DrawTextA
 * is measuring as well as drawing, which is what eleven of the game's call
 * sites depend on. */
static void gdi_text_pass(DDSurface* surf, void* const fonts[4], int frame)
{
    static const char* const kNames[4] = {
        "font 24/700 (SelectFont default)", "font 28/400 (SelectFont 3)",
        "font 20/700 (SelectFont 1)",       "font 18/600 (SelectFont 2)"
    };
    DCSlots* dcv = (DCSlots*)surf->vtbl;
    void* hdc = 0;
    void* rgn;
    void* oldrgn;
    void* oldfont;
    LLRect box;
    int    i, y = 150;

    /* The clip region every Print routine builds from g_clip_rect. */
    rgn = CreateRectRgn(8, 140, 632, 472);
    if (dcv->GetDC(surf, &hdc) != 0 || !hdc)
        return;
    SetBkMode(hdc, 1);                     /* TRANSPARENT, as PrintCent does */
    SetTextColor(hdc, 0x00ffffff);         /* COLORREF 0x00bbggrr */
    oldrgn = SelectObject(hdc, rgn);
    for (i = 0; i < 4; i++) {
        oldfont = SelectObject(hdc, fonts[i]);
        TextOutA(hdc, 16, y, kNames[i], (int)strlen(kNames[i]));
        SelectObject(hdc, oldfont);
        y += 30;
    }
    /* DrawTextA measuring, then drawing in the box it measured. */
    oldfont = SelectObject(hdc, fonts[2]);
    box.left = 16; box.top = y + 10; box.right = 320; box.bottom = y + 10;
    SetTextColor(hdc, 0x0000ffff);         /* yellow: b=0x00 g=0xff r=0xff */
    DrawTextA(hdc, "DT_CALCRECT then draw in the measured box",
              -1, &box, 0x410);            /* DT_CALCRECT | DT_WORDBREAK */
    DrawTextA(hdc, "DT_CALCRECT then draw in the measured box",
              -1, &box, 0x11);             /* DT_CENTER | DT_WORDBREAK */
    /* Opaque text, which is what Print and PrintCentOpaque ask for. */
    SetBkMode(hdc, 2);
    SetBkColor(hdc, 0x00400000);           /* dark blue in 0x00bbggrr */
    SetTextColor(hdc, 0x00ffffff);
    {
        char line[64];
        snprintf(line, sizeof(line), "OPAQUE background, frame %d", frame);
        TextOutA(hdc, 360, y + 10, line, (int)strlen(line));
    }
    /* Clipped: this starts inside the region and runs past its right edge. */
    TextOutA(hdc, 500, y + 50, "clipped at the region's right edge", 33);
    SelectObject(hdc, oldfont);
    SelectObject(hdc, oldrgn);
    dcv->ReleaseDC(surf, hdc);
    DeleteObject(rgn);
}

int main(int argc, char** argv)
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
    void* fonts[4];
    int  depth, w = 640, h = 480;
    int  cx = 320, cy = 240;
    long frame = 0;
    long max_frames = 0;        /* 0 = run until Escape / the tab closes */
    long hr;
    int  argi;

    /* --frames N stops after N presented frames and reports. That is what makes
     * this runnable under node as a regression test of the whole shim with no
     * DOM at all (PORT-B2 deliverable 2); in the browser, leaving it off gives
     * the old endless visual test. */
    for (argi = 1; argi < argc; argi++) {
        if (strcmp(argv[argi], "--frames") == 0 && argi + 1 < argc)
            max_frames = strtol(argv[++argi], 0, 10);
    }

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

    /* The four fonts InitScreen creates, in its order (screen.c:1357-1373). */
    fonts[0] = make_font(24, 700);
    fonts[1] = make_font(28, 400);
    fonts[2] = make_font(20, 700);
    fonts[3] = make_font(18, 600);
    printf("[shimtest] fonts %p %p %p %p\n",
           fonts[0], fonts[1], fonts[2], fonts[3]);

    /* MessageBoxA must ANSWER, not hang: MB_RETRYCANCEL has to come back
     * IDCANCEL (2) or RES_EnsureMounted's `while` never exits. */
    printf("[shimtest] MessageBoxA(MB_RETRYCANCEL) -> %d (2 = IDCANCEL, required)\n",
           MessageBoxA(ll_host_hwnd(), "shimtest: pretend the CD is missing",
                       "CD Missing", 0x50015));
    printf("[shimtest] MessageBoxA(MB_OK) -> %d (1 = IDOK)\n",
           MessageBoxA(ll_host_hwnd(), "shimtest: a plain notice",
                       "LEGOLAND", 0x30));

    /* SPI_GETMOUSE must fill its buffer: ResetController reads it uninitialised
     * otherwise and the game's own mouse acceleration runs on stack garbage. */
    {
        int sp[3];
        sp[0] = sp[1] = sp[2] = -12345;
        SystemParametersInfoA(3, 0, sp, 0);
        printf("[shimtest] SPI_GETMOUSE -> {%d, %d, %d} (must not be -12345)\n",
               sp[0], sp[1], sp[2]);
        if (sp[0] == -12345) printf("FAIL SPI_GETMOUSE did not fill\n");
    }

    printf("[shimtest] all host objects built; entering the game-shaped loop\n");
    fflush(stdout);

    for (;;) {
        int x, y;

        /* --- the pump, the shape ProcessSystemEvents uses ----------------- */
        {
            struct { void* hwnd; unsigned int msg, wp; long lp;
                     unsigned long t; long px, py; } msg;
            while (PeekMessageA((MSG*)&msg, ll_host_hwnd(), 0, 0, 0 /* PM_NOREMOVE */)) {
                if (!GetMessageA((MSG*)&msg, ll_host_hwnd(), 0, 0))
                    goto done;
                TranslateMessage((MSG*)&msg);
                DispatchMessageA((MSG*)&msg);
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

        /* --- GDI text, the shape text.c's Print routines use --------------
         * Outside the Lock/Unlock pair, because GDI needs the surface unlocked:
         * that is why every Print* brackets itself with
         * PushRenderingStatusAndUnlockVideoSurface / PopRenderingStatus
         * (surface.c). PORT-B2. */
        gdi_text_pass(back, fonts, (int)frame);

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
            printf("[shimtest] frame %ld, cursor %d,%d, timeGetTime %u,"
                   " presented %d\n",
                   frame, cx, cy, timeGetTime(), ll_host_frames_presented());
            fflush(stdout);
        }
        if (max_frames > 0 && frame >= max_frames)
            break;
    }
done:
    printf("[shimtest] stopped after %ld frames (%d presented)\n",
           frame, ll_host_frames_presented());
    printf("[shimtest] last MessageBox: %s (answer %d)\n",
           ll_host_last_messagebox(), ll_host_last_messagebox_answer());
    /* Under node this is the whole proof: frames were produced, the pixels
     * changed, and nothing in ll_canvas.js reached for a `document`. */
    printf("[shimtest] display frames %d, last frame checksum %08x\n",
           ll_js_frames(), (unsigned)ll_js_checksum());
    if (ll_js_frames() != ll_host_frames_presented())
        printf("FAIL present count %d != JS frame count %d\n",
               ll_host_frames_presented(), ll_js_frames());
    else if (ll_js_checksum() == 0 && ll_host_frames_presented() > 0)
        printf("FAIL checksum never changed from 0\n");
    else
        printf("[shimtest] PASS\n");
    fflush(stdout);
    return 0;
}
