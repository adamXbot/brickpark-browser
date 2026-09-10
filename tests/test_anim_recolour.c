/* anim_recolour -- SoftBlitAnim, the RECOLOURING type-2 LLS painter.
 * Scope PORT-B5.
 *
 * `LEGOLAND/softblit.c` 0x00465240 is the recolouring sibling of
 * softblit2.c's SoftBlitAnimPlain (0x00464480), which `test_anim_paint.c`
 * covers. Same stream, same four passes, and PORT-B3 tabulated nine
 * differences between the two asm bodies; this test (a) paints the same kind
 * of synthetic sprite through the recolouring painter and compares it against
 * an independent reference image, and (b) drives the two painters over
 * IDENTICAL streams on rows built to isolate each behavioural difference, so
 * the divergence is observed rather than asserted.
 *
 * The stream, and therefore the encoder below, is the one test_anim_paint.c
 * documents: 2-bit codes packed 16 to a 32-bit word, an 8-bit run length
 * spliced out of the low byte for four code slots, pixels as 8-bit indices
 * through g_sp_pal16. The encoder here is written again from the reader's
 * arithmetic rather than shared, so an encoder bug cannot cancel a painter
 * bug.
 *
 * Runs natively and on wasm32: it builds the LLS record it paints, so no
 * asset and no loader is involved. It needs `g_lock` (the lock descriptor)
 * pointed at its own scratch surface. */
#include "ll_tests.h"

/* softblit.c's own local types, copied verbatim so the test sees the layout
 * the painter compiles against. */
typedef struct WinRect { long left, top, right, bottom; } WinRect;
typedef struct Pos { int x, y; } Pos;

typedef struct DDSurfaceDesc {
    unsigned long dwSize, dwFlags, dwHeight, dwWidth;
    long          lPitch;
    unsigned long dwBackBufferCount, dwMipMapCount, dwAlphaBitDepth, dwReserved;
    void*         lpSurface;
    char          pad28[0x6c - 0x28];
} DDSurfaceDesc;

typedef struct LockState {
    DDSurfaceDesc ddsd;
    WinRect       render_clip;
} LockState;

typedef struct LLSRec {
    short          frame;
    short          pad02;
    int            w;
    int            h;
    int            pad0c;
    short          nframes;
    short          pad12;
    unsigned int   flags;
    char           frames[1];
} LLSRec;

extern void SoftBlitAnim(LLSRec* lls, WinRect* src, Pos* dst);      /* 0x00465240 */
extern void SoftBlitAnimPlain(LLSRec* lls, WinRect* src, Pos* dst); /* 0x00464480 */

extern LockState     g_lock;              /* 0x0066809c */
extern int           g_frame_override;    /* 0x004b9ca8 */
extern void*         g_override_palette;  /* 0x006681e8 */
extern int           g_sp_recolour;       /* 0x007fe998 */
extern void*         g_sp_mouse_pixel;    /* 0x007fe9a8 */
extern int           g_blit_hit;          /* 0x007feb14 */

/* ---- the op list --------------------------------------------------------- */

#define ROP_PIX    0   /* bit1 = 0: one index byte                          */
#define ROP_SKIP1  1   /* bit1 = 1, bit0 = 0: one transparent pixel         */
#define ROP_COPY   2   /* length + secondary 00: copy `len` index bytes     */
#define ROP_REPEAT 3   /* length + secondary 01: repeat one index `len` x   */
#define ROP_SKIPN  4   /* length + secondary 1x: skip `len` pixels          */
#define ROP_END    5

typedef struct ROp {
    int           kind;
    int           len;
    unsigned char idx[12];
} ROp;

#define RSPR_W 12
#define RSPR_H 6
#define RROW_OPS 10

/* Six rows of twelve columns: every operation in the grammar, a wholly
 * transparent row, and a row that is one full-width run. */
