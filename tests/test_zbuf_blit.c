/* zbuf_blit -- bigrender.c's ZBufferHelper and blitmisc.c's BltAdvisor.
 * Scope PORT-B5.
 *
 * `LEGOLAND/bigrender.c` 0x00464a90 ZBufferHelper paints a type-2 LLS frame
 * into the 128-dword-wide Z buffer: the same control stream the animation
 * painters read (see test_anim_paint.c / test_anim_recolour.c), but each pixel
 * is `(unsigned)index << 24` written as a DWORD, there is no palette and no
 * mouse test, and the row pitch is 0x200.  The encoder here is written again
 * from the reader's arithmetic, and the expectation is drawn straight from the
 * op list, so an encoder bug cannot cancel a walker bug.
 *
 * `LEGOLAND/blitmisc.c` 0x004659a0 BltAdvisor copies a BOTTOM-UP 16-bpp DIB
 * into the locked surface, widening RGB555 to RGB565 when g_screen_depth == 2.
 * Both the row order and the widening are checked against a direct
 * construction.
 *
 * Runs natively and on wasm32: both functions take their destination as an
 * argument or through plain globals, so no asset, loader or host shim is
 * involved.
 *
 * NOT COVERED HERE, and why:
 *   - blitmisc.c 0x004632b0 ShowCapacityOverlay formats six AICat rows with
 *     `sprintf` and draws them with text.c's `Print` (0x00454ba0), which needs
 *     a loaded font and a locked surface.  Nothing it does is decidable from a
 *     pixel buffer in a headless test; its port is reviewed against the asm's
 *     push order instead (docs/lanes/scope-port-b5.md).
 *   - popup.c 0x00489190 RenderTransSprite opens both its surfaces with
 *     `GetSprite` (printlist.c 0x00497c30), which locks real DirectDraw
 *     surfaces -- a generated trap in this build, where PORT-B's shim is not
 *     linked.  Same treatment. */
#include "ll_tests.h"

typedef struct WinRect { long left, top, right, bottom; } WinRect;
typedef struct Pos { int x, y; } Pos;

extern void ZBufferHelper(char* lls, WinRect* src, Pos* dst, void* zbuf);
                                                           /* 0x00464a90 */
extern int  g_frame_override;   /* 0x004b9ca8 */

/* blitmisc.c's DibHeader and the three globals BltAdvisor reads. */
typedef struct DibHeader {
    int  size;
    int  width;
    int  height;
    char pad0c[0x28 - 0x0c];
} DibHeader;

extern void BltAdvisor(DibHeader* dib, int x, int y);      /* 0x004659a0 */
extern long  g_ddsd_pitch;      /* 0x006680ac */
extern void* g_ddsd_bits;       /* 0x006680c0 */
extern int   g_screen_depth;    /* 0x00668088 */

/* ---- the op list (the type-2 grammar, as test_anim_paint.c documents) ----- */

#define ZOP_PIX    0
#define ZOP_SKIP1  1
#define ZOP_COPY   2
#define ZOP_REPEAT 3
#define ZOP_SKIPN  4
#define ZOP_END    5

typedef struct ZOp {
    int           kind;
    int           len;
    unsigned char idx[12];
} ZOp;

#define ZSPR_W 12
#define ZSPR_H 5
#define ZROW_OPS 8

static const ZOp g_zrows[ZSPR_H][ZROW_OPS] = {
    /* 0 */ { { ZOP_PIX, 0, { 0x11 } },
              { ZOP_SKIP1, 0, { 0 } },
              { ZOP_COPY, 4, { 0x21, 0x22, 0x23, 0x24 } },
              { ZOP_REPEAT, 3, { 0x31 } },
              { ZOP_SKIPN, 2, { 0 } },
              { ZOP_PIX, 0, { 0x12 } },
              { ZOP_END, 0, { 0 } } },
    /* 1 */ { { ZOP_REPEAT, 12, { 0x41 } },
              { ZOP_END, 0, { 0 } } },
    /* 2 */ { { ZOP_SKIPN, 12, { 0 } },
              { ZOP_END, 0, { 0 } } },
    /* 3 */ { { ZOP_COPY, 12, { 0x50, 0x51, 0x52, 0x53, 0x54, 0x55,
                                0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b } },
              { ZOP_END, 0, { 0 } } },
    /* 4 */ { { ZOP_SKIP1, 0, { 0 } },
              { ZOP_REPEAT, 7, { 0x61 } },
              { ZOP_COPY, 3, { 0x71, 0x72, 0x73 } },
              { ZOP_PIX, 0, { 0x13 } },
              { ZOP_END, 0, { 0 } } },
};

