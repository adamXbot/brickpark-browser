/* LEGOLAND portable build -- a bitmap font and the DrawText layout engine
 * (scope PORT-B2).
 *
 * Why this file exists
 * -------------------------------------------------------------------------
 * PORT-B left GDI text as a no-op that reported success, so every label the
 * front end draws was INVISIBLE while its layout maths still ran
 * (docs/lanes/scope-port-b.md §7). That is not good enough for the first
 * front-end frame: the game's title screen, the park name, the money readout
 * and every button caption are GDI text. text.c says so in its own header --
 * "the game does not rasterise text itself: every Print* routine borrows a GDI
 * DC from the DirectDraw draw surface".
 *
 * Worse than invisible: `DrawTextA` is also the game's TEXT MEASURING call.
 * Eleven call sites pass DT_CALCRECT (0x400) and use the rect that comes back
 * to size something -- frontend2.c:521 centres a button caption that way,
 * bighelp.c:316 and bubblecache.c:500 size a speech bubble around the text it
 * has to hold, misc3.c:1105 measures a wrapped paragraph. A DrawTextA that
 * leaves the rect alone and returns 1 does not merely hide the glyphs, it
 * gives the game a 1-pixel-tall answer for every block of text it lays out.
 * So the measuring half has to be real even more than the drawing half, and
 * both have to agree, which is why one engine does both.
 *
 * What the game asks for
 * -------------------------------------------------------------------------
 * InitScreen (screen.c:1343) builds four fonts from one LOGFONT in face
 * "Lego", all upright, no underline, quality 2:
 *
 *     g_font_24   lfHeight 24, lfWeight 700      text.c's SelectFont(.., 1)
 *     g_font_28   lfHeight 28, lfWeight 400      SelectFont(.., default)
 *     g_font_20   lfHeight 20, lfWeight 700      SelectFont(.., 2)
 *     g_font_18   lfHeight 18, lfWeight 600      SelectFont(.., 3)
 *
 * "Lego" is a proportional TrueType face that is not in this tree and could
 * not be rasterised here anyway. The faithful thing is therefore impossible;
 * the useful thing is a face whose METRICS are in the same ballpark, so the
 * game's own layout lands where it landed on Windows. A 6x7 ink box in an
 * 8-row cell, scaled to the requested lfHeight, gives an average advance of
 * about 0.45 em -- a normal proportional-font figure -- so a 640-pixel line
 * holds about 53 characters of the 24-pixel font, which is what the original
 * held. Lines that fitted still fit and lines that wrapped still wrap, within
 * a character.
 *
 * The glyphs are drawn here as ASCII art, one line per character, eight quoted
 * six-column rows each. They are this lane's own drawing, not a copy of any
 * shipped font: no bitmap from the game, from Windows or from any third-party
 * face is reproduced. Rows 0..6 are the ink box; row 7 is the descender row,
 * used only by g j p q y , ; _ and the brackets.
 *
 * Scaling is nearest-neighbour from the 6x7 source into a gw x gh target box
 * computed from lfHeight. It is blocky at 24 and 28 pixels. It is also legible,
 * which is the whole point: the bar set for this lane is "text is visible, not
 * invisible", and a real outline rasteriser in the host shim would be a far
 * bigger thing than the first frame is worth.
 */
#include <string.h>

#include "ll_host.h"

/* ---- the face ----------------------------------------------------------- */
/* 95 printable ASCII characters, 0x20..0x7e, in order. Each entry is eight
 * 6-column rows; '.' is background, anything else is ink. */
#define LL_GLYPH_W 6
#define LL_GLYPH_H 8
#define LL_INK_W   6
#define LL_INK_H   7     /* rows 0..6; row 7 is the descender row */

