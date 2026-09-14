/* keystate -- interior aliases: one object, not N fragments.  Scope PORT-A3.
 *
 * This test is about the GENERATOR, not about a game subsystem, so it needs no
 * oracle: the expectation is the original binary's own layout, which the
 * sources record in their `extern` declarations and their address comments.
 *
 * The defect it pins down (PORT-B2 finding 2, docs/lanes/scope-port-b2.md §5):
 * `gen_link.py` sized every rebuilt global by the gap to the next NAMED
 * address, so a named object that other names reach into at an offset came out
 * as several separately 16-byte-aligned arrays. The DirectInput keyboard state
 * is the sharpest case -- `extern unsigned char g_key_state[256]` (input.c:53)
 * at 0x007fdda0 is ALSO `g_left_ctrl` (+0x1d), `g_left_shift` (+0x2a),
 * `g_right_shift` (+0x36) and `g_right_ctrl` (+0x9d), and those four offsets
 * are exactly the DIK codes DIK_LCONTROL/LSHIFT/RSHIFT/RCONTROL. Split into
 * five arrays:
 *
 *   - `ScanKeyboard`'s `GetDeviceState(kbd, 256, g_key_state)` (input.c:107)
 *     writes 256 bytes into a 29-byte object and scribbles over whatever the
 *     linker put next;
 *   - `IsLShiftDown`/`IsRShiftDown` (sysstubs.c:181/184, `g_left_shift >> 7`)
 *     and `g_left_ctrl`/`g_right_ctrl` (unref6.c:158) read a byte of the wrong
 *     key, so shifted text entry and movie.c's Ctrl+Q skip are dead.
 *
 * Fourteen more objects in the image have the same shape; the biggest is
 * `extern char g_gpu_state[0x3d8]` (util.c:79), which `CheckHostSystemGPU`
 * clears with ONE memset and which contains the DirectDraw object, both
 * surfaces, the clipper and the `DDSURFACEDESC` the software renderer locks
 * through (gpu.c:10's map). Clearing it as a separate object would leave every
 * one of those stale across a display-mode change.
 *
 * Runs natively and on wasm32: every check is a byte offset inside a byte
 * array, which is the same on LP64 and ILP32, and nothing here goes through a
 * serialised struct. No host shim either -- `IsLShiftDown` and `IsRShiftDown`
 * are real recovered game code that touches nothing but these globals. */
#include "ll_tests.h"

/* The declarations are copied verbatim from the game sources named above, so
 * that this test sees exactly the types and extents the game's own
 * translation units see. */
extern unsigned char g_key_state[256];  /* 0x007fdda0  input.c:53 */
extern unsigned char g_left_ctrl;       /* 0x007fddbd  unref6.c:158 */
extern unsigned char g_left_shift;      /* 0x007fddca  sysstubs.c:105 */
extern unsigned char g_right_shift;     /* 0x007fddd6  sysstubs.c:106 */
extern unsigned char g_right_ctrl;      /* 0x007fde3d  unref6.c:159 */
extern int IsLShiftDown(void);          /* 0x00474070  sysstubs.c:181 */
extern int IsRShiftDown(void);          /* 0x00474080  sysstubs.c:184 */

/* The first named address PAST the keyboard array: bighelp.c's pop-up UI block
 * (0x007fdda0 + 256 = 0x007fdea0, and the next name is at 0x007fdea4). A
 * 256-byte write that stays inside its object cannot touch this. */
extern void* g_info_icon_g;             /* 0x007fdea4  iconui.c:91 */

extern char g_gpu_state[0x3d8];         /* 0x00667d70  util.c:79 */
extern void* g_ddraw1;                  /* 0x00667d70  gpu.c:179 */
extern void* g_primary;                 /* 0x00668070  gpu.c map */
extern void* g_draw_surface;            /* 0x0066807c  gpu.c map */
extern unsigned long g_ddsd_width;      /* 0x006680a8  surface.c */
extern unsigned long g_ddsd_height;     /* 0x006680a4  surface.c */
extern unsigned long g_ddsd_pitch;      /* 0x006680ac  surface.c */
extern void* g_ddsd_bits;               /* 0x006680c0  surface.c */
extern int g_video_locked;              /* 0x00668144  surface.c */

extern int g_entrance_x;                /* 0x004b8320  mapobj.c:28 */
extern int g_entrance_y;                /* 0x004b8324  mapobj.c:29 */

extern unsigned char g_cur_save_slot;   /* 0x0080ffe4  frontend2.c:120 */
extern int g_cur_save_slot_wide;        /* 0x0080ffe4  gameframe.c:169 */
extern unsigned char g_profile_unlocked[200]; /* 0x0080ffe6  fpui2.c:168 */

