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

/* PORT-B10 -- the PLAY state. Walking the tutorial means asserting things a
 * screenshot cannot settle: how much money the park has (the money readout is a
 * bitmap number, and a bitmap number is exactly what a frame hash cannot read),
 * how many visitors are inside, whether anything is alive at all. Each is one
 * recovered global:
 *   g_bricks         sweep1.c:51            the money -- what RenderMoneyBar
 *                                           (money.c:154) sprintf("%5d")s
 *   g_bricks_full    money.c:67             the bar's denominator, set at load
 *   g_visitor_count  goalstate.c:178        how many visitors are inside.
 *                                           PORT-M11: this slot used to read
 *                                           g_num_visitors 0x00832bd0, which is
 *                                           really g_power_supply -- power.c:170
 *                                           is its only writer -- so it was 0
 *                                           through the whole tutorial whatever
 *                                           the park did.  0x006661bc is the
 *                                           count: pathobj2.c:458 zeroes it on
 *                                           load, rides.c and goalstate.c move
 *                                           it as blokes arrive and leave.
 *   g_people_head    blokeai.c:164          the live Bloke chain: visitors,
 *                                           workers, gardeners, mechanics
 *   g_visitor_limit  blitmisc.c:127         how many the park is allowed */
extern unsigned char g_bricks[];            /* 0x004b90f8  int */
extern unsigned char g_bricks_full[];       /* 0x00832974  int */
extern unsigned char g_visitor_count[];     /* 0x006661bc  int */
extern unsigned char g_people_head[];       /* 0x0066b574  Bloke* */
extern unsigned char g_visitor_limit[];     /* 0x0083291c  int */

/* PORT-M13 -- the LINK goal and the path network, which is the ONE question a
 * player-reported "the game keeps asking me to link the ride" cannot answer
 * from the canvas. EventTick_Link (eventtick.c:939) tests exactly one square
 * per instance -- the class's ENTRANCE offset added to the instance's base --
 * and asks TileJoinsPathNetwork (pathmisc2.c:179) whether a PathSquare covers
 * it AND carries flag 2, the "reachable from the park entrance" bit that
 * ResolveEntrancePathSquare (pathmask.c:128) re-floods. PORT-M10 had to find
 * all five of these by SCANNING the heap because portable/** was read-only for
 * it; naming them costs nothing and makes every measurement in this lane a
 * read instead of a search.
 *   g_path_squares    pathsq.c:19       the PathSquare list, 0x24 bytes each
 *   g_goal_list       eventgoalprim.c   the pending ScriptEvent list: the LINK
 *                                       goal event and its flags live here
 *   g_entrance_tile   objdoor.c:170     the park entrance's cached walk-to
 *                                       tile -- the flood fill's ROOT
 *   g_path_gfx_batch  pathmisc2.c       the force flag TileJoinsPathNetwork
 *                                       passes to RefreshEntranceTile
 *   g_entrance_tile_time  tinystubs.c   the refresh's 4000 ms clock
 *   g_entrance_elem   objdoor.c:171     the ENTRANCE 1 element, looked up lazily
 *   g_cursor_mapref   objmap.c:338      ScreenToMapRef's output for the live
 *                                       cursor -- the square a click lands on
 *   g_edit_object     fpui2.c           the class the edit cursor is placing */
extern unsigned char g_path_squares[];      /* 0x0066b44c  PathSquare* */
extern unsigned char g_goal_list[];         /* 0x00668728  ScriptEvent* */
extern unsigned char g_entrance_tile[];     /* 0x0066b460  Pos */
extern unsigned char g_path_gfx_batch[];    /* 0x0066b46c  int */
extern unsigned char g_entrance_tile_time[];
extern unsigned char g_entrance_elem[];     /* 0x006661c4  LLElem* */
extern unsigned char g_cursor_mapref[];
extern unsigned char g_edit_object[];
/* fpui3.c:646's UpdateHelpTick walks g_script_event -- the LIVE event list, the
 * one that holds the LINK goal itself; g_goal_list is the notepad's list of
 * unmet objectives that the GoalCheck_* primitives feed. Both are needed: the
 * first says whether the goal passes, the second says what the player is being
 * TOLD. g_map_dirty bit 0x10 is what forces the entrance flood fill (fpui3.c:662). */
