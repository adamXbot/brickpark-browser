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

/* --stages: InitSession's own sequence (startup.c 0x0047f880), one step at a
 * time with a line before each, because that is the only way to find out where
 * a prototype conflict stopped the game. A mismatched signature is not a TRAP
 * and not a call into a named stub: wasm-ld poisons the CALL SITE, so the
 * exception arrives as `RuntimeError: unreachable` attributed to whatever
 * function the call was inlined into -- `main`, for everything static in this
 * file. Bracketing each step with a print is what turns that back into a name.
 *
 * Every prototype below is copied from the file that DEFINES the function, not
 * from the file that declares it, for the same reason. */
extern void       LoadStrings(void);                  /* narration2.c 0x00498d00 */
extern char*      GetString(int id);                  /* text.c */
extern int        LLIDB_LoadICM(void);                /* data2.c 0x0047b1d0 */
extern int        LLIDB_RegisterNewElement(char* name, char* image, unsigned int type);
extern int        InitHostSystemGPU(void);            /* gpu.c */
extern int        InitScreen(void);                   /* screen.c */
extern int        InitInputSystem(void);              /* input2.c */
extern void*      LoadSprite(const char* name, int kind);   /* sprite2.c */
extern void*      RES_OpenFile(const char* name);     /* res.c 0x00489b60 */
extern int        RES_GetFileSize(void* f);           /* sweep4.c 0x00489ce0 */
extern int        RES_CloseFile(void* f);             /* sweep4.c 0x00489de0 -- int */
extern char*      GetGFXFName(const char* name, unsigned char type, char* buf);
extern char* g_gfx_dirs[7];                    /* rin.c 0x004b81c0 */

extern void       RunGame(void);                      /* gamemain.c 0x00459520 */

/* ---- the first present -------------------------------------------------- */
/* RunGame's own prefix, down to the one call that puts a picture on the
 * surface. Every prototype is the DEFINING file's, the same rule as above; the
 * `int` returns are what gamemain.c's own LEGOLAND_PORTABLE arms declare.
 *
 * RunGame itself cannot be used for this: four statements after ShowTitleScreen
 * it enters `while (g_music_disabled == 0) { PeekMessageA; Sleep(100); }`, and
 * nothing clears that flag while DirectSoundCreate reports no driver, so the
 * front end spins (docs/lanes/scope-port-b3.md, PORT-B's dsound.c). Calling the
 * prefix directly is what makes the title screen a TEST rather than something a
 * human watches in a tab. */
extern int        SetupControllers(void);              /* input2.c  0x0048b0d0 */
extern void       ResetController(void);               /* gameframe.c 0x004589a0 */
extern int        SetPointer(int shape);               /* cursor.c  0x00463850 */
extern int        ProcessSystemEvents(void);           /* sysmisc.c 0x00480050 */
extern void       ShowTitleScreen(void);               /* gameframe.c 0x004588c0 */
extern void       LLIDB_ClearOnLevel(void);            /* data2.c */

/* node_shim.c: what the shim saw the game present. */
extern int ll_node_frames(void);
extern int ll_node_first_frame(int* w, int* h, unsigned* nonblack,
                               unsigned* pixels, unsigned* checksum);

/* The gate's two numbers. The threshold is deliberately far below what the real
 * title artwork produces (307,200 pixels, essentially all of them non-black):
 * the assertion this test has to make is "the painters ran and drew the
 * picture", and anything that regresses them collapses the count to a handful of
 * pixels or to zero, never to 40%. The checksum is the identity of the frame and
 * is printed, not asserted, unless --frame-sum says what to expect -- see
 * portable/cmake/headless.cmake for how the ctest pins it. */
#define LL_TITLE_MIN_NONBLACK_PCT 40

static unsigned ll_expect_sum;      /* --frame-sum 0x...: 0 means "just print" */
static unsigned ll_title_sum;       /* the checksum the title stage saw, 0 if none */

