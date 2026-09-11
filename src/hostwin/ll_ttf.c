/* LEGOLAND portable build -- a TrueType rasteriser for the game's own face
 * (scope PORT-B11, closing PORT-B10's PARK-4).
 *
 * WHY THIS FILE EXISTS
 * -------------------------------------------------------------------------
 * The game ships its typeface. `gamedata/main/Lego.TTF` is 77,012 bytes of
 * real sfnt -- 15 tables, 377 glyphs, 2048 units/em -- and gpu.c's
 * `InitHostSystemGPU` (0x00463700) hands it to `AddFontResourceA` before
 * anything draws, so every `CreateFontIndirectA(.. "Lego" ..)` that
 * `InitScreen` (screen.c:1343) makes afterwards is a request for THIS file.
 *
 * PORT-B2 and PORT-B10 could not rasterise it, so they drew a 6x7 bitmap face
 * by hand and scaled it. That face is legible and its metrics are in the right
 * ballpark, but "ballpark" is the problem: the game does all of its own layout
 * from `DrawTextA`'s measurements (eleven DT_CALCRECT call sites) and sizes
 * several boxes from SPRITE dimensions instead, so the shipped data and the
 * shipped art only fit if the metrics are the font's ACTUAL metrics. Both of
 * PORT-B10's text defects were instances of that one class -- the Duty
 * Manager's briefing clipped at 460 px because the face measured too wide, the
 * money readout's zeroes destroyed because the ink box was too tall for the
 * coin-bar sprite. Guessing closed those two. Reading the font closes the class.
 *
 * THE PROOF THAT THE MAPPING IS RIGHT
 * -------------------------------------------------------------------------
 * The tutorial letter is pre-wrapped in the shipped data (movie.c's
 * LoadHelpTextFor reads `Intervals\<key>` as LINES) and uimisc2.c:472's
 * PrintReportLine draws each line DT_SINGLELINE into a box 0x1cc = 460 pixels
 * wide. The longest line -- "forgot, we'll practice building paths, too.
 * There's no point in", 63 characters -- is the tightest constraint the shipped
 * data places on the face. Measured through this file, with the height mapping
 * below, at each of the four LOGFONTs InitScreen creates:
 *
 *     lfHeight 18  (font 2, the report body)    383 px     <-- fits 460
 *     lfHeight 20  (font 1)                     424 px         fits
 *     lfHeight 24  (the default)                508 px         clips
 *     lfHeight 28  (font 3)                     597 px         clips
 *
 * 383 of 460 is 83%: the data was wrapped for this font at this size, and the
 * only one of the four heights with comfortable room to spare is the one
 * text.c:67-70's addresses say the report screen asks for. Two independent
 * facts -- a font file we were handed and a set of line lengths shipped as
 * data -- agree. That is the evidence that both the 18-pixel font id (PORT-B10
 * corrected PORT-B2's off-by-one) and the cell-height mapping are right.
 *
 * HOW lfHeight BECOMES A PIXEL SIZE
 * -------------------------------------------------------------------------
 * GDI's `lfHeight` is not the em size. For lfHeight > 0 the font mapper picks
 * the size at which the font's CELL -- `tmHeight`, ascent plus descent,
 * internal leading included -- equals lfHeight; for lfHeight < 0 it is |h| that
 * is the em size. For a TrueType face the cell comes from OS/2's
 * `usWinAscent` + `usWinDescent`, so
 *
 *     px per font unit  =  lfHeight / (usWinAscent + usWinDescent)
 *     tmAscent          =  round(usWinAscent  * that)
 *     tmDescent         =  round(usWinDescent * that)
 *
 * Lego.TTF gives usWinAscent 2110, usWinDescent 595 over a 2048 em, so the cell
 * is 2705 units and an lfHeight of 18 is 13.6 pixels per em with a 14-pixel
 * ascent. The em is much smaller than the cell because the face's own bounding
 * box is tall (yMax 2110 on a 2048 em -- the studs), which is exactly the kind
 * of thing a guessed face cannot know.
 *
 * `TextOutA`'s y is the top of the cell (TA_TOP), so the baseline sits at
 * y + tmAscent, and that is where the game's sprite-sized boxes expect it:
 * money.c:156 gives the "%5d" brick count a box `g->h` tall -- the coin bar
 * sprite's own ~20 pixels -- for the lfHeight 24 font, whose ascent is 19.
 * The digits fit above the baseline with a pixel to spare and the descender
 * (5 px, and no digit uses it) is what hangs out of the box.
 *
 * SYNTHETIC BOLD
 * -------------------------------------------------------------------------
 * Lego.TTF is a single Regular face: OS/2 `usWeightClass` 400, name id 2
 * "Regular", and there is no companion bold file in the tree. InitScreen asks
 * for weights 700, 700, 600 and 400. GDI answers a request of FW_BOLD or more
 * against a non-bold face by SIMULATING bold -- it double-strikes the glyph one
 * pixel right and adds 1 to the advance (that extra pixel is `tmOverhang`) --
 * and leaves a 600 request alone, because the mapper rounds a weight that is
 * not yet bold down to the face it has. So:
 *
 *     g_font_20 (700)  emboldened      g_font_24 (700)  emboldened
 *     g_font_18 (600)  NOT emboldened  g_font_28 (400)  not emboldened
 *
 * which is the same conclusion PORT-B10 reached from the other end: the report
 * body font must not pay a bold pixel per character, or the 63-character line
 * grows by 63 px and clips again. Here it falls out of the font's own weight
 * class rather than out of a fix.
 *
 * WHAT IS IMPLEMENTED, AND WHAT IS NOT
 * -------------------------------------------------------------------------
 * Implemented: the sfnt directory; `head` (unitsPerEm, indexToLocFormat);
 * `hhea`/`hmtx` (advance widths); `maxp`; `loca`; `glyf` simple AND composite
 * glyphs; `cmap` formats 4 (Windows Unicode) and 0 (Mac Roman) with format 4
 * preferred; OS/2 v0..v5 (usWinAscent/Descent, usWeightClass). The outline is
 * flattened (quadratics subdivided by a curve-length estimate) and filled with
 * a non-zero-winding scanline rasteriser at 5 sub-rows per pixel with exact
 * horizontal coverage, giving 8-bit alpha.
 *
 * NOT implemented, deliberately: the `cvt `/`fpgm`/`prep` hinting programs, the
 * `kern` table, and vertical metrics. Hinting at 13 ppem is what would make a
 * 1999 screenshot pixel-identical, and a bytecode interpreter is a far bigger
 * thing than this lane; anti-aliased coverage (below) avoids the dropout that
 * is the main thing hinting would have prevented. Kerning is correct to omit:
 * GDI's TextOut and DrawText do not apply a font's kern table unless the
 * application asks for it through GetKerningPairs, and no call site in the game
 * does -- so the unkerned advances here are the advances Windows used.
 *
 * ANTI-ALIASING
 * -------------------------------------------------------------------------
 * GDI in 1999, on a 16-bit surface, with `lfQuality` 2 (DRAFT_QUALITY) and
 * font smoothing off by default, would have drawn these glyphs as bilevel
 * bitmaps. This file computes coverage and BLENDS by default instead, because
 * an unhinted bilevel render at 13.6 ppem drops thin stems -- the one visual
 * regression a faithful threshold would buy us. Both are here:
 * `ll_ttf_set_antialias(0)` thresholds coverage at 50% for the bilevel look.
 * Either way the METRICS -- which is what the game lays out from -- are
 * identical, so the choice cannot move a box.
 *
 * THE FALLBACK
 * -------------------------------------------------------------------------
 * If the face cannot be found or cannot be parsed, `ll_ttf_available()` stays 0
 * and ll_font.c uses PORT-B10's bitmap face exactly as before. That is not a
 * theoretical path: the native build has no mounted gamedata, so the native
 * `legoland_tests` and the node headless builds run on the bitmap face and must
 * keep doing so. Nothing in this file is Emscripten-specific.
 *
 * Ownership: PORT-B (docs/SCOPE_PORT_WAVE.md). Declarations: ll_host.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ll_host.h"

/* ---- the face ----------------------------------------------------------- */

