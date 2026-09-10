/* LEGOLAND portable build -- the DirectDraw host shim (scope PORT-B).
 *
 * DirectDrawCreate hands the game C structs whose vtables have the EXACT slot
 * layout the recovered sources call through. The game never names a method: it
 * indexes a vtable at a byte offset recovered from the disassembly, so a slot
 * at the wrong offset is a call into the wrong function with the wrong
 * arguments. Every offset below is justified by a source line.
 *
 * IDirectDrawSurface (DirectX 1 layout, 36 slots, 0x00..0x8c):
 *   0x08 Release         gpu.c:118, screen.c:1231, sprite2.c:54, printlist.c:101
 *   0x0c AddAttachedSurface
 *   0x14 Blt             gpu.c:120, bigrender.c:180, sysmisc.c:513
 *   0x2c Flip            blitmisc.c:51
 *   0x44 GetDC           text.c:44, movie.c:82, frontend2.c:87, bubblecache.c:34,
 *                        fpui4.c:522, renderview.c:1619, unref5.c:101
 *   0x48 GetFlipStatus   blitmisc.c:53
 *   0x58 GetSurfaceDesc  bigrender.c:183
 *   0x60 IsLost          bigrender.c:185
 *   0x64 Lock            surface.c:106, bigrender.c:186, printlist.c:103
 *   0x68 ReleaseDC       text.c:46, movie.c:84, bubblecache.c:36, fpui4.c:524,
 *                        frontend2.c:89, renderview.c:1621, unref5.c:103
 *   0x6c Restore         surface.c:112, gpu.c:123, sysmisc.c:518, savechunks.c:192
 *   0x70 SetClipper      gpu.c:124, screen.c:1233
 *   0x74 SetColorKey     sprite2.c:56, bigrender.c:191, bubblecache.c:38
 *   0x7c SetPalette      spritemisc.c:37, rin.c:202
 *   0x80 Unlock          surface.c:114, gpu.c:126, blitmisc.c:60
 * Those are exactly the DirectX 1 IDirectDrawSurface offsets, so the whole
 * standard vtable is emitted and the slots the game never calls return
 * DDERR_UNSUPPORTED rather than being padding that could be jumped into.
 *
 * IDirectDraw / IDirectDraw2 (the game uses both; same slots, IDirectDraw2
 * only appends GetAvailableVidMem at 0x5c):
 *   0x00 QueryInterface  gpu.c:139 (asks for IID_IDirectDraw2)
 *   0x08 Release         gpu.c:141,148
 *   0x0c Compact         sprite2.c:84
 *   0x10 CreateClipper   screen.c:1249
 *   0x14 CreatePalette   rin.c:194
 *   0x18 CreateSurface   screen.c:1251, bigrender.c:200, sprite2.c:86
 *   0x2c GetCaps         gpu.c:150
 *   0x30 GetDisplayMode  sysmisc.c:280
 *   0x50 SetCooperativeLevel screen.c:1253
 *   0x54 SetDisplayMode  sysmisc.c:281
 *
 * IDirectDrawClipper:
 *   0x1c SetClipList     gpu.c:133, screen.c:1240
 *   0x20 SetHWnd         screen.c:1241
 *
 * Mode and pixel format. SetScreenDisplayMode (sysmisc.c 0x00463ef0) asks for
 * `g_screencfg->w x g_screencfg->h x 16` and then READS the format back with
 * GetDisplayMode, classifying it `g_screen_depth = (dwGBitMask == 0x7e0) + 1`.
 * InitScreen's fullscreen arm independently writes g_screen_depth = 2. The two
 * only agree if the host reports RGB565, so GetDisplayMode reports
 * R 0xf800 / G 0x07e0 / B 0x001f at 16 bpp and every surface is 565. (8 bpp is
 * the fallback SetDisplayMode asks for second; this shim never needs it, and
 * reporting 565 keeps the paletted path -- LoadColourTable, ResendPalette,
 * rin.c's CreatePalette -- entirely out of the picture, since all three are
 * guarded by g_screen_depth == 0.)
 *
 * Surfaces are malloc'd linear 16-bpp buffers with pitch = width * 2, which is
 * what Lock publishes in DDSURFACEDESC::lPitch (+0x10) and ::lpSurface (+0x24)
 * -- the two fields surface.c reads back as g_ddsd_pitch (0x006680ac) and
 * g_ddsd_bits (0x006680c0). Lock also fills dwWidth/dwHeight, which text.c
 * reads as g_ddsd_width / g_ddsd_height.
 *
 * Presenting. The fullscreen path is NOT a flip chain: InitScreen creates the
 * primary with no DDSCAPS_FLIP/BACKBUFFER and sets g_present = FlipPrimary
 * (sysmisc.c 0x004661d0), which Blts g_surface_78 onto the primary. So the
 * present path here is "a Blt whose destination is the primary surface pushes
 * the primary's pixels to the canvas". Flip (blitmisc.c's PresentFlip, the
 * .data default that InitScreen overwrites) is implemented too, and presents
 * the back buffer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ll_host.h"

/* ---- HRESULTs the game compares against --------------------------------- */
#define DD_OK                  0L
#define DDERR_GENERIC          ((long)0x80004005)
#define DDERR_UNSUPPORTED      ((long)0x80004001)
#define DDERR_INVALIDPARAMS    ((long)0x80070057)
#define DDERR_SURFACELOST      ((long)0x887601c2)

/* DDLOCK / DDBLT / DDSCAPS / DDCKEY bits the game passes. */
#define DDBLT_COLORFILL        0x00000400u
#define DDBLT_KEYSRC           0x00008000u
#define DDSCAPS_PRIMARYSURFACE 0x00000200u
#define DDSCAPS_FLIP           0x00000010u
#define DDCKEY_SRCBLT          0x00000008u

