/* LEGOLAND portable build -- the GDI32 host shim (scopes PORT-B, PORT-B2).
 *
 * There is no GDI in a browser, but the game's ONLY text is GDI text, so this
 * file is not allowed to be a pile of no-ops. text.c's own header says it:
 * "the game does not rasterise text itself: every Print* routine borrows a GDI
 * DC from the DirectDraw draw surface (IDirectDrawSurface::GetDC, vtable +0x44)
 * and lets GDI draw into it, then hands it back". PORT-B left the drawing half
 * as a no-op and recorded the consequence -- all GDI text invisible -- as a
 * known divergence. PORT-B2 closes it.
 *
 * What that takes, and why each part is needed:
 *
 *   1. A FACE. portable/src/hostwin/ll_font.c: a 6x7 bitmap face scaled to the
 *      requested lfHeight, plus the DrawText layout engine. Its header explains
 *      the metrics and why they are the metrics.
 *   2. DC STATE. SetTextColor / SetBkColor / SetBkMode / SetTextAlign have to be
 *      remembered per DC, because the Print routines set them and the drawing
 *      call reads them back. Reporting "was 0" and forgetting, as before, meant
 *      PrintCentColref could not draw in its own colour.
 *   3. OBJECT IDENTITY. SelectObject has to know what it was handed. text.c does
 *      SelectObject(hdc, rgn) then SelectFont then SelectObject(hdc, oldfont)
 *      then SelectObject(hdc, oldrgn) -- four calls with two object classes,
 *      restored in reverse, and the font it selects decides the glyph size while
 *      the region it selects decides the clip box. A single opaque cookie cannot
 *      express that, so objects live in a table and their handles encode a class
 *      and a slot.
 *   4. THE CLIP REGION IS REAL. Every Print routine builds
 *      CreateRectRgn(g_clip_rect) and selects it. Ignoring it would let a label
 *      draw over the whole screen instead of inside the panel the game intends.
 *
 * Still deliberately not real, with the consequence recorded:
 *   - StretchDIBits: returns 0 scan lines. The AVI intro frames do not appear;
 *     there is no Indeo 5 decoder to produce them and -nointro skips them.
 *   - printing (StartDocA/StartPage/EndPage/EndDoc): reports failure, so the
 *     print path aborts at its first check rather than walking a half-built
 *     document. certificate.c's TextOutA calls therefore never run.
 *   - CreateCompatibleDC / CreateDCA: a cookie with no pixels behind it.
 *     ll_host_dc_target reports no target for those, and drawing into one is a
 *     no-op that reports success -- the old behaviour, for the paths that are
 *     not on screen anyway.
 */
#include <stdio.h>
#include <string.h>

#include "ll_host.h"

/* ---- object table ------------------------------------------------------- */
/* Handle shape: 0x4c47 'LG' | class << 12 | slot. The class is readable in a
 * debugger and a stray pointer cannot be mistaken for a handle. */
#define LL_OBJ_FONT   1
#define LL_OBJ_DC     2
#define LL_OBJ_BITMAP 3
#define LL_OBJ_PEN    4
#define LL_OBJ_BRUSH  5
#define LL_OBJ_RGN    6
#define LL_OBJ_STOCK  7

#define LL_OBJ_MAX    256
#define LL_OBJ_TAG    0x4c470000u

typedef struct LLObj {
    int           used;
    int           cls;
    LLFontMetrics font;        /* LL_OBJ_FONT */
    unsigned long colour;      /* LL_OBJ_PEN, LL_OBJ_BRUSH */
    LLRect        rc;          /* LL_OBJ_RGN */
} LLObj;

static LLObj g_objs[LL_OBJ_MAX];

static void* obj_new(int cls, LLObj** out)
{
    int i;
    /* Slot 0 is never handed out, so a zero handle stays "no object". Regions
     * are created and DeleteObject'd once per Print call, so slots recycle. */
    for (i = 1; i < LL_OBJ_MAX; i++) {
        if (!g_objs[i].used) {
            memset(&g_objs[i], 0, sizeof(g_objs[i]));
            g_objs[i].used = 1;
            g_objs[i].cls = cls;
            if (out)
                *out = &g_objs[i];
            return (void*)(size_t)(LL_OBJ_TAG | ((unsigned)cls << 12) |
                                   (unsigned)i);
        }
    }
    /* Table full: hand back a classed handle with no slot rather than null, so
     * the caller's DeleteObject and SelectObject still behave. A leak here can
     * only come from a caller that creates objects without deleting them. */
    if (out)
        *out = 0;
    return (void*)(size_t)(LL_OBJ_TAG | ((unsigned)cls << 12));
}