typedef struct LLTtf {
    unsigned char* data;
    unsigned long  size;

    unsigned long  glyf, glyf_len;
    unsigned long  loca, loca_len;
    unsigned long  hmtx, hmtx_len;
    unsigned long  cmap4;            /* 0 if absent */
    unsigned long  cmap0;            /* 0 if absent */

    int            upem;
    int            loca_long;
    int            num_glyphs;
    int            num_hmetrics;
    int            win_ascent;
    int            win_descent;
    int            weight_class;
    int            ready;
} LLTtf;

static LLTtf  g_ttf;
static int    g_ttf_tried;
static int    g_ttf_aa = 1;

/* Big-endian readers. Every offset is bounds-checked against the file, so a
 * truncated or hostile font cannot read past the block: a bad read yields 0,
 * which fails the validation in ll_ttf_load_memory or produces an empty glyph. */
static unsigned int rd_u8(const LLTtf* f, unsigned long o)
{
    return (o < f->size) ? f->data[o] : 0u;
}
static unsigned int rd_u16(const LLTtf* f, unsigned long o)
{
    if (o + 1 >= f->size)
        return 0u;
    return ((unsigned int)f->data[o] << 8) | f->data[o + 1];
}
static int rd_i16(const LLTtf* f, unsigned long o)
{
    int v = (int)rd_u16(f, o);
    return (v & 0x8000) ? v - 0x10000 : v;
}
static unsigned long rd_u32(const LLTtf* f, unsigned long o)
{
    if (o + 3 >= f->size)
        return 0ul;
    return ((unsigned long)f->data[o] << 24) | ((unsigned long)f->data[o + 1] << 16) |
           ((unsigned long)f->data[o + 2] << 8) | (unsigned long)f->data[o + 3];
}