/* ---- DDSURFACEDESC, 0x6c bytes ------------------------------------------
 * The game's own views (gpu.c, surface.c, screen.c, sysmisc.c, bigrender.c)
 * between them pin dwSize 0x00, dwFlags 0x04, dwHeight 0x08, dwWidth 0x0c,
 * lPitch 0x10, lpSurface 0x24, ddpfPixelFormat 0x48 and ddsCaps.dwCaps 0x68.
 * This is the full DirectX 1 record, so the offsets are not a guess. */
typedef struct LLPixelFormat {
    unsigned long dwSize;            /* +0x00 */
    unsigned long dwFlags;           /* +0x04 */
    unsigned long dwFourCC;          /* +0x08 */
    unsigned long dwRGBBitCount;     /* +0x0c */
    unsigned long dwRBitMask;        /* +0x10 */
    unsigned long dwGBitMask;        /* +0x14 */
    unsigned long dwBBitMask;        /* +0x18 */
    unsigned long dwRGBAlphaBitMask; /* +0x1c */
} LLPixelFormat;

typedef struct LLColorKey {
    unsigned long dwColorSpaceLowValue;   /* +0x00 */
    unsigned long dwColorSpaceHighValue;  /* +0x04 */
} LLColorKey;

typedef struct LLSurfaceDesc {
    unsigned long dwSize;              /* +0x00 */
    unsigned long dwFlags;             /* +0x04 */
    unsigned long dwHeight;            /* +0x08 */
    unsigned long dwWidth;             /* +0x0c */
    long          lPitch;              /* +0x10 */
    unsigned long dwBackBufferCount;   /* +0x14 */
    unsigned long dwMipMapCount;       /* +0x18 */
    unsigned long dwAlphaBitDepth;     /* +0x1c */
    unsigned long dwReserved;          /* +0x20 */
    void*         lpSurface;           /* +0x24 */
    LLColorKey    ckDestOverlay;       /* +0x28 */
    LLColorKey    ckDestBlt;           /* +0x30 */
    LLColorKey    ckSrcOverlay;        /* +0x38 */
    LLColorKey    ckSrcBlt;            /* +0x40 */
    LLPixelFormat ddpf;                /* +0x48 */
    unsigned long dwCaps;              /* +0x68 ddsCaps.dwCaps */
} LLSurfaceDesc;                       /* 0x6c */

/* DDBLTFX: only dwSize (+0x00) and dwFillColor (+0x50) are used (gpu.c). */
typedef struct LLBltFx {
    unsigned long dwSize;
    char          pad04[0x50 - 0x04];
    unsigned long dwFillColor;
    char          pad54[0x64 - 0x54];
} LLBltFx;

/* RGNDATA with one rect (gpu.c:101, screen.c:1236). */
typedef struct LLRgnData {
    unsigned long dwSize;
    unsigned long iType;
    unsigned long nCount;
    unsigned long nRgnSize;
    LLRect        rcBound;
    LLRect        rect;
} LLRgnData;

/* ---- the objects -------------------------------------------------------- */
typedef struct LLClipper {
    const void* vtbl;
    int   refs;
    LLRect clip;
    int   has_clip;
    void* hwnd;
} LLClipper;

typedef struct LLSurface {
    const void*     vtbl;
    int             refs;
    int             w, h;
    int             pitch;           /* bytes */
    unsigned short* bits;
    unsigned long   caps;
    int             locked;
    int             is_primary;
    LLClipper*      clipper;
    int             ck_src_set;
    unsigned long   ck_src_low, ck_src_high;
    struct LLSurface* flip_target;   /* for Flip on a (never used) flip chain */
    struct LLSurface* next_live;     /* PORT-B6: the live-surface registry */
} LLSurface;

typedef struct LLDraw {
    const void* vtbl;       /* IDirectDraw or IDirectDraw2: same layout here */
    int         refs;
    int         is_v2;
} LLDraw;

typedef struct LLPalette {
    const void* vtbl;
    int         refs;
    unsigned char entries[256 * 4];
} LLPalette;

/* ---- display state ------------------------------------------------------ */
static int g_mode_w = 640;
static int g_mode_h = 480;
static int g_mode_bpp = 16;
static LLSurface* g_ll_primary;       /* the surface the canvas shows */
static int g_present_count;

void ll_host_display_size(int* w, int* h)
{
    if (w) *w = g_mode_w;
    if (h) *h = g_mode_h;
}

static void fill_pixel_format(LLPixelFormat* pf)
{
    memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(LLPixelFormat);
    pf->dwFlags = 0x40;              /* DDPF_RGB */
    pf->dwRGBBitCount = (unsigned long)g_mode_bpp;
    if (g_mode_bpp == 16) {
        /* RGB565: SetScreenDisplayMode keys g_screen_depth off dwGBitMask. */
        pf->dwRBitMask = 0xf800;
        pf->dwGBitMask = 0x07e0;
        pf->dwBBitMask = 0x001f;
    } else {
        pf->dwFlags = 0x20;          /* DDPF_PALETTEINDEXED8 */
    }
}

/* =========================================================================
 * IDirectDrawSurface
 * ========================================================================= */

static long ll_surf_QueryInterface(LLSurface* s, const void* iid, void** out)
{
    (void)iid;
    if (!out) return DDERR_INVALIDPARAMS;
    s->refs++;
    *out = s;
    return DD_OK;
}
/* PORT-B6: the live-surface registry, defined with new_surface below (which is
 * where its reason lives). Declared here because Release unlinks. */
static void ll_surface_unlink(LLSurface* s);

