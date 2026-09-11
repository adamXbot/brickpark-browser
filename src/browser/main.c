/* LEGOLAND portable build -- the browser entry point (scope PORT-B).
 *
 * The PE entry point called `WinMain(hInstance, NULL, lpCmdLine, nCmdShow)`;
 * this does the same from main(), then lets the game's own loop run. Under
 * ASYNCIFY main() is allowed to unwind at the first yield and is rewound on a
 * later browser tick, so there is no main-loop callback here -- see
 * docs/lanes/scope-port-b.md §1.
 *
 * Command line. The default is `-nointro -nomusic`, and both halves matter:
 *   -nointro  sets g_map->no_intro (startup.c 0x0047fd10), skipping the Indeo 5
 *             AVI intros that have no decoder in this port.
 *   -nomusic  makes InitMusicSystem set g_music_disabled itself (sysstubs.c
 *             0x00495a10) instead of starting a music thread. WITHOUT IT,
 *             RunGame's `while (g_music_disabled == 0) { PeekMessageA(...);
 *             Sleep(100); }` (gamemain.c:330) waits for a thread this port does
 *             not create, and the game never leaves that loop.
 * Add `WINDEBUG` for the windowed branch of InitScreen (g_windowed = 1), which
 * skips SetDisplayMode and builds system-memory surfaces instead.
 *
 * Working directory. RES_OpenVolume (data2.c 0x00489750) _splitpath's the
 * volume name and opens `.\volumes\<stem>.res` first, then
 * `<g_res_path><stem>.res`. So the preloaded asset tree puts the three volumes
 * at /gamedata/volumes/ and everything else at /gamedata, and this chdir's
 * there. See browser.cmake's LL_PRELOAD_* options.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten.h>
#include <emscripten/eventloop.h>       /* emscripten_set_interval (B5's flush) */

#include "ll_host.h"

/* ---- PORT-B7: the page's window into the game's live globals --------------
 *
 * `window.llFrameHash()` answers "did the screen change"; it cannot answer
 * "did the game receive the character". This lane needed the second question
 * five times in a row (the name editor, the map cursor, the panel), and the
 * only honest answer comes from the game's own memory rather than from a
 * screenshot. `Module._ll_dbg_addr(n)` hands the page the linear address of one
 * recovered global; the page then reads it out of HEAPU8/HEAP32 -- one export,
 * no per-global plumbing, and nothing in the game or the closure changes.
 *
 * The names are the closure's own (gen-browser/globals.c), which is why some
 * of them are the record's FIRST recovered name rather than the record's name:
 * `g_temp_name` is `g_temp_profile` and `g_cert_message` is `g_cur_profile`
 * (both 288-byte records PORT-A5/B6 merged). Declared `unsigned char[]` here
 * on purpose: globals.c declares each object with whatever element type its
 * re-pointed words need, and only the ADDRESS is wanted.
 */
extern unsigned char g_key_state[];         /* 0x007fdda0  256 DIK bytes */
extern unsigned char g_key_prev[];          /* 0x00668da8  59, GetInputChar */
extern unsigned char g_key_map[];           /* 0x004bad58  59 {dik, ch} */
extern unsigned char g_temp_name[];         /* 0x007cad60  g_temp_profile */
extern unsigned char g_cert_message[];      /* 0x0080ffa0  g_cur_profile */
extern unsigned char g_profile_name_len[];  /* 0x00798894 */
extern unsigned char g_newprof_popup_up[];  /* 0x007986e8 */

