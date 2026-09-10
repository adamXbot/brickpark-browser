/* anim_paint -- SoftBlitAnimPlain, the type-2 LLS animation painter.
 * Scope PORT-B3.
 *
 * `LEGOLAND/softblit2.c` 0x00464480 is the ImageRec type-2 sibling of the
 * type-3 RLE painters `test_rle_paint.c` covers. Its pixel loop is
 * hand-written asm in the matching build; this test exercises the C in its
 * `#else` arm.
 *
 * A DIFFERENT STREAM. The type-3 painters read 2-bit codes with a rotating
 * mask over a dword pointer. This one caches the dword in a register, shifts
 * it right two per code and counts the codes left in `g_zb_bits`, refilling
 * from the next word when the count goes negative. A run length is not a
 * separate byte stream at all: it is spliced out of the low byte of the
 * control word, costing FOUR code slots, and if fewer than four are left the
 * word is refilled and the length taken from the new word's low byte -- with
 * whatever was left of the old one discarded. `emit_code`/`emit_len` below
 * are that packing, written from the reader's arithmetic.
 *
 * The expectation is independent in the same way as the type-3 test: the
 * sprite is one list of operations per row, `emit_frame` turns it into the
 * control words and the index bytes, and `aexpect` draws it straight into a
 * reference image. Pixels are 8-bit indices through a palette here, so the
 * palette lookup is checked too.
 *
 * Runs natively and on wasm32. It builds the LLS record it paints, so no
 * asset and no loader is involved; it does need `g_lock` (the lock
 * descriptor) pointed at its own scratch surface, which is a plain global
 * the closure provides. */
#include "ll_tests.h"

/* softblit2.c's own local types, copied verbatim so the test sees the same
 * layout the painter compiles against. */
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

extern void SoftBlitAnimPlain(LLSRec* lls, WinRect* src, Pos* dst); /* 0x00464480 */

extern LockState     g_lock;              /* 0x0066809c */
extern int           g_frame_override;    /* 0x004b9ca8 */
extern void*         g_override_palette;  /* 0x006681e8 */
extern void*         g_sp_mouse_pixel;    /* 0x007fe9a8 */
extern int           g_blit_hit;          /* 0x007feb14 */

/* ---- the synthetic sprite ------------------------------------------------ */

#define AOP_PIX    0   /* bit1 = 0: one index byte                          */
#define AOP_SKIP1  1   /* bit1 = 1, bit0 = 0: one transparent pixel         */
#define AOP_COPY   2   /* length + secondary 00: copy `len` index bytes     */
#define AOP_REPEAT 3   /* length + secondary 01: repeat one index `len` x   */
#define AOP_SKIPN  4   /* length + secondary 1x: skip `len` pixels          */
#define AOP_END    5

typedef struct AOp {
    int           kind;
    int           len;
    unsigned char idx[12];
} AOp;

#define ASPR_W 12
#define ASPR_H 6

/* Six rows of twelve columns, every operation the grammar has, plus a wholly
 * transparent row and a row that is one full-width run. */
static const AOp g_arows[ASPR_H][8] = {
    /* 0 */ { { AOP_PIX, 0, { 0x11 } },
              { AOP_SKIP1, 0, { 0 } },
              { AOP_COPY, 4, { 0x21, 0x22, 0x23, 0x24 } },
              { AOP_REPEAT, 3, { 0x31 } },
              { AOP_SKIPN, 2, { 0 } },
              { AOP_PIX, 0, { 0x12 } },
              { AOP_END, 0, { 0 } } },
    /* 1 */ { { AOP_REPEAT, 12, { 0x41 } },
              { AOP_END, 0, { 0 } } },
    /* 2 */ { { AOP_SKIPN, 12, { 0 } },
              { AOP_END, 0, { 0 } } },
    /* 3 */ { { AOP_COPY, 12, { 0x50, 0x51, 0x52, 0x53, 0x54, 0x55,
                                0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b } },
              { AOP_END, 0, { 0 } } },
    /* 4 */ { { AOP_SKIP1, 0, { 0 } },
              { AOP_REPEAT, 7, { 0x61 } },
              { AOP_COPY, 3, { 0x71, 0x72, 0x73 } },
              { AOP_PIX, 0, { 0x13 } },
              { AOP_END, 0, { 0 } } },
    /* 5 */ { { AOP_PIX, 0, { 0x14 } },
              { AOP_SKIPN, 10, { 0 } },
              { AOP_PIX, 0, { 0x15 } },
              { AOP_END, 0, { 0 } } },
};

/* ---- the encoder --------------------------------------------------------- */
/* The reader keeps a count of the codes left in the current word. A plain
 * code takes one slot; a length takes four, and when fewer than four remain
 * the reader refills and takes the length from the NEW word's low byte, so
 * the encoder must do the same and leave the tail of the old word unused. */