static long ll_surf_AddRef(LLSurface* s) { return ++s->refs; }

static long ll_surf_Release(LLSurface* s)
{
    if (!s) return 0;
    if (--s->refs > 0)
        return s->refs;
    if (g_ll_primary == s)
        g_ll_primary = 0;
    ll_surface_unlink(s);
    free(s->bits);
    free(s);
    return 0;
}

static long ll_surf_unsupported(LLSurface* s) { (void)s; return DDERR_UNSUPPORTED; }

/* Clamp a rect to a surface; returns 0 when nothing is left. */
static int clamp_rect(const LLSurface* s, LLRect* r)
{
    if (r->left < 0) r->left = 0;
    if (r->top < 0) r->top = 0;
    if (r->right > s->w) r->right = s->w;
    if (r->bottom > s->h) r->bottom = s->h;
    return r->right > r->left && r->bottom > r->top;
}

static unsigned short* row_of(LLSurface* s, int y)
{
    return (unsigned short*)((char*)s->bits + (long)y * s->pitch);
}

static void present_primary(void)
{
    if (!g_ll_primary || !g_ll_primary->bits)
        return;
    /* The FIRST present only: this is the line that says the game reached a
     * frame, and tracing every one would bury the rest of the trace. */
    if (g_present_count == 0)
        ll_host_trace("first present: %dx%d pitch %d",
                      g_ll_primary->w, g_ll_primary->h, g_ll_primary->pitch);
    g_present_count++;
    ll_host_present16(g_ll_primary->bits, g_ll_primary->w, g_ll_primary->h,
                      g_ll_primary->pitch);
}

/* Blt: colour fill, straight copy, source-colour-keyed copy, and a
 * nearest-neighbour stretch when the rects differ in size (bigrender.c's
 * scaled blit is the only caller that needs the stretch). Presents when the
 * destination is the primary surface -- that is how FlipPrimary gets a frame
 * on the canvas. */
static long ll_surf_Blt(LLSurface* dst, LLRect* dstrect, LLSurface* src,
                        LLRect* srcrect, unsigned long flags, LLBltFx* fx)
{
    LLRect d, s;
    int x, y, dw, dh, sw, sh;

    if (!dst || !dst->bits)
        return DDERR_INVALIDPARAMS;

    d = dstrect ? *dstrect : (LLRect){ 0, 0, dst->w, dst->h };
    if (!clamp_rect(dst, &d))
        return DD_OK;                    /* fully clipped: nothing to do */

    if (flags & DDBLT_COLORFILL) {
        unsigned short c = fx ? (unsigned short)fx->dwFillColor : 0;
        for (y = d.top; y < d.bottom; y++) {
            unsigned short* p = row_of(dst, y);
            for (x = d.left; x < d.right; x++)
                p[x] = c;
        }
        if (dst->is_primary)
            present_primary();
        return DD_OK;
    }

    if (!src || !src->bits)
        return DDERR_INVALIDPARAMS;
    s = srcrect ? *srcrect : (LLRect){ 0, 0, src->w, src->h };
    if (!clamp_rect(src, &s))
        return DD_OK;

    dw = d.right - d.left;  dh = d.bottom - d.top;
    sw = s.right - s.left;  sh = s.bottom - s.top;

    if (dw == sw && dh == sh) {
        if (flags & DDBLT_KEYSRC && src->ck_src_set) {
            for (y = 0; y < dh; y++) {
                unsigned short* sp = row_of(src, s.top + y) + s.left;
                unsigned short* dp = row_of(dst, d.top + y) + d.left;
                for (x = 0; x < dw; x++) {
                    unsigned long v = sp[x];
                    if (v < src->ck_src_low || v > src->ck_src_high)
                        dp[x] = (unsigned short)v;
                }
            }
        } else {
            for (y = 0; y < dh; y++)
                memcpy(row_of(dst, d.top + y) + d.left,
                       row_of(src, s.top + y) + s.left,
                       (size_t)dw * 2);
        }
    } else {
        for (y = 0; y < dh; y++) {
            unsigned short* sp = row_of(src, s.top + (int)((long)y * sh / dh));
            unsigned short* dp = row_of(dst, d.top + y) + d.left;
            if (flags & DDBLT_KEYSRC && src->ck_src_set) {
                for (x = 0; x < dw; x++) {
                    unsigned long v = sp[s.left + (int)((long)x * sw / dw)];
                    if (v < src->ck_src_low || v > src->ck_src_high)
                        dp[x] = (unsigned short)v;
                }
            } else {
                for (x = 0; x < dw; x++)
                    dp[x] = sp[s.left + (int)((long)x * sw / dw)];
            }
        }
    }

    if (dst->is_primary)
        present_primary();
    return DD_OK;
}

static long ll_surf_BltFast(LLSurface* dst, unsigned long x, unsigned long y,
                            LLSurface* src, LLRect* srcrect, unsigned long trans)
{
    LLRect d;
    LLRect s = srcrect ? *srcrect : (LLRect){ 0, 0, src ? src->w : 0, src ? src->h : 0 };
    d.left = (long)x;
    d.top = (long)y;
    d.right = d.left + (s.right - s.left);
    d.bottom = d.top + (s.bottom - s.top);
    return ll_surf_Blt(dst, &d, src, &s, (trans & 1) ? DDBLT_KEYSRC : 0, 0);
}

/* Flip: present the back buffer. The game's fullscreen path never builds a
 * flip chain (InitScreen passes no DDSCAPS_FLIP), so this is only reached if
 * g_present is left at blitmisc.c's PresentFlip. */
