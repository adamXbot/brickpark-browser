/* tri_raster -- tri3d.c's four software triangle rasterisers, and the two
 * ST(0)-ABI sqrt helpers' portable hooks.  Scope PORT-B5.
 *
 * `LEGOLAND/tri3d.c` 0x004877b0 DrawFlatTri, 0x00486590 DrawGouraudTri,
 * 0x00487d40 DrawFlatTexTri and 0x00486c70 DrawGouraudTexTri are
 * `__declspec(naked)` hand-written assembly in the matching build; this test
 * exercises the C in their `#else` arms.
 *
 * THE ORACLE IS NOT A SECOND RASTERISER.  Re-deriving the 16.16 edge walk in
 * the test would share every bug with the thing under test, so what is checked
 * instead is the set of properties the file's own RASTERISATION CONTRACT
 * states, each of which fails differently from the others:
 *   - coverage is inside the triangle's bounding box and non-empty;
 *   - the clip rectangle is honoured on all four sides;
 *   - the Z test is UNSIGNED >= : a larger key wins, a smaller one loses, an
 *     equal one overwrites;
 *   - the mouse pick fires for a pixel that was written and not otherwise;
 *   - only DrawFlatTri writes the shifted shade back into the caller's vertex;
 *   - a Gouraud triangle with three equal shades paints ONE ramp entry, and
 *     with a gradient the entry is monotone down the triangle;
 *   - the texel address is texels[((v>>16) << ushift) + (u>>16)] and not the
 *     transposition the file header used to claim -- driven with a texture
 *     whose value depends only on the ROW, and a triangle whose v is constant
 *     while u sweeps, so a transposed address cannot give one value;
 *   - DrawGouraudTexTri masks the ROW with umask and the COLUMN with vmask --
 *     the crossed pair -- which a constant-(u,v) triangle over a deliberately
 *     non-square mask pair reads straight out of the painted pixel.
 * The ramp tables are built so `table[k] = 0x8000 | k`, which makes every
 * painted pixel report the ramp index (and, for the textured pair, the texel
 * value) that reached it.
 *
 * Runs natively and on wasm32: no asset, no loader, no host shim.  g_surface,
 * g_zbuf and g_texture are pointed at the test's own arrays, so
 * InitRasterZBuffer's MemAlloc is never needed. */
#include "ll_tests.h"

#include <math.h>

typedef struct Pos { int x, y; } Pos;

/* tri3d.c's own types, copied so the test sees the same layout. */
typedef struct Shade {
    int             levels;
    unsigned short* table;
} Shade;

typedef struct Vertex2D {
    int x, y, z, u, v, shade, pad18;
} Vertex2D;

typedef struct LLTexDesc {
    int             ushift;
    int             vshift;
    unsigned char*  texels;
    Shade**         ramps;
    int             umask;
    int             vmask;
    int             pad18[5];
} LLTexDesc;

extern void DrawFlatTri(Vertex2D*, Vertex2D*, Vertex2D*);       /* 0x004877b0 */
extern void DrawGouraudTri(Vertex2D*, Vertex2D*, Vertex2D*);    /* 0x00486590 */
extern void DrawFlatTexTri(Vertex2D*, Vertex2D*, Vertex2D*);    /* 0x00487d40 */
extern void DrawGouraudTexTri(Vertex2D*, Vertex2D*, Vertex2D*); /* 0x00486c70 */
extern int  BuildRecipTable(void);                              /* 0x00486540 */
extern void SetRenderTarget(char*, int, int, int);              /* 0x00485f30 */

extern Shade* g_flat_colour;     /* 0x0066b61c */
extern int    g_recip[];         /* 0x00798000 */
extern char*  g_surface;         /* 0x00797e68 */
extern int    g_pitch;           /* 0x00701e58 */
extern int    g_clip_x0;         /* 0x0081c8d0 */
extern int    g_clip_y0;         /* 0x0081c8d4 */
extern int    g_clip_x1;         /* 0x0081c8d8 */
extern int    g_clip_y1;         /* 0x0081c8dc */
extern char*  g_mouse_pixel;     /* 0x007fe9a8 */
extern int    g_raster_hit;      /* 0x007feb14 */
extern void*  g_texture;         /* 0x0066b630 */
extern void*  g_zbuf;            /* 0x00701e5c */
extern int    g_zbw;             /* 0x0066be40 */
extern int    g_zbh;             /* 0x0066be44 */