static const char* const kFaceArt[95] = {
/*     */ "......" "......" "......" "......" "......" "......" "......" "......",
/*  !  */ "..XX.." "..XX.." "..XX.." "..XX.." "..XX.." "......" "..XX.." "......",
/*  "  */ ".XX.XX" ".XX.XX" ".X..X." "......" "......" "......" "......" "......",
/*  #  */ ".X..X." ".X..X." "XXXXXX" ".X..X." "XXXXXX" ".X..X." ".X..X." "......",
/*  $  */ "..XX.." ".XXXXX" "XX.X.." ".XXXX." "..X.XX" "XXXXX." "..XX.." "......",
/*  %  */ "XX...X" "XX..X." "...X.." "..X..." ".X..XX" "X...XX" "......" "......",
/*  &  */ ".XXX.." "XX..X." "XX.X.." ".XX..." "XX.X.X" "XX..X." ".XXX.X" "......",
/*  '  */ "..XX.." "..XX.." "..X..." "......" "......" "......" "......" "......",
/*  (  */ "...XX." "..XX.." ".XX..." ".XX..." ".XX..." "..XX.." "...XX." "......",
/*  )  */ ".XX..." "..XX.." "...XX." "...XX." "...XX." "..XX.." ".XX..." "......",
/*  *  */ "......" "X..X.." ".XXX.." "XXXXX." ".XXX.." "X..X.." "......" "......",
/*  +  */ "......" "..X..." "..X..." "XXXXX." "..X..." "..X..." "......" "......",
/*  ,  */ "......" "......" "......" "......" "......" "..XX.." "..XX.." ".XX...",
/*  -  */ "......" "......" "......" "XXXXX." "......" "......" "......" "......",
/*  .  */ "......" "......" "......" "......" "......" "..XX.." "..XX.." "......",
/*  /  */ ".....X" "....X." "...X.." "..X..." ".X...." "X....." "......" "......",
/*  0  */ ".XXXX." "XX..XX" "XX.XXX" "XXXXXX" "XXX.XX" "XX..XX" ".XXXX." "......",
/*  1  */ "..XX.." ".XXX.." "..XX.." "..XX.." "..XX.." "..XX.." "XXXXXX" "......",
/*  2  */ ".XXXX." "XX..XX" "....XX" "..XXX." ".XX..." "XX...." "XXXXXX" "......",
/*  3  */ ".XXXX." "XX..XX" "....XX" "..XXX." "....XX" "XX..XX" ".XXXX." "......",
/*  4  */ "...XX." "..XXX." ".XXXX." "XX.XX." "XXXXXX" "...XX." "...XX." "......",
/*  5  */ "XXXXXX" "XX...." "XXXXX." "....XX" "....XX" "XX..XX" ".XXXX." "......",
/*  6  */ "..XXX." ".XX..." "XX...." "XXXXX." "XX..XX" "XX..XX" ".XXXX." "......",
/*  7  */ "XXXXXX" "XX..XX" "....XX" "...XX." "..XX.." "..XX.." "..XX.." "......",
/*  8  */ ".XXXX." "XX..XX" "XX..XX" ".XXXX." "XX..XX" "XX..XX" ".XXXX." "......",
/*  9  */ ".XXXX." "XX..XX" "XX..XX" ".XXXXX" "....XX" "...XX." ".XXX.." "......",
/*  :  */ "......" "..XX.." "..XX.." "......" "..XX.." "..XX.." "......" "......",
/*  ;  */ "......" "..XX.." "..XX.." "......" "..XX.." "..XX.." ".XX..." "......",
/*  <  */ "...XX." "..XX.." "XX...." "..XX.." "...XX." "......" "......" "......",
/*  =  */ "......" "......" "XXXXX." "......" "XXXXX." "......" "......" "......",
/*  >  */ ".XX..." "..XX.." "....XX" "..XX.." ".XX..." "......" "......" "......",
/*  ?  */ ".XXXX." "XX..XX" "....XX" "...XX." "..XX.." "......" "..XX.." "......",
/*  @  */ ".XXXX." "XX..XX" "XX.XXX" "XX.XXX" "XX.XXX" "XX...." ".XXXX." "......",
/*  A  */ "..XX.." ".XXXX." "XX..XX" "XX..XX" "XXXXXX" "XX..XX" "XX..XX" "......",
/*  B  */ "XXXXX." "XX..XX" "XX..XX" "XXXXX." "XX..XX" "XX..XX" "XXXXX." "......",
/*  C  */ ".XXXX." "XX..XX" "XX...." "XX...." "XX...." "XX..XX" ".XXXX." "......",
/*  D  */ "XXXXX." "XX..XX" "XX..XX" "XX..XX" "XX..XX" "XX..XX" "XXXXX." "......",
/*  E  */ "XXXXXX" "XX...." "XX...." "XXXXX." "XX...." "XX...." "XXXXXX" "......",
/*  F  */ "XXXXXX" "XX...." "XX...." "XXXXX." "XX...." "XX...." "XX...." "......",
/*  G  */ ".XXXX." "XX..XX" "XX...." "XX.XXX" "XX..XX" "XX..XX" ".XXXX." "......",
/*  H  */ "XX..XX" "XX..XX" "XX..XX" "XXXXXX" "XX..XX" "XX..XX" "XX..XX" "......",
/*  I  */ ".XXXX." "..XX.." "..XX.." "..XX.." "..XX.." "..XX.." ".XXXX." "......",
/*  J  */ "..XXXX" "...XX." "...XX." "...XX." "...XX." "XX.XX." ".XXX.." "......",
/*  K  */ "XX..XX" "XX.XX." "XXXX.." "XXX..." "XXXX.." "XX.XX." "XX..XX" "......",
/*  L  */ "XX...." "XX...." "XX...." "XX...." "XX...." "XX...." "XXXXXX" "......",
/*  M  */ "X....X" "XX..XX" "XXXXXX" "X.XX.X" "X....X" "X....X" "X....X" "......",
/*  N  */ "XX..XX" "XXX.XX" "XXXXXX" "XX.XXX" "XX..XX" "XX..XX" "XX..XX" "......",
/*  O  */ ".XXXX." "XX..XX" "XX..XX" "XX..XX" "XX..XX" "XX..XX" ".XXXX." "......",
/*  P  */ "XXXXX." "XX..XX" "XX..XX" "XXXXX." "XX...." "XX...." "XX...." "......",
/*  Q  */ ".XXXX." "XX..XX" "XX..XX" "XX..XX" "XX.XXX" "XX.XX." ".XXX.X" "......",
/*  R  */ "XXXXX." "XX..XX" "XX..XX" "XXXXX." "XXXX.." "XX.XX." "XX..XX" "......",
/*  S  */ ".XXXXX" "XX...." "XX...." ".XXXX." "....XX" "....XX" "XXXXX." "......",
/*  T  */ "XXXXXX" "..XX.." "..XX.." "..XX.." "..XX.." "..XX.." "..XX.." "......",
/*  U  */ "XX..XX" "XX..XX" "XX..XX" "XX..XX" "XX..XX" "XX..XX" ".XXXX." "......",
/*  V  */ "XX..XX" "XX..XX" "XX..XX" "XX..XX" "XX..XX" ".XXXX." "..XX.." "......",
/*  W  */ "X....X" "X....X" "X....X" "X.XX.X" "XXXXXX" "XX..XX" "X....X" "......",
/*  X  */ "XX..XX" "XX..XX" ".XXXX." "..XX.." ".XXXX." "XX..XX" "XX..XX" "......",
/*  Y  */ "XX..XX" "XX..XX" "XX..XX" ".XXXX." "..XX.." "..XX.." "..XX.." "......",
/*  Z  */ "XXXXXX" "....XX" "...XX." "..XX.." ".XX..." "XX...." "XXXXXX" "......",
/*  [  */ "..XXXX" "..XX.." "..XX.." "..XX.." "..XX.." "..XX.." "..XXXX" "......",
/*  \  */ "X....." ".X...." "..X..." "...X.." "....X." ".....X" "......" "......",
/*  ]  */ "XXXX.." "..XX.." "..XX.." "..XX.." "..XX.." "..XX.." "XXXX.." "......",
/*  ^  */ "..XX.." ".XXXX." "XX..XX" "......" "......" "......" "......" "......",
/*  _  */ "......" "......" "......" "......" "......" "......" "......" "XXXXXX",
/*  `  */ ".XX..." "..XX.." "......" "......" "......" "......" "......" "......",
/*  a  */ "......" "......" ".XXXX." "....XX" ".XXXXX" "XX..XX" ".XXXXX" "......",
/*  b  */ "XX...." "XX...." "XXXXX." "XX..XX" "XX..XX" "XX..XX" "XXXXX." "......",
/*  c  */ "......" "......" ".XXXXX" "XX...." "XX...." "XX...." ".XXXXX" "......",
/*  d  */ "....XX" "....XX" ".XXXXX" "XX..XX" "XX..XX" "XX..XX" ".XXXXX" "......",
/*  e  */ "......" "......" ".XXXX." "XX..XX" "XXXXXX" "XX...." ".XXXX." "......",
/*  f  */ "...XX." "..XX.X" "..XX.." "XXXXX." "..XX.." "..XX.." "..XX.." "......",
/*  g  */ "......" "......" ".XXXXX" "XX..XX" "XX..XX" ".XXXXX" "....XX" ".XXXX.",
/*  h  */ "XX...." "XX...." "XXXXX." "XX..XX" "XX..XX" "XX..XX" "XX..XX" "......",
/*  i  */ "..XX.." "......" "..XX.." "..XX.." "..XX.." "..XX.." "..XX.." "......",
/*  j  */ "...XX." "......" "...XX." "...XX." "...XX." "...XX." "XX.XX." ".XXX..",
/*  k  */ "XX...." "XX..XX" "XX.XX." "XXXX.." "XXXX.." "XX.XX." "XX..XX" "......",
/*  l  */ ".XX..." ".XX..." ".XX..." ".XX..." ".XX..." ".XX..." "..XXX." "......",
/*  m  */ "......" "......" "XXXXX." "XX.X.X" "XX.X.X" "XX.X.X" "XX.X.X" "......",
/*  n  */ "......" "......" "XXXXX." "XX..XX" "XX..XX" "XX..XX" "XX..XX" "......",
/*  o  */ "......" "......" ".XXXX." "XX..XX" "XX..XX" "XX..XX" ".XXXX." "......",
/*  p  */ "......" "......" "XXXXX." "XX..XX" "XX..XX" "XXXXX." "XX...." "XX....",
/*  q  */ "......" "......" ".XXXXX" "XX..XX" "XX..XX" ".XXXXX" "....XX" "....XX",
/*  r  */ "......" "......" "XX.XXX" "XXXX.." "XX...." "XX...." "XX...." "......",
/*  s  */ "......" "......" ".XXXXX" "XX...." ".XXXX." "....XX" "XXXXX." "......",
/*  t  */ "..XX.." "..XX.." "XXXXX." "..XX.." "..XX.." "..XX.X" "...XX." "......",
/*  u  */ "......" "......" "XX..XX" "XX..XX" "XX..XX" "XX..XX" ".XXXXX" "......",
/*  v  */ "......" "......" "XX..XX" "XX..XX" "XX..XX" ".XXXX." "..XX.." "......",
/*  w  */ "......" "......" "XX.X.X" "XX.X.X" "XX.X.X" "XXXXXX" ".XX.X." "......",
/*  x  */ "......" "......" "XX..XX" ".XXXX." "..XX.." ".XXXX." "XX..XX" "......",
/*  y  */ "......" "......" "XX..XX" "XX..XX" "XX..XX" ".XXXXX" "....XX" ".XXXX.",
/*  z  */ "......" "......" "XXXXXX" "...XX." "..XX.." ".XX..." "XXXXXX" "......",
/*  {  */ "...XXX" "..XX.." "..XX.." ".XX..." "..XX.." "..XX.." "...XXX" "......",
/*  |  */ "..XX.." "..XX.." "..XX.." "..XX.." "..XX.." "..XX.." "..XX.." "......",
/*  }  */ "XXX..." "..XX.." "..XX.." "...XX." "..XX.." "..XX.." "XXX..." "......",
/*  ~  */ "......" ".XX..X" "XX.XX." "X...XX" "......" "......" "......" "......"
};