static long ll_surf_Flip(LLSurface* s, LLSurface* target, unsigned long flags)
{
    LLSurface* from = target ? target : s->flip_target;
    (void)flags;
    if (from && from->bits && s->bits && from->w == s->w && from->h == s->h) {
        int y;
        for (y = 0; y < s->h; y++)
            memcpy(row_of(s, y), row_of(from, y), (size_t)s->w * 2);
    }
    if (s->is_primary)
        present_primary();
    else
        ll_host_yield(0);
    return DD_OK;
}

/* PresentFlip spins on this until it reports the flip retired. Yield first so
 * the spin is one browser turn, then say DD_OK == done. */
static long ll_surf_GetFlipStatus(LLSurface* s, unsigned long flags)
{
    (void)s; (void)flags;
    ll_host_yield(0);
    return DD_OK;
}

/* GDI access. The shim has no GDI, so the "DC" is a cookie gdi32.c accepts and
 * draws nothing with: text and the AVI StretchDIBits are invisible for now.
 * Reporting DD_OK (rather than failing) is deliberate -- several callers
 * (text.c, frontend2.c, movie.c) do not check and would use an uninitialised
 * HDC local. */
static long ll_surf_GetDC(LLSurface* s, void** hdc)
{
    if (!hdc) return DDERR_INVALIDPARAMS;
    *hdc = (void*)s;
    /* A DC from GetDC is a FRESH DC: black text, white opaque background, no
     * clip region. text.c relies on that -- PrintLimitedText (0x00454c70) calls
     * SetTextColor and never restores it, and the next Print must still draw in
     * black. PORT-B2. */
    ll_host_dc_reset((void*)s);
    return DD_OK;
}
static long ll_surf_ReleaseDC(LLSurface* s, void* hdc) { (void)s; (void)hdc; return DD_OK; }

static long ll_surf_GetSurfaceDesc(LLSurface* s, LLSurfaceDesc* desc)
{
    if (!s || !desc) return DDERR_INVALIDPARAMS;
    desc->dwFlags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000;  /* CAPS|HEIGHT|WIDTH|PITCH|PIXELFORMAT */
    desc->dwHeight = (unsigned long)s->h;
    desc->dwWidth = (unsigned long)s->w;
    desc->lPitch = s->pitch;
    desc->dwCaps = s->caps;
    fill_pixel_format(&desc->ddpf);
    return DD_OK;
}

static long ll_surf_GetPixelFormat(LLSurface* s, LLPixelFormat* pf)
{
    (void)s;
    if (!pf) return DDERR_INVALIDPARAMS;
    fill_pixel_format(pf);
    return DD_OK;
}

static long ll_surf_IsLost(LLSurface* s) { (void)s; return DD_OK; }
static long ll_surf_Restore(LLSurface* s) { (void)s; return DD_OK; }

/* Lock publishes pitch and the pixel pointer into the caller's descriptor.
 * surface.c presets dwSize = 0x6c and reads lPitch (+0x10) and lpSurface
 * (+0x24) back through their own global names; text.c reads dwWidth/dwHeight.
 * dwSize is left alone. */
static long ll_surf_Lock(LLSurface* s, LLRect* rect, LLSurfaceDesc* desc,
                         unsigned long flags, void* event)
{
    (void)flags; (void)event;
    if (!s || !s->bits || !desc)
        return DDERR_INVALIDPARAMS;
    desc->dwFlags = 0x1 | 0x2 | 0x4 | 0x8 | 0x400 | 0x1000;
    desc->dwHeight = (unsigned long)s->h;
    desc->dwWidth = (unsigned long)s->w;
    desc->lPitch = s->pitch;
    desc->dwBackBufferCount = 0;
    desc->dwMipMapCount = 0;
    desc->dwAlphaBitDepth = 0;
    desc->dwReserved = 0;
    desc->dwCaps = s->caps;
    fill_pixel_format(&desc->ddpf);
    if (rect) {
        LLRect r = *rect;
        if (!clamp_rect(s, &r))
            return DDERR_INVALIDPARAMS;
        desc->lpSurface = (char*)row_of(s, (int)r.top) + r.left * 2;
        desc->dwWidth = (unsigned long)(r.right - r.left);
        desc->dwHeight = (unsigned long)(r.bottom - r.top);
    } else {
        desc->lpSurface = s->bits;
    }
    s->locked = 1;
    return DD_OK;
}

static long ll_surf_Unlock(LLSurface* s, void* bits)
{
    (void)bits;
    if (!s) return DDERR_INVALIDPARAMS;
    s->locked = 0;
    return DD_OK;
}

static long ll_surf_SetClipper(LLSurface* s, LLClipper* c)
{
    if (!s) return DDERR_INVALIDPARAMS;
    s->clipper = c;
    return DD_OK;
}

static long ll_surf_SetColorKey(LLSurface* s, unsigned long flags, LLColorKey* ck)
{
    if (!s) return DDERR_INVALIDPARAMS;
    if (!ck) {
        s->ck_src_set = 0;
        return DD_OK;
    }
    if (flags & DDCKEY_SRCBLT || flags == 0) {
        s->ck_src_low = ck->dwColorSpaceLowValue;
        s->ck_src_high = ck->dwColorSpaceHighValue;
        if (s->ck_src_high < s->ck_src_low)
            s->ck_src_high = s->ck_src_low;
        s->ck_src_set = 1;
    }
    return DD_OK;
}

static long ll_surf_SetPalette(LLSurface* s, void* pal) { (void)s; (void)pal; return DD_OK; }

/* The whole DirectX 1 IDirectDrawSurface vtable, in slot order. Slots the game
 * never calls point at ll_surf_unsupported so a mis-recovered offset surfaces
 * as DDERR_UNSUPPORTED instead of executing something arbitrary. */
