/* LEGOLAND portable build -- the headless harness: run the recovered game's
 * own entry point under node and see how far the startup spine gets.
 *
 * There is no window and no graphics here. The point is the ORDER in which
 * the game asks the host for things: every host call the shim does not
 * implement is a generated body that prints
 *
 *     TRAP <dll> <symbol> from <the sources that reference it>
 *
 * and exits 70 (portable/tools/gen_link.py), so one run = one missing piece,
 * named. PORT-A walks that list turning KERNEL32-family traps into real calls
 * until the spine reaches the first DirectDraw/USER32/DirectInput call, which
 * is PORT-B's (docs/SCOPE_PORT_WAVE.md).
 *
 * Run it with the game's data directory as the cwd, which $LL_DATA_DIR does
 * for you (gamedata/ is a symlink in a worktree, so cd-ing there leaves the
 * checkout):
 *
 *     LL_DATA_DIR=$PWD/gamedata/main node portable/build-wasm/legoland_headless.js
 *
 * -sNODERAWFS=1 means the emscripten FS calls go straight to the real
 * filesystem, so nothing has to be packaged; the relative paths the loaders
 * build (".\\volumes\\%s.res" and friends) resolve against that cwd.
 *
 * The switches match what the brief asks for: -nointro (skip the Indeo AVIs,
 * which are stubs), -nomusic (no DirectMusic), WINDEBUG (windowed, and
 * g_present = FlipPrimary rather than a full-screen page flip). Pass your own
 * on the command line to override them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* winmain.c, 0x00453d10. __stdcall is ignored off x86. */
extern int WinMain(void* hinst, void* hprev, char* cmdline, int ncmdshow);

#define LL_DEFAULT_SWITCHES "-nointro -nomusic WINDEBUG"

int main(int argc, char** argv)
{
    const char* data_dir = getenv("LL_DATA_DIR");
    char cmdline[1024];
    char cwd[1024];
    int  i;
    int  r;

    /* The loaders build relative paths (".\\volumes\\%s.res", ".\\save\\...")
     * against the current directory, so the run has to happen inside the game
     * data. $LL_DATA_DIR saves the caller from having to cd there first --
     * handy when the data is reached through a symlink. */
    if (data_dir && chdir(data_dir) != 0) {
        fprintf(stderr, "legoland_headless: cannot chdir to LL_DATA_DIR=%s\n", data_dir);
        return 2;
    }
    if (getcwd(cwd, sizeof cwd))
        fprintf(stderr, "legoland_headless: data directory %s\n", cwd);

    cmdline[0] = 0;
    for (i = 1; i < argc; i++) {
        if (cmdline[0])
            strncat(cmdline, " ", sizeof cmdline - strlen(cmdline) - 1);
        strncat(cmdline, argv[i], sizeof cmdline - strlen(cmdline) - 1);
    }
    if (!cmdline[0])
        strcpy(cmdline, LL_DEFAULT_SWITCHES);

    fprintf(stderr, "legoland_headless: WinMain(NULL, NULL, \"%s\", 1)\n", cmdline);
    fflush(stderr);

    r = WinMain(0, 0, cmdline, 1);

    fprintf(stderr, "legoland_headless: WinMain returned %d\n", r);
    fflush(stderr);
    return r == 0 ? 0 : 1;
}