/* coaster5.c / coastermath.c: the ST(0) helpers' hooks and their C twins. */
extern void  FastSqrt_InitTables(void);     /* 0x00426b10 */
extern void  FastRSqrt_InitTables(void);    /* 0x004269e0 */
extern void* g_fast_sqrt;                   /* 0x00829a58 */
extern void* g_fast_rsqrt;                  /* 0x00829a5c */
extern float ll_FastSqrt(float);
extern float ll_FastRSqrt(float);

/* ---- the render target --------------------------------------------------- */

#define TSURF_W 64
#define TSURF_H 48
#define TSURF_PITCH (TSURF_W * 2)
#define TSENTINEL 0x1234

static unsigned short g_tsurf[TSURF_W * TSURF_H];
/* The Z buffer is addressed zbuf[(y << 9) + x*4] whatever the target's width,
 * so it needs the original's 0x200-byte rows. */
static unsigned int   g_tzbuf[128 * 120];

/* table[k] = 0x8000 | k, so a painted pixel reports the index that reached
 * it.  One ramp per texel VALUE for the textured pair. */
#define TRAMPS 256
static unsigned short g_ttable[TRAMPS][64];
static Shade          g_tramp[TRAMPS];
static Shade*         g_tramps[TRAMPS];

/* A ramp whose entry k IS k, so a Gouraud pixel reports the ramp index the
 * interpolated shade reached.  (The per-texel ramps above are flat per ramp,
 * which is what lets the TEXTURED pair report the texel instead.) */
static unsigned short g_tidxtable[64];
static Shade          g_tidxramp;

#define TTEX_W 16
#define TTEX_H 16
static unsigned char  g_ttexels[TTEX_W * TTEX_H];
static LLTexDesc      g_ttex;

static void tsurf_clear(void)
{
    int i;
    for (i = 0; i < TSURF_W * TSURF_H; i++)
        g_tsurf[i] = TSENTINEL;
}

static void tzbuf_clear(void)
{
    int i;
    for (i = 0; i < 128 * 120; i++)
        g_tzbuf[i] = 0;
}

static void tclip_full(void)
{
    g_clip_x0 = 0;
    g_clip_y0 = 0;
    g_clip_x1 = TSURF_W - 1;
    g_clip_y1 = TSURF_H - 1;
}

static int tpainted(void)
{
    int i, n = 0;
    for (i = 0; i < TSURF_W * TSURF_H; i++)
        if (g_tsurf[i] != TSENTINEL) n++;
    return n;
}

/* Every painted pixel must satisfy the box, and at least one must exist. */
static void tcheck_box(const char* what, int x0, int y0, int x1, int y1)
{
    int x, y, bad = 0, n = 0, bx = -1, by = -1;
    for (y = 0; y < TSURF_H; y++)
        for (x = 0; x < TSURF_W; x++)
            if (g_tsurf[y * TSURF_W + x] != TSENTINEL) {
                n++;
                if (x < x0 || x > x1 || y < y0 || y > y1) {
                    if (!bad) { bx = x; by = y; }
                    bad++;
                }
            }
    ll_checks++;
    if (n > 0 && bad == 0)
        ll_pass(what);
    else if (n == 0)
        ll_fail(what, "nothing was painted");
    else
        ll_fail(what, "%d pixel(s) outside, first at (%d,%d)", bad, bx, by);
}

/* Every painted pixel must hold `want`. */
static void tcheck_all(const char* what, unsigned short want)
{
    int i, n = 0, bad = -1;
    for (i = 0; i < TSURF_W * TSURF_H; i++)
        if (g_tsurf[i] != TSENTINEL) {
            n++;
            if (g_tsurf[i] != want && bad < 0) bad = i;
        }
    ll_checks++;
    if (n > 0 && bad < 0)
        ll_pass(what);
    else if (n == 0)
        ll_fail(what, "nothing was painted");
    else
        ll_fail(what, "pixel (%d,%d) is 0x%04x, want 0x%04x",
                bad % TSURF_W, bad / TSURF_W, g_tsurf[bad], want);
}

static void tv(Vertex2D* v, int x, int y, int z, float u, float vv, int shade)
{
    v->x = x << 16;
    v->y = y << 16;
    v->z = z;
    *(float*)&v->u = u;
    *(float*)&v->v = vv;
    v->shade = shade;
    v->pad18 = 0;
}