/* ---- PORT-B9: the RENDER state, by name ----------------------------------
 *
 * A7-1 ("the in-game screen draws the HUD and not the map") cannot be settled
 * from the canvas or from the host-call ring, because the map renderer draws
 * in SOFTWARE into a surface the host has already handed over: between Lock
 * and Unlock the game touches no host entry point at all, so a frame that
 * paints ten thousand terrain tiles and a frame that paints none look
 * identical from `?beat=`. The only witness is the game's own memory.
 *
 * These are exactly the globals renderview.c's "HOST / BROWSER-RENDERER
 * CONTRACT" block (renderview.c:150) lists as RenderView's inputs, plus the
 * three that say whether its ground pass RAN (`g_ground_last_x/y`, which
 * sysmisc3.c's RenderGroundLayer stores after PaintTileLayer returns) and the
 * surface/lock state surface.c publishes. Nothing here changes the game or
 * the closure: each case is one address.
 *
 * `ll_dbg_name` keeps the page's labels from drifting from the addresses --
 * the two lists cannot disagree because there is only one list. */
extern unsigned char g_map[];               /* 0x004bcbf4  MapHdr* (alias of g_game) */
extern unsigned char g_map_rows[];          /* 0x00801400  Cell** */
extern unsigned char g_scroll_x[];          /* 0x00667cb4  24.8 */
extern unsigned char g_scroll_y[];          /* 0x00667cb8  24.8 */
extern unsigned char g_default_tile[];      /* 0x00667ca4 */
extern unsigned char g_tile_sprites[];      /* 0x00805f60  Sprite*[] */
extern unsigned char g_bg_full_update[];    /* 0x004b9220 */
extern unsigned char g_view_dirty[];        /* 0x00667cc4 */
extern unsigned char g_ground_last_x[];     /* 0x004b95e8  RenderGroundLayer's store */
extern unsigned char g_ground_last_y[];     /* 0x004b95ec */
extern unsigned char g_render_clip[];       /* 0x00668108  WinRect */
extern unsigned char g_clip_rect[];         /* 0x004bdea0  WinRect */
extern unsigned char g_ddsd_bits[];         /* 0x006680c0  last Lock's lpSurface */
extern unsigned char g_ddsd_pitch[];        /* 0x006680ac */
extern unsigned char g_video_locked[];      /* 0x00668148 */
extern unsigned char g_present[];           /* 0x004bd3c8  the present callback */
extern unsigned char g_primary[];           /* 0x00668070 */
extern unsigned char g_draw_surface[];      /* 0x0066807c */
extern unsigned char g_surface_78[];        /* 0x00668078 */
extern unsigned char g_target[];            /* 0x00668118 */
extern unsigned char g_game_mode[];         /* 0x008119b4 */
extern unsigned char g_screen_mode[];       /* 0x0080ff88 */
extern unsigned char g_cur_screen[];        /* 0x0080ff84 */
extern unsigned char g_edit_state[];        /* 0x008119b0 */
extern unsigned char g_show_cursor[];       /* 0x00667d40 */
extern unsigned char g_odf_head[];          /* 0x00669240 */
extern unsigned char g_zb_base[];           /* 0x004b3f10 */
extern unsigned char g_park_start_pending[];
extern unsigned char g_sim_frame[];
extern unsigned char g_ci_interface_bg[];   /* the toolbar sprite that DOES draw */

/* The LEVEL LOADER's own state. The map RenderView walks is filled by
 * loadmap.c's LoadBaseMap (0x00461a50), which the level script reaches through
 * levelkw.c's `MAP <name>` keyword (LevelKw_MAP, 0x00478ac0). LoadBaseMap has
 * four numbered early returns before it writes a single cell -- -1 no name,
 * -2 LLIDB_FindElement, -3 RES_OpenFile, and the short `g_perim_count` exit --
 * and each one leaves a different one of these globals at its .data value, so
 * reading them says HOW FAR the load got without a single line of game code
 * changing. */
extern unsigned char g_map_loaded[];        /* set 1 on every LoadBaseMap exit */
extern unsigned char g_map_ready[];         /* StartPark's last store */
extern unsigned char g_map_elem[];          /* the map's LLIDB element */
extern unsigned char g_tsm_mapping_elem[];  /* the tile-set mapping element */
extern unsigned char g_terrain_elem[];      /* the terrain element */
extern unsigned char g_terrain_elem_data[];
extern unsigned char g_terrain_objects[];   /* the perimeter list */
extern unsigned char g_perim_count[];       /* S4's count, read from the file */
extern unsigned char g_perim_aux[];
extern unsigned char g_array_A[];
extern unsigned char g_array_B[];
extern unsigned char g_env_class[];
extern unsigned char g_level_db_active[];
extern unsigned char g_freeplay_db[];
extern unsigned char g_build_in_progress[];
extern unsigned char g_map_loading[];

