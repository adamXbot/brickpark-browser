/* rle_paint -- the ten hand-written type-3 RLE painters.  Scope PORT-B3.
 *
 * The painters under test are `LEGOLAND/rlepaint.c`'s eight
 * (RLEPaintFast/Hit and the six clip variants) and `LEGOLAND/rlepaint2.c`'s
 * two (recolour and highlight).  In the matching build they are
 * `__declspec(naked)` asm; this test exercises the C in their
 * `#else` arms.
 *
 * WHY THE EXPECTATION IS INDEPENDENT.  The sprite is declared once as a list
 * of OPERATIONS per row (one pixel, one transparent pixel, a literal run, a
 * repeat run, a transparent run).  Two unrelated routines read that list:
 *
 *   emit_frame()  builds the A/B/C byte streams the painters consume --
 *                 u16 pixels, u8 run lengths, and 2-bit codes packed 16 to a
 *                 dword, LSB first;
 *   expect()      writes the pixels straight into a reference image at the
 *                 column each operation lands on, and applies only the clip
 *                 rule (which source columns a given leaf makes visible) and
 *                 the recolour mask.
 *
 * So a painter passes only if the encode, the rotating-mask decode, the run
 * arithmetic, the left/right clipping and the row stepping all agree with a
 * direct construction of the same picture.  Nothing here is derived from the
 * painters themselves.
 *
 * Runs natively and on wasm32: the frame the test builds is its own memory and
 * the surface is a local array, so no serialised layout and no `long` field is
 * involved.  No host shim either -- a painter touches nothing but its
 * arguments, `g_blit_hit` and `g_sp_recolour`. */
#include "ll_tests.h"
#include <stdlib.h>
#include "oracle_rlepaint.h"

static void real_sprite(void);

/* ---- the painters, declared exactly as softblit2.c declares them --------- */
extern void RLEPaintHitClipLR(void* dst, void* a, void* b, void* c, int h,
                              int pitch, int top, int left, int w, int spare,
                              void* mouse);                  /* 0x00466d80 */
extern void RLEPaintHitClipL (void* dst, void* a, void* b, void* c, int h,
                              int pitch, int top, int left, int w, int spare,
                              void* mouse);                  /* 0x00467180 */
extern void RLEPaintHitClipR (void* dst, void* a, void* b, void* c, int h,
                              int pitch, int top, int left, int w, int spare,
                              void* mouse);                  /* 0x004673f0 */
extern void RLEPaintHit      (void* dst, void* a, void* b, void* c, int h,
                              int pitch, int top, int left, int w, int spare,
                              void* mouse);                  /* 0x00467640 */
extern void RLEPaintClipLR   (void* dst, void* a, void* b, void* c, int h,
                              int pitch, int top, int left, int w, int spare,
                              void* mouse);                  /* 0x004677b0 */
extern void RLEPaintClipL    (void* dst, void* a, void* b, void* c, int h,
                              int pitch, int top, int left, int w, int spare,
                              void* mouse);                  /* 0x00467b00 */
extern void RLEPaintClipR    (void* dst, void* a, void* b, void* c, int h,
                              int pitch, int top, int left, int w, int spare,
                              void* mouse);                  /* 0x00467d10 */
extern void RLEPaintFast     (void* dst, void* a, void* b, void* c, int h,
                              int pitch, int top);           /* 0x00467f00 */
extern void SoftBlitRLEFrameRecolour(void* dst, void* a, void* b, void* c,
                              int h, int pitch, int top, int left, int w,
                              int spare, void* mouse);       /* 0x00468040 */
extern void SoftBlitRLEFrame (void* dst, void* a, void* b, void* c, int h,
                              int pitch, int top, int left, int w, int spare,
                              void* mouse);                  /* 0x00468410 */

extern int g_blit_hit;      /* 0x007feb14 */
extern int g_sp_recolour;   /* 0x007fe998 */

/* ---- the synthetic sprite ------------------------------------------------ */

#define OP_PIX0   0   /* primary code 0: one literal word                    */
#define OP_PIX1   1   /* primary code 1: one literal word (the defect code)  */
#define OP_SKIP1  2   /* primary code 2: one transparent pixel               */
#define OP_LITRUN 3   /* primary 3 + length + secondary 0: a literal run     */
#define OP_REPRUN 4   /* primary 3 + length + secondary 1: a repeated word   */
#define OP_SKIPRUN 5  /* primary 3 + length + secondary 2: a transparent run */
#define OP_END    6   /* not in the stream: the end of this row's op list     */

