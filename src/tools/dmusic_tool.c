/* LEGOLAND portable build -- the DirectMusic engine on the command line
 * (the DirectMusic lane).
 *
 *   legoland_dmusic render --dir <imusic> --dls <collection.dls>
 *                          [--rate 44100] [--seed N] [--repeats N] [--groove M]
 *                          [--seconds S] [--trace events.txt]
 *                          --out tune.wav <first.sgt> [<then.sgt> ...]
 *   legoland_dmusic selftest [--dir <imusic>] [--dls <collection.dls>]
 *
 * `render` plays the segments one after another the way musicthread.c chains a
 * world's segments -- each next one is queued with DMUS_SEGF_MEASURE when the
 * current one reports "almost end" -- and writes 16-bit stereo WAV. It is the
 * quickest way to hear what the engine makes of the game's music without
 * starting the game.
 *
 * `selftest` is the ctest (dmusic_selftest). Without --dir it checks
 * MusicToMIDI alone; with it, every segment and style in the directory must
 * parse and resolve, a world segment's notification timeline must be the one
 * MusicThread expects, and a measure-aligned transition must start on the next
 * bar line. With --dls it also renders a few seconds and checks there is sound.
 *
 * Only the engine is linked (ll_dls.c, ll_dmfile.c, ll_dmperf.c): no game code,
 * no Win32 shim.
 */
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ll_dmusic.h"

/* ---- files --------------------------------------------------------------- */

static unsigned char* read_file(const char* path, size_t* size)
{
    FILE*          f = fopen(path, "rb");
    unsigned char* data;
    long           n;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return 0;
    }
    data = (unsigned char*)malloc((size_t)n + 1);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = 0;
    }
    fclose(f);
    if (data)
        *size = (size_t)n;
    return data;
}

static int has_ext(const char* name, const char* ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static const char* base_name(const char* path)
{
    const char* s = strrchr(path, '/');
    const char* b = strrchr(path, '\\');
    if (b && (!s || b > s))
        s = b;
    return s ? s + 1 : path;
}

/* ---- the style library --------------------------------------------------- */

typedef struct Library {
    LLDmStyle* styles;
    char**     files;
    int        count;
} Library;

static int library_load(Library* lib, const char* dir)
{
    DIR*           d = opendir(dir);
    struct dirent* ent;
    int            cap = 0;
    memset(lib, 0, sizeof *lib);
    if (!d)
        return -1;
    while ((ent = readdir(d)) != 0) {
        char           path[1024];
        unsigned char* data;
        size_t         size = 0;
        if (!has_ext(ent->d_name, ".sty"))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        data = read_file(path, &size);
        if (!data)
            continue;
        if (lib->count == cap) {
            cap = cap ? cap * 2 : 256;
            lib->styles = (LLDmStyle*)realloc(lib->styles, (size_t)cap * sizeof *lib->styles);
            lib->files = (char**)realloc(lib->files, (size_t)cap * sizeof *lib->files);
        }
        if (ll_dm_parse_style(data, size, &lib->styles[lib->count]) == 0) {
            lib->files[lib->count] = strdup(ent->d_name);
            lib->count++;
        }
        free(data);
    }
    closedir(d);
    return 0;
}

static const LLDmStyle* library_resolve(void* ctx, const LLDmStyleRef* ref, void** owner)
{
    Library* lib = (Library*)ctx;
    int      i;
    *owner = 0;
    if (ref->have_guid)
        for (i = 0; i < lib->count; i++)
            if (ll_dm_guid_equal(&ref->guid, &lib->styles[i].guid))
                return &lib->styles[i];
    for (i = 0; i < lib->count; i++)
        if (ref->name[0] && strcasecmp(ref->name, lib->styles[i].name) == 0)
            return &lib->styles[i];
    for (i = 0; i < lib->count; i++)
        if (ref->file[0] && strcasecmp(base_name(ref->file), lib->files[i]) == 0)
            return &lib->styles[i];
    return 0;
}

static void library_free(Library* lib)
{
    int i;
    for (i = 0; i < lib->count; i++) {
        ll_dm_free_style(&lib->styles[i]);
        free(lib->files[i]);
    }
    free(lib->styles);
    free(lib->files);
}

static int load_segment(const char* path, LLDmSegmentData* seg, Library* lib)
{
    size_t         size = 0;
    unsigned char* data = read_file(path, &size);
    int            rc;
    if (!data)
        return -1;
    rc = ll_dm_parse_segment(data, size, seg);
    free(data);
    if (rc == 0 && lib)
        rc = ll_dm_segment_resolve(seg, library_resolve, lib) ? 1 : 0;
    return rc;
}

/* ---- WAV ----------------------------------------------------------------- */

static void put16(FILE* f, int v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }
static void put32(FILE* f, unsigned v) { put16(f, (int)(v & 0xffff)); put16(f, (int)(v >> 16)); }

static void wav_header(FILE* f, unsigned rate, unsigned frames)
{
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + frames * 4);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, rate);
    put32(f, rate * 4);
    put16(f, 4);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, frames * 4);
}