static const ROp g_rrows[RSPR_H][RROW_OPS] = {
    /* 0 */ { { ROP_PIX, 0, { 0x11 } },
              { ROP_SKIP1, 0, { 0 } },
              { ROP_COPY, 4, { 0x21, 0x22, 0x23, 0x24 } },
              { ROP_REPEAT, 3, { 0x31 } },
              { ROP_SKIPN, 2, { 0 } },
              { ROP_PIX, 0, { 0x12 } },
              { ROP_END, 0, { 0 } } },
    /* 1 */ { { ROP_REPEAT, 12, { 0x41 } },
              { ROP_END, 0, { 0 } } },
    /* 2 */ { { ROP_SKIPN, 12, { 0 } },
              { ROP_END, 0, { 0 } } },
    /* 3 */ { { ROP_COPY, 12, { 0x50, 0x51, 0x52, 0x53, 0x54, 0x55,
                                0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b } },
              { ROP_END, 0, { 0 } } },
    /* 4 */ { { ROP_SKIP1, 0, { 0 } },
              { ROP_REPEAT, 7, { 0x61 } },
              { ROP_COPY, 3, { 0x71, 0x72, 0x73 } },
              { ROP_PIX, 0, { 0x13 } },
              { ROP_END, 0, { 0 } } },
    /* 5 */ { { ROP_PIX, 0, { 0x14 } },
              { ROP_SKIPN, 10, { 0 } },
              { ROP_PIX, 0, { 0x15 } },
              { ROP_END, 0, { 0 } } },
};

/* One row per documented divergence; see the checks at the bottom for what
 * each one isolates. Twelve columns wide like the sweep sprite. */
#define DSPR_H 4
static const ROp g_drows[DSPR_H][RROW_OPS] = {
    /* 0: a left-clip run that ends EXACTLY on the left edge, followed by
     *    singles. `sub edx,ecx / ja` leaves the skip pass here; the plain
     *    painter's `jns` stays in it and eats the rest of the row. */
    /* 1: a skip run that consumes the whole width, then a repeat run that
     *    finds a zero budget. `or ecx,ecx / je endrow` writes nothing; the
     *    plain painter's `rep stosd` path writes one stray pixel first. */
    /* 2: transparent singles that run the budget out. `dec edx / je endrow`
     *    ends the row; in the plain painter the `add edi,2` between the `dec`
     *    and the `je` makes that branch dead (PORT-B3 FINDING 1). */
    /* 3: a left-clip skip run whose visible part exactly fills the row.
     *    `jbe endrow` ends it; the plain painter's `js` carries on with a
     *    zero budget and then writes past the right clip. */
    /* 0 */ { { ROP_COPY, 3, { 0x81, 0x82, 0x83 } },
              { ROP_PIX, 0, { 0x84 } },
              { ROP_PIX, 0, { 0x85 } },
              { ROP_SKIPN, 7, { 0 } },
              { ROP_END, 0, { 0 } } },
    /* 1 */ { { ROP_SKIPN, 12, { 0 } },
              { ROP_REPEAT, 3, { 0x91 } },
              { ROP_END, 0, { 0 } } },
    /* 2 */ { { ROP_SKIP1, 0, { 0 } }, { ROP_SKIP1, 0, { 0 } },
              { ROP_SKIP1, 0, { 0 } }, { ROP_SKIP1, 0, { 0 } },
              { ROP_SKIP1, 0, { 0 } }, { ROP_SKIP1, 0, { 0 } },
              { ROP_SKIP1, 0, { 0 } },
              { ROP_PIX, 0, { 0xa1 } },
              { ROP_END, 0, { 0 } } },
    /* 3 */ { { ROP_SKIPN, 12, { 0 } },
              { ROP_SKIP1, 0, { 0 } },
              { ROP_PIX, 0, { 0xb1 } },
              { ROP_END, 0, { 0 } } },
};

/* ---- the encoder --------------------------------------------------------- */
/* The reader counts the codes left in the current word. A plain code costs one
 * slot; a length costs four, and when fewer than four remain the reader
 * refills and takes the length from the NEW word's low byte -- so the encoder
 * must do the same and leave the tail of the old word unused. */

static unsigned int   g_rwords[256];
static unsigned char  g_ridx[256];
static unsigned int   g_rnwords, g_rslot, g_rnidx;

static void remit_code(unsigned int code)
{
    if (g_rslot >= 16) { g_rnwords++; g_rslot = 0; }
    g_rwords[g_rnwords - 1] |= (code & 3u) << (2 * g_rslot);
    g_rslot++;
}

static void remit_len(unsigned int len)
{
    if (g_rslot + 4 > 16) { g_rnwords++; g_rslot = 0; }
    g_rwords[g_rnwords - 1] |= (len & 0xffu) << (2 * g_rslot);
    g_rslot += 4;
}