static int ll_first_present(void)
{
    int      w = 0, h = 0;
    unsigned nonblack = 0, pixels = 0, sum = 0;
    unsigned pct;

    if (!ll_node_first_frame(&w, &h, &nonblack, &pixels, &sum)) {
        fprintf(stderr, "legoland_headless: FAIL no frame was presented"
                        " (%d present calls)\n", ll_node_frames());
        return 1;
    }
    pct = pixels ? nonblack * 100u / pixels : 0u;
    fprintf(stderr, "legoland_headless: first present %dx%d, %u/%u non-black"
                    " (%u%%), frame checksum 0x%08x, %d present call(s)\n",
            w, h, nonblack, pixels, pct, sum, ll_node_frames());
    if (pct < LL_TITLE_MIN_NONBLACK_PCT) {
        fprintf(stderr, "legoland_headless: FAIL the first frame is %u%%"
                        " non-black, under the %d%% the title screen must"
                        " reach -- the sprite painters drew nothing\n",
                pct, LL_TITLE_MIN_NONBLACK_PCT);
        return 1;
    }
    if (ll_expect_sum && sum != ll_expect_sum) {
        fprintf(stderr, "legoland_headless: FAIL frame checksum 0x%08x,"
                        " expected 0x%08x -- something changed what the title"
                        " screen looks like. If the change was intended, update"
                        " the expected value in portable/cmake/headless.cmake\n",
                sum, ll_expect_sum);
        return 1;
    }
    fprintf(stderr, "legoland_headless: first present OK\n");
    ll_title_sum = sum;
    return 0;
}

/* A stage can be skipped: `--stages loadsprite,rungame`. A live prototype
 * conflict inside the game kills the process, so without this one defect hides
 * every defect behind it, and the only way to enumerate the list PORT-M1 needs
 * is to step over the one just found. The names are the `LL_STAGE` tags below,
 * lower-cased and without punctuation. */
static const char* ll_skip_list = "";

static int ll_skip(const char* tag)
{
    const char* p = ll_skip_list;
    size_t n = strlen(tag);

    while (*p) {
        if (strncmp(p, tag, n) == 0 && (p[n] == 0 || p[n] == ','))
            return 1;
        while (*p && *p != ',')
            p++;
        while (*p == ',')
            p++;
    }
    return 0;
}

