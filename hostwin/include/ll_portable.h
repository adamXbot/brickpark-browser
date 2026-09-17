/* LEGOLAND portable build -- helpers force-included into every LEGOLAND/*.c
 * (see portable/CMakeLists.txt). Only active when LEGOLAND_PORTABLE is
 * defined; the VC6 matching build never sees this file.
 *
 * Rules: this header must NOT include any libc header. Many game sources
 * declare CRT prototypes themselves with VC6 signatures (`unsigned int`
 * sizes) that conflict with a 64-bit host's <string.h>. Use builtins only.
 */
#ifndef LL_PORTABLE_H
#define LL_PORTABLE_H
#ifdef LEGOLAND_PORTABLE

/* Reinterpret 32-bit storage. The sources keep float bits in int slots and
 * vice versa; the portable build compiles with -fno-strict-aliasing. */
#define LL_ASFLT(x) (*(float*)&(x))
#define LL_ASINT(x) (*(int*)&(x))

/* x87 `fistp` with the default control word rounds to nearest-even. The
 * game sets the CW to nearest/masked wherever it uses bare fistp.
 *
 * Spelled out rather than as `__builtin_lrintf` / `__builtin_lrint` (scope
 * PORT-B5).  Those honour the CURRENT rounding mode, so clang cannot fold them
 * and emits a call to libm's `lrintf` / `lrint` -- and in the wasm32 build
 * `gen_link.py` does not see emcc's libm in its defined set, so it generates
 * TRAPPING STUBS for both (`gen-browser/stubs.c`:
 * `lrintf(float) { ll_gen_trap("GAME", "lrintf", "bnvpath.c, coaster10.c,
 * coaster12.c..."); }`).  Every LL_FISTP site in the portable build was
 * therefore a latent trap under node and in the browser -- person3d.c's and
 * math3d.c's included, which is every bloke the renderer draws -- and the
 * first test to reach one (PORT-B5's `tri_raster`, through DrawFlatTexTri's
 * u/v conversion) is what surfaced it.  The helper below hard-codes
 * round-half-to-EVEN, which is the mode the game's own control word selects
 * and the only mode the comment above ever claimed, and it lowers to plain
 * arithmetic with no libcall. */
static inline int ll_fistp_f(float f)
{
    float a = f < 0.0f ? -f : f;
    int   i = (int)a;                       /* truncate */
    float r = a - (float)i;
    if (r > 0.5f) i++;
    else if (r == 0.5f) i += (i & 1);       /* ties to even */
    return f < 0.0f ? -i : i;
}

static inline int ll_fistp_d(double d)
{
    double a = d < 0.0 ? -d : d;
    int    i = (int)a;
    double r = a - (double)i;
    if (r > 0.5) i++;
    else if (r == 0.5) i += (i & 1);
    return d < 0.0 ? -i : i;
}

#define LL_FISTP(f)  ll_fistp_f((float)(f))
#define LL_FISTPD(d) ll_fistp_d((double)(d))

/* `fld v / fmul s / fistp v` on a float lvalue: the int result overwrites
 * the float's storage and the caller reads it back through LL_ASINT. */
#define LL_FISTP_SCALE_INPLACE(v, s) \
    do { int ll_t = LL_FISTP((v) * (s)); __builtin_memcpy(&(v), &ll_t, 4); } while (0)

/* `imul ecx / shrd eax, edx, 16`: 16.16 fixed-point multiply. */
#define LL_FMUL16(a, b) ((int)(((long long)(int)(a) * (long long)(int)(b)) >> 16))

#define LL_DEBUGBREAK() __builtin_trap()

/* An asm body that has not been ported yet. Aborts loudly; the list of
 * these is part of portable/tools/linkreport.py's output. */
void ll_unported_asm(const char* file, int line, const char* func);
#define LL_UNPORTED_ASM() ll_unported_asm(__FILE__, __LINE__, __func__)

/* rdtsc stand-in: the game only ever differences two readings. */
unsigned int ll_rdtsc(void);

/* MSVC CRT names used unprototyped by the sources. */
int stricmp(const char*, const char*);
int strnicmp(const char*, const char*, __SIZE_TYPE__);