typedef struct Op {
    int            kind;
    int            len;      /* OP_LITRUN / OP_REPRUN / OP_SKIPRUN */
    unsigned short pix[12];  /* the words, in column order            */
} Op;

#define SPR_W 12
#define SPR_H 8

/* Eight rows of twelve columns, covering every grammar case and the two
 * interesting degenerate rows (wholly transparent, wholly one run). */
static const Op g_rows[SPR_H][8] = {
    /* 0 */ { { OP_PIX0, 0, { 0xa001 } },
              { OP_PIX0, 0, { 0xa002 } },
              { OP_SKIP1, 0, { 0 } },
              { OP_LITRUN, 3, { 0xb001, 0xb002, 0xb003 } },
              { OP_REPRUN, 2, { 0xc001 } },
              { OP_SKIPRUN, 2, { 0 } },
              { OP_PIX0, 0, { 0xa003 } },
              { OP_END, 0, { 0 } } },
    /* 1 */ { { OP_REPRUN, 12, { 0xc002 } },
              { OP_END, 0, { 0 } } },
    /* 2 */ { { OP_SKIPRUN, 12, { 0 } },
              { OP_END, 0, { 0 } } },
    /* 3 */ { { OP_LITRUN, 12, { 0xd000, 0xd001, 0xd002, 0xd003, 0xd004,
                                 0xd005, 0xd006, 0xd007, 0xd008, 0xd009,
                                 0xd00a, 0xd00b } },
              { OP_END, 0, { 0 } } },
    /* 4 */ { { OP_SKIP1, 0, { 0 } },
              { OP_REPRUN, 10, { 0xc003 } },
              { OP_PIX0, 0, { 0xa004 } },
              { OP_END, 0, { 0 } } },
    /* 5 */ { { OP_LITRUN, 4, { 0xe000, 0xe001, 0xe002, 0xe003 } },
              { OP_SKIPRUN, 4, { 0 } },
              { OP_LITRUN, 4, { 0xe004, 0xe005, 0xe006, 0xe007 } },
              { OP_END, 0, { 0 } } },
    /* 6 */ { { OP_REPRUN, 6, { 0xc005 } },
              { OP_LITRUN, 6, { 0xf000, 0xf001, 0xf002, 0xf003, 0xf004,
                                0xf005 } },
              { OP_END, 0, { 0 } } },
    /* 7 */ { { OP_PIX0, 0, { 0xa005 } },
              { OP_SKIPRUN, 10, { 0 } },
              { OP_PIX0, 0, { 0xa006 } },
              { OP_END, 0, { 0 } } },
};

/* ---- the encoder: an op list -> the A/B/C blocks ------------------------- */

static unsigned short g_blk_a[SPR_W * SPR_H + 16];  /* u16 pixels      */
static unsigned char  g_blk_b[SPR_W * SPR_H + 16];  /* u8 run lengths  */
static unsigned char  g_blk_c[SPR_W * SPR_H + 64];  /* 2-bit codes     */
static unsigned int   g_na, g_nb, g_nc;

static void put_code(unsigned int code)
{
    /* 16 codes per dword, LSB first: code i sits at bits 2*(i%16) of word
     * i/16, which is byte i/4 bit 2*(i%4).  Same addressing the painters'
     * rotating mask produces. */
    g_blk_c[g_nc >> 2] |= (unsigned char)((code & 3u) << (2 * (g_nc & 3u)));
    g_nc++;
}