#define LL_STAGE(tag, what)                                                   \
    if (ll_skip(tag)) {                                                       \
        fprintf(stderr, "legoland_headless: --- %s SKIPPED\n", what);          \
        fflush(stderr);                                                       \
    } else {                                                                  \
        fprintf(stderr, "legoland_headless: --- %s\n", what);                  \
        fflush(stderr);

#define LL_STAGE_END }

static int ll_stages(void)
{
    char* s;
    int   i;
    static const char* const sprites[] = {
        "erase it.lls", "erase it2.lls", "no build.lls", "yes build.lls",
        "rab over icon.lls", "rab over icon2.lls", "question it.lls",
        "question it2.lls"
    };
    static const char* const menus[] = {
        "BUILD MENU", "ATTRACTIONS MENU", "FOOD STORES MENU", "SCENERY MENU",
        "SHOPS MENU"
    };

    LL_STAGE("loadstrings", "LoadStrings()");
    LoadStrings();
    s = GetString(0xcb);
    fprintf(stderr, "legoland_headless: GetString(0xcb) = \"%s\"\n", s ? s : "(null)");
    LL_STAGE_END

    LL_STAGE("inithostsystemgpu", "InitHostSystemGPU()");
    fprintf(stderr, "legoland_headless: = %d\n", InitHostSystemGPU());
    LL_STAGE_END

    LL_STAGE("initscreen", "InitScreen()");
    fprintf(stderr, "legoland_headless: = %d\n", InitScreen());
    LL_STAGE_END

    LL_STAGE("initinputsystem", "InitInputSystem()");
    fprintf(stderr, "legoland_headless: = %d\n", InitInputSystem());
    LL_STAGE_END

    /* Before LoadSprite, prove that the member IS reachable through the
     * mounted volumes: it is in Graphics1.res (1402 bytes, COMP 34x30 bpp16).
     * If this works and LoadSprite does not, the loader's failure is not the
     * archive. */
    LL_STAGE("resopenfile", "RES_OpenFile(GetGFXFName(\"erase it.lls\", kind, 0))");
    {
        int   kind;
        for (kind = 0; kind <= 2; kind++) {
            char* path = GetGFXFName("erase it.lls", (unsigned char)kind, 0);
            void* f = RES_OpenFile(path);
            fprintf(stderr, "legoland_headless:   kind %d -> \"%s\" = %p size %d\n",
                    kind, path, f, f ? RES_GetFileSize(f) : -1);
            if (f)
                RES_CloseFile(f);
        }
    }
    LL_STAGE_END

    /* InitSession's own eight cursor sprites, in its order. */
    LL_STAGE("loadsprite", "LoadSprite() x8, InitSession's cursor set");
    for (i = 0; i < (int)(sizeof sprites / sizeof sprites[0]); i++)
        fprintf(stderr, "legoland_headless:   %-20s = %p\n", sprites[i],
                LoadSprite(sprites[i], 0));
    LL_STAGE_END

    LL_STAGE("llidbloadicm", "LLIDB_LoadICM()");
    fprintf(stderr, "legoland_headless: LLIDB_LoadICM() = %d\n", LLIDB_LoadICM());
    LL_STAGE_END

    LL_STAGE("llidbregister", "LLIDB_RegisterNewElement() x5, InitSession's menus");
    for (i = 0; i < (int)(sizeof menus / sizeof menus[0]); i++)
        fprintf(stderr, "legoland_headless:   %-20s = %d\n", menus[i],
                LLIDB_RegisterNewElement((char*)menus[i], 0, 0x200));
    LL_STAGE_END

    /* RunGame's prefix, down to ShowTitleScreen: the first present. This is the
     * regression gate -- see ll_first_present above for the two numbers and why
     * RunGame itself cannot be the thing that is run. */
    LL_STAGE("title", "RunGame() prefix -> ShowTitleScreen(), the first present");
    SetupControllers();
    LLIDB_ClearOnLevel();
    ResetController();
    SetPointer(0);
    ProcessSystemEvents();
    ShowTitleScreen();
    if (ll_first_present())
        return 1;
    LL_STAGE_END

    /* The last thing InitSession does, and the whole front end. It does not
     * return until the player quits, so under node it runs until the caller's
     * alarm -- which is fine: the point is which prototype conflict it hits
     * first. Off by default for that reason; `--stages none` asks for
     * it. */
    LL_STAGE("rungame", "RunGame()   -- the front end; runs until it is killed");
    RunGame();
    fprintf(stderr, "legoland_headless: RunGame() returned\n");
    LL_STAGE_END

    /* One line the ctest can key on: it appears only after every stage the skip
     * list did not remove has run, and it carries the frame identity, so the
     * gate is "the spine finished AND the title screen looked like this" rather
     * than either half on its own (ctest's PASS_REGULAR_EXPRESSION is an OR, so
     * two separate patterns could not say that). */
    fprintf(stderr, "legoland_headless: --- done\n");
    if (ll_title_sum)
        fprintf(stderr, "legoland_headless: SPINE OK, first present"
                        " checksum 0x%08x\n", ll_title_sum);
    else
        fprintf(stderr, "legoland_headless: SPINE OK, no present"
                        " (the title stage was skipped)\n");
    fflush(stderr);
    return 0;
}

/* ---- --probe-input: does the host's input reach the game's own globals? ----
 *
 * PORT-B4 measured that DirectInput state arrives at the shim's GetDeviceState
 * every frame and that the front end then changes not a pixel over 3,700
 * frames. The chain it could not see into is
 *
 *   ll_host_key_set / ll_host_mouse_* (the shim's queue)
 *     -> ScanKeyboard / ScanMouse               (input.c, GetDeviceState)
 *       -> UpdateControllerFromMouseData        (input.c:328, the Controller)
 *         -> ReadGameButtons                    (bighelp.c 0x00452460)
 *           -> g_input.point / g_input.<button>  (the record at 0x00813a40)
 *             -> what the front end reads: g_gfx_point, g_mouse_buttons ...
 *
 * This mode injects one synthetic gesture through the shim's own entry points
 * and prints, at every link of that chain, what the game's globals contain. It
 * needs no window, no assets past the volumes and no frame loop, so it runs in
 * a second and says which link is broken.
 *
 * It opens with the LAYOUT gate, which is the part that actually found the bug:
 * the record at 0x00813a40 is `GameInput` to the files that WRITE it and a dozen
 * loose globals to the files that READ it, so the two halves only agree if the
 * generated closure put them in ONE object at the image's offsets. The check is
 * pure arithmetic on addresses -- no input, no game state -- and it fails loudly
 * if a regenerated globals.c ever splits the record again. */
typedef struct LLProbePos { int x, y; } LLProbePos;

extern unsigned int g_ui_flags;          /* 0x00813a40  g_input.flags   */
extern LLProbePos   g_gfx_point;         /* 0x00813a44  g_input.point   */
extern int          g_fp_w;              /* 0x00813a6c  g_input.f2c     */
extern unsigned char g_mouse_buttons;    /* 0x00813ac4  g_input.btn0.state */
extern int          g_mouse_btn_a;       /* 0x00813ad4  g_input.ret.mask  */

/* The writer's view of the same record, exactly as bighelp.c declares it --
 * only the fields this probe reads, at the offsets that file documents. */
typedef struct LLProbeInput {
    int        flags;         /* +0x00 */
    LLProbePos point;         /* +0x04 */
    int        pad0c[6];      /* +0x0c mouse_a/b/c */
    int        map_x;         /* +0x24 */
    int        map_y;         /* +0x28 */
    int        f2c;           /* +0x2c */
    int        pad30[9];      /* +0x30..+0x50 */
    int        prev_buttons;  /* +0x54 */
    int        pad58[10];     /* +0x58 up/right/down/left/tab */
    int        btn0_mask;     /* +0x80 */
    int        btn0_state;    /* +0x84 */
} LLProbeInput;
extern LLProbeInput g_input;             /* 0x00813a40 */

typedef struct LLProbeController {
    int x0, y0, x, y, dx, dy, buttons;   /* input.c:57, +0x00..+0x18 */
} LLProbeController;
extern LLProbeController* g_controller;  /* 0x00813b00 */

typedef struct LLProbeMouseState { int lX, lY, lZ; unsigned char rgb[4]; } LLProbeMouseState;
extern LLProbeMouseState g_mouse_state;  /* 0x00668d78 */
extern unsigned char     g_key_state[256];  /* 0x007fdda0 */

/* The game record: `in_game` at +0x1e gates ReadGameButtons' cursor copy. */
extern unsigned char* g_game;            /* 0x004bcbf4 -> 0x004bcbb0 */

extern void ScanKeyboard(void);                            /* 0x00473930 */
extern void ScanMouse(void);                               /* 0x00473a80 */
extern void UpdateControllerFromMouseData(void* c);        /* 0x00473b00 */
extern void UpdateControllerFromKeyboardData(void* c);     /* 0x00473c10 */
extern void ReadGameButtons(void);                         /* 0x00452460 */

static int ll_resmount_ex(int keep);   /* below */

/* portable/hostwin/include/ll_host.h (PORT-B's input injection). */
extern void ll_host_key_set(int dik, int down);
extern void ll_host_mouse_move(int dx, int dy);
extern void ll_host_mouse_button(int button, int down);

#define LL_OFF(member) ((int)((char*)&(member) - (char*)&g_ui_flags))

static int ll_probe_offset(const char* name, int got, int want)
{
    int ok = got == want;
    fprintf(stderr, "legoland_headless:   %-16s +0x%-4x expected +0x%-4x  %s\n",
            name, got, want, ok ? "OK" : "WRONG -- the record is split");
    return ok ? 0 : 1;
}

static int ll_probe_layout(void)
{
    int bad = 0;

    fprintf(stderr, "legoland_headless: --- the 0x00813a40 record: one object,"
                    " or several?\n");
    bad += ll_probe_offset("g_gfx_point", LL_OFF(g_gfx_point), 0x04);
    bad += ll_probe_offset("g_input.point", LL_OFF(g_input.point), 0x04);
    bad += ll_probe_offset("g_fp_w", LL_OFF(g_fp_w), 0x2c);
    bad += ll_probe_offset("g_input.f2c", LL_OFF(g_input.f2c), 0x2c);
    bad += ll_probe_offset("g_mouse_buttons", LL_OFF(g_mouse_buttons), 0x84);
    bad += ll_probe_offset("g_input.btn0.state", LL_OFF(g_input.btn0_state), 0x84);
    bad += ll_probe_offset("g_mouse_btn_a", LL_OFF(g_mouse_btn_a), 0x94);
    if (bad)
        fprintf(stderr, "legoland_headless: FAIL %d of the record's names are at"
                        " the wrong offset. The files that WRITE it (bighelp.c's"
                        " ReadGameButtons) and the files that READ it (screens3.c,"
                        " gameframe.c, popup2.c ...) are addressing different"
                        " memory, so no input can ever reach the front end.\n", bad);
    else
        fprintf(stderr, "legoland_headless: the record is ONE object, every name"
                        " at its image offset\n");
    return bad;
}

static void ll_probe_show(const char* when)
{
    fprintf(stderr, "legoland_headless:   %-22s key[ESC]=%02x key[RET]=%02x"
                    "  mouse=(%d,%d,%d) btn=%02x%02x%02x\n",
            when, g_key_state[0x01], g_key_state[0x1c],
            g_mouse_state.lX, g_mouse_state.lY, g_mouse_state.lZ,
            g_mouse_state.rgb[0], g_mouse_state.rgb[1], g_mouse_state.rgb[2]);
    if (g_controller)
        fprintf(stderr, "legoland_headless:   %-22s controller x=%d y=%d dx=%d"
                        " dy=%d buttons=0x%03x\n", "", g_controller->x,
                g_controller->y, g_controller->dx, g_controller->dy,
                g_controller->buttons);
    fprintf(stderr, "legoland_headless:   %-22s g_input.point=(%d,%d)"
                    " btn0.state=0x%x | READERS: g_gfx_point=(%d,%d)"
                    " g_mouse_buttons=0x%02x\n", "",
            g_input.point.x, g_input.point.y, g_input.btn0_state,
            g_gfx_point.x, g_gfx_point.y, (unsigned)g_mouse_buttons);
}

/* ---- --probe-audio: how many sound buffers does a park load? --------------
 *
 * PORT-B11 made the port audible and then measured what it actually plays on a
 * whole walk to the park with `?trace=1&tracegrep=DSOUND`:
 * `IDirectSound::CreateSoundBuffer` is called ONCE in a session -- 69,228
 * bytes, which decodes to `Space Tower01.wav` -- and zero times on a slightly
 * different route. 23 sound effects should load before the front end draws.
 * The cause is not the shim, the ACM or the archive: `extern FXEntry
 * g_game_fx[];` has no bound, so the closure generator never re-points the
 * table's `.name` words and `sprintf(".\\sfx\\%s", <a raw x86 address>)` builds
 * a member name nothing matches -- and `Load_FXList`'s failure branch is a
 * `DBPrintf` that is silent in this port (docs/lanes/scope-port-b11.md §3).
 *
 * Counting the buffers in a browser tab needs a human with a trace filter. This
 * mode does it under node, in a second, and prints a number a test can assert:
 *
 *   * `InitSoundSampleSystem(0)` -- the game's own audio init (audio4.c:163),
 *     which is what creates `g_dsound`;
 *   * the harness then wraps that object's vtable with a COUNTING copy. The
 *     shim is PORT-B's and the game is the matching lanes'; a test-side vtable
 *     thunk needs neither to change, and it counts the real calls rather than
 *     inferring them from what loaded;
 *   * `Load_FXList(g_game_fx, 0x17)` and `LoadMoneySFX()` -- exactly the calls
 *     `InitGameMap` (mapinit.c:41) and the money HUD make on the B10 walk, and
 *     the ones whose samples never arrived;
 *   * per entry: the `.name` word, whether it is a readable string at all, and
 *     whether `.sample` came back.
 *
 * `--probe-audio [N]` fails when fewer than N buffers were created, so the
 * ctest holds today's number as a floor and a bound landing in the game sources
 * raises it. */
typedef struct LLProbeDSBufferVtbl { void* slot[16]; } LLProbeDSBufferVtbl;

/* data2.c:544-556 / audio3.c:88-100: the IDirectSound vtable, by slot. Only
 * CreateSoundBuffer (+0x0c) is named; the rest are carried as void* so the copy
 * is exact whatever the shim put there. */
typedef struct LLProbeDSoundVtbl {
    void* QueryInterface;                                         /* +0x00 */
    void* AddRef;                                                 /* +0x04 */
    void* Release;                                                /* +0x08 */
    long  (*CreateSoundBuffer)(void*, const void*, void**, void*); /* +0x0c */
    void* GetCaps;                                                /* +0x10 */
    void* DuplicateSoundBuffer;                                   /* +0x14 */
    void* SetCooperativeLevel;                                    /* +0x18 */
    void* rest[9];
} LLProbeDSoundVtbl;

typedef struct LLProbeDSound { LLProbeDSoundVtbl* lpVtbl; } LLProbeDSound;

/* The FX table entry as audiomisc.c:96-100 declares it -- the file that DEFINES
 * `Load_FXList` and writes the slot, so its view is the one that matters:
 * stride 0xc with the sample at +0x08. (joust.c:620 names the same shape
 * `{name, sample, flags}` with the sample at +0x04; for ITS table that may be
 * right, but reading g_game_fx that way shows every entry as unloaded.) */
typedef struct LLProbeFX {
    char* name;      /* +0x00 the .wav member name */
    int   pad4;      /* +0x04 */
    void* sample;    /* +0x08 filled in by Load_FXList */
} LLProbeFX;

extern int   InitSoundSampleSystem(void* hwnd);       /* 0x00492130 */
extern LLProbeDSound* g_dsound;                       /* 0x007cad40 */
extern void  Load_FXList(void* list, int count);      /* 0x00496dd0 */
extern void  LoadMoneySFX(void);                      /* 0x00453900 */
extern int   g_samples_ready;                         /* 0x007988c0 */
extern LLProbeFX g_game_fx[];                         /* 0x004b9228 */
extern LLProbeFX g_money_fx[];                        /* 0x004b87a8 */

static LLProbeDSoundVtbl  ll_ds_vtbl_copy;
static LLProbeDSoundVtbl* ll_ds_vtbl_real;
static int                ll_ds_buffers;
static unsigned int       ll_ds_bytes;

static long ll_count_create_sound_buffer(void* ds, const void* desc,
                                         void** out, void* outer)
{
    long hr = ll_ds_vtbl_real->CreateSoundBuffer(ds, desc, out, outer);
    if (hr == 0) {
        ll_ds_buffers++;
        /* DSBUFFERDESC: dwSize, dwFlags, dwBufferBytes at +0x08 (data2.c:530). */
        ll_ds_bytes += ((const unsigned int*)desc)[2];
    }
    return hr;
}

/* Is `p` a readable C string and not a raw image address? The whole point of
 * the probe is that a `.name` may be an integer, and in a wasm heap every
 * integer is "readable" -- so the test is what the NAME looks like, with a
 * length cap. */
static int ll_probe_is_name(const char* p)
{
    int i;
    if (!p)
        return 0;
    for (i = 0; i < 64; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == 0)
            return i >= 4;          /* "a.wav" is the shortest real member */
        if (c < 0x20 || c >= 0x7f)
            return 0;
    }
    return 0;
}