extern unsigned char g_script_event[];      /* 0x00668784  ScriptEvent* */
extern unsigned char g_map_dirty[];
/* bighelp.c:35's GameInput. `point` (+0x04) is the cursor pixel the SHIM
 * delivered and `map_x/map_y` (+0x24/+0x28) is the cell ScreenToMapRef made of
 * it -- the two halves of "did the click land on the square the player aimed
 * at", which is the only way to separate a shim mouse-mapping defect from a
 * game-side one. */
extern unsigned char g_input[];             /* 0x00813a40  GameInput */

/* PORT-M14: the SELECTION/DESTROY-CURSOR block, which is what decides what an
 * eraser click takes. `HandleMapClick` (gameframe.c:955) classifies the cell
 * under the pointer into `g_hit_info` (+0x00 type, +0x04 obj, +0x08 cell) and
 * publishes the owning class in `g_sel_def` with its BASE cell in
 * `g_sel_bpos`; the class's +0x90 cursor hook (usually
 * `BasicObjectDCalcCursor`, objmap2.c:506) then stamps `g_destroy_cursor`.
 * Note `g_query_cursor` and `g_destroy_cursor` are THE SAME OBJECT --
 * gameframe.c and objmap2.c give 0x00810160 two names -- so one read covers
 * both. Reading type + sel_def->name + sel_def->rect + sel_bpos + the cursor's
 * own rect/origin/validity is the whole answer to "what is this click about to
 * remove, and is that the cell the player pointed at". */
extern unsigned char g_hit_info[];          /* 0x004bdd00  HitInfo */
extern unsigned char g_sel_def[];           /* 0x00667c58  ObjDef* under the cursor */
extern unsigned char g_sel_bpos[];          /* 0x00667c54  its base cell, BPosW */
extern unsigned char g_destroy_cursor[];    /* 0x00810160  == g_query_cursor */
extern unsigned char g_drag_class[];        /* 0x0080ff6c */
extern unsigned char g_query_extra[];       /* 0x00810144 */
extern unsigned char g_edit_cursor[];       /* 0x007febc0 */
extern unsigned char g_tile_info[];         /* 0x00801f40  TileInfo[] */

/* ---- PORT-M17: the 3D-PERSON RASTERISER WITNESSES ------------------------
 *
 * P1-6 ("sixty live visitors and not one is drawn") needs to know how far into
 * `Render3DPerson` (rin.c:535) a person gets, and nothing on the host side can
 * say: between Lock and Unlock the whole model pass is software.
 *
 * These globals are the witness, because of who writes them.  In the WHOLE
 * tree, `SetRenderTarget` (0x00485f30, tri3d.c:527 -- rin.c declares it
 * `SetRasterTarget`), `SetMousePixel` (0x00488700, declared `SetRasterOrigin`)
 * and `Render_SetViewport` (0x00441800, sweep1.c:160) have EXACTLY ONE caller
 * each, and it is rin.c:556-558 -- the three statements immediately AFTER the
 * `GetVideoSurface` early-out and immediately BEFORE `Draw3DPersonModel`.  So
 * `g_surface` / `g_pitch` / `g_width` / `g_rows` / `g_mouse_pixel` /
 * `g_clip_x0..y1` are zero for as long as no person has ever cleared that
 * gate, and they hold the last drawn person's window the moment one has.  That
 * is a yes/no answer to "is the video surface locked while
 * `DrawAndClearPrintList` walks the list" with no game-side tracing at all.
 *
 * `g_clip_x0..y1` (0x0081c8d0..dc) and `g_vp_left/top/right/bottom` are THE
 * SAME FOUR OBJECTS under two names (tri3d.c vs. sweep1.c); the generated
 * globals alias them correctly, and one read covers both.
 *
 * The rest are the pass's own state: the print list's head and bump cursor,
 * the lock stack `PushRenderingStatusAndLockVideoSurface`/`PopRenderingStatus`
 * push and pop, the Z buffer `Draw3DPersonModel` rasterises through, and the
 * two .bss scratch arrays (`g_xverts`, `g_vert_key`) the model's vertex loops
 * fill -- per-vertex churn in those is the proof the loops ran. */