static LLObj* obj_get(void* h, int cls)
{
    unsigned long v = (unsigned long)(size_t)h;
    unsigned slot;
    if ((v & 0xffff0000u) != LL_OBJ_TAG)
        return 0;
    if (((v >> 12) & 0xf) != (unsigned)cls)
        return 0;
    slot = v & 0xfff;
    if (slot == 0 || slot >= LL_OBJ_MAX || !g_objs[slot].used)
        return 0;
    return &g_objs[slot];
}

static int obj_class(void* h)
{
    unsigned long v = (unsigned long)(size_t)h;
    if ((v & 0xffff0000u) != LL_OBJ_TAG)
        return 0;
    return (int)((v >> 12) & 0xf);
}

/* ---- DC state ----------------------------------------------------------- */
/* A handful of DCs is plenty: the game holds one surface DC at a time (GetDC /
 * ReleaseDC bracket every Print) and at most a memory DC and a printer DC
 * besides. The table is keyed by handle and the oldest entry is recycled. */
#define LL_DC_MAX 8

typedef struct LLDC {
    void*         h;
    unsigned long seq;
    unsigned long fg;          /* COLORREF */
    unsigned long bg;          /* COLORREF */
    int           bk_mode;     /* 1 TRANSPARENT, 2 OPAQUE */
    unsigned int  align;
    void*         font;
    void*         rgn;
    void*         pen;
    void*         brush;
    int           pen_x, pen_y;
} LLDC;

static LLDC          g_dcs[LL_DC_MAX];
static unsigned long g_dc_seq;

/* GDI's defaults for a fresh DC: black text, white background, OPAQUE, no clip
 * region, the system font. */
static void dc_defaults(LLDC* dc)
{
    dc->fg = 0x00000000ul;
    dc->bg = 0x00fffffful;
    dc->bk_mode = 2;
    dc->align = 0;
    dc->font = 0;
    dc->rgn = 0;
    dc->pen = 0;
    dc->brush = 0;
    dc->pen_x = 0;
    dc->pen_y = 0;
}

static LLDC* dc_find(void* h)
{
    int i;
    for (i = 0; i < LL_DC_MAX; i++)
        if (g_dcs[i].h == h)
            return &g_dcs[i];
    return 0;
}

static LLDC* dc_of(void* h)
{
    LLDC* dc = dc_find(h);
    int i, oldest = 0;
    if (dc) {
        dc->seq = ++g_dc_seq;
        return dc;
    }
    for (i = 0; i < LL_DC_MAX; i++) {
        if (!g_dcs[i].h) { oldest = i; break; }
        if (g_dcs[i].seq < g_dcs[oldest].seq) oldest = i;
    }
    dc = &g_dcs[oldest];
    dc->h = h;
    dc->seq = ++g_dc_seq;
    dc_defaults(dc);
    return dc;
}

void ll_host_dc_reset(void* hdc)
{
    LLDC* dc = dc_of(hdc);
    dc_defaults(dc);
}

/* ---- what the font engine needs ---------------------------------------- */
int ll_host_dc_target(void* hdc, LLFontTarget* t)
{
    unsigned short* bits;
    int w, h, pitch;
    LLDC* dc;

    if (!t || !ll_host_surface_pixels(hdc, &bits, &w, &h, &pitch))
        return 0;
    t->bits = bits;
    t->w = w;
    t->h = h;
    t->pitch = pitch;
    t->clip.left = 0;
    t->clip.top = 0;
    t->clip.right = w;
    t->clip.bottom = h;

    /* The selected clip region, if the caller selected one. text.c always does:
     * CreateRectRgn(g_clip_rect.left, .top, .right, .bottom), which is the
     * game's own clip window (g_clip_rect @ 0x4bdea0). The game's rects are
     * INCLUSIVE -- surface.c builds {0,0,w-1,h-1} -- and a GDI region is
     * half-open, so the region as the game builds it is one pixel short on the
     * right and bottom. That asymmetry lives in the game; it is honoured, not
     * corrected, exactly as user32.c's rect maths honours it. */
    dc = dc_find(hdc);
    if (dc && dc->rgn) {
        LLObj* r = obj_get(dc->rgn, LL_OBJ_RGN);
        if (r) {
            if (r->rc.left   > t->clip.left)   t->clip.left   = r->rc.left;
            if (r->rc.top    > t->clip.top)    t->clip.top    = r->rc.top;
            if (r->rc.right  < t->clip.right)  t->clip.right  = r->rc.right;
            if (r->rc.bottom < t->clip.bottom) t->clip.bottom = r->rc.bottom;
        }
    }
    return t->clip.left < t->clip.right && t->clip.top < t->clip.bottom;
}

