/* PORT-C headless test harness (docs/SCOPE_PORT_WAVE.md, docs/lanes/scope-port-c.md).
 *
 * Tiny check/report harness shared by portable/tests/test_*.c. One executable
 * with subcommands: `legoland_tests <name>` runs one test and exits 0 only if
 * every check passed. Every check prints one line, so a ctest failure log is
 * the diagnosis.
 *
 * A check is never "approximately right": a mismatch between the recovered C
 * and the clean-room Python decoder is a FINDING, recorded in the lane notes
 * with a verdict on which side is wrong. LL_NOTE is how a test reports one
 * without turning it into a silent pass. */
#ifndef LL_TESTS_H
#define LL_TESTS_H

#include <stdio.h>
#include <string.h>

extern int ll_checks;
extern int ll_fails;
extern int ll_notes;

void ll_pass(const char* what);
void ll_fail(const char* what, const char* fmt, ...);
void ll_note(const char* what, const char* fmt, ...);
/* A check that could not be made, with the reason. NOT a pass and NOT a
 * failure: `LL_TESTS_NO_GAMEDATA` builds (CI has no game assets, see
 * portable/cmake/tests.cmake) run the synthetic cases of a test and skip the
 * ones that read a shipped file, and a skip has to be visible in the log or the
 * test silently claims coverage it does not have. */
void ll_skip(const char* what, const char* why);

/* FNV-1a 64: the canonical digest both the oracles and the tests compute over
 * a rendering of a whole table, so a committed/generated expectation is one
 * number instead of a copy of the game's data. */
typedef unsigned long long ll_u64;
#define LL_FNV_BASIS 0xcbf29ce484222325ull
#define LL_FNV_PRIME 0x100000001b3ull
ll_u64 ll_fnv_init(void);
ll_u64 ll_fnv_bytes(ll_u64 h, const void* p, unsigned int n);
ll_u64 ll_fnv_str(ll_u64 h, const char* s);
/* Decimal render of a signed value, then a separator: the same text the
 * Python oracles feed their hash. */
ll_u64 ll_fnv_int(ll_u64 h, long long v);

#define LL_CHECK_INT(what, got, want)                                        \
    do {                                                                     \
        long long g_ = (long long)(got), w_ = (long long)(want);              \
        ll_checks++;                                                          \
        if (g_ == w_) ll_pass(what);                                          \
        else ll_fail(what, "got %lld, want %lld", g_, w_);                    \
    } while (0)

#define LL_CHECK_HEX(what, got, want)                                        \
    do {                                                                     \
        ll_u64 g_ = (ll_u64)(got), w_ = (ll_u64)(want);                       \
        ll_checks++;                                                          \
        if (g_ == w_) ll_pass(what);                                          \
        else ll_fail(what, "got 0x%016llx, want 0x%016llx", g_, w_);          \
    } while (0)

/* A NULL `want` means the game is expected to store a NULL POINTER, which is
 * not the same thing as an empty string and is not interchangeable with it:
 * LLIDB_LoadICM (data2.c 0x0047aff0) reads a length and, `if (len == 0)`,
 * stores 0 rather than allocating a one-byte "". An oracle that reports such a
 * field as "" is reporting the file honestly and the C contract wrongly.
 * (PORT-A2; see docs/lanes/scope-port-a2.md §3.) */
#define LL_CHECK_STR(what, got, want)                                        \
    do {                                                                     \
        const char* g_ = (got); const char* w_ = (want);                      \
        ll_checks++;                                                          \
        if (g_ == w_ || (g_ && w_ && strcmp(g_, w_) == 0)) ll_pass(what);     \
        else ll_fail(what, "got \"%s\", want \"%s\"", g_ ? g_ : "(null)",      \
                     w_ ? w_ : "(null)");                                     \
    } while (0)

#define LL_CHECK_TRUE(what, cond)                                            \
    do {                                                                     \
        ll_checks++;                                                          \
        if (cond) ll_pass(what);                                              \
        else ll_fail(what, "false");                                          \
    } while (0)

/* Each test: returns nothing, reports through the macros above. */
void test_save_framing(void);
void test_keystate(void);   /* PORT-A3: interior aliases, no oracle */
void test_rle_paint(void);  /* PORT-B3: the ten type-3 RLE painters */
void test_anim_paint(void); /* PORT-B3: the type-2 LLS animation painter */
#ifndef LL_TESTS_NO_GAMEDATA
/* These four need gamedata/: their oracles read the shipped volumes, so without
 * the assets they cannot even be given a header. tests.cmake leaves their
 * sources out of the driver entirely (PORT-A4). */
void test_tile_geometry(void);
void test_res_archive(void);
void test_llidb_icm(void);
void test_loadpos(void);
/* ---- scope PORT-B5: the remaining inline-asm bodies --------------------- */
void test_anim_recolour(void); /* SoftBlitAnim, the recolouring type-2 painter */
void test_tri_raster(void);    /* tri3d.c's four triangle rasterisers */
void test_zbuf_blit(void);     /* ZBufferHelper and BltAdvisor */
#endif
/* ---- scope PORT-M5: the last three inline-asm span fillers --------------- */
void test_coaster_span(void);  /* Span_FillShade/_FillShadeZ, TrackShade_FillPoly */

#endif /* LL_TESTS_H */