static unsigned int   g_words[256];   /* the control stream */
static unsigned char  g_idx[256];     /* the index bytes    */
static unsigned int   g_nwords, g_slot, g_nidx;

static void emit_code(unsigned int code)
{
    if (g_slot >= 16) { g_nwords++; g_slot = 0; }
    g_words[g_nwords - 1] |= (code & 3u) << (2 * g_slot);
    g_slot++;
}

static void emit_len(unsigned int len)
{
    if (g_slot + 4 > 16) { g_nwords++; g_slot = 0; }
    g_words[g_nwords - 1] |= (len & 0xffu) << (2 * g_slot);
    g_slot += 4;
}

static void emit_frame(const AOp rows[ASPR_H][8], int nrows)
{
    int r, i, k;

    for (i = 0; i < 256; i++) g_words[i] = 0;
    g_nwords = 0;
    g_slot = 16;                       /* forces the first word */
    g_nidx = 0;

    for (r = 0; r < nrows; r++) {
        for (i = 0; rows[r][i].kind != AOP_END; i++) {
            const AOp* op = &rows[r][i];
            switch (op->kind) {
            case AOP_PIX:
                emit_code(0);
                g_idx[g_nidx++] = op->idx[0];
                break;
            case AOP_SKIP1:
                emit_code(2);
                break;
            case AOP_COPY:
                emit_code(3);
                emit_len((unsigned int)op->len);
                emit_code(0);
                for (k = 0; k < op->len; k++)
                    g_idx[g_nidx++] = op->idx[k];
                break;
            case AOP_REPEAT:
                emit_code(3);
                emit_len((unsigned int)op->len);
                emit_code(1);
                g_idx[g_nidx++] = op->idx[0];
                break;
            case AOP_SKIPN:
                emit_code(3);
                emit_len((unsigned int)op->len);
                emit_code(2);
                break;
            default:
                break;
            }
        }
        emit_code(3);                  /* the end-of-row marker: an escape */
        emit_len(0);                   /* with a zero length               */
    }
}

/* ---- the surface and the reference image --------------------------------- */

#define ASURF_W 24
#define ASURF_H 10
#define ASURF_PITCH (ASURF_W * 2)
#define ASENTINEL 0x1234

static unsigned short g_asurf[ASURF_W * ASURF_H];
static unsigned short g_awant[ASURF_W * ASURF_H];
static unsigned short g_apal[256];

static void asurf_fill(unsigned short* s)
{
    int i;
    for (i = 0; i < ASURF_W * ASURF_H; i++)
        s[i] = ASENTINEL;
}

/* `dxoff` is where source column x lands: surface column x + dxoff. Unlike
 * the type-3 painters -- whose dispatcher biases the destination by
 * -src.left and which then STEP the output over each clipped pixel -- this
 * painter does neither: `lc_one`/`lc_step` advance only the index pointer,
 * so the first VISIBLE pixel lands on dst.x and dxoff is -src.left. */
static void aexpect(const AOp rows[ASPR_H][8], int top, int h,
                    int vis_lo, int vis_hi, int dxoff)
{
    int r, i, k, x, y;

    asurf_fill(g_awant);
    for (y = 0; y < h; y++) {
        r = top + y;
        x = 0;
        for (i = 0; rows[r][i].kind != AOP_END; i++) {
            const AOp* op = &rows[r][i];
            int n = (op->kind == AOP_COPY || op->kind == AOP_REPEAT
                  || op->kind == AOP_SKIPN) ? op->len : 1;
            for (k = 0; k < n; k++, x++) {
                if (op->kind == AOP_SKIP1 || op->kind == AOP_SKIPN)
                    continue;
                if (x < vis_lo || x >= vis_hi)
                    continue;
                g_awant[y * ASURF_W + x + dxoff] =
                    g_apal[op->kind == AOP_COPY ? op->idx[k] : op->idx[0]];
            }
        }
    }
}

static void acompare(const char* what)
{
    int i, bad = -1;
    ll_checks++;
    for (i = 0; i < ASURF_W * ASURF_H; i++)
        if (g_asurf[i] != g_awant[i]) { bad = i; break; }
    if (bad < 0)
        ll_pass(what);
    else
        ll_fail(what, "pixel (%d,%d): got 0x%04x, want 0x%04x",
                bad % ASURF_W, bad / ASURF_W, g_asurf[bad], g_awant[bad]);
}

/* The record the painter walks: an LLSRec header with ONE frame following it
 * at +0x18, no base image and no override. The AnimFrame header is
 * {int size; int npixels;}, then the index block, then -- for frame 0 only --
 * the 0x200-byte palette, then the control words. Held as an int array so the
 * dword loads inside the painter are aligned. */
#define AFRAME_BYTES (8 + 256 + 0x200 + 1024)
static int g_rec[(0x18 + AFRAME_BYTES) / 4 + 1];