/* An interior alias is invisible to the OPTIMIZER: `g_ddraw1` and
 * `g_gpu_state` are two distinct extern objects as far as the C type system is
 * concerned, so a compiler that sees a store to one and a load from the other
 * in the same function may assume they cannot overlap and keep the cached
 * value. (This is not type-based aliasing -- `-fno-strict-aliasing` does not
 * change it -- it is the distinct-global assumption.) Measured: at -O2 on
 * wasm32 this test's own `memset(g_gpu_state, 0, 0x3d8); g_ddraw1 == 0` read
 * the pre-memset pointer, and the three-byte overlay read its pre-store value;
 * at -O0 natively both pass. Every access that deliberately crosses from a host
 * to an alias therefore goes through one of these, which forces the real load
 * or store. The game's own code never does it inside one function -- see
 * docs/lanes/scope-port-a3.md §1 for the condition and the sweep. */
#define LL_VPTR(x)  (*(void* volatile*)&(x))
#define LL_VINT(x)  (*(volatile int*)&(x))
#define LL_VU32(x)  (*(volatile unsigned int*)&(x))
#define LL_VU8(x)   (*(volatile unsigned char*)&(x))

/* `what` reads as the image fact being asserted, so a failure line names the
 * two symbols and the offset between them that the binary requires. */
static void offset_is(const char* what, const void* base, const void* inner,
                      long want)
{
    long got = (long)((const char*)inner - (const char*)base);
    ll_checks++;
    if (got == want)
        ll_pass(what);
    else
        ll_fail(what, "offset %ld, want %ld (not one object)", got, want);
}

