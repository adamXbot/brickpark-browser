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
#include <string.h>
#include <unistd.h>

#include "ll_host.h"

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

    printf("[browser] LEGOLAND portable: WinMain(\"%s\")\n", cmdline);
    fflush(stdout);

    /* hInstance must be non-null: GameMain stores it in g_hinstance, InitScreen
     * copies it into the window class, and InitInputSystem reads it back out
     * with GetWindowLongA(GWL_HINSTANCE) and passes it to DirectInputCreateA
     * without a check (input2.c:266). */
    r = WinMain((void*)0x00400000, 0, cmdline, 1);

    printf("[browser] WinMain returned %d\n", r);
    fflush(stdout);
    return r;
}