static void emit_frame(const Op rows[SPR_H][8], int nrows)
{
    int r, i, k;

    for (i = 0; i < (int)sizeof g_blk_c; i++) g_blk_c[i] = 0;
    g_na = g_nb = g_nc = 0;

    for (r = 0; r < nrows; r++) {
        for (i = 0; rows[r][i].kind != OP_END; i++) {
            const Op* op = &rows[r][i];
            switch (op->kind) {
            case OP_PIX0:
                put_code(0);
                g_blk_a[g_na++] = op->pix[0];
                break;
            case OP_PIX1:
                put_code(1);
                g_blk_a[g_na++] = op->pix[0];
                break;
            case OP_SKIP1:
                put_code(2);
                break;
            case OP_LITRUN:
                put_code(3);
                g_blk_b[g_nb++] = (unsigned char)op->len;
                put_code(0);
                for (k = 0; k < op->len; k++)
                    g_blk_a[g_na++] = op->pix[k];
                break;
            case OP_REPRUN:
                put_code(3);
                g_blk_b[g_nb++] = (unsigned char)op->len;
                put_code(1);
                g_blk_a[g_na++] = op->pix[0];
                break;
            case OP_SKIPRUN:
                put_code(3);
                g_blk_b[g_nb++] = (unsigned char)op->len;
                put_code(2);
                break;
            default:
                break;
            }
        }
        put_code(3);                /* the end-of-row marker: code 3 ... */
        g_blk_b[g_nb++] = 0;        /* ... with a zero length            */
    }
}

/* ---- the reference image: the same op list, drawn directly --------------- */

#define SURF_W 24
#define SURF_H 10
#define SURF_PITCH (SURF_W * 2)
#define SENTINEL 0x1234            /* what an untouched surface pixel holds */

static unsigned short g_surf[SURF_W * SURF_H];
static unsigned short g_want[SURF_W * SURF_H];

static void surf_fill(unsigned short* s)
{
    int i;
    for (i = 0; i < SURF_W * SURF_H; i++)
        s[i] = SENTINEL;
}

/* `vis_lo`/`vis_hi` are the half-open range of SOURCE columns the leaf makes
 * visible; `shift` is the highlight painter's extra `shr ax,1`. */
static void expect(const Op rows[SPR_H][8], int top, int h,
                   int vis_lo, int vis_hi, unsigned int mask, int shift)
{
    int r, i, k, x, y;

    surf_fill(g_want);
    for (y = 0; y < h; y++) {
        r = top + y;
        x = 0;
        for (i = 0; rows[r][i].kind != OP_END; i++) {
            const Op* op = &rows[r][i];
            int n = 1;
            if (op->kind == OP_LITRUN || op->kind == OP_REPRUN
             || op->kind == OP_SKIPRUN)
                n = op->len;
            for (k = 0; k < n; k++, x++) {
                unsigned int v;
                if (op->kind == OP_SKIP1 || op->kind == OP_SKIPRUN)
                    continue;                      /* transparent: untouched */
                if (x < vis_lo || x >= vis_hi)
                    continue;                      /* clipped away           */
                v = op->kind == OP_LITRUN ? op->pix[k] : op->pix[0];
                v &= mask;
                if (shift)
                    v = (v & 0xffffu) >> 1;
                g_want[y * SURF_W + x] = (unsigned short)v;
            }
        }
    }
}

static void compare(const char* what)
{
    int i, bad = -1;
    ll_checks++;
    for (i = 0; i < SURF_W * SURF_H; i++) {
        if (g_surf[i] != g_want[i]) { bad = i; break; }
    }
    if (bad < 0)
        ll_pass(what);
    else
        ll_fail(what, "pixel (%d,%d): got 0x%04x, want 0x%04x",
                bad % SURF_W, bad / SURF_W, g_surf[bad], g_want[bad]);
}

/* ---- the run -------------------------------------------------------------- */

#define TOP  2        /* source rows 0..1 consumed by the top-skip pass */
#define ROWS 5        /* source rows 2..6 painted to surface rows 0..4  */
#define LEFT 3        /* the left clip                                   */
#define VISW 6        /* the visible width                               */
#define MASK 0x7befu  /* a recolour mask that changes every test pixel   */

static void* row0(void) { return (void*)g_surf; }

