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
 * game sets the CW to nearest/masked wherever it uses bare fistp. */
#define LL_FISTP(f)  ((int)__builtin_lrintf((float)(f)))
#define LL_FISTPD(d) ((int)__builtin_lrint((double)(d)))

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