static int to_pcm16(float x)
{
    /* The same soft knee ll_dmusic.c puts in front of Web Audio. */
    double y = x;
    if (y > 0.75)
        y = 0.75 + 0.25 * tanh((y - 0.75) / 0.25);
    else if (y < -0.75)
        y = -0.75 - 0.25 * tanh((-y - 0.75) / 0.25);
    return (int)lrint(y * 32767.0);
}

/* ---- render -------------------------------------------------------------- */

typedef struct Chain {
    LLDmPerf*         perf;
    LLDmSegmentData*  segs;
    int               count;
    int               next;
    uint32_t          repeats;
    int               ended;
    FILE*             trace;
} Chain;

static void chain_hook(void* ctx)
{
    Chain*     c = (Chain*)ctx;
    LLDmNotify n;
    while (ll_dmperf_pop_notification(c->perf, &n)) {
        if (c->trace && n.kind == 0)
            fprintf(c->trace, "%10.1f segment option %d\n", n.tick, n.option);
        if (n.kind == 0 && n.option == LL_DMUS_SEGALMOSTEND && c->next < c->count) {
            ll_dmperf_play(c->perf, &c->segs[c->next], 0, c->repeats, LL_DMUS_SEGF_MEASURE);
            c->next++;
        } else if (n.kind == 0 && (n.option == LL_DMUS_SEGEND || n.option == LL_DMUS_SEGABORT) &&
                   c->next >= c->count && !ll_dmperf_playing(c->perf)) {
            c->ended = 1;
        }
    }
}

static void trace_event(void* ctx, double tick, int ch, int kind, int a, int b)
{
    static const char* names[] = { "?", "on", "off", "cc", "bend", "program", "pattern" };
    FILE* f = (FILE*)ctx;
    fprintf(f, "%10.1f ch%-2d %-7s %d %d\n", tick, ch, names[kind < 7 ? kind : 0], a, b);
}