void test_keystate(void)
{
    unsigned char probe[256];
    void* icon_before;
    int i;

    /* ---- 1. the keyboard array is ONE object ------------------------- */
    offset_is("g_left_ctrl is g_key_state + DIK_LCONTROL (0x1d)",
              g_key_state, &g_left_ctrl, 0x1d);
    offset_is("g_left_shift is g_key_state + DIK_LSHIFT (0x2a)",
              g_key_state, &g_left_shift, 0x2a);
    offset_is("g_right_shift is g_key_state + DIK_RSHIFT (0x36)",
              g_key_state, &g_right_shift, 0x36);
    offset_is("g_right_ctrl is g_key_state + DIK_RCONTROL (0x9d)",
              g_key_state, &g_right_ctrl, 0x9d);

    /* ---- 2. a 256-byte GetDeviceState write stays inside it ---------- */
    /* ScanKeyboard's exact shape: one 256-byte store into g_key_state. The
     * pattern is per-index so a misplaced fragment reads a different value
     * rather than coincidentally the right one. */
    icon_before = LL_VPTR(g_info_icon_g);
    /* i*7 is a bijection mod 256 (7 is odd), so no two indices share a value
     * and a fragment placed at the wrong offset cannot read the right byte. */
    for (i = 0; i < 256; i++)
        probe[i] = (unsigned char)(i * 7 + 0x11);
    memset(g_key_state, 0, 256);
    for (i = 0; i < 256; i++)
        g_key_state[i] = probe[i];

    ll_checks++;
    if (LL_VPTR(g_info_icon_g) == icon_before)
        ll_pass("256 bytes into g_key_state do not reach g_info_icon_g (0x007fdea4)");
    else
        ll_fail("256 bytes into g_key_state do not reach g_info_icon_g (0x007fdea4)",
                "the write overran the object");

    LL_CHECK_HEX("g_left_ctrl reads key[0x1d]", LL_VU8(g_left_ctrl), probe[0x1d]);
    LL_CHECK_HEX("g_left_shift reads key[0x2a]", LL_VU8(g_left_shift), probe[0x2a]);
    LL_CHECK_HEX("g_right_shift reads key[0x36]", LL_VU8(g_right_shift), probe[0x36]);
    LL_CHECK_HEX("g_right_ctrl reads key[0x9d]", LL_VU8(g_right_ctrl), probe[0x9d]);

    /* ---- 3. the game's own readers agree ----------------------------- */
    /* DirectInput sets the high bit of a down key; `x >> 7` is the test. */
    memset(g_key_state, 0, 256);
    g_key_state[0x2a] = 0x80;
    LL_CHECK_INT("IsLShiftDown with only DIK_LSHIFT down", IsLShiftDown(), 1);
    LL_CHECK_INT("IsRShiftDown with only DIK_LSHIFT down", IsRShiftDown(), 0);
    g_key_state[0x2a] = 0;
    g_key_state[0x36] = 0x80;
    LL_CHECK_INT("IsLShiftDown with only DIK_RSHIFT down", IsLShiftDown(), 0);
    LL_CHECK_INT("IsRShiftDown with only DIK_RSHIFT down", IsRShiftDown(), 1);
    memset(g_key_state, 0, 256);
    LL_CHECK_INT("IsLShiftDown with nothing down", IsLShiftDown(), 0);
    LL_CHECK_INT("IsRShiftDown with nothing down", IsRShiftDown(), 0);

    /* ---- 4. the host GPU block is ONE object ------------------------- */
    /* gpu.c:10's map of 0x00667d70..0x00668148, field by field. */
    offset_is("g_ddraw1 is g_gpu_state + 0", g_gpu_state, &g_ddraw1, 0);
    offset_is("g_primary is g_gpu_state + 0x300", g_gpu_state, &g_primary, 0x300);
    offset_is("g_draw_surface is g_gpu_state + 0x30c",
              g_gpu_state, &g_draw_surface, 0x30c);
    offset_is("g_ddsd_height is g_gpu_state + 0x334",
              g_gpu_state, &g_ddsd_height, 0x334);
    offset_is("g_ddsd_width is g_gpu_state + 0x338",
              g_gpu_state, &g_ddsd_width, 0x338);
    offset_is("g_ddsd_pitch is g_gpu_state + 0x33c",
              g_gpu_state, &g_ddsd_pitch, 0x33c);
    offset_is("g_ddsd_bits (lpSurface) is g_gpu_state + 0x350",
              g_gpu_state, &g_ddsd_bits, 0x350);
    offset_is("g_video_locked is g_gpu_state + 0x3d4",
              g_gpu_state, &g_video_locked, 0x3d4);

    /* CheckHostSystemGPU (util.c 0x004637c0) is this memset plus
     * InitHostSystemGPU; the memset is the half that has to reach every name
     * in the block. InitHostSystemGPU itself is PORT-B's DirectDraw and is not
     * called here, so this test needs no host shim. */
    LL_VPTR(g_ddraw1) = (void*)&probe[0];
    LL_VPTR(g_primary) = (void*)&probe[1];
    LL_VPTR(g_ddsd_bits) = (void*)&probe[2];
    LL_VINT(g_video_locked) = 1;
    memset(g_gpu_state, 0, sizeof g_gpu_state);
    LL_CHECK_TRUE("CheckHostSystemGPU's memset clears g_ddraw1",
                  LL_VPTR(g_ddraw1) == 0);
    LL_CHECK_TRUE("CheckHostSystemGPU's memset clears g_primary",
                  LL_VPTR(g_primary) == 0);
    LL_CHECK_TRUE("CheckHostSystemGPU's memset clears g_ddsd_bits",
                  LL_VPTR(g_ddsd_bits) == 0);
    LL_CHECK_INT("CheckHostSystemGPU's memset clears g_video_locked",
                 LL_VINT(g_video_locked), 0);

    /* ---- 5. the profile block, read whole and byte by byte ----------- */
    /* gameframe.c:169 reads CurProfile+0x44 as a dword that spans the save
     * slot, the save type and the first two bytes of the unlocked block. */
    offset_is("g_profile_unlocked is g_cur_save_slot + 2",
              &g_cur_save_slot, g_profile_unlocked, 2);
    LL_VU32(g_cur_save_slot_wide) = 0;
    LL_VU8(g_cur_save_slot) = 0x11;
    LL_VU8(g_profile_unlocked[0]) = 0x33;
    LL_VU8(g_profile_unlocked[1]) = 0x44;
    /* Little-endian on every target this port has (x86, wasm32, arm64). */
    LL_CHECK_HEX("g_cur_save_slot_wide spans slot, type and the unlocked block",
                 LL_VU32(g_cur_save_slot_wide), 0x44330011u);

    /* ---- 6. the spot leaving visitors walk to is ONE Pos -------------- */
    /* goalstate.c's BlokeAction_LeavePark hands `&g_entrance_x` to the path
     * finders as a Pos; mapobj.c and savegame.c store its y as g_entrance_y.
     * As two 4-byte objects the y half read alignment padding (0), and every
     * leaving visitor walked to map row 0 before turning back for the gate. */
    offset_is("g_entrance_y is g_entrance_x + 4", &g_entrance_x, &g_entrance_y, 4);
    LL_VINT(g_entrance_x) = 65 << 8;
    LL_VINT(g_entrance_y) = 53 << 8;
    LL_CHECK_INT("a Pos read at g_entrance_x sees g_entrance_y as its y",
                 ((volatile int*)&g_entrance_x)[1], 53 << 8);
}