/* The RES master directory (audio3.c:537). Every mounted volume's members are
 * flattened into a list of directory BUCKETS, each {next, files, name}, and
 * `RES_OpenFile` (res.c:93) splits its path at the last backslash and looks the
 * bucket up by name before walking the bucket's members. Walking the two lists
 * from the page answers "is the file the loader asked for actually in an
 * archive, and under what name" without opening anything. */
extern unsigned char g_master_dirs[];       /* 0x00798624  MasterDir* */
extern unsigned char g_master_vols[];       /* 0x00798628  MasterVol* */

/* movie3.c's ParseKeywordFile (0x004781f0) copies the script's name here and
 * ONLY on the arm where RES_OpenFile succeeded, so this string is the exact
 * witness for "did the level script open". `g_level_number` is what
 * LevelKw_CURRENCY and the other level-1-only keywords gate on. */
extern unsigned char g_keyword_file_name[]; /* 0x00668fd0 */
extern unsigned char g_level_number[];

/* A7-1's root cause, for the page to read and to PATCH (see index.html's
 * llFixKeywordTable). `movie.c:239` declares the 93-pair level keyword table
 *
 *     extern const void* g_level_db_sections;   / * 0x004bb6f8 93 pairs * /
 *
 * as a SINGLE `const void*` -- pointer depth 1, one declared pointer word --
 * so PORT-A7's "a word is a pointer slot when some declaration's type puts a
 * pointer FIELD at that address" rule is never asked about words 1..188. The
 * handler halves are re-pointed anyway (an exact function address goes through
 * the symbol path), but all 92 KEYWORD STRING halves keep their raw x86 VAs.
 * 89 of them are interior to `g_str_purge` and one (MAP's, 0x004bc028) to
 * `g_str_none`; exposing those two bases lets the page translate and prove it. */
extern unsigned char g_level_db_sections[]; /* 0x004bb6f8, 756 bytes */
extern unsigned char g_str_purge[];         /* 0x004bb9ec, 992 bytes */
extern unsigned char g_str_none[];          /* 0x004bbdcc, 696 bytes */

/* One table, two accessors. LL_DBG(n, sym) keeps index, name and address on
 * the same line so none of the three can drift from the others. */
#define LL_DBG_TABLE(X)                 \
    X( 0, g_key_state)                  \
    X( 1, g_key_prev)                   \
    X( 2, g_key_map)                    \
    X( 3, g_temp_name)                  \
    X( 4, g_cert_message)               \
    X( 5, g_profile_name_len)           \
    X( 6, g_newprof_popup_up)           \
    X( 7, g_map)                        \
    X( 8, g_map_rows)                   \
    X( 9, g_scroll_x)                   \
    X(10, g_scroll_y)                   \
    X(11, g_default_tile)               \
    X(12, g_tile_sprites)               \
    X(13, g_bg_full_update)             \
    X(14, g_view_dirty)                 \
    X(15, g_ground_last_x)              \
    X(16, g_ground_last_y)              \
    X(17, g_render_clip)                \
    X(18, g_clip_rect)                  \
    X(19, g_ddsd_bits)                  \
    X(20, g_ddsd_pitch)                 \
    X(21, g_video_locked)               \
    X(22, g_present)                    \
    X(23, g_primary)                    \
    X(24, g_draw_surface)               \
    X(25, g_surface_78)                 \
    X(26, g_target)                     \
    X(27, g_game_mode)                  \
    X(28, g_screen_mode)                \
    X(29, g_cur_screen)                 \
    X(30, g_edit_state)                 \
    X(31, g_show_cursor)                \
    X(32, g_odf_head)                   \
    X(33, g_zb_base)                    \
    X(34, g_park_start_pending)         \
    X(35, g_sim_frame)                  \
    X(36, g_ci_interface_bg)            \
    X(37, g_map_loaded)                 \
    X(38, g_map_ready)                  \
    X(39, g_map_elem)                   \
    X(40, g_tsm_mapping_elem)           \
    X(41, g_terrain_elem)               \
    X(42, g_terrain_elem_data)          \
    X(43, g_terrain_objects)            \
    X(44, g_perim_count)                \
    X(45, g_perim_aux)                  \
    X(46, g_array_A)                    \
    X(47, g_array_B)                    \
    X(48, g_env_class)                  \
    X(49, g_level_db_active)            \
    X(50, g_freeplay_db)                \
    X(51, g_build_in_progress)          \
    X(52, g_map_loading)                \
    X(53, g_master_dirs)                \
    X(54, g_master_vols)                \
    X(55, g_keyword_file_name)          \
    X(56, g_level_number)               \
    X(57, g_level_db_sections)          \
    X(58, g_str_purge)                  \
    X(59, g_str_none)