/* Rows packed to bits, bit (LL_GLYPH_W-1 - column). Built once. */
static unsigned char g_face[95][LL_GLYPH_H];
static int           g_face_ready;

static void face_build(void)
{
    int g, y, x;
    if (g_face_ready)
        return;
    for (g = 0; g < 95; g++) {
        const char* art = kFaceArt[g];
        for (y = 0; y < LL_GLYPH_H; y++) {
            unsigned char bits = 0;
            for (x = 0; x < LL_GLYPH_W; x++)
                if (art[y * LL_GLYPH_W + x] != '.')
                    bits |= (unsigned char)(1u << (LL_GLYPH_W - 1 - x));
            g_face[g][y] = bits;
        }
    }
    g_face_ready = 1;
}

/* The glyph rows for a byte. Everything outside 0x20..0x7e draws as a blank,
 * which is what the game's own strings need: the string table is plain ASCII
 * (text.c's GetString) and a stray high byte must not read out of bounds. */
static const unsigned char* glyph_of(unsigned char ch)
{
    static const unsigned char blank[LL_GLYPH_H] = { 0 };
    face_build();
    if (ch < 0x20 || ch > 0x7e)
        return blank;
    return g_face[ch - 0x20];
}

/* ---- metrics ------------------------------------------------------------ */
/* lfHeight is a CELL height: em plus internal leading. The ink box is sized
 * from it so that the four fonts InitScreen creates stay distinguishable and
 * the average advance lands near 0.45 em:
 *
 *   lfHeight  ink box   advance (400)   advance (>=600, bold)
 *       18      7x15          8                  9
 *       20      8x16          9                 10
 *       24     10x20         11                 12
 *       28     11x23         12                 13
 *
 * A bold face is the same glyph smeared one pixel to the right, so it costs one
 * pixel of advance -- the same relationship a real bold face has.
 */