static void remit_frame(const ROp rows[][RROW_OPS], int nrows)
{
    int r, i, k;

    for (i = 0; i < 256; i++) g_rwords[i] = 0;
    g_rnwords = 0;
    g_rslot = 16;                       /* forces the first word */
    g_rnidx = 0;

    for (r = 0; r < nrows; r++) {
        for (i = 0; rows[r][i].kind != ROP_END; i++) {
            const ROp* op = &rows[r][i];
            switch (op->kind) {
            case ROP_PIX:
                remit_code(0);
                g_ridx[g_rnidx++] = op->idx[0];
                break;
            case ROP_SKIP1:
                remit_code(2);
                break;
            case ROP_COPY:
                remit_code(3);
                remit_len((unsigned int)op->len);
                remit_code(0);
                for (k = 0; k < op->len; k++)
                    g_ridx[g_rnidx++] = op->idx[k];
                break;
            case ROP_REPEAT:
                remit_code(3);
                remit_len((unsigned int)op->len);
                remit_code(1);
                g_ridx[g_rnidx++] = op->idx[0];
                break;
            case ROP_SKIPN:
                remit_code(3);
                remit_len((unsigned int)op->len);
                remit_code(2);
                break;
            default:
                break;
            }
        }
        remit_code(3);                  /* the end-of-row marker: an escape */
        remit_len(0);                   /* with a zero length               */
    }
}

/* ---- the surface and the reference image --------------------------------- */

#define RSURF_W 24
#define RSURF_H 10
#define RSURF_PITCH (RSURF_W * 2)
#define RSENTINEL 0x1234

static unsigned short g_rsurf[RSURF_W * RSURF_H];
static unsigned short g_rsurf2[RSURF_W * RSURF_H];
static unsigned short g_rwant[RSURF_W * RSURF_H];
static unsigned short g_rpal[256];

static void rsurf_fill(unsigned short* s)
{
    int i;
    for (i = 0; i < RSURF_W * RSURF_H; i++)
        s[i] = RSENTINEL;
}

/* `dxoff` is where source column x lands: surface column x + dxoff. Like its
 * plain sibling -- and unlike the type-3 family -- this painter neither
 * receives a destination biased by -src.left nor steps the output over the
 * clipped pixels, so the first VISIBLE source column lands on dst.x and
 * dxoff is -src.left. `mask` is g_sp_recolour's low word. */
static void rexpect(const ROp rows[][RROW_OPS], int top, int h,
                    int vis_lo, int vis_hi, int dxoff, unsigned int mask)
{
    int r, i, k, x, y;

    rsurf_fill(g_rwant);
    for (y = 0; y < h; y++) {
        r = top + y;
        x = 0;
        for (i = 0; rows[r][i].kind != ROP_END; i++) {
            const ROp* op = &rows[r][i];
            int n = (op->kind == ROP_COPY || op->kind == ROP_REPEAT
                  || op->kind == ROP_SKIPN) ? op->len : 1;
            for (k = 0; k < n; k++, x++) {
                if (op->kind == ROP_SKIP1 || op->kind == ROP_SKIPN)
                    continue;
                if (x < vis_lo || x >= vis_hi)
                    continue;
                g_rwant[y * RSURF_W + x + dxoff] = (unsigned short)
                    (g_rpal[op->kind == ROP_COPY ? op->idx[k] : op->idx[0]]
                     & mask);
            }
        }
    }
}

static void rcompare(const char* what)
{
    int i, bad = -1;
    ll_checks++;
    for (i = 0; i < RSURF_W * RSURF_H; i++)
        if (g_rsurf[i] != g_rwant[i]) { bad = i; break; }
    if (bad < 0)
        ll_pass(what);
    else
        ll_fail(what, "pixel (%d,%d): got 0x%04x, want 0x%04x",
                bad % RSURF_W, bad / RSURF_W, g_rsurf[bad], g_rwant[bad]);
}

/* The record the painter walks: an LLSRec header with ONE frame at +0x18, no
 * base image and no override. The AnimFrame header is {int size; int
 * npixels;}, then the index block, then -- for frame 0 -- the 0x200-byte
 * palette, then the control words. Held as an int array so every dword load
 * inside the painter is aligned. */
#define RFRAME_BYTES (8 + 256 + 0x200 + 1024)
static int g_rrec[(0x18 + RFRAME_BYTES) / 4 + 1];