EMSCRIPTEN_KEEPALIVE unsigned int ll_dbg_addr(int which)
{
    switch (which) {
#define LL_DBG_ADDR(n, sym) case n: return (unsigned int)(size_t)sym;
    LL_DBG_TABLE(LL_DBG_ADDR)
#undef LL_DBG_ADDR
    default: return 0;
    }
}

/* The name of index `which`, as a NUL-terminated string in linear memory, or
 * 0 past the end of the table -- the page walks until it gets 0. */
EMSCRIPTEN_KEEPALIVE unsigned int ll_dbg_name(int which)
{
    switch (which) {
#define LL_DBG_NAME(n, sym) case n: return (unsigned int)(size_t)#sym;
    LL_DBG_TABLE(LL_DBG_NAME)
#undef LL_DBG_NAME
    default: return 0;
    }
}

/* ---- PORT-B9/B5: profiles across a page reload ---------------------------
 *
 * PORT-A7 §8 wrote this patch and could not afford the link; this is it,
 * applied. MEMFS is rebuilt from the .data package on every load, so a profile
 * written this session is gone on the next one. IDBFS is MEMFS plus an
 * IndexedDB image of it that `syncfs` moves in each direction; mounting it over
 * the profile directory ALONE keeps the rest of /gamedata read-only and cheap.
 *
 * The read has to finish BEFORE WinMain, because ScanForProfiles runs in the
 * first front-end frame. ASYNCIFY makes that expressible: spin on a flag the
 * callback clears, yielding through emscripten_sleep -- the same yield every
 * blocking construct in this port already uses. */
static volatile int g_syncfs_pending;

EM_JS(void, ll_mount_profiles, (int* flag), {
    try {
        FS.mkdirTree('/gamedata/profiles');
        FS.mount(IDBFS, {}, '/gamedata/profiles');
    } catch (e) {
        console.warn('[browser] IDBFS mount failed:', e);
        HEAP32[flag >> 2] = 0;                 /* carry on with MEMFS */
        return;
    }
    FS.syncfs(true, function (err) {           /* IndexedDB -> MEMFS */
        if (err) console.warn('[browser] profile restore failed:', err);
        HEAP32[flag >> 2] = 0;
    });
});

EM_JS(void, ll_flush_profiles, (void), {
    FS.syncfs(false, function (err) {          /* MEMFS -> IndexedDB */
        if (err) console.warn('[browser] profile save failed:', err);
    });
});

/* The write-back. The honest hook is SaveProfileToDisk finishing, which the
 * host cannot see, so this is a dumb timer: a flush with nothing dirty is an
 * IndexedDB transaction over a 272-byte file, and five seconds is far below
 * the cost of one frame's ~22,000 timeGetTime calls. NOT per frame. */