void ll_font_metrics(int cell_h, int weight, LLFontMetrics* m)
{
    int h = cell_h;
    if (h < 0)
        h = -h;                     /* a negative lfHeight is a CHARACTER height */
    if (h < 8)
        h = 8;
    if (h > 200)
        h = 200;
    m->cell_h = h;
    m->weight = weight;
    m->bold   = weight >= 600;
    m->gh     = h * 5 / 6;
    m->gw     = h * 5 / 12;
    if (m->gh < LL_INK_H) m->gh = LL_INK_H;
    if (m->gw < 3)        m->gw = 3;
    m->advance = m->gw + 1 + (m->bold ? 1 : 0);
    m->line_h  = h;
    m->ascent  = m->gh;
}

void ll_font_default_metrics(LLFontMetrics* m) { ll_font_metrics(20, 400, m); }

int ll_font_text_width(const LLFontMetrics* m, const char* s, int n)
{
    int w = 0, i;
    if (!s)
        return 0;
    for (i = 0; i < n; i++) {
        if (s[i] == '\n' || s[i] == '\r')
            break;
        w += m->advance;
    }
    return w;
}

/* ---- one glyph ---------------------------------------------------------- */
/* Nearest-neighbour from the 6x7 ink box (plus the descender row) into a
 * gw x gh target box whose top-left is (x, y). The source is sampled over
 * LL_INK_H+1 rows so descenders land below the box, exactly as they do in a
 * real font's cell. */