static unsigned int   g_zwords[256];
static unsigned char  g_zidx[256];
static unsigned int   g_znwords, g_zslot, g_znidx;

static void zemit_code(unsigned int code)
{
    if (g_zslot >= 16) { g_znwords++; g_zslot = 0; }
    g_zwords[g_znwords - 1] |= (code & 3u) << (2 * g_zslot);
    g_zslot++;
}

static void zemit_len(unsigned int len)
{
    if (g_zslot + 4 > 16) { g_znwords++; g_zslot = 0; }
    g_zwords[g_znwords - 1] |= (len & 0xffu) << (2 * g_zslot);
    g_zslot += 4;
}

static void zemit_frame(void)
{
    int r, i, k;

    for (i = 0; i < 256; i++) g_zwords[i] = 0;
    g_znwords = 0;
    g_zslot = 16;
    g_znidx = 0;

    for (r = 0; r < ZSPR_H; r++) {
        for (i = 0; g_zrows[r][i].kind != ZOP_END; i++) {
            const ZOp* op = &g_zrows[r][i];
            switch (op->kind) {
            case ZOP_PIX:
                zemit_code(0);
                g_zidx[g_znidx++] = op->idx[0];
                break;
            case ZOP_SKIP1:
                zemit_code(2);
                break;
            case ZOP_COPY:
                zemit_code(3);
                zemit_len((unsigned int)op->len);
                zemit_code(0);
                for (k = 0; k < op->len; k++)
                    g_zidx[g_znidx++] = op->idx[k];
                break;
            case ZOP_REPEAT:
                zemit_code(3);
                zemit_len((unsigned int)op->len);
                zemit_code(1);
                g_zidx[g_znidx++] = op->idx[0];
                break;
            case ZOP_SKIPN:
                zemit_code(3);
                zemit_len((unsigned int)op->len);
                zemit_code(2);
                break;
            default:
                break;
            }
        }
        zemit_code(3);
        zemit_len(0);
    }
}

/* ---- the Z buffer and the reference ------------------------------------- */

#define ZB_ROWDW 128                 /* 0x200 bytes per row */
#define ZB_ROWS  16
#define ZSENTINEL 0xdeadbeefu

static unsigned int g_zb[ZB_ROWDW * ZB_ROWS];
static unsigned int g_zwant[ZB_ROWDW * ZB_ROWS];

static void zb_fill(unsigned int* b)
{
    int i;
    for (i = 0; i < ZB_ROWDW * ZB_ROWS; i++)
        b[i] = ZSENTINEL;
}

/* `dxoff` is where source column x lands, as in the animation painters: this
 * walker neither receives a biased destination nor steps the output over the
 * clipped pixels, so the first VISIBLE column lands on dst.x. */
static void zexpect(int top, int h, int vis_lo, int vis_hi, int dxoff)
{
    int r, i, k, x, y;

    zb_fill(g_zwant);
    for (y = 0; y < h; y++) {
        r = top + y;
        x = 0;
        for (i = 0; g_zrows[r][i].kind != ZOP_END; i++) {
            const ZOp* op = &g_zrows[r][i];
            int n = (op->kind == ZOP_COPY || op->kind == ZOP_REPEAT
                  || op->kind == ZOP_SKIPN) ? op->len : 1;
            for (k = 0; k < n; k++, x++) {
                if (op->kind == ZOP_SKIP1 || op->kind == ZOP_SKIPN)
                    continue;
                if (x < vis_lo || x >= vis_hi)
                    continue;
                g_zwant[y * ZB_ROWDW + x + dxoff] = (unsigned int)
                    (op->kind == ZOP_COPY ? op->idx[k] : op->idx[0]) << 24;
            }
        }
    }
}

static void zcompare(const char* what)
{
    int i, bad = -1;
    ll_checks++;
    for (i = 0; i < ZB_ROWDW * ZB_ROWS; i++)
        if (g_zb[i] != g_zwant[i]) { bad = i; break; }
    if (bad < 0)
        ll_pass(what);
    else
        ll_fail(what, "dword (%d,%d): got 0x%08x, want 0x%08x",
                bad % ZB_ROWDW, bad / ZB_ROWDW, g_zb[bad], g_zwant[bad]);
}

/* The LLS record: a 0x18-byte header, then one frame -- {int size; int
 * npixels;}, the index bytes, a 0x200-byte table (frame 0 only, which is why
 * the C walks to p + n + 0x208), then the control words.  Held as an int array
 * so the dword loads inside the walker are aligned. */