static int cmd_render(int argc, char** argv)
{
    const char* dir = 0;
    const char* dls_path = 0;
    const char* out = 0;
    const char* trace_path = 0;
    double      rate = 44100.0;
    double      seconds = 0.0;
    uint32_t    seed = 1;
    int         groove = 0;
    long        repeats = -1;
    const char* files[64];
    int         nfiles = 0, i;
    Library     lib;
    LLDls*      dls = 0;
    LLSynth*    synth = 0;
    Chain       chain;
    FILE*       wav;
    unsigned    frames_out = 0, limit;
    float       L[1024], R[1024];

    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--dls") && i + 1 < argc) dls_path = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_path = argv[++i];
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--groove") && i + 1 < argc) groove = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--repeats") && i + 1 < argc) repeats = atol(argv[++i]);
        else if (nfiles < 64) files[nfiles++] = argv[i];
    }
    if (!dir || !out || !nfiles) {
        fprintf(stderr, "render: --dir, --out and at least one segment are required\n");
        return 2;
    }
    if (library_load(&lib, dir) != 0) {
        fprintf(stderr, "render: cannot read %s\n", dir);
        return 1;
    }
    if (dls_path) {
        size_t         size = 0;
        unsigned char* data = read_file(dls_path, &size);
        dls = data ? ll_dls_load(data, size) : 0;
        free(data);
        if (!dls) {
            fprintf(stderr, "render: %s is not a DLS collection\n", dls_path);
            return 1;
        }
        synth = ll_synth_new(dls, rate, 32);
    }

    memset(&chain, 0, sizeof chain);
    chain.segs = (LLDmSegmentData*)calloc((size_t)nfiles, sizeof *chain.segs);
    chain.count = nfiles;
    for (i = 0; i < nfiles; i++) {
        int rc = load_segment(files[i], &chain.segs[i], &lib);
        if (rc < 0) {
            fprintf(stderr, "render: %s is not a segment\n", files[i]);
            return 1;
        }
        if (rc > 0)
            fprintf(stderr, "render: %s: some styles did not resolve\n", files[i]);
    }
    chain.repeats = repeats < 0 ? 0 : (uint32_t)repeats;
    chain.perf = ll_dmperf_new(rate, seed);
    ll_dmperf_set_synth(chain.perf, synth);
    ll_dmperf_set_groove_modifier(chain.perf, groove);
    ll_dmperf_set_notify_hook(chain.perf, chain_hook, &chain);
    if (trace_path) {
        chain.trace = fopen(trace_path, "w");
        if (chain.trace)
            ll_dmperf_set_trace(chain.perf, trace_event, chain.trace);
    }
    /* Once each unless asked: MusicThread's SetRepeats(0), not the 200 the
     * files were authored with. */
    ll_dmperf_play(chain.perf, &chain.segs[0], 0, chain.repeats, 0);
    chain.next = 1;

    wav = fopen(out, "wb");
    if (!wav) {
        fprintf(stderr, "render: cannot write %s\n", out);
        return 1;
    }
    wav_header(wav, (unsigned)rate, 0);
    limit = seconds > 0 ? (unsigned)(seconds * rate) : (unsigned)(rate * 3600.0);
    while (frames_out < limit) {
        int n = 1024, k;
        if (limit - frames_out < (unsigned)n)
            n = (int)(limit - frames_out);
        ll_dmperf_render(chain.perf, L, R, n);
        for (k = 0; k < n; k++) {
            put16(wav, to_pcm16(L[k]));
            put16(wav, to_pcm16(R[k]));
        }
        frames_out += (unsigned)n;
        if (chain.ended && seconds <= 0 && ll_synth_active_voices(synth) == 0)
            break;
        if (!ll_dmperf_playing(chain.perf) && !synth && seconds <= 0)
            break;
    }
    fseek(wav, 0, SEEK_SET);
    wav_header(wav, (unsigned)rate, frames_out);
    fclose(wav);
    {
        LLDmPerfStats st;
        ll_dmperf_stats(chain.perf, &st);
        fprintf(stderr, "render: %s -- %.1f s, %u notes, %u patterns, %u segments, "
                        "missing programs %u\n",
                out, frames_out / rate, st.notes_on, st.patterns, st.segments_started,
                ll_synth_missing_programs(synth));
    }
    if (chain.trace)
        fclose(chain.trace);
    ll_dmperf_free(chain.perf);
    ll_synth_free(synth);
    ll_dls_free(dls);
    for (i = 0; i < nfiles; i++)
        ll_dm_free_segment(&chain.segs[i]);
    free(chain.segs);
    library_free(&lib);
    return 0;
}

/* ---- selftest ------------------------------------------------------------ */

static int g_failures;