static void draw_glyph(const LLFontTarget* t, const LLFontMetrics* m,
                       int x, int y, unsigned char ch, unsigned short fg)
{
    const unsigned char* rows = glyph_of(ch);
    int box_h = m->gh * (LL_INK_H + 1) / LL_INK_H;   /* room for row 7 */
    int ty, tx;

    for (ty = 0; ty < box_h; ty++) {
        int py = y + ty;
        int sy = ty * (LL_INK_H + 1) / box_h;
        unsigned short* row;
        unsigned char bits;
        if (sy > LL_GLYPH_H - 1) sy = LL_GLYPH_H - 1;
        bits = rows[sy];
        if (!bits)
            continue;
        if (py < t->clip.top || py >= t->clip.bottom)
            continue;
        row = (unsigned short*)((char*)t->bits + (long)py * t->pitch);
        for (tx = 0; tx < m->gw; tx++) {
            int sx = tx * LL_INK_W / m->gw;
            int px = x + tx;
            if (!(bits & (1u << (LL_GLYPH_W - 1 - sx))))
                continue;
            if (px >= t->clip.left && px < t->clip.right)
                row[px] = fg;
            if (m->bold && px + 1 >= t->clip.left && px + 1 < t->clip.right)
                row[px + 1] = fg;
        }
    }
}

