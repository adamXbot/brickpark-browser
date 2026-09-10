/* coaster_span -- the last three inline-asm span fillers.  Scope PORT-M5.
 *
 * `LEGOLAND/coastershade2.c` 0x0041fd80 Span_FillShade and 0x0041ff80
 * Span_FillShadeZ, and `LEGOLAND/coaster13.c` 0x00428860 TrackShade_FillPoly,
 * have hand-written `__asm` inner loops in the matching build; this test
 * exercises the C in their `#else` arms.  Span_FillFlat (0x0041f8d0), which was
 * ported earlier, is driven first as a check on the test's OWN plumbing: if the
 * key/edge list or the row seeding were wrong, the flat case would fail and
 * every judgement about the other three would be worthless.
 *
 * THE ORACLE IS NOT A SECOND SPAN FILLER (PORT-B3/B5's rule).  Re-deriving the
 * carry-chained shade walk here would share its bugs with the thing under
 * test.  What is checked instead is the set of arithmetic facts the asm states,
 * each of which fails differently:
 *
 *   - the span is [x0, x1] INCLUSIVE, and nothing outside it or outside the
 *     scanline range is touched (the negative-index loop `add ebx,1 / jle`
 *     off a pointer biased to x1 is easy to get wrong by one at either end);
 *   - the 48-bit shade accumulator: a dshade of exactly 0.5 must advance the
 *     clamp index on every SECOND pixel, which is a direct read of the
 *     `add eax,lo / adc ecx,hi` carry chain and not of any one instruction;
 *   - `flip` walks that index BACKWARDS at the same rate -- the negative arm
 *     uses `sbb` for the integer half while still ADDING the fraction, and the
 *     claim that this is rate-correct is exactly what the mirrored sequence
 *     proves;
 *   - the narrow arm (span < 0x8000) still steps the per-scanline shade;
 *   - Span_FillShade takes the integer shade UNSIGNED (`and ecx,0ffffh`) while
 *     Span_FillShadeZ takes it SIGNED (`sar ecx,16`), so a negative shade
 *     indexes BELOW g_shade_clamp_mid in the second and not in the first;
 *   - Span_FillShadeZ packs the Z into bits 0..23 of the same register as the
 *     shade fraction and clears bit 24 after every carry, so a Z carry cannot
 *     reach the shade;
 *   - the Z test is a 16-bit UNSIGNED `>=`: a smaller key loses, an equal key
 *     OVERWRITES, a larger key wins;
 *   - TrackShade_FillPoly's texel address is
 *     `texels[((v >> 20) << 4) + (u >> 20)]` for a 16x16 texture -- V is the
 *     ROW -- which is driven with a texture whose value depends only on its row
 *     and a span whose v is constant while u sweeps, the same way PORT-B5's
 *     tri_raster drives its FINDING 1;
 *   - and its colour comes from g_span_lut, which the prologue builds from
 *     `g_shade_tab[i][grad[0]]` for i < g_shade_count.
 *
 * The ramps are built so `ramp[k] == 0x8000 | k` and the clamp table so
 * `clamp_mid[k] == (unsigned char)k`, which makes every painted pixel report
 * the index that reached it.  g_span_lut is likewise built so a TrackShade
 * pixel reports its TEXEL value.
 *
 * Runs natively and on wasm32: no asset, no loader, no host shim.  The row
 * pointers, the pitch and the tables are pointed at this file's own arrays. */
#include "ll_tests.h"

/* coastershade2.c / coaster13.c's own types, copied so the test sees the same
 * layout.  SpanEdge is 0x30 and identical in both files. */
typedef struct SortKey { int y; int idx; } SortKey;
typedef struct SpanEdge {
    short y0;                  /* +0x00 */
    short y1;                  /* +0x02 */
    int   dir;                 /* +0x04  0 = the LEFT edge, non-zero = RIGHT */
    int   a[5];                /* +0x08  x, shade/u, z/v, z, - */
    int   d[5];                /* +0x1c  the per-scanline steps */
} SpanEdge;                    /* 0x30 */

extern void Span_FillFlat(int tag, int* grad, int n, SortKey* key, SpanEdge* e);
extern void Span_FillShade(int tag, int* grad, int n, SortKey* key, SpanEdge* e);
extern void Span_FillShadeZ(int tag, int* grad, int n, SortKey* key, SpanEdge* e);
extern void TrackShade_FillPoly(int tag, int* grad, int n, SortKey* key,
                                SpanEdge* e);

