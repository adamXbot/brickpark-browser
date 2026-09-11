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

EMSCRIPTEN_KEEPALIVE unsigned int ll_dbg_addr(int which)
{
    switch (which) {
    case 0: return (unsigned int)(size_t)g_key_state;
    case 1: return (unsigned int)(size_t)g_key_prev;
    case 2: return (unsigned int)(size_t)g_key_map;
    case 3: return (unsigned int)(size_t)g_temp_name;
    case 4: return (unsigned int)(size_t)g_cert_message;
    case 5: return (unsigned int)(size_t)g_profile_name_len;
    case 6: return (unsigned int)(size_t)g_newprof_popup_up;
    default: return 0;
    }
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
     * MEMFS is per-tab and vanishes on reload: persisting profiles wants IDBFS
     * mounted here instead, which is noted in scope-port-b4.md §6.
     *
     * EEXIST is the normal answer on a reload-free rebuild; only a real failure
     * is worth a line. */
    if (mkdir(LL_GAMEDATA "/profiles", 0777) != 0) {
        /* errno is not checked against EEXIST because MEMFS has no persistence
         * across page loads: a second run always starts with the directory
         * absent, so a failure here is always real. */
        fprintf(stderr, "[browser] mkdir(%s/profiles) failed; "
                        "the front end's profile screens will not load\n",
                LL_GAMEDATA);
    }

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