static void rbuild(const ROp rows[][RROW_OPS], int nrows, int w, int h)
{
    LLSRec* lls = (LLSRec*)g_rrec;
    int*    hdr;

    remit_frame(rows, nrows);

    lls->frame = 0;
    lls->pad02 = 0;
    lls->w = w;
    lls->h = h;
    lls->pad0c = 0;
    lls->nframes = 1;
    lls->pad12 = 0;
    lls->flags = 0;

    hdr = (int*)lls->frames;
    hdr[1] = (int)g_rnidx;                                    /* npixels */
    memcpy(lls->frames + 8, g_ridx, g_rnidx);
    memcpy(lls->frames + 8 + g_rnidx, g_rpal, 0x200);
    memcpy(lls->frames + 8 + g_rnidx + 0x200, g_rwords, g_rnwords * 4);
    hdr[0] = (int)(8 + g_rnidx + 0x200 + g_rnwords * 4);      /* size */
}

/* Count the pixels two runs of the SAME stream disagree on. */
static int rdiff(int* first)
{
    int i, n = 0;
    *first = -1;
    for (i = 0; i < RSURF_W * RSURF_H; i++)
        if (g_rsurf[i] != g_rsurf2[i]) {
            if (*first < 0) *first = i;
            n++;
        }
    return n;
}