void ll_host_dc_font(void* hdc, LLFontMetrics* m)
{
    LLDC*  dc = dc_find(hdc);
    LLObj* f  = dc ? obj_get(dc->font, LL_OBJ_FONT) : 0;
    if (f)
        *m = f->font;
    else
        ll_font_default_metrics(m);   /* no font selected: the system font */
}

unsigned short ll_host_dc_fg(void* hdc)
{
    LLDC* dc = dc_find(hdc);
    return ll_font_colorref_to_565(dc ? dc->fg : 0x00000000ul);
}

unsigned short ll_host_dc_bg(void* hdc)
{
    LLDC* dc = dc_find(hdc);
    return ll_font_colorref_to_565(dc ? dc->bg : 0x00fffffful);
}

int ll_host_dc_opaque(void* hdc)
{
    LLDC* dc = dc_find(hdc);
    return dc ? dc->bk_mode == 2 : 1;
}

unsigned long ll_host_brush_colour(void* brush)
{
    LLObj* b = obj_get(brush, LL_OBJ_BRUSH);
    if (b)
        return b->colour;
    /* GetStockObject(5) is GRAY_BRUSH, the class background screen.c asks for. */
    if (obj_class(brush) == LL_OBJ_STOCK)
        return 0x00808080ul;
    return 0x00000000ul;
}

/* ---- fonts -------------------------------------------------------------- */
/* InitHostSystemGPU adds "Lego.ttf" and KillHostSystemGPU removes it; neither
 * checks the result. Reporting one font added keeps both honest. */
int AddFontResourceA(const char* file) { (void)file; return 1; }
int RemoveFontResourceA(const char* file) { (void)file; return 1; }

/* LOGFONTA as the game declares it (screen.c:1183): lfHeight +0x00,
 * lfWeight +0x10, lfItalic +0x14, lfFaceName +0x1c. InitScreen builds four of
 * these in face "Lego" -- 24/700, 28/400, 20/700, 18/600 -- and keeps the
 * handles at 0x0066808c..0x00668098, which text.c's SelectFont picks between.
 * Only the height and the weight reach the face; "Lego" itself is a TrueType
 * file this port cannot rasterise (see ll_font.c). */
typedef struct LLLogFont {
    long          lfHeight;
    long          lfWidth;
    long          lfEscapement;
    long          lfOrientation;
    long          lfWeight;
    unsigned char lfItalic, lfUnderline, lfStrikeOut, lfCharSet;
    unsigned char lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
    char          lfFaceName[32];
} LLLogFont;

void* CreateFontIndirectA(const void* logfont)
{
    const LLLogFont* lf = (const LLLogFont*)logfont;
    LLObj* o;
    void*  h = obj_new(LL_OBJ_FONT, &o);
    if (o) {
        if (lf)
            ll_font_metrics((int)lf->lfHeight, (int)lf->lfWeight, &o->font);
        else
            ll_font_default_metrics(&o->font);
        ll_host_trace("CreateFontIndirectA h=%ld w=%ld \"%s\" -> advance %d",
                      lf ? lf->lfHeight : 0, lf ? lf->lfWeight : 0,
                      lf ? lf->lfFaceName : "", o->font.advance);
    }
    return h;
}

/* ---- device contexts ---------------------------------------------------- */
void* CreateCompatibleDC(void* hdc) { (void)hdc; return obj_new(LL_OBJ_DC, 0); }
void* CreateDCA(const char* driver, const char* device, const char* output,
                const void* devmode)
{ (void)driver; (void)device; (void)output; (void)devmode;
  return obj_new(LL_OBJ_DC, 0); }

int DeleteDC(void* hdc)
{
    LLDC* dc = dc_find(hdc);
    if (dc)
        dc->h = 0;
    return 1;
}

/* ---- objects ------------------------------------------------------------ */
void* CreateDIBitmap(void* hdc, const void* hdr, unsigned long init,
                     const void* bits, const void* info, unsigned int usage)
{ (void)hdc; (void)hdr; (void)init; (void)bits; (void)info; (void)usage;
  return obj_new(LL_OBJ_BITMAP, 0); }