void test_anim_paint(void)
{
    WinRect src;
    Pos     dst;
    LLSRec* lls;
    int*    hdr;
    int     i;

    for (i = 0; i < 256; i++)
        g_apal[i] = (unsigned short)(0x8000u | (i * 7u));

    emit_frame(g_arows, ASPR_H);
    /* 27 = 7 + 1 + 0 + 12 + 5 + 2, counted from g_arows by hand. */
    LL_CHECK_INT("the frame holds 27 index bytes", g_nidx, 27);
    LL_CHECK_TRUE("the control stream fits the scratch words",
                  g_nwords > 0 && g_nwords < 64);

    lls = (LLSRec*)g_rec;
    lls->frame = 0;
    lls->pad02 = 0;
    lls->w = ASPR_W;
    lls->h = ASPR_H;
    lls->pad0c = 0;
    lls->nframes = 1;
    lls->pad12 = 0;
    lls->flags = 0;

    hdr = (int*)lls->frames;
    hdr[1] = (int)g_nidx;                                    /* npixels */
    memcpy(lls->frames + 8, g_idx, g_nidx);
    memcpy(lls->frames + 8 + g_nidx, g_apal, 0x200);         /* the palette */
    memcpy(lls->frames + 8 + g_nidx + 0x200, g_words, g_nwords * 4);
    hdr[0] = (int)(8 + g_nidx + 0x200 + g_nwords * 4);       /* size */

    g_lock.ddsd.lpSurface = g_asurf;
    g_lock.ddsd.lPitch = ASURF_PITCH;
    g_frame_override = -1;
    g_override_palette = 0;
    g_sp_mouse_pixel = 0;

    /* ---- 1. unclipped ------------------------------------------------- */
    src.left = 0; src.top = 0; src.right = ASPR_W; src.bottom = ASPR_H;
    dst.x = 0; dst.y = 0;
    g_blit_hit = 0;
    asurf_fill(g_asurf);
    SoftBlitAnimPlain(lls, &src, &dst);
    aexpect(g_arows, 0, ASPR_H, 0, ASPR_W, 0);
    acompare("SoftBlitAnimPlain 0x00464480 paints the whole frame");
    LL_CHECK_INT("no mouse, no hit", g_blit_hit, 0);

    /* ---- 2. src->top rows consumed ------------------------------------- */
    src.left = 0; src.top = 2; src.right = ASPR_W; src.bottom = ASPR_H;
    asurf_fill(g_asurf);
    SoftBlitAnimPlain(lls, &src, &dst);
    aexpect(g_arows, 2, ASPR_H - 2, 0, ASPR_W, 0);
    acompare("the top-skip pass consumes two whole rows");

    /* ---- 3. the right clip -------------------------------------------- */
    src.left = 0; src.top = 0; src.right = 7; src.bottom = ASPR_H;
    asurf_fill(g_asurf);
    SoftBlitAnimPlain(lls, &src, &dst);
    aexpect(g_arows, 0, ASPR_H, 0, 7, 0);
    acompare("a width of 7 keeps source columns 0..6");

    /* ---- 4. the left clip ---------------------------------------------- */
    /* The first VISIBLE source column lands on dst.x: this painter neither
     * receives a biased destination nor steps the output over the skipped
     * pixels, so with dst.x = 0 source column 3 lands on surface column 0. */
    src.left = 3; src.top = 0; src.right = ASPR_W; src.bottom = ASPR_H;
    asurf_fill(g_asurf);
    SoftBlitAnimPlain(lls, &src, &dst);
    aexpect(g_arows, 0, ASPR_H, 3, ASPR_W, -3);
    acompare("a left skip of 3 keeps source columns 3..11");

    /* ---- 5. both edges -------------------------------------------------- */
    src.left = 3; src.top = 1; src.right = 9; src.bottom = ASPR_H;
    asurf_fill(g_asurf);
    SoftBlitAnimPlain(lls, &src, &dst);
    aexpect(g_arows, 1, ASPR_H - 1, 3, 9, -3);
    acompare("left 3 and width 6 keep source columns 3..8");

    /* ---- 6. the mouse hit test ------------------------------------------ */
    src.left = 0; src.top = 0; src.right = ASPR_W; src.bottom = ASPR_H;
    dst.x = 0; dst.y = 0;
    g_blit_hit = 0;
    asurf_fill(g_asurf);
    g_sp_mouse_pixel = (void*)&g_asurf[1 * ASURF_W + 5];   /* row 1's run */
    SoftBlitAnimPlain(lls, &src, &dst);
    LL_CHECK_INT("a mouse inside a painted run hits", g_blit_hit, 1);

    g_blit_hit = 0;
    asurf_fill(g_asurf);
    g_sp_mouse_pixel = (void*)&g_asurf[2 * ASURF_W + 5];   /* row 2 is empty */
    SoftBlitAnimPlain(lls, &src, &dst);
    LL_CHECK_INT("a mouse on a wholly transparent row does not hit",
                 g_blit_hit, 0);

    g_sp_mouse_pixel = 0;
}