/* Find a table by its four-character tag. */
static int ttf_table(const LLTtf* f, const char* tag,
                     unsigned long* off, unsigned long* len)
{
    unsigned int n = rd_u16(f, 4);
    unsigned int i;
    for (i = 0; i < n; i++) {
        unsigned long e = 12ul + 16ul * i;
        if (e + 16 > f->size)
            return 0;
        if (memcmp(f->data + e, tag, 4) == 0) {
            unsigned long o = rd_u32(f, e + 8);
            unsigned long l = rd_u32(f, e + 12);
            if (o >= f->size)
                return 0;
            if (o + l > f->size)
                l = f->size - o;
            if (off) *off = o;
            if (len) *len = l;
            return 1;
        }
    }
    return 0;
}

/* ---- loading ------------------------------------------------------------ */

int ll_ttf_load_memory(void* data, unsigned long size)
{
    LLTtf f;
    unsigned long head, hhea, maxp, os2, cmap, clen;
    unsigned long ver;

    memset(&f, 0, sizeof(f));
    f.data = (unsigned char*)data;
    f.size = size;
    if (!data || size < 256)
        return 0;

    /* 0x00010000 is TrueType outlines; 'true' is the Mac flavour. 'OTTO' is
     * CFF/PostScript outlines, which this file cannot draw, so it is refused
     * rather than half-read. */
    ver = rd_u32(&f, 0);
    if (ver != 0x00010000ul && memcmp(f.data, "true", 4) != 0)
        return 0;

    if (!ttf_table(&f, "head", &head, 0)) return 0;
    if (!ttf_table(&f, "hhea", &hhea, 0)) return 0;
    if (!ttf_table(&f, "maxp", &maxp, 0)) return 0;
    if (!ttf_table(&f, "glyf", &f.glyf, &f.glyf_len)) return 0;
    if (!ttf_table(&f, "loca", &f.loca, &f.loca_len)) return 0;
    if (!ttf_table(&f, "hmtx", &f.hmtx, &f.hmtx_len)) return 0;
    if (!ttf_table(&f, "cmap", &cmap, &clen)) return 0;

    f.upem         = (int)rd_u16(&f, head + 18);
    f.loca_long    = rd_i16(&f, head + 50) ? 1 : 0;
    f.num_glyphs   = (int)rd_u16(&f, maxp + 4);
    f.num_hmetrics = (int)rd_u16(&f, hhea + 34);
    if (f.upem < 16 || f.num_glyphs <= 0 || f.num_hmetrics <= 0)
        return 0;

    /* The cell. OS/2's usWinAscent/usWinDescent are what GDI's tmAscent and
     * tmDescent come from; a face with no OS/2 (or one too short to hold them)
     * falls back to hhea's ascender/descender, which is what GDI does too. */
    f.win_ascent  = 0;
    f.win_descent = 0;
    f.weight_class = 400;
    if (ttf_table(&f, "OS/2", &os2, 0)) {
        unsigned long olen = 0;
        ttf_table(&f, "OS/2", &os2, &olen);
        f.weight_class = (int)rd_u16(&f, os2 + 4);
        if (olen >= 78) {
            f.win_ascent  = (int)rd_u16(&f, os2 + 74);
            f.win_descent = (int)rd_u16(&f, os2 + 76);
        }
    }
    if (f.win_ascent + f.win_descent <= 0) {
        f.win_ascent  = rd_i16(&f, hhea + 4);
        f.win_descent = -rd_i16(&f, hhea + 6);
    }
    if (f.win_ascent + f.win_descent <= 0)
        return 0;

    /* The character maps. (3,1) format 4 is the one Windows uses and the one
     * Lego.TTF leads with; (1,0) format 0 is the 256-byte Mac table, kept as a
     * fallback so a face with only that still maps ASCII. */
    {
        unsigned int n = rd_u16(&f, cmap + 2), i;
        for (i = 0; i < n; i++) {
            unsigned long e = cmap + 4 + 8ul * i;
            unsigned int  pid = rd_u16(&f, e);
            unsigned long sub = cmap + rd_u32(&f, e + 4);
            unsigned int  fmt;
            if (sub >= f.size)
                continue;
            fmt = rd_u16(&f, sub);
            if (fmt == 4 && !f.cmap4)
                f.cmap4 = sub;
            else if (fmt == 0 && !f.cmap0)
                f.cmap0 = sub;
            (void)pid;
        }
    }
    if (!f.cmap4 && !f.cmap0)
        return 0;

    f.ready = 1;
    if (g_ttf.data && g_ttf.data != (unsigned char*)data)
        free(g_ttf.data);
    g_ttf = f;
    ll_host_trace("TTF loaded: %lu bytes, upem %d, %d glyphs, "
                  "winAscent %d winDescent %d, weight %d",
                  size, f.upem, f.num_glyphs, f.win_ascent, f.win_descent,
                  f.weight_class);
    return 1;
}