static void ll_probe_fx_table(const char* what, LLProbeFX* t, int n)
{
    int i, loaded = 0, unreadable = 0;

    for (i = 0; i < n; i++) {
        const char* nm = t[i].name;
        int ok = ll_probe_is_name(nm);
        if (t[i].sample)
            loaded++;
        if (!ok)
            unreadable++;
        fprintf(stderr, "legoland_headless:   %s[%2d] name=%p %-42s sample=%s\n",
                what, i, (void*)nm,
                ok ? nm : "<not a string -- a RAW image address>",
                t[i].sample ? "loaded" : "NULL");
    }
    fprintf(stderr, "legoland_headless: %s: %d/%d loaded, %d name(s) still raw\n",
            what, loaded, n, unreadable);
}

static int ll_probe_audio(int want)
{
    int before;

    if (ll_resmount_ex(1))
        return 1;
    fprintf(stderr, "legoland_headless: --- InitSoundSampleSystem(0)\n");
    if (!InitSoundSampleSystem(0) || !g_samples_ready) {
        fprintf(stderr, "legoland_headless: FAIL InitSoundSampleSystem:"
                        " DirectSoundCreate or SetCooperativeLevel refused,"
                        " so no sample can ever load\n");
        return 1;
    }
    {
        LLProbeDSound* ds = g_dsound;
        if (!ds || !ds->lpVtbl) {
            fprintf(stderr, "legoland_headless: FAIL g_dsound is NULL after a"
                            " successful init\n");
            return 1;
        }
        ll_ds_vtbl_real = ds->lpVtbl;
        ll_ds_vtbl_copy = *ds->lpVtbl;
        ll_ds_vtbl_copy.CreateSoundBuffer = ll_count_create_sound_buffer;
        ds->lpVtbl = &ll_ds_vtbl_copy;
        fprintf(stderr, "legoland_headless: counting CreateSoundBuffer through a"
                        " wrapped vtable (real %p, copy %p)\n",
                (void*)ll_ds_vtbl_real, (void*)&ll_ds_vtbl_copy);
    }

    fprintf(stderr, "legoland_headless: --- Load_FXList(g_game_fx, 0x17)"
                    " (InitGameMap's own call, mapinit.c:41)\n");
    before = ll_ds_buffers;
    Load_FXList(g_game_fx, 0x17);
    ll_probe_fx_table("g_game_fx", g_game_fx, 0x17);
    fprintf(stderr, "legoland_headless:   %d buffer(s) from this table\n",
            ll_ds_buffers - before);

    fprintf(stderr, "legoland_headless: --- LoadMoneySFX()"
                    " (audiomisc.c:184, the money HUD's two effects)\n");
    before = ll_ds_buffers;
    LoadMoneySFX();
    ll_probe_fx_table("g_money_fx", g_money_fx, 2);
    fprintf(stderr, "legoland_headless:   %d buffer(s) from this table\n",
            ll_ds_buffers - before);

    fprintf(stderr, "legoland_headless: AUDIO %d sound buffer(s) created,"
                    " %u byte(s) of PCM\n", ll_ds_buffers, ll_ds_bytes);
    if (ll_ds_buffers < want) {
        fprintf(stderr, "legoland_headless: FAIL expected at least %d sound"
                        " buffer(s), got %d. Every miss is an FX table whose"
                        " `.name` word is still a raw x86 address, which is a"
                        " missing bound on its declaration"
                        " (docs/lanes/scope-port-b11.md §3, and the row in"
                        " portable/tests/rawwords_baseline.txt)\n",
                want, ll_ds_buffers);
        return 1;
    }
    if (ll_ds_buffers > want && want > 0)
        fprintf(stderr, "legoland_headless: the floor is %d and %d loaded --"
                        " raise LL_AUDIO_BUFFERS in portable/cmake/"
                        "headless.cmake in the commit that fixed it\n",
                want, ll_ds_buffers);
    return 0;
}