/* Fill the cell behind a run of text (SetBkMode(OPAQUE), which Print and
 * PrintCentOpaque both ask for). Win32 fills the character cell, not the ink
 * box, so the fill is advance x line_h. */
static void fill_cells(const LLFontTarget* t, const LLFontMetrics* m,
                       int x, int y, int w, unsigned short bg)
{
    int py, px;
    for (py = y; py < y + m->line_h; py++) {
        unsigned short* row;
        if (py < t->clip.top || py >= t->clip.bottom)
            continue;
        row = (unsigned short*)((char*)t->bits + (long)py * t->pitch);
        for (px = x; px < x + w; px++)
            if (px >= t->clip.left && px < t->clip.right)
                row[px] = bg;
    }
}

/* ---- TextOut ------------------------------------------------------------ */
/* (x, y) is the top-left of the cell: the game never calls SetTextAlign with
 * anything but 0 on a surface DC, so TA_TOP|TA_LEFT is the only alignment that
 * has to be right. The ink is offset down by the cell's internal leading so a
 * 24-pixel cell puts its 20-pixel ink box 2 pixels from the top. */
void ll_font_text_out(const LLFontTarget* t, const LLFontMetrics* m,
                      int x, int y, const char* s, int n,
                      unsigned short fg, int opaque, unsigned short bg)
{
    int i;
    int lead = (m->line_h - m->gh) / 2;
    if (!t || !t->bits || !s || n <= 0)
        return;
    if (lead < 0)
        lead = 0;
    if (opaque)
        fill_cells(t, m, x, y, n * m->advance, bg);
    for (i = 0; i < n; i++) {
        draw_glyph(t, m, x, y + lead, (unsigned char)s[i], fg);
        x += m->advance;
    }
}

/* ---- DrawText ---------------------------------------------------------- */
#define DT_CENTER      0x0001
#define DT_RIGHT       0x0002
#define DT_VCENTER     0x0004
#define DT_BOTTOM      0x0008
#define DT_WORDBREAK   0x0010
#define DT_SINGLELINE  0x0020
#define DT_NOCLIP      0x0100
#define DT_CALCRECT    0x0400

/* Break `s` into lines. Returns the line count; `starts`/`lens` hold at most
 * `max` of them. A line ends at '\n' (unless DT_SINGLELINE), or, under
 * DT_WORDBREAK, at the last space that still fits in `width` -- and if a single
 * word is wider than the box it is broken mid-word, which is what GDI does
 * rather than overflow. */
static int break_lines(const LLFontMetrics* m, const char* s, int n,
                       unsigned int format, int width,
                       const char** starts, int* lens, int max)
{
    int count = 0;
    int i = 0;

    if (format & DT_SINGLELINE) {
        if (max > 0) { starts[0] = s; lens[0] = n; }
        return 1;
    }
    while (i <= n) {
        int start = i;
        int last_space = -1;
        int end;
        if (i == n && count > 0)
            break;                  /* trailing empty line only if the text is */
        while (i < n && s[i] != '\n' && s[i] != '\r') {
            if ((format & DT_WORDBREAK) && width > 0 &&
                (i - start + 1) * m->advance > width && i > start) {
                break;
            }
            if (s[i] == ' ')
                last_space = i;
            i++;
        }
        end = i;
        if ((format & DT_WORDBREAK) && i < n && s[i] != '\n' && s[i] != '\r') {
            if (last_space > start) {
                end = last_space;   /* wrap at the space, drop it */
                i = last_space + 1;
            }
            /* else: an over-long word, broken where it stopped fitting */
        } else if (i < n) {
            /* consume the line break, CRLF as one */
            if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n')
                i += 2;
            else
                i += 1;
        } else {
            i = n + 1;              /* last line consumed */
        }
        if (count < max) { starts[count] = s + start; lens[count] = end - start; }
        count++;
        if (count >= 256)
            break;
    }
    return count ? count : 1;
}