int ll_ttf_load_file(const char* path)
{
    FILE* fp;
    long  n;
    void* buf;

    if (!path)
        return 0;
    fp = fopen(path, "rb");
    if (!fp)
        return 0;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return 0; }
    n = ftell(fp);
    if (n <= 0 || n > 8 * 1024 * 1024) { fclose(fp); return 0; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return 0; }
    buf = malloc((size_t)n);
    if (!buf) { fclose(fp); return 0; }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf); fclose(fp); return 0;
    }
    fclose(fp);
    if (!ll_ttf_load_memory(buf, (unsigned long)n)) {
        free(buf);
        return 0;
    }
    return 1;
}

/* The places the face can be. The browser build preloads `gamedata/main` at
 * `/gamedata` (portable/cmake/browser.cmake:158), which is where the first of
 * these hits; the rest let a native or node run that happens to sit in the
 * right directory find it too. MEMFS path resolution is case-insensitive
 * (PORT-A3), so on the page the `.TTF` and `.ttf` spellings are the same file;
 * on a case-sensitive host both are tried because the tree ships `Lego.TTF`
 * while gpu.c's string is "Lego.ttf". */
static const char* const kFontPaths[] = {
    "/gamedata/Lego.TTF",
    "/gamedata/Lego.ttf",
    "gamedata/main/Lego.TTF",
    "gamedata/main/Lego.ttf",
    "../gamedata/main/Lego.TTF",
    "../../gamedata/main/Lego.TTF",
    "Lego.TTF",
    "Lego.ttf",
    0
};

int ll_ttf_available(void)
{
    int i;
    if (g_ttf.ready)
        return 1;
    if (g_ttf_tried)
        return 0;
    g_ttf_tried = 1;
    for (i = 0; kFontPaths[i]; i++)
        if (ll_ttf_load_file(kFontPaths[i]))
            return 1;
    ll_host_trace("TTF: Lego.TTF not found; using the bitmap face");
    return 0;
}

void ll_ttf_set_antialias(int on) { g_ttf_aa = on ? 1 : 0; }
int  ll_ttf_antialias(void)       { return g_ttf_aa; }

/* ---- character to glyph ------------------------------------------------- */

static int cmap4_lookup(const LLTtf* f, unsigned int c)
{
    unsigned long t = f->cmap4;
    unsigned int segX2 = rd_u16(f, t + 6);
    unsigned long ends = t + 14;
    unsigned long starts = ends + segX2 + 2;
    unsigned long deltas = starts + segX2;
    unsigned long ranges = deltas + segX2;
    unsigned int s;

    if (c > 0xffff)
        return 0;
    for (s = 0; s + 1 < segX2; s += 2) {
        unsigned int end = rd_u16(f, ends + s);
        if (c <= end) {
            unsigned int start = rd_u16(f, starts + s);
            int          delta;
            unsigned int ro;
            if (c < start)
                return 0;
            delta = rd_i16(f, deltas + s);
            ro    = rd_u16(f, ranges + s);
            if (ro == 0)
                return (int)((c + (unsigned int)delta) & 0xffffu);
            {
                unsigned long a = ranges + s + ro + 2ul * (c - start);
                unsigned int  g = rd_u16(f, a);
                if (g == 0)
                    return 0;
                return (int)((g + (unsigned int)delta) & 0xffffu);
            }
        }
    }
    return 0;
}

/* The game's strings are plain ASCII (text.c's GetString), so a byte is a
 * Unicode code point for the format 4 table and a Mac Roman index for the
 * format 0 one -- identical over 0x20..0x7e, which is everything that is
 * actually drawn. */
static int ttf_glyph_index(const LLTtf* f, unsigned char ch)
{
    int g = 0;
    if (f->cmap4)
        g = cmap4_lookup(f, ch);
    if (!g && f->cmap0)
        g = (int)rd_u8(f, f->cmap0 + 6 + ch);
    if (g < 0 || g >= f->num_glyphs)
        return 0;
    return g;
}

static int ttf_advance_units(const LLTtf* f, int gid)
{
    int idx = gid < f->num_hmetrics ? gid : f->num_hmetrics - 1;
    if (idx < 0)
        return 0;
    return (int)rd_u16(f, f->hmtx + 4ul * (unsigned long)idx);
}

static int ttf_glyph_range(const LLTtf* f, int gid,
                           unsigned long* start, unsigned long* end)
{
    unsigned long a, b;
    if (gid < 0 || gid >= f->num_glyphs)
        return 0;
    if (f->loca_long) {
        a = rd_u32(f, f->loca + 4ul * (unsigned long)gid);
        b = rd_u32(f, f->loca + 4ul * (unsigned long)gid + 4);
    } else {
        a = 2ul * rd_u16(f, f->loca + 2ul * (unsigned long)gid);
        b = 2ul * rd_u16(f, f->loca + 2ul * (unsigned long)gid + 2);
    }
    if (b <= a || b > f->glyf_len)
        return 0;                    /* an empty glyph: the space, for one */
    *start = f->glyf + a;
    *end   = f->glyf + b;
    return 1;
}

/* ---- metrics ------------------------------------------------------------ */