extern short*          g_raster_bits;      /* 0x004b5b20 */
extern short*          g_zb_base;          /* 0x004b5b24 */
extern int             g_zb_pitch;         /* 0x004b5b28 */
extern int             g_zb_polys;         /* 0x0060f900 */
extern unsigned short* g_shade_tab[];      /* 0x00829c60 */
extern unsigned char*  g_shade_clamp_mid;  /* 0x004d89c4 */
extern unsigned short* g_span_ramp;        /* 0x004d89c0 */
extern int             g_span_dshade_hi;   /* 0x004d89b4 */
extern int             g_span_dshade_lo;   /* 0x004d89bc */
extern void*           g_coaster_tab_c[];  /* 0x004d89c8 */
extern int             g_shade_count;      /* 0x0060f904 */
extern short           g_span_lut[];       /* 0x00611960 */
extern int             g_span_mask;        /* 0x0061195c */
extern int             g_span_tmask;       /* 0x00612174 */

/* ---- the render target ---------------------------------------------------- */

#define SW 48                      /* pixels per row */
#define SH 32                      /* rows */
#define SENTINEL ((short)0x0777)   /* "nothing wrote here" */
#define ZCLEAR   ((short)0x0100)   /* a mid z key, so both sides are testable */

static short g_csurf[SW * SH];
static short g_czbuf[SW * SH];

/* The shade ramp: one entry per clamp VALUE, ramp[k] = 0x8000 | k. */
#define NRAMP 256
static unsigned short g_ramp[NRAMP];

/* The clamp table, with `mid` in the middle so a NEGATIVE index is legal and
 * distinguishable: clamp[0x200 + k] = (unsigned char)k. */
static unsigned char g_clamp[0x400];

/* TrackShade's texture: {int w; int h; unsigned char texels[w*h]} and
 * CoasterTex_Get returns the head of it (coaster13.c adds 8 itself). */
#define TTEX_W 16
#define TTEX_H 16
static struct TTex {
    int           w;
    int           h;
    unsigned char texels[TTEX_W * TTEX_H];
} g_ttex;
#define TTAG 3                     /* the g_coaster_tab_c slot this test uses */

/* One ramp per texel VALUE for TrackShade's g_span_lut build: entry `i` of the
 * lut is `g_shade_tab[i][grad[0]]`, so a painted pixel reports its texel. */
static unsigned short g_lutsrc[NRAMP][2];

static SortKey  g_keys[8];
static SpanEdge g_edges[4];
static int      g_grad[8];

static void clear_target(void)
{
    int i;
    for (i = 0; i < SW * SH; i++) {
        g_csurf[i] = SENTINEL;
        g_czbuf[i] = ZCLEAR;
    }
}

static void arm_globals(void)
{
    g_raster_bits = g_csurf;
    g_zb_base     = g_czbuf;
    g_zb_pitch    = SW;
    g_shade_clamp_mid = &g_clamp[0x200];
}

/* A two-edge polygon: the LEFT edge carries (x, shade/u, z/v, z) as 16.16 with
 * per-scanline steps, the RIGHT edge only x.  Rows ytop..ybot inclusive.
 *
 * The interpolant homes are seeded `a - d` because every filler's span loop
 * does `ed[0] += ed[1]` BEFORE it paints, so the first scanline sees `a`. */
static void build(int ytop, int ybot, const int* la, const int* ld,
                  int rx, int rdx)
{
    int i;
    for (i = 0; i < 8; i++) { g_keys[i].y = 0; g_keys[i].idx = 0; }
    for (i = 0; i < 4; i++) {
        int j;
        g_edges[i].y0 = 0; g_edges[i].y1 = 0; g_edges[i].dir = 0;
        for (j = 0; j < 5; j++) { g_edges[i].a[j] = 0; g_edges[i].d[j] = 0; }
    }
    g_edges[0].dir = 0;
    g_edges[0].y0  = (short)ytop;
    g_edges[0].y1  = (short)ybot;
    for (i = 0; i < 5; i++) { g_edges[0].a[i] = la[i]; g_edges[0].d[i] = ld[i]; }
    g_edges[1].dir = 1;
    g_edges[1].y0  = (short)ytop;
    g_edges[1].y1  = (short)ybot;
    g_edges[1].a[0] = rx;
    g_edges[1].d[0] = rdx;
    g_keys[0].y = ytop; g_keys[0].idx = 0;
    g_keys[1].y = ytop; g_keys[1].idx = 1;
}