void* CreatePen(int style, int width, unsigned long colour)
{
    LLObj* o;
    void*  h = obj_new(LL_OBJ_PEN, &o);
    (void)style; (void)width;
    if (o) o->colour = colour;
    return h;
}

void* CreateSolidBrush(unsigned long colour)
{
    LLObj* o;
    void*  h = obj_new(LL_OBJ_BRUSH, &o);
    if (o) o->colour = colour;
    return h;
}

void* CreateRectRgn(int l, int t, int r, int b)
{
    LLObj* o;
    void*  h = obj_new(LL_OBJ_RGN, &o);
    if (o) {
        o->rc.left = l; o->rc.top = t; o->rc.right = r; o->rc.bottom = b;
    }
    return h;
}

void* CreateRectRgnIndirect(const LLRect* rc)
{
    if (!rc)
        return CreateRectRgn(0, 0, 0, 0);
    return CreateRectRgn((int)rc->left, (int)rc->top,
                         (int)rc->right, (int)rc->bottom);
}

/* screen.c asks for stock object 5 (GRAY_BRUSH) as the window class
 * background. Stock objects are never deleted, so they get their own class with
 * no table slot. */
void* GetStockObject(int index)
{ return (void*)(size_t)(LL_OBJ_TAG | ((unsigned)LL_OBJ_STOCK << 12) |
                         ((unsigned)index & 0xfff)); }

/* SelectObject returns the PREVIOUSLY selected object of the same class, which
 * is what every caller hands back to restore it. Returning a fresh cookie (as
 * before) made the restore a no-op; returning null would make the game store a
 * null and restore nothing. */
void* SelectObject(void* hdc, void* obj)
{
    LLDC* dc = dc_of(hdc);
    void* prev;
    switch (obj_class(obj)) {
    case LL_OBJ_FONT:  prev = dc->font;  dc->font = obj;  return prev;
    case LL_OBJ_RGN:   prev = dc->rgn;   dc->rgn = obj;   return prev;
    case LL_OBJ_PEN:   prev = dc->pen;   dc->pen = obj;   return prev;
    case LL_OBJ_BRUSH:
    case LL_OBJ_STOCK: prev = dc->brush; dc->brush = obj; return prev;
    default:           return obj;      /* unknown class: identity */
    }
}

int DeleteObject(void* obj)
{
    unsigned long v = (unsigned long)(size_t)obj;
    unsigned slot = v & 0xfff;
    int i;
    if ((v & 0xffff0000u) != LL_OBJ_TAG || obj_class(obj) == LL_OBJ_STOCK)
        return 1;
    if (slot == 0 || slot >= LL_OBJ_MAX)
        return 1;
    /* A selected object must not vanish from under a DC: clear the references
     * first, which is also what GDI does (it refuses, leaving the DC valid). */
    for (i = 0; i < LL_DC_MAX; i++) {
        if (g_dcs[i].font == obj)  g_dcs[i].font = 0;
        if (g_dcs[i].rgn == obj)   g_dcs[i].rgn = 0;
        if (g_dcs[i].pen == obj)   g_dcs[i].pen = 0;
        if (g_dcs[i].brush == obj) g_dcs[i].brush = 0;
    }
    g_objs[slot].used = 0;
    return 1;
}

/* ---- queries ------------------------------------------------------------ */
int GetDeviceCaps(void* hdc, int index)
{
    int w, h;
    (void)hdc;
    ll_host_display_size(&w, &h);
    switch (index) {
    case 8:   return w;     /* HORZRES */
    case 10:  return h;     /* VERTRES */
    case 12:  return 16;    /* BITSPIXEL */
    case 14:  return 1;     /* PLANES */
    case 88:  return 96;    /* LOGPIXELSX */
    case 90:  return 96;    /* LOGPIXELSY */
    default:  return 0;
    }
}

/* The 16-bpp surfaces hold exactly what is written to them, so the nearest
 * colour is the colour. */
unsigned long GetNearestColor(void* hdc, unsigned long colour)
{ (void)hdc; return colour; }

/* ---- drawing state ------------------------------------------------------ */
unsigned long SetBkColor(void* hdc, unsigned long colour)
{
    LLDC* dc = dc_of(hdc);
    unsigned long prev = dc->bg;
    dc->bg = colour;
    return prev;
}