typedef long (*ll_slot)(void);
static const ll_slot g_surface_vtbl[] = {
    (ll_slot)ll_surf_QueryInterface,    /* 0x00 */
    (ll_slot)ll_surf_AddRef,            /* 0x04 */
    (ll_slot)ll_surf_Release,           /* 0x08 */
    (ll_slot)ll_surf_unsupported,       /* 0x0c AddAttachedSurface */
    (ll_slot)ll_surf_unsupported,       /* 0x10 AddOverlayDirtyRect */
    (ll_slot)ll_surf_Blt,               /* 0x14 */
    (ll_slot)ll_surf_unsupported,       /* 0x18 BltBatch */
    (ll_slot)ll_surf_BltFast,           /* 0x1c */
    (ll_slot)ll_surf_unsupported,       /* 0x20 DeleteAttachedSurface */
    (ll_slot)ll_surf_unsupported,       /* 0x24 EnumAttachedSurfaces */
    (ll_slot)ll_surf_unsupported,       /* 0x28 EnumOverlayZOrders */
    (ll_slot)ll_surf_Flip,              /* 0x2c */
    (ll_slot)ll_surf_unsupported,       /* 0x30 GetAttachedSurface */
    (ll_slot)ll_surf_IsLost,            /* 0x34 GetBltStatus -> DD_OK */
    (ll_slot)ll_surf_unsupported,       /* 0x38 GetCaps */
    (ll_slot)ll_surf_unsupported,       /* 0x3c GetClipper */
    (ll_slot)ll_surf_unsupported,       /* 0x40 GetColorKey */
    (ll_slot)ll_surf_GetDC,             /* 0x44 */
    (ll_slot)ll_surf_GetFlipStatus,     /* 0x48 */
    (ll_slot)ll_surf_unsupported,       /* 0x4c GetOverlayPosition */
    (ll_slot)ll_surf_unsupported,       /* 0x50 GetPalette */
    (ll_slot)ll_surf_GetPixelFormat,    /* 0x54 */
    (ll_slot)ll_surf_GetSurfaceDesc,    /* 0x58 */
    (ll_slot)ll_surf_unsupported,       /* 0x5c Initialize */
    (ll_slot)ll_surf_IsLost,            /* 0x60 */
    (ll_slot)ll_surf_Lock,              /* 0x64 */
    (ll_slot)ll_surf_ReleaseDC,         /* 0x68 */
    (ll_slot)ll_surf_Restore,           /* 0x6c */
    (ll_slot)ll_surf_SetClipper,        /* 0x70 */
    (ll_slot)ll_surf_SetColorKey,       /* 0x74 */
    (ll_slot)ll_surf_unsupported,       /* 0x78 SetOverlayPosition */
    (ll_slot)ll_surf_SetPalette,        /* 0x7c */
    (ll_slot)ll_surf_Unlock,            /* 0x80 */
    (ll_slot)ll_surf_unsupported,       /* 0x84 UpdateOverlay */
    (ll_slot)ll_surf_unsupported,       /* 0x88 UpdateOverlayDisplay */
    (ll_slot)ll_surf_unsupported        /* 0x8c UpdateOverlayZOrder */
};

/* ---- the live-surface registry (PORT-B6) --------------------------------
 *
 * `ll_host_surface_pixels` is handed an HDC and has to answer "is this one of
 * mine, and where are its pixels". It used to answer by casting the handle to
 * an LLSurface* and reading `s->vtbl` -- which is only safe if every HDC in the
 * program is either null or a real surface pointer, and it is not:
 *
 *     fpui2.c 0x00470... HTBubbleHelp
 *         dc = CreateCompatibleDC(0);          <- gdi32.c: a COOKIE, 0x4c47xxxx
 *         SetBkMode(dc, 1);
 *         oldfont = SelectFont(dc, font);
 *         h = DrawTextA(dc, text, strlen(text), &rc, 0x410);   /. DT_CALCRECT ./
 *
 * A GDI object handle in this shim is `0x4c470000 | class << 12 | slot`, i.e.
 * about 1.28 GB, and the wasm heap is 256 MB -- so reading `((LLSurface*)dc)
 * ->vtbl` was a hard `RuntimeError: memory access out of bounds`, in
 * `ll_host_dc_target`, from `DrawTextA`, from `HTBubbleHelp`, from
 * `ProcessFrontEndHelp`, from `GameFrame`. `DrawTextA` was already written to
 * cope with "no target" (a DT_CALCRECT measure needs no pixels); it never got
 * the chance to.
 *
 * Why it had never fired: `ProcessFrontEndHelp` only runs when an icon is under
 * the cursor, and no front-end icon could ever be focussed while the mouse-hit
 * record at 0x004bdd00 was split into three objects (docs/lanes/scope-port-b6.md
 * §3 B1). The two defects hid each other.
 *
 * So the registry: every surface this file makes is linked in and unlinked on
 * the last Release, and the lookup is a walk of that list. No unknown pointer is
 * ever dereferenced. The list is short -- the primary, the back buffer and the
 * handful of offscreen surfaces the text cache and the bubble help hold -- and
 * the lookup happens once per DrawTextA/TextOutA, not per pixel. */
static LLSurface* g_ll_surfaces;          /* singly linked through ->next_live */

static int ll_surface_live(const LLSurface* s)
{
    const LLSurface* p;
    for (p = g_ll_surfaces; p; p = p->next_live)
        if (p == s)
            return 1;
    return 0;
}

static void ll_surface_unlink(LLSurface* s)
{
    LLSurface** link = &g_ll_surfaces;
    while (*link) {
        if (*link == s) { *link = s->next_live; return; }
        link = &(*link)->next_live;
    }
}