static int ll_probe_input(void)
{
    int bad = ll_probe_layout();

    if (ll_resmount_ex(1))
        return 1;
    LoadStrings();
    fprintf(stderr, "legoland_headless: --- InitHostSystemGPU/InitScreen/"
                    "InitInputSystem/SetupControllers\n");
    InitHostSystemGPU();
    InitScreen();
    if (!InitInputSystem()) {
        fprintf(stderr, "legoland_headless: FAIL InitInputSystem\n");
        return 1;
    }
    SetupControllers();
    ResetController();
    if (!g_controller) {
        fprintf(stderr, "legoland_headless: FAIL no Controller record\n");
        return 1;
    }
    /* ReadGameButtons only copies the cursor when the game record says a map is
     * up (bighelp.c:161). InitSession sets it; this probe does not run
     * InitSession, so it sets the one byte itself and says so. */
    if (g_game) {
        g_game[0x1e] = 1;
        fprintf(stderr, "legoland_headless: g_game->in_game := 1 (the probe sets"
                        " it; InitSession would)\n");
    }

    fprintf(stderr, "legoland_headless: --- before any input\n");
    ll_probe_show("idle");

    /* One gesture: move the pointer, press mouse button 0, press ESCAPE. */
    fprintf(stderr, "legoland_headless: --- ll_host_mouse_move(+120,+60),"
                    " button 0 down, DIK_ESCAPE down\n");
    ll_host_mouse_move(120, 60);
    ll_host_mouse_button(0, 1);
    ll_host_key_set(0x01, 1);          /* DIK_ESCAPE */

    ScanKeyboard();
    ScanMouse();
    ll_probe_show("after Scan*");
    UpdateControllerFromMouseData(g_controller);
    UpdateControllerFromKeyboardData(g_controller);
    ll_probe_show("after UpdateController");
    ReadGameButtons();
    ll_probe_show("after ReadGameButtons");

    /* What the front end would see. The button bit the game builds for mouse 0
     * is 0x001 (input.c:70), and bighelp.c's SetGameButton puts the press edge
     * in btn0.state, which screens3.c reads as g_mouse_buttons. */
    if (g_controller->x == 0 && g_controller->y == 0) {
        fprintf(stderr, "legoland_headless: FAIL the Controller did not move:"
                        " the break is in the shim or in ScanMouse\n");
        bad++;
    } else if (g_gfx_point.x != g_controller->x ||
               g_gfx_point.y != g_controller->y) {
        fprintf(stderr, "legoland_headless: FAIL the Controller moved to (%d,%d)"
                        " but the READERS' cursor is (%d,%d): the break is"
                        " between ReadGameButtons and the record\n",
                g_controller->x, g_controller->y, g_gfx_point.x, g_gfx_point.y);
        bad++;
    } else if (!(g_controller->buttons & 1)) {
        fprintf(stderr, "legoland_headless: FAIL the button did not reach the"
                        " Controller (buttons=0x%03x)\n", g_controller->buttons);
        bad++;
    } else if (g_mouse_buttons == 0) {
        fprintf(stderr, "legoland_headless: FAIL the button reached the"
                        " Controller but not g_mouse_buttons, which is what the"
                        " front end tests\n");
        bad++;
    } else {
        fprintf(stderr, "legoland_headless: INPUT OK -- the gesture reached"
                        " g_gfx_point=(%d,%d) and g_mouse_buttons=0x%02x, which"
                        " is what screens3.c reads\n",
                g_gfx_point.x, g_gfx_point.y, (unsigned)g_mouse_buttons);
    }
    fprintf(stderr, "legoland_headless: --- done\n");
    fflush(stderr);
    return bad ? 1 : 0;
}