void ll_ttf_metrics(int lf_height, int weight, LLTtfMetrics* m)
{
    const LLTtf* f = &g_ttf;
    int h = lf_height;

    memset(m, 0, sizeof(*m));
    if (!f->ready)
        return;
    if (h < 0) {
        /* A negative lfHeight is a CHARACTER height: |h| is the em. */
        h = -h;
        m->scale = (double)h / (double)f->upem;
        m->cell_h = (int)((double)(f->win_ascent + f->win_descent) * m->scale + 0.5);
    } else {
        if (h < 4)  h = 4;
        if (h > 400) h = 400;
        m->scale = (double)h / (double)(f->win_ascent + f->win_descent);
        m->cell_h = h;
    }
    m->ascent  = (int)((double)f->win_ascent  * m->scale + 0.5);
    m->descent = (int)((double)f->win_descent * m->scale + 0.5);
    if (m->cell_h < m->ascent + m->descent)
        m->cell_h = m->ascent + m->descent;
    /* GDI simulates bold only for a request of FW_BOLD or more against a face
     * that is not already bold; the extra pixel is tmOverhang. */
    m->embolden = (weight >= 700 && f->weight_class < 700) ? 1 : 0;
    m->ppem     = (int)((double)f->upem * m->scale + 0.5);
    m->ready    = 1;
}

int ll_ttf_char_advance(const LLTtfMetrics* m, unsigned char ch)
{
    const LLTtf* f = &g_ttf;
    int gid, a;
    if (!m->ready || !f->ready)
        return 0;
    gid = ttf_glyph_index(f, ch);
    a = (int)((double)ttf_advance_units(f, gid) * m->scale + 0.5);
    a += m->embolden;
    /* GDI never advances by a negative amount, and a zero advance for a
     * printable character would stack the whole line in one place. */
    if (a < 1 && ch != 0)
        a = 1;
    return a;
}

/* ---- outline extraction ------------------------------------------------- */
/* Points in 16.16 device pixels: the em is at most 400 px here and the glyph
 * bbox at most a few ems, so nothing comes near overflowing. */

#define TTF_MAX_PTS   1024
#define TTF_MAX_EDGES 1024
#define TTF_FIX       65536

typedef struct { int x, y; } TtfPt;

typedef struct {
    TtfPt pts[TTF_MAX_PTS];
    int   n;
    struct { int x0, y0, x1, y1; } edges[TTF_MAX_EDGES];
    int   nedges;
} TtfOutline;

static void ttf_edge(TtfOutline* o, int x0, int y0, int x1, int y1)
{
    if (y0 == y1)
        return;                      /* horizontal edges contribute nothing */
    if (o->nedges >= TTF_MAX_EDGES)
        return;
    o->edges[o->nedges].x0 = x0;
    o->edges[o->nedges].y0 = y0;
    o->edges[o->nedges].x1 = x1;
    o->edges[o->nedges].y1 = y1;
    o->nedges++;
}

/* One quadratic Bezier, subdivided by a crude arc-length estimate: the control
 * point's distance from the chord midpoint in pixels, clamped to 2..16
 * segments. At the sizes the game asks for (13..22 ppem) that is 2 or 3. */
static void ttf_quad(TtfOutline* o, int x0, int y0, int cx, int cy, int x1, int y1)
{
    int dx = cx * 2 - x0 - x1;
    int dy = cy * 2 - y0 - y1;
    int d  = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    int n  = d / (TTF_FIX / 2) + 2;
    int i;
    int px = x0, py = y0;
    if (n > 16) n = 16;
    for (i = 1; i <= n; i++) {
        /* Evaluated in doubles: the fixed-point product of three 16.16 terms
         * would overflow 32 bits for a large em. */
        double t = (double)i / (double)n;
        double u = 1.0 - t;
        double qx = u * u * (double)x0 + 2.0 * u * t * (double)cx + t * t * (double)x1;
        double qy = u * u * (double)y0 + 2.0 * u * t * (double)cy + t * t * (double)y1;
        int nx = (int)(qx + (qx < 0 ? -0.5 : 0.5));
        int ny = (int)(qy + (qy < 0 ? -0.5 : 0.5));
        ttf_edge(o, px, py, nx, ny);
        px = nx; py = ny;
    }
}

/* Walk one glyph's contours into `o`, offset by (ox, oy) in 16.16 pixels.
 * `depth` bounds composite recursion. Returns 0 only on a malformed glyph. */