/* ---- optional quality-of-life switches ------------------------------------
 *
 * Changes to how the game plays, each OFF unless the host passes its switch.
 * portable.c's ll_qol_take_switches takes them out of the command line before
 * WinMain's parser sees it, and the arms test LL_QOL(bit). They are choices,
 * not docs/QUIRKS.md's fixes, so nothing turns one on by default, and a
 * faithful build (LL_FAITHFUL) compiles every one of them out.
 *
 *   LL_QOL_FREEPLAY_ALL  -ll-freeplay-all: the title's Free Play button is open
 *       from the start (screens2.c); the picker offers every class (fpui2.c)
 *       with no 20000 budget (uimisc3.c, and fpui.c's gauge stops at full); a
 *       picker with nothing ticked starts with everything ticked (fpui3.c's
 *       ll_freeplay_tick_all); a new free-play park opens all four theme tabs
 *       (screens3.c); and what a free-play park makes available is not
 *       written into the profile as earned (frontend2.c).
 */
#define LL_QOL_FREEPLAY_ALL 0x1u
unsigned int ll_qol(void);
unsigned int ll_qol_take_switches(char* cmdline);
void ll_freeplay_tick_all(void);
#ifdef LL_FAITHFUL
#define LL_QOL(bit) 0
#else
#define LL_QOL(bit) ((ll_qol() & (bit)) != 0)
#endif

/* ---- the type-3 RLE control stream (rlepaint.c / rlepaint2.c) -----------
 *
 * The ten hand-written painters all walk the same stream with a ROTATING
 * 2-bit mask: ebx starts at 3, `rol ebx,2` after every code, and the dword
 * pointer edx advances by 4 when the mask has wrapped back to 3
 * (`and ebx,1` / `lea edx,[edx+ebx*4]`).  The code is never shifted down to
 * bit 0: the asm keeps the ISOLATED field `word & mask` and asks which half
 * of the pair is set with `test ebp,0aaaaaaaah` (the high bit) and
 * `test ebp,55555555h` (the low bit).  ll_rle_code returns that same
 * isolated field so LL_RLE_HI/LL_RLE_LO read exactly like the two tests.
 *
 * C is only 2-byte aligned in the shipped assets (A is `frame+0x10`, B is
 * `A + 2*pixel_count` with an odd pixel count allowed, and B's padded length
 * is a multiple of 4), so the dword load must not assume 4-byte alignment. */
typedef struct LLRleCtl {
    const unsigned char* w;     /* edx: the current control dword */
    unsigned int         mask;  /* ebx: the rotating 2-bit mask */
} LLRleCtl;

static inline void ll_rle_open(LLRleCtl* s, const void* c)
{
    s->w = (const unsigned char*)c;
    s->mask = 3;
}

static inline unsigned int ll_rle_code(LLRleCtl* s)
{
    unsigned int word;
    unsigned int field;
    unsigned int rot;

    __builtin_memcpy(&word, s->w, 4);
    field = word & s->mask;                       /* and ebp, ebx       */
    rot = (s->mask << 2) | (s->mask >> 30);       /* rol ebx, 2         */
    s->mask = rot;
    s->w += (rot & 1u) * 4u;                      /* lea edx,[edx+ebx*4]*/
    return field;
}

#define LL_RLE_HI(f) (((f) & 0xaaaaaaaau) != 0u)  /* test ebp,0aaaaaaaah */
#define LL_RLE_LO(f) (((f) & 0x55555555u) != 0u)  /* test ebp,55555555h  */

/* The run hit test the four plain Hit leaves and both rlepaint2.c leaves
 * spell as `mov eax,mouse / sub eax,edi / sar eax,1 / cmp eax,ecx / jae`:
 * a SIGNED halving of the 32-bit byte difference, compared UNSIGNED against
 * the run length, so a mouse before the run wraps huge and cannot hit. */
static inline int ll_rle_hit_run(const void* mouse, const void* dst,
                                 unsigned int len)
{
    unsigned int d = (unsigned int)((const char*)mouse - (const char*)dst);
    return (unsigned int)((int)d >> 1) < len;
}