/* `keep` leaves the volumes mounted, which is what the game does: InitSession
 * only closes them on a failure path or at shutdown, and everything after the
 * mount reads through them. */
static int ll_resmount_ex(int keep)
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
        if (!keep)
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

    /* --frame-sum 0x...: pin the first present's checksum, so a change that
     * alters the title screen fails instead of being noticed by nobody. Parsed
     * before the mode switch because every mode may want it. */
    for (i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--frame-sum") == 0) {
            ll_expect_sum = (unsigned)strtoul(argv[i + 1], 0, 0);
            /* Drop the pair so the remaining argv is still a mode + skip list. */
            for (r = i; r + 2 < argc; r++)
                argv[r] = argv[r + 2];
            argc -= 2;
            break;
        }

    if (argc > 1 && strcmp(argv[1], "--resmount") == 0)
        return ll_resmount_ex(0);
    /* --probe-input: the host's input injected straight into the game's own
     * ScanMouse/ScanKeyboard/ReadGameButtons, with the 0x00813a40 record's
     * layout checked first. See ll_probe_input. */
    if (argc > 1 && strcmp(argv[1], "--probe-input") == 0)
        return ll_probe_input();
    /* --probe-audio [N]: count IDirectSound::CreateSoundBuffer calls over the
     * FX loads the park makes, and fail below N. See ll_probe_audio. */
    if (argc > 1 && strcmp(argv[1], "--probe-audio") == 0)
        return ll_probe_audio(argc > 2 ? (int)strtol(argv[2], 0, 0) : 0);
    /* --stages: mount the volumes, then walk InitSession's own sequence one
     * named step at a time. */
    if (argc > 1 && strcmp(argv[1], "--stages") == 0) {
        /* `--stages [<skip list>]`. The default skips RunGame, which does not
         * return; `--stages none` runs every stage; `--stages loadsprite,rungame`
         * steps over a stage whose prototype conflict would otherwise hide
         * everything behind it. */
        ll_skip_list = argc > 2 ? argv[2] : "rungame";
        if (strcmp(ll_skip_list, "none") == 0)
            ll_skip_list = "";
        r = ll_resmount_ex(1);
        return r ? r : ll_stages();
    }

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