static int ttf_outline(const LLTtf* f, int gid, TtfOutline* o,
                       double scale, int ox, int oy, int flip_y, int depth)
{
    unsigned long g0, g1;
    int nc, i;

    if (depth > 4)
        return 1;
    if (!ttf_glyph_range(f, gid, &g0, &g1))
        return 1;                    /* empty glyph: no contours, not an error */

    nc = rd_i16(f, g0);

    if (nc < 0) {
        /* A composite glyph: components with a 2x2 transform. The game's ASCII
         * is all simple glyphs, but the accented Latin-1 range is composite and
         * the level data is not guaranteed to be pure ASCII. */
        unsigned long p = g0 + 10;
        for (;;) {
            unsigned int flags, cgid;
            int a1, a2, dx = 0, dy = 0;
            if (p + 4 > g1)
                break;
            flags = rd_u16(f, p);
            cgid  = rd_u16(f, p + 2);
            p += 4;
            if (flags & 0x0001) {    /* ARG_1_AND_2_ARE_WORDS */
                a1 = rd_i16(f, p); a2 = rd_i16(f, p + 2); p += 4;
            } else {
                a1 = (int)(signed char)rd_u8(f, p);
                a2 = (int)(signed char)rd_u8(f, p + 1);
                p += 2;
            }
            if (flags & 0x0002) {    /* ARGS_ARE_XY_VALUES */
                dx = a1; dy = a2;
            }
            /* A scale or 2x2 on a component is read past but not applied: no
             * glyph in this face uses one, and applying it would mean carrying
             * a matrix through the whole walk for a case that never fires. */
            if (flags & 0x0008)       p += 2;
            else if (flags & 0x0040)  p += 4;
            else if (flags & 0x0080)  p += 8;
            {
                int cox = ox + (int)((double)dx * scale * TTF_FIX);
                int coy = oy + (flip_y ? -(int)((double)dy * scale * TTF_FIX)
                                       :  (int)((double)dy * scale * TTF_FIX));
                ttf_outline(f, (int)cgid, o, scale, cox, coy, flip_y, depth + 1);
            }
            if (!(flags & 0x0020))   /* MORE_COMPONENTS */
                break;
        }
        return 1;
    }

    /* A simple glyph. */
    {
        unsigned long ends = g0 + 10;
        unsigned long p;
        int npts, ilen;
        unsigned char flags[TTF_MAX_PTS];
        int xs[TTF_MAX_PTS], ys[TTF_MAX_PTS];
        int contour_end[64];
        int j, x = 0, y = 0;

        if (nc <= 0 || nc > 64)
            return 1;
        for (i = 0; i < nc; i++)
            contour_end[i] = (int)rd_u16(f, ends + 2ul * (unsigned long)i);
        npts = contour_end[nc - 1] + 1;
        if (npts <= 0 || npts > TTF_MAX_PTS)
            return 0;
        p = ends + 2ul * (unsigned long)nc;
        ilen = (int)rd_u16(f, p);
        p += 2 + (unsigned long)ilen;   /* the hinting program is skipped */

        for (i = 0; i < npts; ) {
            unsigned int fl = rd_u8(f, p++);
            flags[i++] = (unsigned char)fl;
            if (fl & 0x08) {                /* REPEAT */
                unsigned int r = rd_u8(f, p++);
                while (r-- && i < npts)
                    flags[i++] = (unsigned char)fl;
            }
        }
        for (i = 0; i < npts; i++) {
            unsigned int fl = flags[i];
            if (fl & 0x02) {                /* X_SHORT */
                unsigned int d = rd_u8(f, p++);
                x += (fl & 0x10) ? (int)d : -(int)d;
            } else if (!(fl & 0x10)) {      /* not X_SAME */
                x += rd_i16(f, p); p += 2;
            }
            xs[i] = x;
        }
        for (i = 0; i < npts; i++) {
            unsigned int fl = flags[i];
            if (fl & 0x04) {                /* Y_SHORT */
                unsigned int d = rd_u8(f, p++);
                y += (fl & 0x20) ? (int)d : -(int)d;
            } else if (!(fl & 0x20)) {      /* not Y_SAME */
                y += rd_i16(f, p); p += 2;
            }
            ys[i] = y;
        }

        /* Contour by contour, emitting straight edges between on-curve points
         * and a quadratic through every off-curve control. Two consecutive
         * off-curve points imply an on-curve point at their midpoint, which is
         * TrueType's compact form. */
        j = 0;
        for (i = 0; i < nc; i++) {
            int s = j, e = contour_end[i];
            int k, count;
            int sx, sy, have_ctl = 0, cx = 0, cy = 0;
            int px, py, fx, fy;
            if (e < s) { j = e + 1; continue; }
            count = e - s + 1;
            j = e + 1;

            /* The start point: the first on-curve point, or the midpoint of the
             * first two if the contour begins off-curve. */
            {
                int first_on = -1;
                for (k = 0; k < count; k++)
                    if (flags[s + k] & 0x01) { first_on = k; break; }
                if (first_on < 0) {
                    sx = (xs[s] + xs[e]) / 2;
                    sy = (ys[s] + ys[e]) / 2;
                    first_on = 0;
                } else {
                    sx = xs[s + first_on];
                    sy = ys[s + first_on];
                }
                #define TX(v) (ox + (int)((double)(v) * scale * TTF_FIX))
                #define TY(v) (flip_y ? oy - (int)((double)(v) * scale * TTF_FIX) \
                                      : oy + (int)((double)(v) * scale * TTF_FIX))
                fx = px = TX(sx);
                fy = py = TY(sy);
                for (k = 1; k <= count; k++) {
                    int idx = s + ((first_on + k) % count);
                    int tx = TX(xs[idx]);
                    int ty = TY(ys[idx]);
                    if (flags[idx] & 0x01) {
                        if (have_ctl) { ttf_quad(o, px, py, cx, cy, tx, ty); have_ctl = 0; }
                        else          { ttf_edge(o, px, py, tx, ty); }
                        px = tx; py = ty;
                    } else {
                        if (have_ctl) {
                            int mx = (cx + tx) / 2, my = (cy + ty) / 2;
                            ttf_quad(o, px, py, cx, cy, mx, my);
                            px = mx; py = my;
                        }
                        cx = tx; cy = ty; have_ctl = 1;
                    }
                }
                if (have_ctl) ttf_quad(o, px, py, cx, cy, fx, fy);
                else          ttf_edge(o, px, py, fx, fy);
                #undef TX
                #undef TY
            }
        }
    }
    return 1;
}