extern unsigned char g_surface[];           /* 0x00797e68  locked 16bpp base */
extern unsigned char g_pitch[];             /* 0x00701e58 */
extern unsigned char g_width[];             /* 0x00701e60 */
extern unsigned char g_rows[];              /* 0x0066be48 */
extern unsigned char g_clip_x0[];           /* 0x0081c8d0  == g_vp_left */
extern unsigned char g_clip_y0[];           /* 0x0081c8d4  == g_vp_top */
extern unsigned char g_clip_x1[];           /* 0x0081c8d8  == g_vp_right */
extern unsigned char g_clip_y1[];           /* 0x0081c8dc  == g_vp_bottom */
extern unsigned char g_mouse_pixel[];       /* 0x007fe9a8 */
extern unsigned char g_raster_hit[];        /* 0x007feb14 */
extern unsigned char g_printlist[];         /* 0x0066b5a4  PrintNode* head */
extern unsigned char g_printlist_x[];       /* 0x0066b5a8  bump cursor */
extern unsigned char g_printlist_drawn[];   /* 0x0066b5ac */
extern unsigned char g_status_stack[];      /* 0x00668164 */
extern unsigned char g_status_sp[];         /* 0x006681e4 */
extern unsigned char g_zbuf[];              /* 0x00701e5c  128*120 dwords */
extern unsigned char g_zbw[];               /* 0x0066be40 */
extern unsigned char g_zbh[];               /* 0x0066be44 */
extern unsigned char g_zbpitch[];           /* 0x0066be4c */
extern unsigned char g_green_bits[];        /* 0x007cb5e0  6 = 565, 5 = 555 */
extern unsigned char g_clear_pixel[];       /* 0x007cb5e4 */
extern unsigned char g_xverts[];            /* 0x00643ee8  3 ints per vertex */
extern unsigned char g_vert_key[];          /* 0x00641004  one key per vertex */

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
    X(59, g_str_none)                   \
    X(60, g_bricks)                     \
    X(61, g_bricks_full)                \
    X(62, g_visitor_count)              \
    X(63, g_people_head)                \
    X(64, g_visitor_limit)              \
    X(65, g_path_squares)               \
    X(66, g_goal_list)                  \
    X(67, g_entrance_tile)              \
    X(68, g_path_gfx_batch)             \
    X(69, g_entrance_tile_time)         \
    X(70, g_entrance_elem)              \
    X(71, g_cursor_mapref)              \
    X(72, g_edit_object)                \
    X(73, g_script_event)               \
    X(74, g_map_dirty)                  \
    X(75, g_input)                      \
    X(76, g_hit_info)                   \
    X(77, g_sel_def)                    \
    X(78, g_sel_bpos)                   \
    X(79, g_destroy_cursor)             \
    X(80, g_drag_class)                 \
    X(81, g_query_extra)                \
    X(82, g_edit_cursor)                \
    X(83, g_tile_info)                  \
    X(84, g_surface)                    \
    X(85, g_pitch)                      \
    X(86, g_width)                      \
    X(87, g_rows)                       \
    X(88, g_clip_x0)                    \
    X(89, g_clip_y0)                    \
    X(90, g_clip_x1)                    \
    X(91, g_clip_y1)                    \
    X(92, g_mouse_pixel)                \
    X(93, g_raster_hit)                 \
    X(94, g_printlist)                  \
    X(95, g_printlist_x)                \
    X(96, g_printlist_drawn)            \
    X(97, g_status_stack)               \
    X(98, g_status_sp)                  \
    X(99, g_zbuf)                       \
    X(100, g_zbw)                       \
    X(101, g_zbh)                       \
    X(102, g_zbpitch)                   \
    X(103, g_green_bits)                \
    X(104, g_clear_pixel)               \
    X(105, g_xverts)                    \
    X(106, g_vert_key)

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