void test_rle_paint(void)
{
    void* a;
    void* b;
    void* c;
    void* mouse_drawn;
    void* mouse_clear;
    int   hit_fast;
    int   hit_clip;

    emit_frame(g_rows, SPR_H);
    a = (void*)g_blk_a;
    b = (void*)g_blk_b;
    c = (void*)g_blk_c;

    /* The encoder itself: 39 pixel words, 21 length bytes (one per run plus
     * one per row), 42 codes.  Counted from g_rows by hand, so a mistake in
     * emit_frame cannot hide behind a matching mistake in expect(). */
    LL_CHECK_INT("A holds 39 pixel words", g_na, 39);
    LL_CHECK_INT("B holds 21 length bytes", g_nb, 21);
    LL_CHECK_INT("C holds 42 two-bit codes", g_nc, 42);

    /* ---- 1. the unclipped leaves ---------------------------------------- */
    g_blit_hit = 0;
    surf_fill(g_surf);
    RLEPaintFast(row0(), a, b, c, ROWS, SURF_PITCH, TOP);
    expect(g_rows, TOP, ROWS, 0, SPR_W, 0xffffu, 0);
    compare("RLEPaintFast 0x00467f00 paints the whole sprite");
    LL_CHECK_INT("RLEPaintFast never touches g_blit_hit", g_blit_hit, 0);

    /* The same picture through the hit leaf, with the mouse over a pixel the
     * sprite does NOT write (surface row 0 is source row 2, all transparent). */
    mouse_clear = (void*)&g_surf[0 * SURF_W + 4];
    mouse_drawn = (void*)&g_surf[1 * SURF_W + 5];   /* inside row 3's run */
    g_blit_hit = 0;
    surf_fill(g_surf);
    RLEPaintHit(row0(), a, b, c, ROWS, SURF_PITCH, TOP, 0, SPR_W, 0,
                mouse_clear);
    compare("RLEPaintHit 0x00467640 paints the same picture");
    LL_CHECK_INT("a mouse on a transparent pixel does not hit", g_blit_hit, 0);

    g_blit_hit = 0;
    surf_fill(g_surf);
    RLEPaintHit(row0(), a, b, c, ROWS, SURF_PITCH, TOP, 0, SPR_W, 0,
                mouse_drawn);
    compare("RLEPaintHit again, mouse inside a drawn run");
    LL_CHECK_INT("a mouse inside a drawn run hits", g_blit_hit, 1);
    hit_fast = g_blit_hit;

    /* ---- 2. right clip only --------------------------------------------- */
    g_blit_hit = 0;
    surf_fill(g_surf);
    RLEPaintClipR(row0(), a, b, c, ROWS, SURF_PITCH, TOP, 0, VISW, 0, 0);
    expect(g_rows, TOP, ROWS, 0, VISW, 0xffffu, 0);
    compare("RLEPaintClipR 0x00467d10 keeps source columns 0..5");
    LL_CHECK_INT("RLEPaintClipR never touches g_blit_hit", g_blit_hit, 0);

    g_blit_hit = 0;
    surf_fill(g_surf);
    RLEPaintHitClipR(row0(), a, b, c, ROWS, SURF_PITCH, TOP, 0, VISW, 0,
                     mouse_drawn);
    compare("RLEPaintHitClipR 0x004673f0 paints the same");
    LL_CHECK_INT("HitClipR hits inside the visible span", g_blit_hit, 1);

    /* A mouse in the CLIPPED-AWAY tail must not hit: column 9 of surface row 1
     * is past the 6-pixel budget, so nothing is written there. */
    g_blit_hit = 0;
    surf_fill(g_surf);
    RLEPaintHitClipR(row0(), a, b, c, ROWS, SURF_PITCH, TOP, 0, VISW, 0,
                     (void*)&g_surf[1 * SURF_W + 9]);
    compare("RLEPaintHitClipR, mouse in the clipped tail");
    LL_CHECK_INT("a mouse in the clipped right tail does not hit",
                 g_blit_hit, 0);

    /* ---- 3. left clip only ---------------------------------------------- */
    g_blit_hit = 0;
    surf_fill(g_surf);
    RLEPaintClipL(row0(), a, b, c, ROWS, SURF_PITCH, TOP, LEFT, SPR_W, 0, 0);
    expect(g_rows, TOP, ROWS, LEFT, SPR_W, 0xffffu, 0);
    compare("RLEPaintClipL 0x00467b00 keeps source columns 3..11");
    LL_CHECK_INT("RLEPaintClipL never touches g_blit_hit", g_blit_hit, 0);

    g_blit_hit = 0;
    surf_fill(g_surf);
    RLEPaintHitClipL(row0(), a, b, c, ROWS, SURF_PITCH, TOP, LEFT, SPR_W, 0,
                     mouse_drawn);
    compare("RLEPaintHitClipL 0x00467180 paints the same");
    LL_CHECK_INT("HitClipL hits inside a left-split remainder", g_blit_hit, 1);

    /* Column 1 of surface row 1 is inside the left clip: written by nobody. */
    g_blit_hit = 0;
    surf_fill(g_surf);
    RLEPaintHitClipL(row0(), a, b, c, ROWS, SURF_PITCH, TOP, LEFT, SPR_W, 0,
                     (void*)&g_surf[1 * SURF_W + 1]);
    compare("RLEPaintHitClipL, mouse inside the left clip");
    LL_CHECK_INT("a mouse inside the left clip does not hit", g_blit_hit, 0);

    /* ---- 4. both edges -------------------------------------------------- */
    g_blit_hit = 0;
    surf_fill(g_surf);
    RLEPaintClipLR(row0(), a, b, c, ROWS, SURF_PITCH, TOP, LEFT, VISW, 0, 0);
    expect(g_rows, TOP, ROWS, LEFT, LEFT + VISW, 0xffffu, 0);
    compare("RLEPaintClipLR 0x004677b0 keeps source columns 3..8");
    LL_CHECK_INT("RLEPaintClipLR never touches g_blit_hit", g_blit_hit, 0);

    g_blit_hit = 0;
    surf_fill(g_surf);
    RLEPaintHitClipLR(row0(), a, b, c, ROWS, SURF_PITCH, TOP, LEFT, VISW, 0,
                      mouse_drawn);
    compare("RLEPaintHitClipLR 0x00466d80 paints the same");
    LL_CHECK_INT("HitClipLR hits inside the doubly-clipped span",
                 g_blit_hit, 1);
    hit_clip = g_blit_hit;
    LL_CHECK_INT("every Hit leaf agrees on the same mouse pixel",
                 hit_clip, hit_fast);

    /* ---- 5. recolour and highlight -------------------------------------- */
    /* Surface row 3 is source row 5: a 4-pixel literal run at columns 0..3, a
     * 4-pixel transparent run, then a 4-pixel literal run at columns 8..11.
     * With LEFT=3 and VISW=6 the first run is split by the left edge, and the
     * second STARTS inside the visible loop at column 8 with one pixel of
     * budget left -- which is where the recolour leaves do their single hit
     * test, and they do it with the run's ORIGINAL length of 4. */
    g_sp_recolour = (int)MASK;
    g_blit_hit = 0;
    surf_fill(g_surf);
    SoftBlitRLEFrameRecolour(row0(), a, b, c, ROWS, SURF_PITCH, TOP, LEFT,
                             VISW, 0, (void*)&g_surf[3 * SURF_W + 8]);
    expect(g_rows, TOP, ROWS, LEFT, LEFT + VISW, MASK, 0);
    compare("SoftBlitRLEFrameRecolour 0x00468040 masks every store");
    LL_CHECK_INT("the recolour leaf hits on a run it starts drawing",
                 g_blit_hit, 1);

    g_blit_hit = 0;
    surf_fill(g_surf);
    SoftBlitRLEFrame(row0(), a, b, c, ROWS, SURF_PITCH, TOP, LEFT, VISW, 0,
                     (void*)&g_surf[3 * SURF_W + 8]);
    expect(g_rows, TOP, ROWS, LEFT, LEFT + VISW, MASK, 1);
    compare("SoftBlitRLEFrame 0x00468410 masks then halves every store");
    LL_CHECK_INT("the highlight leaf hits on a run it starts drawing",
                 g_blit_hit, 1);

    /* DIVERGENCE 1 (docs/runtime/presentation-data.md §"Original leaf
     * quirks"): the recolour leaves test the run's length BEFORE the right
     * edge cuts it, so a mouse in the unwritten right tail still sets the
     * flag.  Column 10 of surface row 3 is inside that 4-pixel run but past
     * the one pixel of budget, so it is never written -- the recolour leaf
     * hits there and the plain Hit leaf does not. */
    g_blit_hit = 0;
    surf_fill(g_surf);
    SoftBlitRLEFrameRecolour(row0(), a, b, c, ROWS, SURF_PITCH, TOP, LEFT,
                             VISW, 0, (void*)&g_surf[3 * SURF_W + 10]);
    LL_CHECK_INT("the recolour leaf hits past the pixels it wrote",
                 g_blit_hit, 1);
    g_blit_hit = 0;
    surf_fill(g_surf);
    RLEPaintHitClipLR(row0(), a, b, c, ROWS, SURF_PITCH, TOP, LEFT, VISW, 0,
                      (void*)&g_surf[3 * SURF_W + 10]);
    LL_CHECK_INT("the plain Hit leaf tests only what it wrote", g_blit_hit, 0);

    /* DIVERGENCE 2: the recolour leaves do not hit-test the remainder of a run
     * that crosses the LEFT clip.  `mouse_drawn` sits in exactly such a
     * remainder (surface row 1 is source row 3, one 12-pixel run cut at both
     * edges), where RLEPaintHitClipLR hit above. */
    g_blit_hit = 0;
    surf_fill(g_surf);
    SoftBlitRLEFrameRecolour(row0(), a, b, c, ROWS, SURF_PITCH, TOP, LEFT,
                             VISW, 0, mouse_drawn);
    LL_CHECK_INT("the recolour leaf does NOT hit a left-split remainder",
                 g_blit_hit, 0);

    /* DIVERGENCE 3: the recolour leaves never test a singleton against the
     * mouse (the asm has no `cmp eax,edi` in the literal-single path).
     * Surface row 5 is source row 7: a singleton at column 0, a 10-pixel
     * transparent run, a singleton at column 11. */
    g_blit_hit = 0;
    surf_fill(g_surf);
    RLEPaintHit(row0(), a, b, c, 6, SURF_PITCH, TOP, 0, SPR_W, 0,
                (void*)&g_surf[5 * SURF_W + 0]);
    LL_CHECK_INT("RLEPaintHit hits on a singleton", g_blit_hit, 1);
    g_blit_hit = 0;
    surf_fill(g_surf);
    SoftBlitRLEFrameRecolour(row0(), a, b, c, 6, SURF_PITCH, TOP, 0, SPR_W, 0,
                             (void*)&g_surf[5 * SURF_W + 0]);
    LL_CHECK_INT("the recolour leaf does NOT hit on a singleton",
                 g_blit_hit, 0);

    /* ---- 6. the primary-code-1 top-skip defect -------------------------- */
    /* The same sprite with a primary code 1 in the FIRST row -- i.e. inside
     * the top-skip region.  RLEPaintFast and RLEPaintHitClipLR walk it
     * correctly; RLEPaintHit falls through into escape processing after
     * consuming the literal word, eats a B length byte, and desynchronises.
     * The check is that the correct leaf still paints the reference picture
     * and the defective one does not -- the defect's exact mis-decode is not
     * re-derived here, it is recorded in docs/lanes/scope-port-b3.md. */
    {
        static Op defect[SPR_H][8];
        int r, i;
        unsigned short after_fast[SURF_W * SURF_H];

        for (r = 0; r < SPR_H; r++)
            for (i = 0; i < 8; i++)
                defect[r][i] = g_rows[r][i];
        defect[0][0].kind = OP_PIX1;          /* row 0 is top-skipped */
        emit_frame(defect, SPR_H);
        a = (void*)g_blk_a;
        b = (void*)g_blk_b;
        c = (void*)g_blk_c;

        surf_fill(g_surf);
        RLEPaintFast(row0(), a, b, c, ROWS, SURF_PITCH, TOP);
        expect(defect, TOP, ROWS, 0, SPR_W, 0xffffu, 0);
        compare("RLEPaintFast walks a primary code 1 in the top-skip");
        for (i = 0; i < SURF_W * SURF_H; i++)
            after_fast[i] = g_surf[i];

        g_blit_hit = 0;
        surf_fill(g_surf);
        RLEPaintHit(row0(), a, b, c, ROWS, SURF_PITCH, TOP, 0, SPR_W, 0,
                    mouse_clear);
        LL_CHECK_TRUE("RLEPaintHit's top-skip defect is observable",
                      memcmp(g_surf, after_fast,
                             sizeof after_fast) != 0);

        surf_fill(g_surf);
        RLEPaintHitClipLR(row0(), a, b, c, ROWS, SURF_PITCH, TOP, 0, SPR_W, 0,
                          mouse_clear);
        expect(defect, TOP, ROWS, 0, SPR_W, 0xffffu, 0);
        compare("RLEPaintHitClipLR's top-skip is the correct one");
    }

    /* ---- 7. one REAL sprite, against the Python decoder ------------------ */
    real_sprite();
}