/* ---- the rasteriser ----------------------------------------------------- */
/* Non-zero winding, 5 sub-rows per pixel, exact horizontal coverage. */

#define TTF_SUBS 5

static void ttf_fill(const TtfOutline* o, unsigned char* cov,
                     int w, int h, int ox_px, int oy_px)
{
    int row;
    static double xs[TTF_MAX_EDGES];
    static int    dirs[TTF_MAX_EDGES];
    static double acc[512];

    if (w <= 0 || h <= 0 || w > 512)
        return;
    for (row = 0; row < h; row++) {
        int s, i;
        for (i = 0; i < w; i++)
            acc[i] = 0.0;
        for (s = 0; s < TTF_SUBS; s++) {
            double sy = (double)(oy_px + row) + ((double)s + 0.5) / (double)TTF_SUBS;
            double fy = sy * (double)TTF_FIX;
            int n = 0;
            for (i = 0; i < o->nedges; i++) {
                double y0 = (double)o->edges[i].y0;
                double y1 = (double)o->edges[i].y1;
                double x0 = (double)o->edges[i].x0;
                double x1 = (double)o->edges[i].x1;
                int dir = 1;
                if (y0 > y1) {
                    double t;
                    t = y0; y0 = y1; y1 = t;
                    t = x0; x0 = x1; x1 = t;
                    dir = -1;
                }
                /* Half-open in y, so a vertex shared by two edges is counted
                 * once and a scanline through it does not double-wind. */
                if (fy < y0 || fy >= y1)
                    continue;
                if (n >= TTF_MAX_EDGES)
                    break;
                xs[n]   = (x0 + (x1 - x0) * (fy - y0) / (y1 - y0)) / (double)TTF_FIX;
                dirs[n] = dir;
                n++;
            }
            /* Insertion sort: n is the number of edges crossing ONE scanline,
             * which for a glyph is single digits. */
            for (i = 1; i < n; i++) {
                double kx = xs[i];
                int    kd = dirs[i];
                int    j = i - 1;
                while (j >= 0 && xs[j] > kx) { xs[j+1] = xs[j]; dirs[j+1] = dirs[j]; j--; }
                xs[j+1] = kx; dirs[j+1] = kd;
            }
            {
                int wind = 0;
                double span0 = 0.0;
                for (i = 0; i < n; i++) {
                    int prev = wind;
                    wind += dirs[i];
                    if (prev == 0 && wind != 0) {
                        span0 = xs[i];
                    } else if (prev != 0 && wind == 0) {
                        /* Add this span's coverage to the row. */
                        double a = span0 - (double)ox_px;
                        double b = xs[i] - (double)ox_px;
                        int    p0, p1, p;
                        if (b <= 0.0 || a >= (double)w)
                            continue;
                        if (a < 0.0) a = 0.0;
                        if (b > (double)w) b = (double)w;
                        p0 = (int)a;
                        p1 = (int)b;
                        if (p1 >= w) p1 = w - 1;
                        if (p0 == p1) {
                            acc[p0] += (b - a) / (double)TTF_SUBS;
                        } else {
                            acc[p0] += ((double)(p0 + 1) - a) / (double)TTF_SUBS;
                            for (p = p0 + 1; p < p1; p++)
                                acc[p] += 1.0 / (double)TTF_SUBS;
                            acc[p1] += (b - (double)p1) / (double)TTF_SUBS;
                        }
                    }
                }
            }
        }
        for (i = 0; i < w; i++) {
            double a = acc[i];
            int    v;
            if (a <= 0.0) { cov[row * w + i] = 0; continue; }
            if (a > 1.0) a = 1.0;
            v = (int)(a * 255.0 + 0.5);
            cov[row * w + i] = (unsigned char)v;
        }
    }
}

/* ---- the glyph cache ---------------------------------------------------- */
/* Four LOGFONTs x 95 printable characters is the whole working set, and a
 * re-render is ~30 us, so a fixed arena with a direct-mapped index is plenty.
 * When the arena fills, it is reset wholesale: the alternative (an LRU) would
 * be more code for a cache that never actually fills in this game. */

#define TTF_CACHE_SLOTS 512
#define TTF_ARENA_BYTES (512 * 1024)

typedef struct {
    unsigned int key;        /* 0 = empty */
    int          w, h, bx, by, adv;
    unsigned long off;       /* into the arena */
} TtfCacheEnt;