void test_anim_recolour(void)
{
    WinRect src;
    Pos     dst;
    LLSRec* lls = (LLSRec*)g_rrec;
    int     i, n, first;

    for (i = 0; i < 256; i++)
        g_rpal[i] = (unsigned short)(0x8000u | (i * 7u));

    g_lock.ddsd.lpSurface = g_rsurf;
    g_lock.ddsd.lPitch = RSURF_PITCH;
    g_frame_override = -1;
    g_override_palette = 0;
    g_sp_mouse_pixel = 0;
    g_sp_recolour = 0xffff;

    /* ---- the grammar sweep --------------------------------------------- */
    rbuild(g_rrows, RSPR_H, RSPR_W, RSPR_H);
    /* 27 = 7 + 1 + 0 + 12 + 5 + 2, counted from g_rrows by hand. */
    LL_CHECK_INT("the frame holds 27 index bytes", g_rnidx, 27);
    LL_CHECK_TRUE("the control stream fits the scratch words",
                  g_rnwords > 0 && g_rnwords < 64);

    src.left = 0; src.top = 0; src.right = RSPR_W; src.bottom = RSPR_H;
    dst.x = 0; dst.y = 0;
    g_blit_hit = 0;
    rsurf_fill(g_rsurf);
    SoftBlitAnim(lls, &src, &dst);
    rexpect(g_rrows, 0, RSPR_H, 0, RSPR_W, 0, 0xffff);
    rcompare("SoftBlitAnim 0x00465240 paints the whole frame");
    LL_CHECK_INT("no mouse, no hit", g_blit_hit, 0);

    src.left = 0; src.top = 2; src.right = RSPR_W; src.bottom = RSPR_H;
    rsurf_fill(g_rsurf);
    SoftBlitAnim(lls, &src, &dst);
    rexpect(g_rrows, 2, RSPR_H - 2, 0, RSPR_W, 0, 0xffff);
    rcompare("the top-skip pass consumes two whole rows");

    src.left = 0; src.top = 0; src.right = 7; src.bottom = RSPR_H;
    rsurf_fill(g_rsurf);
    SoftBlitAnim(lls, &src, &dst);
    rexpect(g_rrows, 0, RSPR_H, 0, 7, 0, 0xffff);
    rcompare("a width of 7 keeps source columns 0..6");

    /* The first VISIBLE source column lands on dst.x. */
    src.left = 3; src.top = 0; src.right = RSPR_W; src.bottom = RSPR_H;
    rsurf_fill(g_rsurf);
    SoftBlitAnim(lls, &src, &dst);
    rexpect(g_rrows, 0, RSPR_H, 3, RSPR_W, -3, 0xffff);
    rcompare("a left skip of 3 keeps source columns 3..11");

    src.left = 3; src.top = 1; src.right = 9; src.bottom = RSPR_H;
    rsurf_fill(g_rsurf);
    SoftBlitAnim(lls, &src, &dst);
    rexpect(g_rrows, 1, RSPR_H - 1, 3, 9, -3, 0xffff);
    rcompare("left 3 and width 6 keep source columns 3..8");

    /* ---- the recolour mask -- the ONE thing every store here adds ------ */
    g_sp_recolour = 0x7bef;                 /* RGB565 halved per channel */
    src.left = 0; src.top = 0; src.right = RSPR_W; src.bottom = RSPR_H;
    rsurf_fill(g_rsurf);
    SoftBlitAnim(lls, &src, &dst);
    rexpect(g_rrows, 0, RSPR_H, 0, RSPR_W, 0, 0x7bef);
    rcompare("every store is ANDed with g_sp_recolour");

    /* The asm is `and ax, word ptr g_sp_recolour`: the LOW WORD only, so a
     * high half of the int is ignored. */
    g_sp_recolour = (int)0x1234ffff;
    rsurf_fill(g_rsurf);
    SoftBlitAnim(lls, &src, &dst);
    rexpect(g_rrows, 0, RSPR_H, 0, RSPR_W, 0, 0xffff);
    rcompare("only the LOW WORD of g_sp_recolour masks the pixel");
    g_sp_recolour = 0xffff;

    /* ---- the mouse hit test -------------------------------------------- */
    g_blit_hit = 0;
    rsurf_fill(g_rsurf);
    g_sp_mouse_pixel = (void*)&g_rsurf[1 * RSURF_W + 5];   /* row 1's run */
    SoftBlitAnim(lls, &src, &dst);
    LL_CHECK_INT("a mouse inside a painted run hits", g_blit_hit, 1);

    g_blit_hit = 0;
    rsurf_fill(g_rsurf);
    g_sp_mouse_pixel = (void*)&g_rsurf[2 * RSURF_W + 5];   /* row 2 is empty */
    SoftBlitAnim(lls, &src, &dst);
    LL_CHECK_INT("a mouse on a wholly transparent row does not hit",
                 g_blit_hit, 0);
    g_sp_mouse_pixel = 0;

    /* ---- `seta` versus the plain painter's `sbb ecx,-1` ----------------- */
    /* Divergence 6. The run hit is `cmp edi,mouse / seta al` AFTER the run,
     * so the test is strictly `dp > mouse`; the plain painter's
     * `sbb ecx,-1` gives `dp >= mouse`. A mouse on the pixel just PAST a run
     * therefore hits there and not here. g_drows row 0 is a 3-pixel copy run
     * followed by singles, so with a width of 3 nothing else can set the
     * flag. */
    rbuild(g_drows, DSPR_H, RSPR_W, DSPR_H);
    src.left = 0; src.top = 0; src.right = 3; src.bottom = 1;
    dst.x = 0; dst.y = 0;

    g_blit_hit = 0;
    rsurf_fill(g_rsurf);
    g_sp_mouse_pixel = (void*)&g_rsurf[2];              /* the LAST pixel */
    SoftBlitAnim(lls, &src, &dst);
    LL_CHECK_INT("a mouse on a run's last pixel hits", g_blit_hit, 1);

    g_blit_hit = 0;
    rsurf_fill(g_rsurf);
    g_sp_mouse_pixel = (void*)&g_rsurf[3];              /* one PAST the run */
    SoftBlitAnim(lls, &src, &dst);
    LL_CHECK_INT("a mouse one pixel past a run does NOT hit (seta)",
                 g_blit_hit, 0);

    g_blit_hit = 0;
    rsurf_fill(g_rsurf);
    SoftBlitAnimPlain(lls, &src, &dst);
    LL_CHECK_INT("the plain painter DOES hit there (sbb ecx,-1)",
                 g_blit_hit, 1);
    g_sp_mouse_pixel = 0;

    /* ---- the four structural divergences, same stream, both painters --- */
    /* Each row below is painted by BOTH painters and the two surfaces are
     * compared, so the difference is observed and not asserted. */

    /* 1. row 0 with a left skip of 3: the copy run ends EXACTLY on the left
     *    edge. `sub edx,ecx / ja` falls out of the skip pass and the two
     *    singles that follow are painted; the plain painter's `jns` stays in
     *    it with a skip of 0, `lc_step` drives it to -1, and the rest of the
     *    row is consumed with nothing drawn. */
    src.left = 3; src.top = 0; src.right = RSPR_W; src.bottom = 1;
    rsurf_fill(g_rsurf);
    SoftBlitAnim(lls, &src, &dst);
    LL_CHECK_HEX("a run ending on the left edge leaves the skip pass: "
                 "source column 3 is painted",
                 g_rsurf[0], (unsigned short)g_rpal[0x84]);
    memcpy(g_rsurf2, g_rsurf, sizeof g_rsurf);
    rsurf_fill(g_rsurf);
    SoftBlitAnimPlain(lls, &src, &dst);
    n = 0;
    for (i = 0; i < RSURF_W * RSURF_H; i++)
        if (g_rsurf[i] != RSENTINEL) n++;
    LL_CHECK_INT("the plain painter (`jns`) paints nothing on that row", n, 0);

    /* 2. row 1: a skip run consumes the whole width, leaving a zero budget,
     *    and the repeat run that follows writes NOTHING here
     *    (`or ecx,ecx / je endrow`). The plain painter's doubled-dword path
     *    emits one word before it looks at the count. */
    src.left = 0; src.top = 1; src.right = RSPR_W; src.bottom = 2;
    rsurf_fill(g_rsurf);
    SoftBlitAnim(lls, &src, &dst);
    memcpy(g_rsurf2, g_rsurf, sizeof g_rsurf);      /* what SoftBlitAnim did */
    rsurf_fill(g_rsurf);
    SoftBlitAnimPlain(lls, &src, &dst);
    n = 0;
    for (i = 0; i < RSURF_W * RSURF_H; i++)
        if (g_rsurf2[i] != RSENTINEL) n++;
    LL_CHECK_INT("a repeat run with a zero budget writes nothing", n, 0);
    LL_CHECK_HEX("the plain painter writes one stray pixel there",
                 g_rsurf[RSPR_W], (unsigned short)g_rpal[0x91]);
    n = rdiff(&first);
    LL_CHECK_INT("...and that stray pixel is the ONLY difference", n, 1);
    LL_CHECK_INT("at the column the skip run left the output on", first,
                 RSPR_W);

    /* 3. row 2: SEVEN transparent singles against a width of 6. Here the
     *    sixth takes the budget to zero and `dec edx / je endrow` ends the
     *    row, so nothing after it is drawn. In the plain painter the
     *    `add edi,2` between the `dec` and the `je` kills that branch
     *    (PORT-B3 FINDING 1): all seven run, the budget reaches -1, and
     *    because the run-versus-budget test is UNSIGNED the single pixel that
     *    follows compares BELOW it and is written one column past the right
     *    clip. Six singles would not show it -- at a budget of exactly zero
     *    `copy_run`'s own test still refuses the pixel; it takes a NEGATIVE
     *    budget to turn the comparison round. */
    src.left = 0; src.top = 2; src.right = 6; src.bottom = 3;
    rsurf_fill(g_rsurf);
    SoftBlitAnim(lls, &src, &dst);
    n = 0;
    for (i = 0; i < RSURF_W * RSURF_H; i++)
        if (g_rsurf[i] != RSENTINEL) n++;
    LL_CHECK_INT("a transparent single that empties the budget ends the row",
                 n, 0);
    rsurf_fill(g_rsurf);
    SoftBlitAnimPlain(lls, &src, &dst);
    LL_CHECK_HEX("the plain painter's dead `je` writes past the right clip",
                 g_rsurf[7], (unsigned short)g_rpal[0xa1]);

    /* 4. row 3 with a left skip of 3 and a width of 9: the skip run's visible
     *    part is exactly the row, and `jbe endrow` ends it. The plain
     *    painter's `js` carries on with a zero budget; the transparent single
     *    drives it negative, and because the run-versus-budget test is
     *    UNSIGNED the pixel after it is written past the right clip. */
    src.left = 3; src.top = 3; src.right = RSPR_W; src.bottom = 4;
    rsurf_fill(g_rsurf);
    SoftBlitAnim(lls, &src, &dst);
    n = 0;
    for (i = 0; i < RSURF_W * RSURF_H; i++)
        if (g_rsurf[i] != RSENTINEL) n++;
    LL_CHECK_INT("a left-clip skip run that fills the row ends it (jbe)",
                 n, 0);
    rsurf_fill(g_rsurf);
    SoftBlitAnimPlain(lls, &src, &dst);
    LL_CHECK_HEX("the plain painter (`js`) writes one pixel past the clip",
                 g_rsurf[10], (unsigned short)g_rpal[0xb1]);

    g_sp_recolour = 0xffff;
    g_sp_mouse_pixel = 0;
}