/* ---- a real shipped frame, against tools/comp.py ------------------------- */
/* Everything above is the test's own idea of what a frame looks like. This is
 * one frame of one sprite out of gamedata/disc/Graphics1.res, painted by the
 * recovered C and compared -- as an FNV-1a 64 digest of the whole scratch
 * surface -- against tools/oracle_rlepaint.py's decode of the same bytes with
 * the clean-room reader in tools/comp.py. The oracle picks the member and
 * emits its byte range, the sentinel to pre-fill with, and the digests; the
 * frame's own bytes never leave the archive. */

#ifdef LL_RLE_NO_ASSETS
/* gamedata/ is absent, so tools/oracle_rlepaint.py could not read Graphics1.res
 * and there are no digests to compare against. The nine synthetic cases above
 * are the test in that build (portable/cmake/tests.cmake wrote the stub header,
 * see docs/lanes/scope-port-a4.md §4); this one says so instead of passing
 * quietly or failing on a missing file. */
static void real_sprite(void)
{
    ll_skip("one real shipped sprite, bit-exact against tools/comp.py",
            "gamedata/ is absent, so the oracle has no digests -- the nine "
            "synthetic cases above are this build's coverage");
}
#else
static void real_sprite(void)
{
    FILE*           f;
    unsigned char*  mem;
    unsigned char*  frame;
    unsigned short* surf;
    unsigned int    np;
    unsigned int    nl;
    unsigned int    fsize;
    unsigned int    npix;
    unsigned int    i;
    ll_u64          d;
    void*           a;
    void*           b;
    void*           c;

    f = fopen(LL_RLE_RES_PATH, "rb");
    ll_checks++;
    if (!f) {
        ll_fail("open " LL_RLE_MEMBER, "cannot open " LL_RLE_RES_PATH);
        return;
    }
    ll_pass("open the archive holding " LL_RLE_MEMBER);

    mem = (unsigned char*)malloc(LL_RLE_SIZE);
    if (!mem) { fclose(f); return; }
    if (fseek(f, (long)LL_RLE_OFFSET, SEEK_SET) != 0
     || fread(mem, 1, LL_RLE_SIZE, f) != LL_RLE_SIZE) {
        ll_checks++;
        ll_fail("read " LL_RLE_MEMBER, "short read");
        fclose(f);
        free(mem);
        return;
    }
    fclose(f);

    LL_CHECK_TRUE("the member is a COMP block", memcmp(mem, "COMP", 4) == 0);

    /* COMP header 0x18, then the frame header: size, pixel words, padded
     * length bytes, padded opcode count -- then A, B, C. */
    frame = mem + 24;
    memcpy(&fsize, frame + 0, 4);
    memcpy(&np, frame + 4, 4);
    memcpy(&nl, frame + 8, 4);
    LL_CHECK_INT("frame 0 is the size the oracle read", fsize,
                 LL_RLE_FRAME_SIZE);
    a = (void*)(frame + 16);
    b = (void*)(frame + 16 + 2 * np);
    c = (void*)(frame + 16 + 2 * np + nl);

    npix = (unsigned int)LL_RLE_W * (unsigned int)LL_RLE_H;
    surf = (unsigned short*)malloc(npix * 2);
    if (!surf) { free(mem); return; }

    for (i = 0; i < npix; i++) surf[i] = (unsigned short)LL_RLE_SENTINEL;
    RLEPaintFast((void*)surf, a, b, c, LL_RLE_H, LL_RLE_W * 2, 0);
    d = ll_fnv_bytes(ll_fnv_init(), surf, npix * 2);
    LL_CHECK_HEX("RLEPaintFast reproduces " LL_RLE_MEMBER " exactly",
                 d, LL_RLE_DIGEST);

    /* The same frame with the right edge cut at half its width: the painter
     * must write columns 0..CLIP_W-1 and leave the rest at the sentinel, with
     * the three streams still aligned for every following row -- which is
     * what makes this a check of the clipping and not just of the decode. */
    for (i = 0; i < npix; i++) surf[i] = (unsigned short)LL_RLE_SENTINEL;
    RLEPaintClipR((void*)surf, a, b, c, LL_RLE_H, LL_RLE_W * 2, 0,
                  0, LL_RLE_CLIP_W, 0, 0);
    d = ll_fnv_bytes(ll_fnv_init(), surf, npix * 2);
    LL_CHECK_HEX("RLEPaintClipR cuts " LL_RLE_MEMBER " at half width",
                 d, LL_RLE_DIGEST_CLIP);

    free(surf);
    free(mem);
}
#endif /* LL_RLE_NO_ASSETS */