static TtfCacheEnt   g_cache[TTF_CACHE_SLOTS];
static unsigned char g_arena[TTF_ARENA_BYTES];
static unsigned long g_arena_used;

static unsigned int ttf_cache_key(unsigned char ch, int cell_h, int embolden)
{
    /* Never 0: the cell height is at least 4. */
    return ((unsigned int)cell_h << 9) | ((unsigned int)embolden << 8) | ch;
}

const unsigned char* ll_ttf_glyph(const LLTtfMetrics* m, unsigned char ch,
                                  int* w, int* h, int* bx, int* by)
{
    const LLTtf* f = &g_ttf;
    unsigned int key;
    unsigned int slot;
    TtfCacheEnt* e;
    TtfOutline*  o;
    static TtfOutline outline;       /* 16 KB: not on the stack, ASYNCIFY copies it */
    int gid, i;
    int xmin = 0, ymin = 0, xmax = 0, ymax = 0;
    int gw, gh, ox_px, oy_px;
    unsigned long need;

    *w = *h = *bx = *by = 0;
    if (!m->ready || !f->ready)
        return 0;

    key  = ttf_cache_key(ch, m->cell_h, m->embolden);
    slot = key % TTF_CACHE_SLOTS;
    e    = &g_cache[slot];
    if (e->key == key) {
        *w = e->w; *h = e->h; *bx = e->bx; *by = e->by;
        return e->w ? g_arena + e->off : 0;
    }

    gid = ttf_glyph_index(f, ch);
    o   = &outline;
    o->n = 0;
    o->nedges = 0;
    /* The outline is built with the baseline at device y = 0 and x = 0 at the
     * pen, y growing DOWNWARD (flip_y), which is the surface's orientation. */
    if (!ttf_outline(f, gid, o, m->scale, 0, 0, 1, 0) || o->nedges == 0) {
        e->key = key; e->w = e->h = 0; e->bx = e->by = 0;
        return 0;                    /* the space, and anything unmapped */
    }

    for (i = 0; i < o->nedges; i++) {
        int v[4];
        v[0] = o->edges[i].x0; v[1] = o->edges[i].x1;
        v[2] = o->edges[i].y0; v[3] = o->edges[i].y1;
        if (i == 0) { xmin = xmax = v[0]; ymin = ymax = v[2]; }
        if (v[0] < xmin) xmin = v[0];  if (v[0] > xmax) xmax = v[0];
        if (v[1] < xmin) xmin = v[1];  if (v[1] > xmax) xmax = v[1];
        if (v[2] < ymin) ymin = v[2];  if (v[2] > ymax) ymax = v[2];
        if (v[3] < ymin) ymin = v[3];  if (v[3] > ymax) ymax = v[3];
    }
    /* Pixel bounds, outset by a pixel so partial coverage at the edges is not
     * clipped off, plus the embolden column on the right. */
    ox_px = (xmin >> 16) - 1;
    oy_px = (ymin >> 16) - 1;
    gw    = (xmax >> 16) - ox_px + 2 + m->embolden;
    gh    = (ymax >> 16) - oy_px + 2;
    if (gw <= 0 || gh <= 0 || gw > 256 || gh > 256) {
        e->key = key; e->w = e->h = 0;
        return 0;
    }

    need = (unsigned long)gw * (unsigned long)gh;
    if (g_arena_used + need > TTF_ARENA_BYTES) {
        memset(g_cache, 0, sizeof(g_cache));
        g_arena_used = 0;
        e = &g_cache[slot];
        if (need > TTF_ARENA_BYTES)
            return 0;
    }
    memset(g_arena + g_arena_used, 0, need);
    ttf_fill(o, g_arena + g_arena_used, gw, gh, ox_px, oy_px);

    if (m->embolden) {
        /* GDI's synthetic bold: the glyph struck again one pixel right. OR of
         * the coverages, which for a double strike is what the ink does. */
        int y, x;
        unsigned char* p = g_arena + g_arena_used;
        for (y = 0; y < gh; y++)
            for (x = gw - 1; x > 0; x--) {
                unsigned char a = p[y * gw + x];
                unsigned char b = p[y * gw + x - 1];
                p[y * gw + x] = a > b ? a : b;
            }
    }

    e->key = key;
    e->w   = gw;
    e->h   = gh;
    e->bx  = ox_px;                  /* left side bearing, pixels from the pen */
    e->by  = oy_px;                  /* top row, pixels from the BASELINE (<0 above) */
    e->off = g_arena_used;
    g_arena_used += need;

    *w = gw; *h = gh; *bx = e->bx; *by = e->by;
    return g_arena + e->off;
}

/* ---- what the page reports --------------------------------------------- */

int ll_ttf_face_info(int* upem, int* glyphs, int* win_asc, int* win_desc,
                     int* weight)
{
    if (upem)     *upem     = g_ttf.upem;
    if (glyphs)   *glyphs   = g_ttf.num_glyphs;
    if (win_asc)  *win_asc  = g_ttf.win_ascent;
    if (win_desc) *win_desc = g_ttf.win_descent;
    if (weight)   *weight   = g_ttf.weight_class;
    return g_ttf.ready;
}