#define LL_MAX_LINES 128

int ll_font_draw_text(const LLFontTarget* t, const LLFontMetrics* m,
                      LLRect* rc, const char* s, int n, unsigned int format,
                      unsigned short fg, int opaque, unsigned short bg)
{
    const char* starts[LL_MAX_LINES];
    int         lens[LL_MAX_LINES];
    int         nlines, i, total_h, y, box_w, widest = 0;
    LLFontTarget use;

    if (!rc || !s)
        return 0;
    if (n < 0)
        n = (int)strlen(s);
    if (n == 0) {
        /* An empty string measures zero, which is what the callers that size a
         * box around text need (bighelp.c:316 grows a bubble by the height this
         * returns; a phantom line would grow an empty bubble). */
        if (format & DT_CALCRECT) {
            rc->bottom = rc->top;
            if (!(format & DT_WORDBREAK))
                rc->right = rc->left;
        }
        return 0;
    }
    box_w = (int)(rc->right - rc->left);

    nlines = break_lines(m, s, n, format, box_w, starts, lens, LL_MAX_LINES);
    if (nlines > LL_MAX_LINES)
        nlines = LL_MAX_LINES;
    for (i = 0; i < nlines; i++) {
        int w = lens[i] * m->advance;
        if (w > widest)
            widest = w;
    }
    total_h = nlines * m->line_h;

    if (format & DT_CALCRECT) {
        /* GDI: the height always comes back; the width comes back too unless
         * DT_WORDBREAK fixed it. This is the branch frontend2.c:521,
         * bighelp.c:316, bubblecache.c:500 and misc3.c:1105 read. */
        rc->bottom = rc->top + total_h;
        if (!(format & DT_WORDBREAK))
            rc->right = rc->left + widest;
        return total_h;
    }
    if (!t || !t->bits)
        return total_h;

    /* Clip to the DC's clip box and, unless DT_NOCLIP, to the rect. */
    use = *t;
    if (!(format & DT_NOCLIP)) {
        if (rc->left   > use.clip.left)   use.clip.left   = rc->left;
        if (rc->top    > use.clip.top)    use.clip.top    = rc->top;
        if (rc->right  < use.clip.right)  use.clip.right  = rc->right;
        if (rc->bottom < use.clip.bottom) use.clip.bottom = rc->bottom;
    }
    if (use.clip.left >= use.clip.right || use.clip.top >= use.clip.bottom)
        return total_h;

    y = (int)rc->top;
    if (format & DT_SINGLELINE) {
        if (format & DT_VCENTER)
            y = (int)rc->top + ((int)(rc->bottom - rc->top) - total_h) / 2;
        else if (format & DT_BOTTOM)
            y = (int)rc->bottom - total_h;
    }
    for (i = 0; i < nlines; i++) {
        int w = lens[i] * m->advance;
        int x = (int)rc->left;
        if (format & DT_CENTER)
            x = (int)rc->left + (box_w - w) / 2;
        else if (format & DT_RIGHT)
            x = (int)rc->right - w;
        ll_font_text_out(&use, m, x, y, starts[i], lens[i], fg, opaque, bg);
        y += m->line_h;
    }
    return total_h;
}

/* ---- colour ------------------------------------------------------------- */
/* A Win32 COLORREF is 0x00bbggrr. The surfaces are RGB565 (ddraw.c refuses
 * anything else, so the format is not in question). */
unsigned short ll_font_colorref_to_565(unsigned long cr)
{
    unsigned int r = (unsigned int)(cr & 0xff);
    unsigned int g = (unsigned int)((cr >> 8) & 0xff);
    unsigned int b = (unsigned int)((cr >> 16) & 0xff);
    return (unsigned short)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}