static void check(int ok, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf(ok ? "  ok    " : "  FAIL  ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!ok)
        g_failures++;
}

static void selftest_music_to_midi(void)
{
    /* The twelve (chord position, scale position, accidental) triples the
     * game's notes use, at octave 5, against C: the pitch classes C..B. */
    static const struct { uint16_t value; int midi; } vec[] = {
        { 0x5000, 60 }, { 0x5001, 61 }, { 0x5010, 62 }, { 0x510f, 63 },
        { 0x5100, 64 }, { 0x5110, 65 }, { 0x520f, 66 }, { 0x5200, 67 },
        { 0x5201, 68 }, { 0x5210, 69 }, { 0x530f, 70 }, { 0x5300, 71 },
        { 0x3200, 43 }, { 0x6300, 83 },
    };
    static const uint32_t chords[] = { 0x91, 0x891, 0x1 };
    static const char*    names[] = { "M triad", "M7", "root only" };
    int c, i;
    for (c = 0; c < 3; c++) {
        LLDmSubChord sub;
        int          bad = 0;
        memset(&sub, 0, sizeof sub);
        sub.chord_pattern = chords[c];
        sub.scale_pattern = 0xab5ab5;
        sub.chord_root = 12;
        sub.levels = 0xffffffffu;
        for (i = 0; i < (int)(sizeof vec / sizeof vec[0]); i++) {
            int got = ll_dm_music_to_midi(&sub, 0x0e, vec[i].value);
            if (got != vec[i].midi) {
                printf("        %s: 0x%04x -> %d, want %d\n", names[c], vec[i].value, got,
                       vec[i].midi);
                bad++;
            }
        }
        check(!bad, "MusicToMIDI, ALWAYSPLAY against C %s: all twelve pitch classes", names[c]);
    }
    {
        LLDmSubChord sub;
        memset(&sub, 0, sizeof sub);
        sub.chord_pattern = 0x91;
        sub.scale_pattern = 0xab5ab5;
        sub.chord_root = 12;
        check(ll_dm_music_to_midi(&sub, 0, 42) == 42, "MusicToMIDI, FIXED passes the MIDI note through");
    }
}

typedef struct Timeline {
    LLDmPerf*  perf;
    int        starts, almost, ends, aborts, loops, beats;
    double     almost_tick, end_tick, abort_tick, start_tick[4];
    int        beat_measure_max;
    int        queue_on_measure;     /* queue `then` at this measure's downbeat */
    int        queued;
    const LLDmSegmentData* then;
} Timeline;

static void timeline_hook(void* ctx)
{
    Timeline*  t = (Timeline*)ctx;
    LLDmNotify n;
    while (ll_dmperf_pop_notification(t->perf, &n)) {
        if (n.kind == 1) {
            t->beats++;
            if (n.field2 > t->beat_measure_max)
                t->beat_measure_max = n.field2;
            if (t->then && !t->queued && n.field1 == 0 && n.field2 == t->queue_on_measure) {
                ll_dmperf_play(t->perf, t->then, 0, 0, LL_DMUS_SEGF_MEASURE);
                t->queued = 1;
            }
            continue;
        }
        switch (n.option) {
        case LL_DMUS_SEGSTART:
            if (t->starts < 4)
                t->start_tick[t->starts] = n.tick;
            t->starts++;
            break;
        case LL_DMUS_SEGALMOSTEND: t->almost++; t->almost_tick = n.tick; break;
        case LL_DMUS_SEGEND: t->ends++; t->end_tick = n.tick; break;
        case LL_DMUS_SEGABORT: t->aborts++; t->abort_tick = n.tick; break;
        case LL_DMUS_SEGLOOP: t->loops++; break;
        }
    }
}

static void run_perf(LLDmPerf* p, double seconds, double rate)
{
    float    L[512], R[512];
    unsigned left = (unsigned)(seconds * rate);
    while (left) {
        int n = left > 512 ? 512 : (int)left;
        ll_dmperf_render(p, L, R, n);
        left -= (unsigned)n;
    }
}

static int find_segment(const char* dir, const char* want, char* out, size_t size)
{
    DIR*           d = opendir(dir);
    struct dirent* ent;
    int            found = 0;
    if (!d)
        return 0;
    while ((ent = readdir(d)) != 0) {
        if (strcasecmp(ent->d_name, want) == 0) {
            snprintf(out, size, "%s/%s", dir, ent->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static void selftest_data(const char* dir, const char* dls_path)
{
    Library        lib;
    DIR*           d;
    struct dirent* ent;
    int            nseg = 0, nbad = 0, unresolved = 0, i;
    long           patterns = 0, parts = 0, notes = 0;
    char           path[1024];
    const double   rate = 22050.0;

    if (library_load(&lib, dir) != 0) {
        check(0, "read %s", dir);
        return;
    }
    for (i = 0; i < lib.count; i++) {
        int p;
        patterns += lib.styles[i].npatterns;
        parts += lib.styles[i].nparts;
        for (p = 0; p < lib.styles[i].nparts; p++)
            notes += lib.styles[i].parts[p].nnotes;
    }
    check(lib.count == 212, "212 styles parse (got %d)", lib.count);
    check(patterns == 475 && parts == 3002 && notes == 57798,
          "475 patterns, 3002 parts, 57798 notes (got %ld, %ld, %ld)", patterns, parts, notes);

    d = opendir(dir);
    while (d && (ent = readdir(d)) != 0) {
        LLDmSegmentData seg;
        int             rc;
        if (!has_ext(ent->d_name, ".sgt"))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        rc = load_segment(path, &seg, &lib);
        if (rc < 0)
            nbad++;
        else {
            nseg++;
            unresolved += ll_dm_segment_resolve(&seg, library_resolve, &lib);
            ll_dm_free_segment(&seg);
        }
    }
    if (d)
        closedir(d);
    check(nseg == 30 && nbad == 0, "30 segments parse (got %d, %d failed)", nseg, nbad);
    check(unresolved == 0, "every style reference resolves (%d do not)", unresolved);

    /* A world segment's timeline, the way MusicThread sees it after
     * SetRepeats(0): start, a downbeat every 3072 ticks, "almost end" one
     * second (768 ticks at 60 BPM) before the end, then the end. */
    if (find_segment(dir, "SegEgypt1.sgt", path, sizeof path)) {
        LLDmSegmentData seg;
        Timeline        t;
        if (load_segment(path, &seg, &lib) >= 0) {
            memset(&t, 0, sizeof t);
            t.perf = ll_dmperf_new(rate, 7);
            ll_dmperf_set_notify_hook(t.perf, timeline_hook, &t);
            ll_dmperf_play(t.perf, &seg, 0, 0, LL_DMUS_SEGF_MEASURE);
            run_perf(t.perf, seg.length / (double)LL_DM_PPQ + 2.0, rate);
            check(t.starts == 1 && t.ends == 1 && t.aborts == 0, "SegEgypt1: one start, one end");
            check(t.almost == 1 && fabs(t.almost_tick - (seg.length - 768.0)) < 1.0,
                  "SegEgypt1: almost-end at %.0f (want %d)", t.almost_tick, seg.length - 768);
            check(fabs(t.end_tick - seg.length) < 1.0, "SegEgypt1: end at %.0f (want %d)",
                  t.end_tick, seg.length);
            check(t.beats == seg.length / 768 && t.beat_measure_max == seg.length / 3072 - 1,
                  "SegEgypt1: %d beats over %d measures", t.beats, t.beat_measure_max + 1);
            {
                LLDmPerfStats st;
                ll_dmperf_stats(t.perf, &st);
                check(st.notes_on > 1000 && st.patterns == (uint32_t)(seg.length / 3072),
                      "SegEgypt1: %u notes from %u patterns", st.notes_on, st.patterns);
            }
            ll_dmperf_free(t.perf);
            ll_dm_free_segment(&seg);
        }
    }

    /* A measure-aligned transition queued on a downbeat starts one bar later,
     * aborting the segment it cuts (MusicThread's state 6 -> 7). */
    {
        char            path2[1024];
        LLDmSegmentData a, b;
        if (find_segment(dir, "Segwest2.sgt", path, sizeof path) &&
            find_segment(dir, "WEtran2.sgt", path2, sizeof path2) &&
            load_segment(path, &a, &lib) >= 0 && load_segment(path2, &b, &lib) >= 0) {
            Timeline t;
            memset(&t, 0, sizeof t);
            t.perf = ll_dmperf_new(rate, 7);
            t.then = &b;
            t.queue_on_measure = 3;
            ll_dmperf_set_notify_hook(t.perf, timeline_hook, &t);
            ll_dmperf_play(t.perf, &a, 0, 0, LL_DMUS_SEGF_MEASURE);
            run_perf(t.perf, (4 * 3072 + b.length) / (double)LL_DM_PPQ + 2.0, rate);
            check(t.queued && t.aborts == 1 && fabs(t.abort_tick - 4 * 3072.0) < 1.0,
                  "transition queued on bar 3's downbeat cuts Segwest2 at %.0f (want %d)",
                  t.abort_tick, 4 * 3072);
            check(t.starts == 2 && fabs(t.start_tick[1] - 4 * 3072.0) < 1.0,
                  "WEtran2 starts at %.0f", t.start_tick[1]);
            check(t.ends == 1 && fabs(t.end_tick - (4 * 3072.0 + b.length)) < 1.0,
                  "WEtran2 ends at %.0f after its %d ticks", t.end_tick, b.length);
            ll_dmperf_free(t.perf);
            ll_dm_free_segment(&a);
            ll_dm_free_segment(&b);
        }
    }

    /* The same transition queued in Segwest2's LAST bar starts exactly at its
     * end -- and must still abort it: MusicThread leaves state 6 only on
     * SEGABORT, so a SEGEND here strands the thread and the music stops when
     * the transition does (found in the browser, Egypt -> Inca near the end
     * of SegEgypt2). */
    {
        char            path2[1024];
        LLDmSegmentData a, b;
        if (find_segment(dir, "Segwest2.sgt", path, sizeof path) &&
            find_segment(dir, "WEtran2.sgt", path2, sizeof path2) &&
            load_segment(path, &a, &lib) >= 0 && load_segment(path2, &b, &lib) >= 0) {
            Timeline t;
            double   end = (double)a.length;
            memset(&t, 0, sizeof t);
            t.perf = ll_dmperf_new(rate, 7);
            t.then = &b;
            t.queue_on_measure = a.length / 3072 - 1;
            ll_dmperf_set_notify_hook(t.perf, timeline_hook, &t);
            ll_dmperf_play(t.perf, &a, 0, 0, LL_DMUS_SEGF_MEASURE);
            run_perf(t.perf, (a.length + b.length) / (double)LL_DM_PPQ + 2.0, rate);
            check(t.queued && t.aborts == 1 && fabs(t.abort_tick - end) < 1.0,
                  "a transition queued in the last bar aborts Segwest2 at its end (%.0f, want %.0f)",
                  t.abort_tick, end);
            check(t.ends == 1 && fabs(t.end_tick - (end + b.length)) < 1.0,
                  "and the only SEGEND is the transition's own, at %.0f", t.end_tick);
            ll_dmperf_free(t.perf);
            ll_dm_free_segment(&a);
            ll_dm_free_segment(&b);
        }
    }

    /* The synth over a real collection: the theme's first bars make sound. */
    if (dls_path && find_segment(dir, "Segtheme1.sgt", path, sizeof path)) {
        size_t          size = 0;
        unsigned char*  data = read_file(dls_path, &size);
        LLDls*          dls = data ? ll_dls_load(data, size) : 0;
        LLDmSegmentData seg;
        free(data);
        check(dls != 0, "%s loads (%d instruments, %d waves)", dls_path,
              ll_dls_instrument_count(dls), ll_dls_wave_count(dls));
        if (dls && load_segment(path, &seg, &lib) >= 0) {
            LLSynth*  synth = ll_synth_new(dls, 44100.0, 32);
            LLDmPerf* perf = ll_dmperf_new(44100.0, 3);
            float     L[1024], R[1024];
            double    sum = 0.0;
            long      frames = 0, bad = 0;
            ll_dmperf_set_synth(perf, synth);
            ll_dmperf_play(perf, &seg, 0, 0, 0);
            while (frames < 44100L * 12) {
                int k;
                ll_dmperf_render(perf, L, R, 1024);
                for (k = 0; k < 1024; k++) {
                    if (!isfinite(L[k]) || !isfinite(R[k]))
                        bad++;
                    else
                        sum += (double)L[k] * L[k] + (double)R[k] * R[k];
                }
                frames += 1024;
            }
            check(bad == 0, "the rendered theme has no NaN or Inf samples");
            check(sqrt(sum / (2.0 * frames)) > 0.01, "the theme's first 12 seconds are audible "
                  "(RMS %.4f)", sqrt(sum / (2.0 * frames)));
            check(ll_synth_missing_programs(synth) == 0, "every band program has an instrument "
                  "(%u missing)", ll_synth_missing_programs(synth));
            ll_dmperf_free(perf);
            ll_synth_free(synth);
            ll_dm_free_segment(&seg);
        }
        ll_dls_free(dls);
    }
    library_free(&lib);
}

static int cmd_selftest(int argc, char** argv)
{
    const char* dir = 0;
    const char* dls = 0;
    int         i;
    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--dls") && i + 1 < argc) dls = argv[++i];
    }
    printf("dmusic selftest\n");
    selftest_music_to_midi();
    if (dir && *dir)
        selftest_data(dir, dls && *dls ? dls : 0);
    else
        printf("  skip  no --dir: the game's music data was not checked\n");
    printf(g_failures ? "dmusic selftest: %d FAILED\n" : "dmusic selftest: all passed\n",
           g_failures);
    return g_failures ? 1 : 0;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && !strcmp(argv[1], "render"))
        return cmd_render(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "selftest"))
        return cmd_selftest(argc - 2, argv + 2);
    fprintf(stderr, "usage: %s render|selftest ...  (see the header of dmusic_tool.c)\n", argv[0]);
    return 2;
}