unsigned long SetTextColor(void* hdc, unsigned long colour)
{
    LLDC* dc = dc_of(hdc);
    unsigned long prev = dc->fg;
    dc->fg = colour;
    return prev;
}

int SetBkMode(void* hdc, int mode)
{
    LLDC* dc = dc_of(hdc);
    int prev = dc->bk_mode;
    if (mode == 1 || mode == 2)
        dc->bk_mode = mode;
    return prev;
}

unsigned int SetTextAlign(void* hdc, unsigned int align)
{
    LLDC* dc = dc_of(hdc);
    unsigned int prev = dc->align;
    dc->align = align;
    return prev;
}

/* ---- text --------------------------------------------------------------- */
/* text.c's Print (0x00454ba0) is SetBkMode(OPAQUE) + SelectObject(rgn) +
 * SelectFont + TextOutA, so the opaque cell fill and the clip region both come
 * from the DC, and (x, y) is the top-left of the cell: the game never calls
 * SetTextAlign on a surface DC, so TA_TOP|TA_LEFT is the only alignment that
 * has to be right. */
int TextOutA(void* hdc, int x, int y, const char* text, int len)
{
    LLFontTarget  t;
    LLFontMetrics m;
    if (!text || len <= 0)
        return 1;
    if (!ll_host_dc_target(hdc, &t))
        return 1;                      /* a memory or printer DC: nothing to do */
    ll_host_dc_font(hdc, &m);
    ll_font_text_out(&t, &m, x, y, text, len,
                     ll_host_dc_fg(hdc), ll_host_dc_opaque(hdc),
                     ll_host_dc_bg(hdc));
    return 1;
}

/* ---- lines -------------------------------------------------------------- */
/* renderview.c's full-map overlay (2835-2843) draws the ride links with
 * MoveToEx/LineTo in the selected pen's colour. In-game rather than front end,
 * but it is twenty lines of Bresenham and it keeps the map view honest. */
static void draw_line(const LLFontTarget* t, int x0, int y0, int x1, int y1,
                      unsigned short c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2;
    for (;;) {
        if (x0 >= t->clip.left && x0 < t->clip.right &&
            y0 >= t->clip.top  && y0 < t->clip.bottom)
            ((unsigned short*)((char*)t->bits + (long)y0 * t->pitch))[x0] = c;
        if (x0 == x1 && y0 == y1)
            break;
        {
            int e2 = err;
            if (e2 > -dx) { err -= dy; x0 += sx; }
            if (e2 <  dy) { err += dx; y0 += sy; }
        }
    }
}

int MoveToEx(void* hdc, int x, int y, LLPoint* prev)
{
    LLDC* dc = dc_of(hdc);
    if (prev) { prev->x = dc->pen_x; prev->y = dc->pen_y; }
    dc->pen_x = x;
    dc->pen_y = y;
    return 1;
}

int LineTo(void* hdc, int x, int y)
{
    LLFontTarget t;
    LLDC* dc = dc_of(hdc);
    if (ll_host_dc_target(hdc, &t)) {
        LLObj* pen = obj_get(dc->pen, LL_OBJ_PEN);
        draw_line(&t, dc->pen_x, dc->pen_y, x, y,
                  ll_font_colorref_to_565(pen ? pen->colour : 0));
    }
    dc->pen_x = x;
    dc->pen_y = y;
    return 1;
}

/* ---- still stubbed ------------------------------------------------------ */
/* The AVI intro path (movie.c) pushes decoded Indeo frames through here. The
 * decoder is not ported, so there is nothing to copy: report 0 scan lines. */
int StretchDIBits(void* hdc, int xd, int yd, int wd, int hd,
                  int xs, int ys, int ws, int hs, const void* bits,
                  const void* info, unsigned int usage, unsigned long rop)
{
    (void)hdc; (void)xd; (void)yd; (void)wd; (void)hd;
    (void)xs; (void)ys; (void)ws; (void)hs; (void)bits; (void)info;
    (void)usage; (void)rop;
    return 0;
}

/* ---- printing: documented failure --------------------------------------- */
/* StartDocA returns <= 0 on failure, which is where the print path stops. */
int StartDocA(void* hdc, const void* docinfo) { (void)hdc; (void)docinfo; return 0; }
int StartPage(void* hdc) { (void)hdc; return 0; }
int EndPage(void* hdc) { (void)hdc; return 0; }
int EndDoc(void* hdc) { (void)hdc; return 0; }