static LLSurface* new_surface(int w, int h, unsigned long caps)
{
    LLSurface* s;
    if (w <= 0 || h <= 0)
        return 0;
    s = (LLSurface*)calloc(1, sizeof(LLSurface));
    if (!s)
        return 0;
    s->vtbl = g_surface_vtbl;
    s->refs = 1;
    s->w = w;
    s->h = h;
    s->pitch = w * 2;                       /* 16 bpp, tightly packed */
    s->caps = caps;
    s->bits = (unsigned short*)calloc((size_t)h, (size_t)s->pitch);
    if (!s->bits) {
        free(s);
        return 0;
    }
    s->next_live = g_ll_surfaces;
    g_ll_surfaces = s;
    return s;
}

/* =========================================================================
 * IDirectDrawClipper -- the clip list is recorded and otherwise ignored: every
 * caller (RenderBlock in gpu.c) has already intersected the rect it passes
 * with g_clip_rect, and Blt clamps to the surface anyway.
 * ========================================================================= */

static long ll_clip_QueryInterface(LLClipper* c, const void* iid, void** out)
{ (void)iid; if (!out) return DDERR_INVALIDPARAMS; c->refs++; *out = c; return DD_OK; }
static long ll_clip_AddRef(LLClipper* c) { return ++c->refs; }
static long ll_clip_Release(LLClipper* c)
{ if (!c) return 0; if (--c->refs > 0) return c->refs; free(c); return 0; }
static long ll_clip_unsupported(LLClipper* c) { (void)c; return DDERR_UNSUPPORTED; }

static long ll_clip_SetClipList(LLClipper* c, LLRgnData* rgn, unsigned long flags)
{
    (void)flags;
    if (!c) return DDERR_INVALIDPARAMS;
    if (rgn && rgn->nCount >= 1) {
        c->clip = rgn->rect;
        c->has_clip = 1;
    } else {
        c->has_clip = 0;
    }
    return DD_OK;
}

static long ll_clip_SetHWnd(LLClipper* c, unsigned long flags, void* hwnd)
{
    (void)flags;
    if (!c) return DDERR_INVALIDPARAMS;
    c->hwnd = hwnd;
    c->has_clip = 0;
    return DD_OK;
}

static const ll_slot g_clipper_vtbl[] = {
    (ll_slot)ll_clip_QueryInterface,   /* 0x00 */
    (ll_slot)ll_clip_AddRef,           /* 0x04 */
    (ll_slot)ll_clip_Release,          /* 0x08 */
    (ll_slot)ll_clip_unsupported,      /* 0x0c GetClipList */
    (ll_slot)ll_clip_unsupported,      /* 0x10 GetHWnd */
    (ll_slot)ll_clip_unsupported,      /* 0x14 Initialize */
    (ll_slot)ll_clip_unsupported,      /* 0x18 IsClipListChanged */
    (ll_slot)ll_clip_SetClipList,      /* 0x1c */
    (ll_slot)ll_clip_SetHWnd           /* 0x20 */
};

/* =========================================================================
 * IDirectDrawPalette -- only reached in 8-bpp mode (rin.c), which the 565
 * report above keeps the game out of. Present so nothing traps.
 * ========================================================================= */

static long ll_pal_QueryInterface(LLPalette* p, const void* iid, void** out)
{ (void)iid; if (!out) return DDERR_INVALIDPARAMS; p->refs++; *out = p; return DD_OK; }
static long ll_pal_AddRef(LLPalette* p) { return ++p->refs; }
static long ll_pal_Release(LLPalette* p)
{ if (!p) return 0; if (--p->refs > 0) return p->refs; free(p); return 0; }
static long ll_pal_GetCaps(LLPalette* p, unsigned long* caps)
{ (void)p; if (caps) *caps = 0x804; return DD_OK; }   /* 8BIT | ALLOW256 */
static long ll_pal_GetEntries(LLPalette* p, unsigned long flags,
                              unsigned long base, unsigned long count, void* out)
{
    (void)flags;
    if (!p || !out || base + count > 256) return DDERR_INVALIDPARAMS;
    memcpy(out, p->entries + base * 4, (size_t)count * 4);
    return DD_OK;
}
static long ll_pal_Initialize(LLPalette* p) { (void)p; return DDERR_UNSUPPORTED; }
static long ll_pal_SetEntries(LLPalette* p, unsigned long flags,
                              unsigned long base, unsigned long count, const void* in)
{
    (void)flags;
    if (!p || !in || base + count > 256) return DDERR_INVALIDPARAMS;
    memcpy(p->entries + base * 4, in, (size_t)count * 4);
    return DD_OK;
}
static const ll_slot g_palette_vtbl[] = {
    (ll_slot)ll_pal_QueryInterface,    /* 0x00 */
    (ll_slot)ll_pal_AddRef,            /* 0x04 */
    (ll_slot)ll_pal_Release,           /* 0x08 */
    (ll_slot)ll_pal_GetCaps,           /* 0x0c */
    (ll_slot)ll_pal_GetEntries,        /* 0x10 */
    (ll_slot)ll_pal_Initialize,        /* 0x14 */
    (ll_slot)ll_pal_SetEntries         /* 0x18 */
};

/* =========================================================================
 * IDirectDraw / IDirectDraw2
 * ========================================================================= */

static LLDraw g_ddraw_v1;
static LLDraw g_ddraw_v2;
extern const ll_slot g_draw_vtbl[];

/* QueryInterface: the only GUID the game asks for is IID_IDirectDraw2
 * (gpu.c:421). Any GUID gets the v2 object -- the vtable is the same shape and
 * the extra slot (GetAvailableVidMem 0x5c) is never called. */