/* ---- the type-2 LLS control stream (softblit.c / softblit2.c) -----------
 *
 * The animation painters walk a DIFFERENT stream from the type-3 painters
 * above: `ebp` steps 32-bit control words, `ebx` holds the word being
 * consumed, and `g_zb_bits` counts the codes left in it. The refill idiom is
 *
 *     mov eax,g_zb_bits / shr ebx,2 / dec eax / jns have
 *     mov eax,0Fh / mov ebx,[ebp] / add ebp,4
 *   have: mov g_zb_bits,eax
 *
 * -- the shift happens unconditionally and is thrown away when the word runs
 * out, and the code is then the LOW TWO BITS of ebx. An 8-bit run length is
 * spliced out of the low byte (`shr ebx,2 / movzx ecx,bl / shr ebx,6`) and
 * costs four codes, so its own underflow reloads with a count of 0Ch and
 * takes the length from the NEW word's low byte, discarding what was left of
 * the old one.
 *
 * Grammar: bit1 = 0 -> one index byte; bit1 = 1, bit0 = 0 -> one transparent
 * pixel; bit1 = 1, bit0 = 1 -> a length follows, zero ends the row, and a
 * second code says skip (bit1 = 1), copy `n` index bytes (00) or repeat one
 * index byte `n` times (01). The pixels are 8-bit indices through
 * `g_sp_pal16`, not the u16 words of the type-3 stream. */
typedef struct LLAnimCtl {
    const unsigned char* w;     /* ebp: the next control dword       */
    unsigned int         cur;   /* ebx: the word being consumed      */
    int                  bits;  /* g_zb_bits: codes left in `cur`    */
} LLAnimCtl;

static inline void ll_anim_open(LLAnimCtl* s, const void* words)
{
    s->w = (const unsigned char*)words;
    s->cur = 0;
    s->bits = 0;
}

static inline unsigned int ll_anim_code(LLAnimCtl* s)
{
    s->cur >>= 2;
    if (--s->bits < 0) {
        s->bits = 15;
        __builtin_memcpy(&s->cur, s->w, 4);
        s->w += 4;
    }
    return s->cur & 3u;
}

static inline unsigned int ll_anim_count(LLAnimCtl* s)
{
    unsigned int n;

    s->cur >>= 2;
    s->bits -= 4;
    n = s->cur & 0xffu;
    if (s->bits < 0) {
        s->bits = 12;
        __builtin_memcpy(&s->cur, s->w, 4);
        s->w += 4;
        n = s->cur & 0xffu;
    }
    s->cur >>= 6;
    return n;
}

/* `cmp edi,mouse / movzx eax,g_sp_hit_armed / sbb ecx,-1 / and eax,ecx`, with
 * ecx zero after the `rep`: sbb turns the borrow into `dst >= mouse`, and the
 * result is OR'ed into the LOW BYTE of g_blit_hit (the animation painters
 * address the flag as a byte; the type-3 painters use `or dword`). */
static inline void ll_anim_hit(const void* dst, const void* mouse,
                               unsigned char armed, int* flag)
{
    unsigned int inr = ((const unsigned char*)dst
                        >= (const unsigned char*)mouse);
    unsigned char* b = (unsigned char*)flag;
    *b = (unsigned char)((armed & inr) | *b);
}

/* The two straight-line software blits that the game writes as inline asm
 * in softblit.c and bigrender.c: an 8-bit paletted source and a 16-bit
 * source, both into the locked 16-bpp surface, skipping the transparent
 * colour, with an optional recolour mask (0xffff = none). `spw` is the
 * source row stride in BYTES; `sl..sb` the source rectangle in pixels. */
static inline void ll_blit8(void* surf, long pitch, long dx, long dy,
                            const void* pix, long spw,
                            long sl, long st, long sr, long sb,
                            const void* pal, int transparent, int recolour)
{
    const unsigned char*  s = (const unsigned char*)pix + st * spw + sl;
    unsigned short*       d = (unsigned short*)((char*)surf + dy * pitch + dx * 2);
    const unsigned short* p = (const unsigned short*)pal;
    long x, y, w = sr - sl, h = sb - st;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned int v = p[s[x]];
            if ((int)v != transparent)
                d[x] = (unsigned short)(v & (unsigned int)recolour);
        }
        s += spw;
        d = (unsigned short*)((char*)d + pitch);
    }
}

static inline void ll_blit16(void* surf, long pitch, long dx, long dy,
                             const void* pix, long spw,
                             long sl, long st, long sr, long sb,
                             int transparent, int recolour)
{
    const unsigned short* s = (const unsigned short*)((const char*)pix + st * spw + sl * 2);
    unsigned short*       d = (unsigned short*)((char*)surf + dy * pitch + dx * 2);
    long x, y, w = sr - sl, h = sb - st;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned int v = s[x];
            if ((int)v != transparent)
                d[x] = (unsigned short)(v & (unsigned int)recolour);
        }
        s = (const unsigned short*)((const char*)s + spw);
        d = (unsigned short*)((char*)d + pitch);
    }
}

#endif /* LEGOLAND_PORTABLE */
#endif /* LL_PORTABLE_H */
