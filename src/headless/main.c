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

/* --resmount: the one piece of the spine that sits BEHIND the DirectDraw call
 * and can still be reached without a host surface -- finding and opening the
 * resource volumes (sysmisc.c / sysmisc2.c / data2.c). InitSession does this
 * after CheckHostSystemGPU, so a whole-game run cannot get here until PORT-B
 * lands; calling it directly is how the file and drive half of the KERNEL32
 * shim was tested, and it is the cheapest way to find what the loaders need
 * next. */
extern int   RES_EnsureMounted(const char* volume);   /* 0x004515e0 */
extern void* RES_OpenVolume(const char* name);        /* 0x00489750 */
/* 0x00489dc0. `int`, NOT the `void` startup.c declares it with: sweep4.c
 * defines it as `int RES_CloseVolume(RVol*)`, and on wasm the two prototypes
 * are two different function types, so a `void`-typed call is replaced by
 * wasm-ld with a trapping stub -- a bare `RuntimeError: unreachable` with no
 * symbol name, which is exactly what this probe hit before. The game's own
 * calls (startup.c:140/153/161/183) carry the `void` prototype and WILL trap
 * the same way: it is one of the 542 prototype conflicts in
 * docs/lanes/scope-port-a.md, and the first one proved to be live. */
extern int   RES_CloseVolume(void* vol);
extern const char* g_volume_names[3];                 /* 0x004bcba4 */
extern char  g_res_path[];                            /* 0x00813b04 */

/* The mounted-volume records, as LEGOLAND/resaudio2.c declares them (the same
 * layout portable/tests/test_res_archive.c checks field by field). Walking
 * them is how the probe proves the directory really was parsed: a volume whose
 * name table was still full of raw x86 addresses opened a file called ".res"
 * and produced no members at all. */
typedef struct LLResEnt {
    struct LLResEnt* anext;   /* +0x00 every member of every volume */
    struct LLResEnt* next;    /* +0x04 next in the directory bucket */
    struct LLResEnt* vnext;   /* +0x08 next in THIS volume */
    void*            dir;     /* +0x0c */
    void*            vol;     /* +0x10 */
    int              size;    /* +0x14 */
    int              base;    /* +0x18 offset in the volume */
    char*            name;    /* +0x1c */
} LLResEnt;

typedef struct LLResVol {
    struct LLResVol* next;    /* +0x00 */
    LLResEnt*        files;   /* +0x04 */
    char             name[0x14];  /* +0x08 upper-cased */
    int              handle;  /* +0x1c */
} LLResVol;

#define LL_RES_SHOW 6         /* members printed per volume */

static void ll_res_list(void* v)
{
    LLResVol* vol = (LLResVol*)v;
    LLResEnt* e;
    int n = 0;
    long bytes = 0;

    for (e = vol->files; e; e = e->vnext) {
        if (n < LL_RES_SHOW)
            fprintf(stderr, "legoland_headless:     %-16s %8d bytes @ %d\n",
                    e->name ? e->name : "(null)", e->size, e->base);
        n++;
        bytes += e->size;
    }
    fprintf(stderr, "legoland_headless:   volume \"%s\" handle %d: %d members,"
                    " %ld bytes%s\n", vol->name, vol->handle, n, bytes,
            n > LL_RES_SHOW ? " (first few shown)" : "");
}

static int ll_resmount(void)
{
    int i;

    /* startup.c passes the literal 1: the parameter is only tested for
     * truthiness, and non-zero means "look for the CD on every drive". */
    if (!RES_EnsureMounted((const char*)1)) {
        fprintf(stderr, "legoland_headless: RES_EnsureMounted failed"
                        " (no CD: set LL_CD_DIR to the directory holding"
                        " Legoland.res)\n");
        return 1;
    }
    fprintf(stderr, "legoland_headless: volumes mounted, g_res_path=\"%s\"\n", g_res_path);
    for (i = 0; i < 3; i++) {
        void* v = RES_OpenVolume(g_volume_names[i]);
        fprintf(stderr, "legoland_headless: RES_OpenVolume(\"%s\") = %p\n",
                g_volume_names[i], v);
        if (!v)
            return 1;
        ll_res_list(v);
        fprintf(stderr, "legoland_headless:   RES_CloseVolume = %d\n",
                RES_CloseVolume(v));
    }
    return 0;
}

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

    if (argc > 1 && strcmp(argv[1], "--resmount") == 0)
        return ll_resmount();

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