static long ll_draw_QueryInterface(LLDraw* d, const void* iid, void** out)
{
    (void)d; (void)iid;
    if (!out) return DDERR_INVALIDPARAMS;
    g_ddraw_v2.refs++;
    *out = &g_ddraw_v2;
    return DD_OK;
}
static long ll_draw_AddRef(LLDraw* d) { return ++d->refs; }
static long ll_draw_Release(LLDraw* d) { if (d->refs > 0) d->refs--; return d->refs; }
static long ll_draw_unsupported(LLDraw* d) { (void)d; return DDERR_UNSUPPORTED; }

/* sprite2.c's RecreateSprite calls Compact between a failed video-memory
 * CreateSurface and its retry. Nothing to compact; success keeps the retry. */
static long ll_draw_Compact(LLDraw* d) { (void)d; return DD_OK; }

static long ll_draw_CreateClipper(LLDraw* d, unsigned long flags,
                                  LLClipper** out, void* outer)
{
    LLClipper* c;
    (void)d; (void)flags; (void)outer;
    if (!out) return DDERR_INVALIDPARAMS;
    c = (LLClipper*)calloc(1, sizeof(LLClipper));
    if (!c) return DDERR_GENERIC;
    c->vtbl = g_clipper_vtbl;
    c->refs = 1;
    *out = c;
    return DD_OK;
}

static long ll_draw_CreatePalette(LLDraw* d, unsigned long flags,
                                  const void* entries, LLPalette** out, void* outer)
{
    LLPalette* p;
    (void)d; (void)flags; (void)outer;
    if (!out) return DDERR_INVALIDPARAMS;
    p = (LLPalette*)calloc(1, sizeof(LLPalette));
    if (!p) return DDERR_GENERIC;
    p->vtbl = g_palette_vtbl;
    p->refs = 1;
    if (entries)
        memcpy(p->entries, entries, sizeof(p->entries));
    *out = p;
    return DD_OK;
}

/* CreateSurface. Three shapes reach it:
 *   primary  dwFlags 1 (CAPS only), caps 0x4200 / 0x200 -- no size, so the
 *            surface is the display mode's size (screen.c:1397, :1462)
 *   offscreen dwFlags 7 (CAPS|HEIGHT|WIDTH), caps 0x800 / 0x4000 / 0x40 /
 *            0x840 (screen.c:1408/1417/1469/1481, sprite2.c:592/601,
 *            bigrender.c:520)
 * The caps bits are recorded but not honoured: there is no video memory to run
 * out of, so every request succeeds in malloc'd system memory. */
static long ll_draw_CreateSurface(LLDraw* d, LLSurfaceDesc* desc,
                                  LLSurface** out, void* outer)
{
    LLSurface* s;
    int w, h;
    (void)d; (void)outer;
    if (!desc || !out)
        return DDERR_INVALIDPARAMS;

    if (desc->dwCaps & DDSCAPS_PRIMARYSURFACE) {
        w = g_mode_w;
        h = g_mode_h;
    } else {
        w = (int)desc->dwWidth;
        h = (int)desc->dwHeight;
        if (w <= 0 || h <= 0)
            return DDERR_INVALIDPARAMS;
    }

    s = new_surface(w, h, desc->dwCaps);
    if (!s)
        return DDERR_GENERIC;
    ll_host_trace("CreateSurface %dx%d caps 0x%lx%s", w, h,
                  (unsigned long)desc->dwCaps,
                  (desc->dwCaps & DDSCAPS_PRIMARYSURFACE) ? " PRIMARY" : "");

    if (desc->dwCaps & DDSCAPS_PRIMARYSURFACE) {
        s->is_primary = 1;
        g_ll_primary = s;
        ll_host_display_open(g_mode_w, g_mode_h);
        /* A flip chain would need a back buffer here; the game never asks for
         * one (no DDSCAPS_FLIP / dwBackBufferCount anywhere), so Flip falls
         * back to flip_target == 0 and just presents. */
    }
    *out = s;
    return DD_OK;
}

/* GetCaps: InitHostSystemGPU presets dwSize = 0x17c on both blocks and only
 * checks the HRESULT (gpu.c:428). Report success with the caps left zeroed --
 * nothing in the game reads a cap bit back. */
static long ll_draw_GetCaps(LLDraw* d, void* driver, void* hel)
{
    (void)d;
    if (driver) { unsigned long sz = *(unsigned long*)driver;
                  memset(driver, 0, sz ? sz : 0x17c);
                  *(unsigned long*)driver = sz ? sz : 0x17c; }
    if (hel)    { unsigned long sz = *(unsigned long*)hel;
                  memset(hel, 0, sz ? sz : 0x17c);
                  *(unsigned long*)hel = sz ? sz : 0x17c; }
    return DD_OK;
}

/* GetDisplayMode: the format SetScreenDisplayMode classifies. RGB565 at 16 bpp
 * is what makes g_screen_depth come out 2, matching InitScreen's own write. */
static long ll_draw_GetDisplayMode(LLDraw* d, LLSurfaceDesc* desc)
{
    (void)d;
    if (!desc) return DDERR_INVALIDPARAMS;
    desc->dwFlags = 0x2 | 0x4 | 0x8 | 0x1000;
    desc->dwHeight = (unsigned long)g_mode_h;
    desc->dwWidth = (unsigned long)g_mode_w;
    desc->lPitch = g_mode_w * (g_mode_bpp / 8);
    fill_pixel_format(&desc->ddpf);
    return DD_OK;
}

static long ll_draw_GetMonitorFrequency(LLDraw* d, unsigned long* hz)
{ (void)d; if (hz) *hz = 60; return DD_OK; }

static long ll_draw_SetCooperativeLevel(LLDraw* d, void* hwnd, unsigned long flags)
{ (void)d; (void)hwnd; (void)flags; return DD_OK; }