static short px(int x, int y) { return g_csurf[y * SW + x]; }
static short zx(int x, int y) { return g_czbuf[y * SW + x]; }

/* Every pixel outside the rectangle [x0,x1] x [y0,y1] still SENTINEL? */
static int only_inside(int x0, int x1, int y0, int y1)
{
    int x, y;
    for (y = 0; y < SH; y++)
        for (x = 0; x < SW; x++) {
            int in = (x >= x0 && x <= x1 && y >= y0 && y <= y1);
            if (!in && g_csurf[y * SW + x] != SENTINEL)
                return 0;
            if (in && g_csurf[y * SW + x] == SENTINEL)
                return 0;
        }
    return 1;
}

/* ---- the tests ------------------------------------------------------------ */

void test_coaster_span(void)
{
    int i, j;
    int la[5], ld[5];

    for (i = 0; i < NRAMP; i++)
        g_ramp[i] = (unsigned short)(0x8000 | i);
    for (i = 0; i < 0x400; i++)
        g_clamp[i] = (unsigned char)(i - 0x200);
    for (i = 0; i < NRAMP; i++) {
        g_lutsrc[i][0] = (unsigned short)(0x8000 | i);
        g_lutsrc[i][1] = 0xdead;
        g_shade_tab[i] = g_lutsrc[i];
    }
    arm_globals();

    /* ---- 0. the harness itself, through the already-ported flat filler ---- */
    clear_target();
    for (i = 0; i < 5; i++) { la[i] = 0; ld[i] = 0; }
    la[0] = 10 << 16;
    g_grad[0] = 1;                      /* g_shade_tab[tag][grad[0]] */
    g_shade_tab[1] = g_lutsrc[0];       /* tag 1 -> a pair we can read */
    build(4, 6, la, ld, 20 << 16, 0);
    Span_FillFlat(1, g_grad, 2, g_keys, g_edges);
    LL_CHECK_TRUE("flat: painted exactly rows 4..6, columns 10..20",
                  only_inside(10, 20, 4, 6));
    LL_CHECK_HEX("flat: the colour is g_shade_tab[tag][grad[0]]",
                 (unsigned short)px(15, 5), 0xdead);

    /* ---- 1. Span_FillShade: a flat shade ---------------------------------- */
    clear_target();
    for (i = 0; i < 5; i++) { la[i] = 0; ld[i] = 0; }
    la[0] = 12 << 16;
    la[1] = 9 << 16;                    /* shade 9.0, no per-scanline step */
    g_grad[0] = 0;
    g_grad[1] = 0;                      /* dshade = 0 */
    g_shade_tab[2] = g_ramp;
    build(8, 10, la, ld, 19 << 16, 0);
    Span_FillShade(2, g_grad, 2, g_keys, g_edges);
    LL_CHECK_TRUE("shade: painted exactly rows 8..10, columns 12..19",
                  only_inside(12, 19, 8, 10));
    LL_CHECK_HEX("shade: a zero dshade paints one ramp entry (clamp 9)",
                 (unsigned short)px(12, 9), 0x8000 | 9);
    LL_CHECK_HEX("shade: ... and the same at the right end",
                 (unsigned short)px(19, 9), 0x8000 | 9);

    /* ---- 2. dshade = 1.0 per pixel: the index steps every pixel ---------- */
    clear_target();
    la[1] = 0;                          /* shade 0.0 */
    g_grad[1] = 1 << 16;                /* +1.0 per pixel */
    build(8, 8, la, ld, 19 << 16, 0);
    Span_FillShade(2, g_grad, 2, g_keys, g_edges);
    {
        int ok = 1;
        for (i = 12; i <= 19; i++)
            if ((unsigned short)px(i, 8) != (unsigned short)(0x8000 | (i - 12)))
                ok = 0;
        LL_CHECK_TRUE("shade: dshade 1.0 -> index 0,1,2,... across the span", ok);
    }

    /* ---- 3. dshade = 0.5: the carry chain, one step every SECOND pixel --- */
    clear_target();
    la[1] = 0;
    g_grad[1] = 1 << 15;                /* +0.5 per pixel */
    build(8, 8, la, ld, 19 << 16, 0);
    Span_FillShade(2, g_grad, 2, g_keys, g_edges);
    {
        int ok = 1;
        for (i = 12; i <= 19; i++) {
            int want = (i - 12) / 2;
            if ((unsigned short)px(i, 8) != (unsigned short)(0x8000 | want))
                ok = 0;
        }
        LL_CHECK_TRUE("shade: dshade 0.5 -> 0,0,1,1,2,2,3,3 (the adc chain)",
                      ok);
    }

    /* ---- 4. flip: a negative dshade walks the index backwards ------------- */
    clear_target();
    la[1] = 40 << 16;                   /* start at 40 so it stays positive */
    g_grad[1] = -(1 << 15);             /* -0.5 per pixel */
    build(8, 8, la, ld, 19 << 16, 0);
    Span_FillShade(2, g_grad, 2, g_keys, g_edges);
    LL_CHECK_INT("shade: flip negated grad[1] in place", g_grad[1], 1 << 15);
    {
        int ok = 1;
        for (i = 12; i <= 19; i++) {
            int want = 40 - (i - 12) / 2;
            if ((unsigned short)px(i, 8) != (unsigned short)(0x8000 | want))
                ok = 0;
        }
        LL_CHECK_TRUE("shade: flip -> 40,40,39,39,38,38 (sbb at the same rate)",
                      ok);
    }

    /* ---- 5. the per-scanline shade step, and the narrow arm --------------- */
    clear_target();
    la[1] = 0;
    ld[1] = 4 << 16;                    /* +4.0 per scanline */
    g_grad[1] = 0;                      /* no per-pixel step */
    build(8, 10, la, ld, 19 << 16, 0);
    Span_FillShade(2, g_grad, 2, g_keys, g_edges);
    LL_CHECK_HEX("shade: row 8 starts at the left edge's own shade",
                 (unsigned short)px(12, 8), 0x8000 | 0);
    LL_CHECK_HEX("shade: row 9 is one per-scanline step along",
                 (unsigned short)px(12, 9), 0x8000 | 4);
    LL_CHECK_HEX("shade: row 10 is two", (unsigned short)px(12, 10),
                 0x8000 | 8);

    /* A span NARROWER than 0x8000 paints nothing but must still step the
     * shade, so the row after it is two steps along, not one. */
    clear_target();
    la[0] = 12 << 16;
    la[1] = 0;
    ld[0] = 0;
    ld[1] = 4 << 16;
    build(8, 10, la, ld, 19 << 16, 0);
    /* make the MIDDLE scanline narrow by walking the right edge in hard and
     * back out again: a per-scanline step of -8 pixels on the left edge. */
    g_edges[0].d[0] = 4 << 16;          /* left edge moves right 4/scanline */
    g_edges[1].a[0] = 19 << 16;
    g_edges[1].d[0] = -(4 << 16);       /* right edge moves left 4/scanline */
    Span_FillShade(2, g_grad, 2, g_keys, g_edges);
    /* row 8: 12..19, row 9: 16..15 (empty), row 10: 20..11 (empty).  The
     * narrow rows paint nothing, which is itself the check that the arm is
     * reached, and row 8's shade is the seeded one. */
    LL_CHECK_TRUE("shade: the narrow arm paints nothing on rows 9 and 10",
                  only_inside(12, 19, 8, 8));
    LL_CHECK_HEX("shade: ... and row 8 is unaffected by it",
                 (unsigned short)px(12, 8), 0x8000 | 0);

    /* ---- 6. Span_FillShadeZ: the Z keys and the shade --------------------- */
    clear_target();
    for (i = 0; i < 5; i++) { la[i] = 0; ld[i] = 0; }
    la[0] = 12 << 16;
    la[1] = 5 << 16;                    /* shade 5.0 */
    /* z = 0x0400.0000 in 16.16, so the key is 0x0400 -- ahead of ZCLEAR. */
    la[2] = 0x04000000;
    g_grad[0] = 0;
    g_grad[1] = 0;                      /* dshade = 0 */
    g_grad[2] = 0x00010000;             /* dz = 1.0 per pixel */
    g_shade_tab[4] = g_ramp;
    build(8, 9, la, ld, 19 << 16, 0);
    Span_FillShadeZ(4, g_grad, 2, g_keys, g_edges);
    LL_CHECK_TRUE("shadez: painted exactly rows 8..9, columns 12..19",
                  only_inside(12, 19, 8, 9));
    LL_CHECK_HEX("shadez: the colour is the clamped shade through the ramp",
                 (unsigned short)px(15, 8), 0x8000 | 5);
    {
        int ok = 1;
        for (i = 12; i <= 19; i++)
            if ((unsigned short)zx(i, 8) != (unsigned short)(0x400 + (i - 12)))
                ok = 0;
        LL_CHECK_TRUE("shadez: the z key written is (z >> 16), stepped by dz",
                      ok);
    }

    /* ---- 7. the Z test: smaller loses, equal overwrites, larger wins ------ */
    clear_target();
    la[2] = (int)((unsigned int)(ZCLEAR - 1) << 16);
    g_grad[2] = 0;                      /* dz = 0 */
    build(8, 8, la, ld, 19 << 16, 0);
    Span_FillShadeZ(4, g_grad, 2, g_keys, g_edges);
    LL_CHECK_HEX("shadez: a SMALLER z key does not draw",
                 (unsigned short)px(15, 8), (unsigned short)SENTINEL);
    LL_CHECK_HEX("shadez: ... and leaves the z buffer alone",
                 (unsigned short)zx(15, 8), (unsigned short)ZCLEAR);

    clear_target();
    la[2] = (int)((unsigned int)ZCLEAR << 16);
    build(8, 8, la, ld, 19 << 16, 0);
    Span_FillShadeZ(4, g_grad, 2, g_keys, g_edges);
    LL_CHECK_HEX("shadez: an EQUAL z key overwrites",
                 (unsigned short)px(15, 8), 0x8000 | 5);

    clear_target();
    la[2] = (int)((unsigned int)(ZCLEAR + 1) << 16);
    build(8, 8, la, ld, 19 << 16, 0);
    Span_FillShadeZ(4, g_grad, 2, g_keys, g_edges);
    LL_CHECK_HEX("shadez: a LARGER z key draws",
                 (unsigned short)px(15, 8), 0x8000 | 5);
    LL_CHECK_HEX("shadez: ... and writes itself into the z buffer",
                 (unsigned short)zx(15, 8), (unsigned short)(ZCLEAR + 1));

    /* ---- 8. the SIGNED integer shade, which Span_FillShade does not have -- */
    clear_target();
    la[1] = -(3 << 16);                 /* shade -3.0 */
    la[2] = (int)((unsigned int)(ZCLEAR + 1) << 16);
    g_grad[1] = 0;
    build(8, 8, la, ld, 19 << 16, 0);
    Span_FillShadeZ(4, g_grad, 2, g_keys, g_edges);
    LL_CHECK_HEX("shadez: a negative shade indexes BELOW g_shade_clamp_mid",
                 (unsigned short)px(15, 8),
                 (unsigned short)(0x8000 | g_clamp[0x200 - 3]));

    /* ---- 9. the packed accumulator: a z carry must not reach the shade ---- */
    /* dz is large enough that (z >> 8) overflows bit 23 inside the span, which
     * is the carry `and eax,0feffffffh` exists to swallow.  The shade index
     * must stay put across it. */
    clear_target();
    la[1] = 6 << 16;
    la[2] = (int)0xfff00000;            /* z >> 8 is near the top of 24 bits */
    g_grad[1] = 0;
    g_grad[2] = 0x00200000;             /* dz = 32.0 per pixel */
    build(8, 8, la, ld, 19 << 16, 0);
    Span_FillShadeZ(4, g_grad, 2, g_keys, g_edges);
    {
        int ok = 1;
        for (i = 12; i <= 19; i++) {
            unsigned short got = (unsigned short)px(i, 8);
            if (got != (unsigned short)(0x8000 | 6) &&
                got != (unsigned short)SENTINEL)
                ok = 0;
        }
        LL_CHECK_TRUE("shadez: a z carry out of bit 23 leaves the shade index "
                      "alone", ok);
    }

    /* ---- 10. Span_FillShadeZ's flip arm ---------------------------------- */
    clear_target();
    la[1] = 40 << 16;
    la[2] = (int)((unsigned int)(ZCLEAR + 1) << 16);
    g_grad[1] = -(1 << 16);             /* -1.0 per pixel */
    g_grad[2] = 0;
    build(8, 8, la, ld, 19 << 16, 0);
    Span_FillShadeZ(4, g_grad, 2, g_keys, g_edges);
    {
        int ok = 1;
        for (i = 12; i <= 19; i++)
            if ((unsigned short)px(i, 8) !=
                (unsigned short)(0x8000 | (40 - (i - 12))))
                ok = 0;
        LL_CHECK_TRUE("shadez: flip walks the clamp index backwards", ok);
    }

    /* ---- 11. TrackShade_FillPoly: the lut build and the texel address ---- */
    /* The texture's value is its ROW times 16, so it depends on v ONLY. */
    g_ttex.w = TTEX_W;
    g_ttex.h = TTEX_H;
    for (j = 0; j < TTEX_H; j++)
        for (i = 0; i < TTEX_W; i++)
            g_ttex.texels[j * TTEX_W + i] = (unsigned char)(j * 16);
    g_coaster_tab_c[TTAG] = &g_ttex;
    g_shade_count = NRAMP;
    for (i = 0; i < NRAMP; i++)
        g_shade_tab[i] = g_lutsrc[i];

    clear_target();
    for (i = 0; i < 5; i++) { la[i] = 0; ld[i] = 0; }
    la[0] = 12 << 16;                   /* x */
    la[1] = 0;                          /* u = 0.0 in 12.20 */
    la[2] = 6 << 20;                    /* v = 6 (the texture ROW)           */
    la[3] = (int)((unsigned int)(ZCLEAR + 1) << 16);  /* z, which must draw  */
    g_grad[0] = 0;                      /* the lut column */
    g_grad[1] = 1 << 20;                /* du = 1 texel per pixel */
    g_grad[2] = 0;                      /* dv = 0 */
    g_grad[3] = 0;                      /* dz = 0 */
    build(8, 9, la, ld, 19 << 16, 0);
    TrackShade_FillPoly(TTAG, g_grad, 2, g_keys, g_edges);
    LL_CHECK_HEX("track: g_span_lut[i] is g_shade_tab[i][grad[0]]",
                 (unsigned short)g_span_lut[6 * 16], 0x8000 | (6 * 16));
    LL_CHECK_INT("track: the shifts came out of the texture header "
                 "(tmask = w*h - 1)", g_span_tmask, TTEX_W * TTEX_H - 1);
    LL_CHECK_TRUE("track: painted exactly rows 8..9, columns 12..19",
                  only_inside(12, 19, 8, 9));
    {
        /* v constant, u sweeping: a correct address gives ONE value for the
         * whole span; a transposed one would vary with u. */
        int ok = 1;
        for (i = 12; i <= 19; i++)
            if ((unsigned short)px(i, 8) != (unsigned short)(0x8000 | (6 * 16)))
                ok = 0;
        LL_CHECK_TRUE("track: V is the ROW -- u sweeps and the texel does not "
                      "change", ok);
    }

    /* Now the other way round: u constant, v sweeping, so the value MUST
     * change with v and by exactly one row per pixel. */
    clear_target();
    la[1] = 3 << 20;                    /* u = 3, fixed */
    la[2] = 0;                          /* v = 0 */
    g_grad[1] = 0;                      /* du = 0 */
    g_grad[2] = 1 << 20;                /* dv = 1 texel per pixel */
    build(8, 8, la, ld, 19 << 16, 0);
    TrackShade_FillPoly(TTAG, g_grad, 2, g_keys, g_edges);
    {
        int ok = 1;
        for (i = 12; i <= 19; i++)
            if ((unsigned short)px(i, 8) !=
                (unsigned short)(0x8000 | ((i - 12) * 16)))
                ok = 0;
        LL_CHECK_TRUE("track: ... and the texel follows v, one row per pixel",
                      ok);
    }

    /* The column half, read off a texture whose value is its COLUMN. */
    for (j = 0; j < TTEX_H; j++)
        for (i = 0; i < TTEX_W; i++)
            g_ttex.texels[j * TTEX_W + i] = (unsigned char)i;
    clear_target();
    la[1] = 0;                          /* u = 0 */
    la[2] = 5 << 20;                    /* v = 5, fixed */
    g_grad[1] = 1 << 20;                /* du = 1 texel per pixel */
    g_grad[2] = 0;
    build(8, 8, la, ld, 19 << 16, 0);
    TrackShade_FillPoly(TTAG, g_grad, 2, g_keys, g_edges);
    {
        int ok = 1;
        for (i = 12; i <= 19; i++)
            if ((unsigned short)px(i, 8) !=
                (unsigned short)(0x8000 | (i - 12)))
                ok = 0;
        LL_CHECK_TRUE("track: U is the fast axis, one column per pixel", ok);
    }

    /* g_span_tmask wraps the whole address, so column 16 is column 0 of the
     * NEXT row -- a 16x16 texture with a column-valued image makes that
     * visible as a wrap back to 0. */
    clear_target();
    la[1] = 14 << 20;                   /* u = 14, so the span crosses 16 */
    build(8, 8, la, ld, 19 << 16, 0);
    TrackShade_FillPoly(TTAG, g_grad, 2, g_keys, g_edges);
    LL_CHECK_HEX("track: u = 15 is column 15",
                 (unsigned short)px(13, 8), 0x8000 | 15);
    LL_CHECK_HEX("track: u = 16 wraps to column 0 (g_span_tmask)",
                 (unsigned short)px(14, 8), 0x8000 | 0);

    /* ---- 12. TrackShade_FillPoly's Z test --------------------------------- */
    for (j = 0; j < TTEX_H; j++)
        for (i = 0; i < TTEX_W; i++)
            g_ttex.texels[j * TTEX_W + i] = 0x11;
    la[1] = 0;
    la[2] = 0;
    g_grad[1] = 0;
    g_grad[2] = 0;

    clear_target();
    la[3] = (int)((unsigned int)(ZCLEAR - 1) << 16);
    build(8, 8, la, ld, 19 << 16, 0);
    TrackShade_FillPoly(TTAG, g_grad, 2, g_keys, g_edges);
    LL_CHECK_HEX("track: a SMALLER z key does not draw",
                 (unsigned short)px(15, 8), (unsigned short)SENTINEL);
    LL_CHECK_HEX("track: ... and leaves the z buffer alone",
                 (unsigned short)zx(15, 8), (unsigned short)ZCLEAR);

    clear_target();
    la[3] = (int)((unsigned int)ZCLEAR << 16);
    build(8, 8, la, ld, 19 << 16, 0);
    TrackShade_FillPoly(TTAG, g_grad, 2, g_keys, g_edges);
    LL_CHECK_HEX("track: an EQUAL z key overwrites",
                 (unsigned short)px(15, 8), 0x8000 | 0x11);

    clear_target();
    la[3] = (int)((unsigned int)(ZCLEAR + 3) << 16);
    build(8, 8, la, ld, 19 << 16, 0);
    TrackShade_FillPoly(TTAG, g_grad, 2, g_keys, g_edges);
    LL_CHECK_HEX("track: a LARGER z key draws",
                 (unsigned short)px(15, 8), 0x8000 | 0x11);
    LL_CHECK_HEX("track: ... and writes itself into the z buffer",
                 (unsigned short)zx(15, 8), (unsigned short)(ZCLEAR + 3));

    /* ---- 13. the per-pixel z step --------------------------------------- */
    clear_target();
    la[3] = (int)((unsigned int)(ZCLEAR + 1) << 16);
    g_grad[3] = 1 << 16;                /* dz = 1.0 per pixel */
    build(8, 8, la, ld, 19 << 16, 0);
    TrackShade_FillPoly(TTAG, g_grad, 2, g_keys, g_edges);
    {
        int ok = 1;
        for (i = 12; i <= 19; i++)
            if ((unsigned short)zx(i, 8) !=
                (unsigned short)(ZCLEAR + 1 + (i - 12)))
                ok = 0;
        LL_CHECK_TRUE("track: the z key steps by g_span_dz across the span",
                      ok);
    }
    LL_CHECK_TRUE("all three fillers bumped g_zb_polys", g_zb_polys > 0);
}