static void ll_flush_profiles_cb(void* arg)
{
    (void)arg;
    ll_flush_profiles();
}

/* winmain.c 0x00453d10. __stdcall is ignored off x86. */
extern int WinMain(void* hinst, void* hprev, char* cmdline, int ncmdshow);

#define LL_DEFAULT_SWITCHES "-nointro -nomusic"
#define LL_GAMEDATA "/gamedata"

int main(int argc, char** argv)
{
    char cmdline[1024];
    int  i;
    int  r;

    cmdline[0] = 0;
    for (i = 1; i < argc; i++) {
        if (cmdline[0])
            strncat(cmdline, " ", sizeof(cmdline) - strlen(cmdline) - 1);
        strncat(cmdline, argv[i], sizeof(cmdline) - strlen(cmdline) - 1);
    }
    if (!cmdline[0])
        strcpy(cmdline, LL_DEFAULT_SWITCHES);

    if (chdir(LL_GAMEDATA) != 0)
        fprintf(stderr, "[browser] chdir(%s) failed; resource volumes will not open\n",
                LL_GAMEDATA);

    /* The profile directory (PORT-B4). The shipped install had `profiles\` and
     * the asset extraction has no subdirectories at all, so the preloaded tree
     * arrives without it. Goto_ProfileDir (profiles.c 0x00491360) would create
     * it -- `if (!isdir) return _mkdir(g_str_profiles) == 0;` -- except that it
     * gets there through
     *
     *     h = _findfirst("profiles", &fd);
     *     if (h != -1) { do ... while (_findnext(h, &fd) != -1); }
     *     _findclose(h);                      <-- UNCONDITIONAL, as shipped
     *
     * and msvcrt.c's _findclose dereferences the handle after only a NULL
     * check, so _findclose(-1) reads address 0xffffffff and the module traps
     * with "memory access out of bounds". That is a one-line fix in a file this
     * lane does not own; see docs/lanes/scope-port-b4.md §4.
     *
     * Creating the directory here is not a workaround for that bug -- it is
     * what the host owes the game either way, because MEMFS starts empty and
     * every profile the player makes is written into it (UpDateCurrentProfile,
     * profiles.c 0x00491680). It does mean the page does not hit the bug.
     * MEMFS is per-tab and vanished on reload, which is B5; PORT-B9 replaced
     * the bare mkdir with the IDBFS mount above, so the directory is made by
     * FS.mkdirTree and then BACKED by IndexedDB. The mkdir is gone rather than
     * kept as a fallback because ll_mount_profiles makes the directory on both
     * paths -- the mount and the caught-exception path -- and a second mkdir
     * over a mounted filesystem is only a way to get a confusing EEXIST line. */
    g_syncfs_pending = 1;
    ll_mount_profiles((int*)&g_syncfs_pending);
    while (g_syncfs_pending)
        emscripten_sleep(10);
    emscripten_set_interval(ll_flush_profiles_cb, 5000, NULL);

    printf("[browser] LEGOLAND portable: WinMain(\"%s\")\n", cmdline);
    fflush(stdout);

    /* hInstance must be non-null: GameMain stores it in g_hinstance, InitScreen
     * copies it into the window class, and InitInputSystem reads it back out
     * with GetWindowLongA(GWL_HINSTANCE) and passes it to DirectInputCreateA
     * without a check (input2.c:266). */
    ll_host_sleep_hook = ll_host_yield;   /* KERNEL32 Sleep yields through PORT-B's ASYNCIFY yield */
    /* The CD check (sysmisc.c) wants a CD-ROM drive whose volume is "LEGOLAND"
     * on CDFS; kernel32.c emulates one at $LL_CD_DIR (drive D:). The preload
     * puts the three .res volumes under /gamedata/volumes. */
    setenv("LL_CD_DIR", LL_GAMEDATA "/volumes", 0);
    r = WinMain((void*)0x00400000, 0, cmdline, 1);

    printf("[browser] WinMain returned %d\n", r);
    fflush(stdout);
    return r;
}