static long ll_draw_RestoreDisplayMode(LLDraw* d) { (void)d; return DD_OK; }

/* SetDisplayMode decides the canvas geometry. The game asks for 16 bpp first
 * and 8 bpp only if that fails, so 16 always wins here. */
static long ll_draw_SetDisplayMode(LLDraw* d, unsigned long w, unsigned long h,
                                   unsigned long bpp, unsigned long refresh,
                                   unsigned long flags)
{
    (void)d; (void)refresh; (void)flags;
    if (bpp != 16)
        return DDERR_UNSUPPORTED;      /* refuse 8 bpp: the game prefers 16 */
    if (w == 0 || h == 0 || w > 4096 || h > 4096)
        return DDERR_INVALIDPARAMS;
    g_mode_w = (int)w;
    g_mode_h = (int)h;
    g_mode_bpp = 16;
    ll_host_trace("SetDisplayMode %lux%lu 16bpp (RGB565)", w, h);
    ll_host_display_open(g_mode_w, g_mode_h);
    return DD_OK;
}

static long ll_draw_WaitForVerticalBlank(LLDraw* d, unsigned long flags, void* ev)
{ (void)d; (void)flags; (void)ev; ll_host_yield(0); return DD_OK; }

const ll_slot g_draw_vtbl[] = {
    (ll_slot)ll_draw_QueryInterface,       /* 0x00 */
    (ll_slot)ll_draw_AddRef,               /* 0x04 */
    (ll_slot)ll_draw_Release,              /* 0x08 */
    (ll_slot)ll_draw_Compact,              /* 0x0c */
    (ll_slot)ll_draw_CreateClipper,        /* 0x10 */
    (ll_slot)ll_draw_CreatePalette,        /* 0x14 */
    (ll_slot)ll_draw_CreateSurface,        /* 0x18 */
    (ll_slot)ll_draw_unsupported,          /* 0x1c DuplicateSurface */
    (ll_slot)ll_draw_unsupported,          /* 0x20 EnumDisplayModes */
    (ll_slot)ll_draw_unsupported,          /* 0x24 EnumSurfaces */
    (ll_slot)ll_draw_unsupported,          /* 0x28 FlipToGDISurface */
    (ll_slot)ll_draw_GetCaps,              /* 0x2c */
    (ll_slot)ll_draw_GetDisplayMode,       /* 0x30 */
    (ll_slot)ll_draw_unsupported,          /* 0x34 GetFourCCCodes */
    (ll_slot)ll_draw_unsupported,          /* 0x38 GetGDISurface */
    (ll_slot)ll_draw_GetMonitorFrequency,  /* 0x3c */
    (ll_slot)ll_draw_unsupported,          /* 0x40 GetScanLine */
    (ll_slot)ll_draw_unsupported,          /* 0x44 GetVerticalBlankStatus */
    (ll_slot)ll_draw_unsupported,          /* 0x48 Initialize */
    (ll_slot)ll_draw_RestoreDisplayMode,   /* 0x4c */
    (ll_slot)ll_draw_SetCooperativeLevel,  /* 0x50 */
    (ll_slot)ll_draw_SetDisplayMode,       /* 0x54 */
    (ll_slot)ll_draw_WaitForVerticalBlank, /* 0x58 */
    (ll_slot)ll_draw_unsupported           /* 0x5c GetAvailableVidMem (v2 only) */
};

/* The one exported DDRAW entry (reached through the game's jump thunk at
 * 0x0049d314). gpu.c's InitHostSystemGPU checks only `== 0`. */
long DirectDrawCreate(void* guid, void** out, void* outer)
{
    (void)guid; (void)outer;
    if (!out)
        return DDERR_INVALIDPARAMS;
    ll_host_trace("DirectDrawCreate");
    g_ddraw_v1.vtbl = g_draw_vtbl;
    g_ddraw_v1.refs++;
    g_ddraw_v1.is_v2 = 0;
    g_ddraw_v2.vtbl = g_draw_vtbl;
    g_ddraw_v2.is_v2 = 1;
    *out = &g_ddraw_v1;
    return DD_OK;
}

/* For the page's status line / the headless harness. */
int ll_host_frames_presented(void) { return g_present_count; }

/* ---- the surface behind an HDC (PORT-B2) -------------------------------- */
/* GetDC hands out the LLSurface pointer itself, so gdi32.c can draw straight
 * into the surface's 16-bpp buffer -- which is what makes GDI text visible
 * instead of a no-op. The vtable pointer is the identity check: a cookie from
 * CreateCompatibleDC or CreateDCA (gdi32.c's printer/memory DCs) is not a
 * surface and must be rejected, not dereferenced.
 *
 * Lock state is deliberately not consulted. text.c unlocks before GetDC
 * (PushRenderingStatusAndUnlockVideoSurface, because real GDI needs an unlocked
 * surface) and InfoPrintCent assumes its caller already did, but in this shim
 * the pixel buffer is a plain malloc'd block that is valid either way. */
int ll_host_surface_pixels(void* hdc, unsigned short** bits,
                           int* w, int* h, int* pitch)
{
    LLSurface* s = (LLSurface*)hdc;
    /* PORT-B6: membership FIRST. `hdc` may be a gdi32.c object cookie
     * (0x4c47xxxx, far outside the heap) from CreateCompatibleDC, and reading
     * s->vtbl to find that out is the out-of-bounds trap itself. See the
     * registry's comment above new_surface. */
    if (!s || !ll_surface_live(s) || !s->bits)
        return 0;
    if (bits)  *bits = s->bits;
    if (w)     *w = s->w;
    if (h)     *h = s->h;
    if (pitch) *pitch = s->pitch;
    return 1;
}