#define ZFRAME_BYTES (8 + 256 + 0x200 + 1024)
static int g_zrec[(0x18 + ZFRAME_BYTES) / 4 + 1];

/* ---- BltAdvisor's DIB and destination ----------------------------------- */

#define DIB_W 6
#define DIB_H 4
#define DSURF_W 16
#define DSURF_H 8
#define DSURF_PITCH (DSURF_W * 2)

static struct {
    DibHeader      hdr;
    unsigned short pix[DIB_W * DIB_H];
} g_dib;
static unsigned short g_dsurf[DSURF_W * DSURF_H];

void test_zbuf_blit(void)
{
    WinRect src;
    Pos     dst;
    char*   lls;
    int*    hdr;
    int     i, x, y, bad;

    /* ---- ZBufferHelper ------------------------------------------------- */
    zemit_frame();
    /* 27 = 7 + 1 + 0 + 12 + 11 ... counted from g_zrows by hand:
     * row 0 has 1+4+1+1 = 7, row 1 has 1, row 2 has 0, row 3 has 12,
     * row 4 has 1+3+1 = 5.  Total 25. */
    LL_CHECK_INT("the frame holds 25 index bytes", g_znidx, 25);
    LL_CHECK_TRUE("the control stream fits the scratch words",
                  g_znwords > 0 && g_znwords < 64);

    lls = (char*)g_zrec;
    *(short*)lls = 0;                            /* frame */
    *(short*)(lls + 0x10) = 1;                   /* nframes */
    hdr = (int*)(lls + 0x18);
    hdr[1] = (int)g_znidx;                       /* npixels */
    memcpy(lls + 0x18 + 8, g_zidx, g_znidx);
    memset(lls + 0x18 + 8 + g_znidx, 0, 0x200);
    memcpy(lls + 0x18 + 8 + g_znidx + 0x200, g_zwords, g_znwords * 4);
    hdr[0] = (int)(8 + g_znidx + 0x200 + g_znwords * 4);

    g_frame_override = -1;

    /* 1. unclipped */
    src.left = 0; src.top = 0; src.right = ZSPR_W; src.bottom = ZSPR_H;
    dst.x = 0; dst.y = 0;
    zb_fill(g_zb);
    ZBufferHelper(lls, &src, &dst, g_zb);
    zexpect(0, ZSPR_H, 0, ZSPR_W, 0);
    zcompare("ZBufferHelper 0x00464a90 paints the frame as (index << 24) "
             "dwords, 128 per row");

    /* 2. the top skip */
    src.top = 2;
    zb_fill(g_zb);
    ZBufferHelper(lls, &src, &dst, g_zb);
    zexpect(2, ZSPR_H - 2, 0, ZSPR_W, 0);
    zcompare("the top-skip pass consumes two whole rows");

    /* 3. the right clip */
    src.top = 0; src.right = 7;
    zb_fill(g_zb);
    ZBufferHelper(lls, &src, &dst, g_zb);
    zexpect(0, ZSPR_H, 0, 7, 0);
    zcompare("a width of 7 keeps source columns 0..6");

    /* 4. a destination offset: dst.x*4 + dst.y*0x200 */
    src.right = ZSPR_W;
    dst.x = 3; dst.y = 2;
    zb_fill(g_zb);
    ZBufferHelper(lls, &src, &dst, g_zb);
    bad = 0;
    for (y = 0; y < ZSPR_H; y++)
        for (x = 0; x < ZSPR_W; x++) {
            unsigned int got = g_zb[(y + 2) * ZB_ROWDW + x + 3];
            zexpect(0, ZSPR_H, 0, ZSPR_W, 0);
            if (got != g_zwant[y * ZB_ROWDW + x]) bad++;
        }
    LL_CHECK_INT("dst (3,2) offsets by 3 dwords and two 0x200-byte rows",
                 bad, 0);
    dst.x = 0; dst.y = 0;

    /* 5. THE *2 FINDING.  Row 0 opens with a literal, a TRANSPARENT single and
     * then a 4-long literal run.  With src->left = 2 the skip pass eats the
     * first two source pixels with singles (the `c_inc`/`c_dec` path, which is
     * sound), so to reach the broken path the clip has to land inside a
     * transparent RUN.  Row 4 of the sprite opens with one transparent single
     * and row 2 is one 12-long transparent run, so a left clip of 4 on row 2
     * drives `neg edx / lea edi,[edi+edx*2]` with an overhang of 8: the output
     * lands 4 dwords in instead of 8.  Row 2 paints nothing either way, so
     * what the check can see is that the walker still RETURNS and leaves the
     * row untouched -- the visible consequence needs a painted run after the
     * transparent one, which row 2 has not got.  The arithmetic is recorded in
     * the C at the site and in the lane notes; the regression value of this
     * check is that the path is EXERCISED. */
    src.left = 4; src.top = 2; src.right = ZSPR_W; src.bottom = 3;
    zb_fill(g_zb);
    ZBufferHelper(lls, &src, &dst, g_zb);
    bad = 0;
    for (i = 0; i < ZB_ROWDW * ZB_ROWS; i++)
        if (g_zb[i] != ZSENTINEL) bad++;
    LL_CHECK_INT("a left clip inside a transparent run (the `*2` path) runs "
                 "and paints nothing on a wholly transparent row", bad, 0);

    /* 6. the left clip on a painted row */
    src.left = 3; src.top = 0; src.right = ZSPR_W; src.bottom = ZSPR_H;
    zb_fill(g_zb);
    ZBufferHelper(lls, &src, &dst, g_zb);
    zexpect(0, ZSPR_H, 3, ZSPR_W, -3);
    zcompare("a left skip of 3 keeps source columns 3..11");

    /* ---- BltAdvisor ---------------------------------------------------- */
    /* A 6x4 bottom-up DIB: the LAST row of g_dib.pix is the TOP row of the
     * image on screen, because the source starts at dib+0x28+(h-1)*w*2 and
     * walks up. */
    g_dib.hdr.size = 0x28;
    g_dib.hdr.width = DIB_W;
    g_dib.hdr.height = DIB_H;
    for (y = 0; y < DIB_H; y++)
        for (x = 0; x < DIB_W; x++)
            g_dib.pix[y * DIB_W + x] = (unsigned short)(0x0821u * (y * DIB_W + x + 1));

    g_ddsd_bits = g_dsurf;
    g_ddsd_pitch = DSURF_PITCH;
    g_screen_depth = 0;                 /* no widening */
    for (i = 0; i < DSURF_W * DSURF_H; i++)
        g_dsurf[i] = 0x1234;
    BltAdvisor(&g_dib.hdr, 2, 1);
    bad = 0;
    for (y = 0; y < DIB_H; y++)
        for (x = 0; x < DIB_W; x++)
            if (g_dsurf[(1 + y) * DSURF_W + 2 + x]
                != g_dib.pix[(DIB_H - 1 - y) * DIB_W + x])
                bad++;
    LL_CHECK_INT("BltAdvisor 0x004659a0 copies the DIB BOTTOM-UP at (2,1)",
                 bad, 0);
    bad = 0;
    for (y = 0; y < DSURF_H; y++)
        for (x = 0; x < DSURF_W; x++)
            if (x < 2 || x >= 2 + DIB_W || y < 1 || y >= 1 + DIB_H)
                if (g_dsurf[y * DSURF_W + x] != 0x1234) bad++;
    LL_CHECK_INT("and touches nothing outside the destination rectangle",
                 bad, 0);

    /* g_screen_depth == 2 widens RGB555 to RGB565: the low 5 bits stay and
     * everything above them moves up one, which also drops bit 15. */
    g_screen_depth = 2;
    for (i = 0; i < DSURF_W * DSURF_H; i++)
        g_dsurf[i] = 0x1234;
    BltAdvisor(&g_dib.hdr, 2, 1);
    bad = 0;
    for (y = 0; y < DIB_H; y++)
        for (x = 0; x < DIB_W; x++) {
            unsigned int s = g_dib.pix[(DIB_H - 1 - y) * DIB_W + x];
            unsigned int w = (s & 0x1fu) | (((s & 0xffe0u) << 1) & 0xffffu);
            if (g_dsurf[(1 + y) * DSURF_W + 2 + x] != (unsigned short)w)
                bad++;
        }
    LL_CHECK_INT("depth 2 widens 555 to 565 (blue kept, the rest shifted up)",
                 bad, 0);
    g_screen_depth = 0;

    /* A zero-height DIB must return without storing anything. */
    g_dib.hdr.height = 0;
    for (i = 0; i < DSURF_W * DSURF_H; i++)
        g_dsurf[i] = 0x1234;
    BltAdvisor(&g_dib.hdr, 2, 1);
    bad = 0;
    for (i = 0; i < DSURF_W * DSURF_H; i++)
        if (g_dsurf[i] != 0x1234) bad++;
    LL_CHECK_INT("a zero-height DIB writes nothing (`test ecx,ecx / je`)",
                 bad, 0);
    g_dib.hdr.height = DIB_H;
}
