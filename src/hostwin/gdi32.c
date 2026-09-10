/* LEGOLAND portable build -- the GDI32 host shim (scope PORT-B).
 *
 * There is no GDI in a browser. Every entry point here is therefore one of
 * three things, and NONE of them traps:
 *
 *   (a) a handle factory -- hands back a distinct non-null cookie, because the
 *       game stores these handles, passes them back to SelectObject and
 *       DeleteObject's them at shutdown (KillHostSystemGPU, gpu.c 0x004637e0,
 *       deletes the four fonts InitScreen created). A null font handle would
 *       make the four DeleteObject calls no-ops, which is fine, but a distinct
 *       cookie also lets the shim tell them apart later when a real bitmap
 *       font arrives;
 *   (b) a drawing call -- a no-op that reports success. The visible cost is
 *       that GDI TEXT IS INVISIBLE (text.c's Print path goes
 *       surface->GetDC -> SelectObject(font) -> TextOutA) and that the AVI
 *       intro frames do not appear (movie.c's StretchDIBits). Both are listed
 *       in docs/lanes/scope-port-b.md §7;
 *   (c) a printing call -- reports FAILURE, so the print path aborts at its
 *       first check instead of walking a half-built document.
 *
 * The DC these calls receive is whatever IDirectDrawSurface::GetDC handed out
 * (ddraw.c returns the surface pointer itself), or a cookie from
 * CreateCompatibleDC / CreateDCA. Nothing dereferences it here.
 */
#include <stdio.h>

#include "ll_host.h"

/* Distinct non-null cookies, one per object class, with a counter in the low
 * bits so every CreateFontIndirectA result differs (the game keeps four). */
static unsigned long g_obj_serial = 1;

static void* cookie(unsigned long kind)
{
    return (void*)(size_t)(0x4c470000u | (kind << 12) | ((g_obj_serial++) & 0xfff));
}

/* ---- fonts -------------------------------------------------------------- */
/* InitHostSystemGPU adds "Lego.ttf" and KillHostSystemGPU removes it; the game
 * only checks nothing. Reporting one font added keeps both honest. */
int AddFontResourceA(const char* file) { (void)file; return 1; }
int RemoveFontResourceA(const char* file) { (void)file; return 1; }

/* InitScreen builds four fonts from one LOGFONT (24/700, 28/400, 20/700,
 * 18/600 in face "Lego") and keeps them at 0x0066808c..0x00668098. */
void* CreateFontIndirectA(const void* logfont) { (void)logfont; return cookie(1); }

/* ---- device contexts ---------------------------------------------------- */
void* CreateCompatibleDC(void* hdc) { (void)hdc; return cookie(2); }
void* CreateDCA(const char* driver, const char* device, const char* output,
                const void* devmode)
{ (void)driver; (void)device; (void)output; (void)devmode; return cookie(2); }
int DeleteDC(void* hdc) { (void)hdc; return 1; }

/* ---- objects ------------------------------------------------------------ */
void* CreateDIBitmap(void* hdc, const void* hdr, unsigned long init,
                     const void* bits, const void* info, unsigned int usage)
{ (void)hdc; (void)hdr; (void)init; (void)bits; (void)info; (void)usage; return cookie(3); }
void* CreatePen(int style, int width, unsigned long colour)
{ (void)style; (void)width; (void)colour; return cookie(4); }
void* CreateSolidBrush(unsigned long colour) { (void)colour; return cookie(5); }
void* CreateRectRgn(int l, int t, int r, int b)
{ (void)l; (void)t; (void)r; (void)b; return cookie(6); }
void* CreateRectRgnIndirect(const LLRect* rc) { (void)rc; return cookie(6); }
/* screen.c asks for stock object 5 (GRAY_BRUSH) as the class background. */
void* GetStockObject(int index) { (void)index; return cookie(7); }

/* SelectObject returns the PREVIOUSLY selected object; text.c's SelectFont
 * hands that result back to the caller to restore, so it must be non-null. */
void* SelectObject(void* hdc, void* obj) { (void)hdc; (void)obj; return cookie(8); }
int   DeleteObject(void* obj) { (void)obj; return 1; }

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

/* The game uses this only to snap a requested colour; the 16-bpp surfaces hold
 * exactly what is written to them, so the nearest colour is the colour. */
unsigned long GetNearestColor(void* hdc, unsigned long colour)
{ (void)hdc; return colour; }

/* ---- drawing state: accepted, recorded nowhere, reported as "was 0" ------ */
unsigned long SetBkColor(void* hdc, unsigned long colour)
{ (void)hdc; (void)colour; return 0; }
unsigned long SetTextColor(void* hdc, unsigned long colour)
{ (void)hdc; (void)colour; return 0; }
int SetBkMode(void* hdc, int mode) { (void)hdc; (void)mode; return 1; }
unsigned int SetTextAlign(void* hdc, unsigned int align)
{ (void)hdc; (void)align; return 0; }

/* ---- drawing: no-ops that report success -------------------------------- */
/* GDI text is invisible until a bitmap font lands. Reporting success keeps the
 * callers (text.c's Print, the front-end's labels) walking their own layout
 * maths, which is what the rest of the UI geometry depends on. */
int TextOutA(void* hdc, int x, int y, const char* text, int len)
{ (void)hdc; (void)x; (void)y; (void)text; (void)len; return 1; }
int MoveToEx(void* hdc, int x, int y, LLPoint* prev)
{
    (void)hdc;
    if (prev) { prev->x = x; prev->y = y; }
    return 1;
}
int LineTo(void* hdc, int x, int y) { (void)hdc; (void)x; (void)y; return 1; }

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