void test_tri_raster(void)
{
    Vertex2D a, b, c;
    int      i, k, row, col;
    int      first_row, last_row, idx0, idxN;

    /* ---- the reciprocal table ----------------------------------------- */
    BuildRecipTable();
    /* CORRECTED by scope PORT-M5.  The shipped BuildRecipTable converts with a
     * CALL to 0x00458930 (`0x00486564: fdivr qword ptr [0x4ab558] / call
     * 0x458930`), and that helper is a bare `fistp` under the game's
     * round-to-nearest control word -- so the table is ROUNDED, not truncated.
     * `65536 / 30` in C is 2184 and the shipped value is 2185; `65536 / 99` is
     * 661 and the shipped value is 662.  The C integer division was the wrong
     * expectation; see docs/lanes/scope-port-m5.md section 3b. */
    LL_CHECK_INT("g_recip[1] is 1.0 in 16.16", g_recip[1], 0x10000);
    LL_CHECK_INT("g_recip[30] is ROUND(65536/30)", g_recip[30], 2185);
    LL_CHECK_INT("g_recip[99] is ROUND(65536/99)", g_recip[99], 662);

    /* ---- the ramps and the texture ------------------------------------ */
    for (k = 0; k < TRAMPS; k++) {
        for (i = 0; i < 64; i++)
            g_ttable[k][i] = (unsigned short)(0x8000u | (unsigned)k);
        g_tramp[k].levels = 64;
        g_tramp[k].table = g_ttable[k];
        g_tramps[k] = &g_tramp[k];
    }
    for (i = 0; i < 64; i++)
        g_tidxtable[i] = (unsigned short)(0x8000u | (unsigned)i);
    g_tidxramp.levels = 64;
    g_tidxramp.table = g_tidxtable;

    /* A texel whose value is its ROW, so a pixel reports which row was read. */
    for (row = 0; row < TTEX_H; row++)
        for (col = 0; col < TTEX_W; col++)
            g_ttexels[(row << 4) + col] = (unsigned char)row;
    g_ttex.ushift = 4;                /* log2(16): the ROW shift */
    g_ttex.vshift = 4;
    g_ttex.texels = g_ttexels;
    g_ttex.ramps = g_tramps;
    g_ttex.umask = TTEX_W - 1;
    g_ttex.vmask = TTEX_H - 1;
    g_texture = &g_ttex;

    SetRenderTarget((char*)g_tsurf, TSURF_PITCH, TSURF_W, TSURF_H);
    g_zbuf = g_tzbuf;
    g_zbw = 128;
    g_zbh = 120;
    g_mouse_pixel = 0;
    g_raster_hit = 0;
    tclip_full();
    g_flat_colour = &g_tramp[7];      /* every entry 0x8007 */

    /* ---- 1. DrawFlatTri: coverage, and the destructive shade write ----- */
    /* A flat-topped triangle: a and b on row 5, c below a.  Edge A is the
     * vertical a->c, edge B the diagonal b->c, so the covered columns are
     * 10 .. 39 and the rows 5 .. 34. */
    tsurf_clear();
    tzbuf_clear();
    tv(&a, 10, 5, 0x1000, 0.0f, 0.0f, 0);
    tv(&b, 40, 5, 0x1000, 0.0f, 0.0f, 0);
    tv(&c, 10, 35, 0x1000, 0.0f, 0.0f, 0);
    DrawFlatTri(&a, &b, &c);
    tcheck_box("DrawFlatTri 0x004877b0 paints inside the triangle's box",
               10, 5, 39, 34);
    tcheck_all("every pixel is the flat ramp entry", 0x8007);
    LL_CHECK_TRUE("the triangle covers a plausible number of pixels",
                  tpainted() > 300 && tpainted() < 31 * 30);
    LL_CHECK_HEX("the first pixel of the top row is painted",
                 g_tsurf[5 * TSURF_W + 10], 0x8007);
    LL_CHECK_HEX("one column left of it is not",
                 g_tsurf[5 * TSURF_W + 9], TSENTINEL);

    /* `shl dword ptr [edx+0x14], 6` -- vertex a, BEFORE the sort, in the
     * CALLER's record.  Only this filler does it. */
    tv(&a, 10, 5, 0x1000, 0.0f, 0.0f, 0x123);
    tv(&b, 40, 5, 0x1000, 0.0f, 0.0f, 0x456);
    tv(&c, 10, 35, 0x1000, 0.0f, 0.0f, 0x789);
    tsurf_clear();
    tzbuf_clear();
    DrawFlatTri(&a, &b, &c);
    LL_CHECK_HEX("DrawFlatTri shifts the caller's vertex a shade left by 6",
                 a.shade, 0x123 << 6);
    LL_CHECK_HEX("...and leaves b alone", b.shade, 0x456);
    LL_CHECK_HEX("...and c", c.shade, 0x789);

    /* ---- 2. the clip rectangle ---------------------------------------- */
    tsurf_clear();
    tzbuf_clear();
    g_clip_x0 = 15; g_clip_x1 = 30;
    g_clip_y0 = 10; g_clip_y1 = 20;
    tv(&a, 10, 5, 0x1000, 0.0f, 0.0f, 0);
    tv(&b, 40, 5, 0x1000, 0.0f, 0.0f, 0);
    tv(&c, 10, 35, 0x1000, 0.0f, 0.0f, 0);
    DrawFlatTri(&a, &b, &c);
    tcheck_box("the clip rectangle bounds the span on all four sides",
               15, 10, 30, 20);
    tclip_full();

    /* ---- 3. the Z test ------------------------------------------------- */
    tsurf_clear();
    tzbuf_clear();
    tv(&a, 10, 5, 0x1000, 0.0f, 0.0f, 0);
    tv(&b, 40, 5, 0x1000, 0.0f, 0.0f, 0);
    tv(&c, 10, 35, 0x1000, 0.0f, 0.0f, 0);
    DrawFlatTri(&a, &b, &c);

    g_flat_colour = &g_tramp[9];
    tv(&a, 10, 5, 0x0800, 0.0f, 0.0f, 0);     /* FARTHER: must lose */
    tv(&b, 40, 5, 0x0800, 0.0f, 0.0f, 0);
    tv(&c, 10, 35, 0x0800, 0.0f, 0.0f, 0);
    DrawFlatTri(&a, &b, &c);
    LL_CHECK_HEX("a smaller Z key does not overwrite",
                 g_tsurf[6 * TSURF_W + 12], 0x8007);

    g_flat_colour = &g_tramp[11];
    tv(&a, 10, 5, 0x1000, 0.0f, 0.0f, 0);     /* EQUAL: must win */
    tv(&b, 40, 5, 0x1000, 0.0f, 0.0f, 0);
    tv(&c, 10, 35, 0x1000, 0.0f, 0.0f, 0);
    DrawFlatTri(&a, &b, &c);
    LL_CHECK_HEX("an equal Z key overwrites (the test is `jb`, so >= wins)",
                 g_tsurf[6 * TSURF_W + 12], 0x800b);

    g_flat_colour = &g_tramp[13];
    tv(&a, 10, 5, 0x2000, 0.0f, 0.0f, 0);     /* NEARER: must win */
    tv(&b, 40, 5, 0x2000, 0.0f, 0.0f, 0);
    tv(&c, 10, 35, 0x2000, 0.0f, 0.0f, 0);
    DrawFlatTri(&a, &b, &c);
    LL_CHECK_HEX("a larger Z key overwrites", g_tsurf[6 * TSURF_W + 12], 0x800d);
    g_flat_colour = &g_tramp[7];

    /* ---- 4. the mouse pick -------------------------------------------- */
    tsurf_clear();
    tzbuf_clear();
    g_raster_hit = 0;
    g_mouse_pixel = (char*)&g_tsurf[6 * TSURF_W + 12];
    tv(&a, 10, 5, 0x1000, 0.0f, 0.0f, 0);
    tv(&b, 40, 5, 0x1000, 0.0f, 0.0f, 0);
    tv(&c, 10, 35, 0x1000, 0.0f, 0.0f, 0);
    DrawFlatTri(&a, &b, &c);
    LL_CHECK_INT("a mouse on a painted pixel sets g_raster_hit",
                 g_raster_hit, 1);

    tsurf_clear();
    tzbuf_clear();
    g_raster_hit = 0;
    g_mouse_pixel = (char*)&g_tsurf[2 * TSURF_W + 2];   /* above the triangle */
    DrawFlatTri(&a, &b, &c);
    LL_CHECK_INT("a mouse outside the triangle does not", g_raster_hit, 0);
    g_mouse_pixel = 0;

    /* ---- 5. DrawGouraudTri -------------------------------------------- */
    /* The index ramp from here on, so the painted word IS the ramp index.
     * A shade of 0x400 is 0x10000 after the `<< 6`, so its high word -- the
     * ramp index -- is 1.  Three equal shades must paint ONE entry. */
    g_flat_colour = &g_tidxramp;
    tsurf_clear();
    tzbuf_clear();
    tv(&a, 10, 5, 0x1000, 0.0f, 0.0f, 0x400);
    tv(&b, 40, 5, 0x1000, 0.0f, 0.0f, 0x400);
    tv(&c, 10, 35, 0x1000, 0.0f, 0.0f, 0x400);
    DrawGouraudTri(&a, &b, &c);
    tcheck_all("DrawGouraudTri 0x00486590: three equal shades paint ONE "
               "ramp entry, index (shade << 6) >> 16", 0x8001);
    LL_CHECK_HEX("it does NOT shift the caller's vertex a", a.shade, 0x400);

    /* A gradient down the triangle: index 0 along the top edge, 20 at c. */
    tsurf_clear();
    tzbuf_clear();
    tv(&a, 10, 5, 0x1000, 0.0f, 0.0f, 0);
    tv(&b, 40, 5, 0x1000, 0.0f, 0.0f, 0);
    tv(&c, 10, 35, 0x1000, 0.0f, 0.0f, 0x400 * 20);
    DrawGouraudTri(&a, &b, &c);
    LL_CHECK_HEX("the top row takes vertex a's index",
                 g_tsurf[5 * TSURF_W + 12], 0x8000);
    first_row = g_tsurf[6 * TSURF_W + 11] & 0x7fff;
    last_row = g_tsurf[30 * TSURF_W + 10] & 0x7fff;
    LL_CHECK_TRUE("the ramp index grows down the triangle",
                  last_row > first_row && last_row <= 20);
    /* Monotone, column 10, which edge A holds at every row. */
    idx0 = -1;
    idxN = 1;
    for (i = 5; i < 35; i++) {
        unsigned short p = g_tsurf[i * TSURF_W + 10];
        if (p == TSENTINEL) continue;
        if ((int)(p & 0x7fff) < idx0) idxN = 0;
        idx0 = (int)(p & 0x7fff);
    }
    LL_CHECK_INT("and never goes back down", idxN, 1);

    /* ---- 6. DrawFlatTexTri: the texel address ------------------------- */
    /* v is CONSTANT 0.25 -- (0.25*65536 & 0xffff) << 4 = 0x40000, high word 4
     * -- while u sweeps 0 .. 0.9.  The texture's value is its ROW, so every
     * painted pixel must read row 4.  Were the address
     * texels[(u << ushift) + v], as the file header used to say, the value
     * would follow u and vary across the span. */
    tsurf_clear();
    tzbuf_clear();
    tv(&a, 10, 5, 0x1000, 0.0f, 0.25f, 0);
    tv(&b, 40, 5, 0x1000, 0.9f, 0.25f, 0);
    tv(&c, 10, 35, 0x1000, 0.0f, 0.25f, 0);
    DrawFlatTexTri(&a, &b, &c);
    tcheck_all("DrawFlatTexTri 0x00487d40 addresses texels["
               "((v>>16) << ushift) + (u>>16)]: a constant v over a sweeping "
               "u reads ONE texture row", 0x8004);
    LL_CHECK_HEX("it does not shift the caller's vertex a either",
                 a.shade, 0);

    /* Move v to row 9 and the whole triangle must follow. */
    tsurf_clear();
    tzbuf_clear();
    tv(&a, 10, 5, 0x1000, 0.0f, 9.0f / 16.0f, 0);
    tv(&b, 40, 5, 0x1000, 0.9f, 9.0f / 16.0f, 0);
    tv(&c, 10, 35, 0x1000, 0.0f, 9.0f / 16.0f, 0);
    DrawFlatTexTri(&a, &b, &c);
    tcheck_all("...and v = 9/16 reads row 9", 0x8009);

    /* One shade level for the whole triangle: vertex a's.  0x400 -> index 1,
     * and the ramp tables are flat, so the value is still the texel's. */
    tsurf_clear();
    tzbuf_clear();
    tv(&a, 10, 5, 0x1000, 0.0f, 0.25f, 0x400);
    tv(&b, 40, 5, 0x1000, 0.9f, 0.25f, 0);
    tv(&c, 10, 35, 0x1000, 0.0f, 0.25f, 0);
    DrawFlatTexTri(&a, &b, &c);
    tcheck_all("a flat shade taken from vertex a alone", 0x8004);

    /* ---- 7. DrawGouraudTexTri and the CROSSED masks -------------------- */
    /* Constant u = 5/16 and v = 6/16 so the span deltas are zero and the
     * sampled texel is decidable by hand.  The texel value is now
     * (row << 4) | col, so the pixel reports BOTH halves of the address. */
    for (row = 0; row < TTEX_H; row++)
        for (col = 0; col < TTEX_W; col++)
            g_ttexels[(row << 4) + col] = (unsigned char)((row << 4) | col);

    tsurf_clear();
    tzbuf_clear();
    g_ttex.umask = TTEX_W - 1;
    g_ttex.vmask = TTEX_H - 1;
    tv(&a, 10, 5, 0x1000, 5.0f / 16.0f, 6.0f / 16.0f, 0);
    tv(&b, 40, 5, 0x1000, 5.0f / 16.0f, 6.0f / 16.0f, 0);
    tv(&c, 10, 35, 0x1000, 5.0f / 16.0f, 6.0f / 16.0f, 0);
    DrawGouraudTexTri(&a, &b, &c);
    tcheck_all("DrawGouraudTexTri 0x00486c70 samples row 6, column 5",
               (unsigned short)(0x8000u | ((6 << 4) | 5)));

    /* Now a deliberately NON-SQUARE mask pair: umask 15, vmask 3, as a 16x4
     * texture would give.  The asm masks the ROW with umask and the COLUMN
     * with vmask, so the address is (6 & 15) << 4 | (5 & 3) = 0x61.  Masking
     * the way the field NAMES imply -- row & vmask, column & umask -- would
     * give (6 & 3) << 4 | (5 & 15) = 0x25.  The texel array is the full 16x16
     * either way, so the over-read stays inside the test's own buffer. */
    tsurf_clear();
    tzbuf_clear();
    g_ttex.umask = 15;
    g_ttex.vmask = 3;
    DrawGouraudTexTri(&a, &b, &c);
    tcheck_all("the two masks are CROSSED: the ROW is masked with umask and "
               "the COLUMN with vmask (0x61, not 0x25)",
               (unsigned short)(0x8000u | 0x61u));
    g_ttex.umask = TTEX_W - 1;
    g_ttex.vmask = TTEX_H - 1;

    /* A Gouraud shade gradient with a flat texture: the ramp tables are flat
     * per index, so the painted value follows the TEXEL, which is constant --
     * what this checks is that a varying shade does not disturb the address. */
    tsurf_clear();
    tzbuf_clear();
    tv(&c, 10, 35, 0x1000, 5.0f / 16.0f, 6.0f / 16.0f, 0x400 * 20);
    DrawGouraudTexTri(&a, &b, &c);
    tcheck_all("a shade gradient does not move the texel address",
               (unsigned short)(0x8000u | ((6 << 4) | 5)));

    /* ---- 8. the ST(0) helpers' hooks ---------------------------------- */
    /* FastSqrt (0x00426ab0) and FastRSqrt (0x00426980) take their argument in
     * ST(0) and cannot be given a C signature, so their portable arms are
     * traps.  They are UNREACHABLE, and this is what makes that checkable:
     * the two init functions are the only things in the image that name them,
     * and their portable arms install the C twins instead. */
    FastSqrt_InitTables();
    FastRSqrt_InitTables();
    LL_CHECK_TRUE("FastSqrt_InitTables installs ll_FastSqrt in g_fast_sqrt, "
                  "so the ST(0) FastSqrt is unreachable",
                  g_fast_sqrt == (void*)ll_FastSqrt);
    LL_CHECK_TRUE("FastRSqrt_InitTables installs ll_FastRSqrt, so the ST(0) "
                  "FastRSqrt is unreachable",
                  g_fast_rsqrt == (void*)ll_FastRSqrt);
    {
        int   bad_s = 0, bad_r = 0;
        float x;
        for (i = 1; i <= 40; i++) {
            x = (float)i * 0.37f;
            if (fabs((double)ll_FastSqrt(x) - sqrt((double)x)) > 0.01)
                bad_s++;
            if (fabs((double)ll_FastRSqrt(x) - 1.0 / sqrt((double)x)) > 0.01)
                bad_r++;
        }
        LL_CHECK_INT("ll_FastSqrt agrees with libm to the table's precision",
                     bad_s, 0);
        LL_CHECK_INT("ll_FastRSqrt agrees with libm to the table's precision",
                     bad_r, 0);
    }
}
