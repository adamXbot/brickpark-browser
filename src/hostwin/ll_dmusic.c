/* LEGOLAND portable build -- DirectMusic, as MusicThread calls it (the
 * DirectMusic lane).
 *
 * WHAT THE GAME USES
 * -------------------------------------------------------------------------
 * Every DirectMusic call in the program is in musicthread.c's MusicThread
 * (0x00492db0), plus audio2.c's SetMusicGrooveLevel and sysmisc3.c's
 * KillMusicSystem. music.c's style/band/chordmap loaders and composer wrappers
 * have no callers. The slots reached, by object:
 *
 *   Performance  Init +0x0c, PlaySegment +0x10, Stop +0x14, FreePMsg +0x44,
 *                SetNotificationHandle +0x50, GetNotificationPMsg +0x54,
 *                AddNotificationType +0x58, AddPort +0x60,
 *                AssignPChannelBlock +0x68, SetGlobalParam +0x88,
 *                CloseDown +0x98, Release
 *   IDirectMusic CreatePort +0x14
 *   Port         Activate +0x3c, SetDirectSound +0x48, GetFormat +0x4c, Release
 *   Loader       GetObject +0x0c, SetSearchDirectory +0x14, ScanDirectory +0x18,
 *                ClearCache +0x24, EnableCache +0x28, EnumObject +0x2c, Release
 *   Segment      SetRepeats +0x18, SetParam +0x4c (GUID_Download)
 *   Composer     Release (created, never composed with)
 *
 * On wasm32 an indirect call traps unless the callee's signature matches the
 * caller's exactly (PORT-M3), so every slot of every vtable here is a real
 * function typed from the game's own declarations -- not a generic stub -- and
 * the offsets are the DirectX 6 ones the call sites prove.
 *
 * WHEN DIRECTMUSIC EXISTS
 * -------------------------------------------------------------------------
 * CoCreateInstance (dsound.c) asks ll_dmusic_create, which answers
 * REGDB_E_CLASSNOTREG -- the designed no-DirectMusic path PORT-B4 relied on --
 * unless all of these hold:
 *   - Web Audio exists (so never under node: the headless and test targets
 *     keep running MusicThread's first failure rung, exactly as before);
 *   - the page was not opened with ?music=0;
 *   - `imusic` (resolved case-insensitively from the working directory) holds
 *     segments -- MusicThread dereferences the segments it loads without a
 *     check, so a thread given DirectMusic without its data would trap;
 *   - a DLS collection loads from $LL_MUSIC_DLS, dls/gm.dls, gm.dls or
 *     imusic/gm.dls (browser.cmake preloads one at /gamedata/dls/gm.dls).
 *
 * THE LOADER
 * -------------------------------------------------------------------------
 * SetSearchDirectory + ScanDirectory index the files by GUID and name the way
 * DirectMusic's does; EnumObject walks that index (MusicThread loads all 212
 * styles through it); GetObject finds an object by GUID, then name, then file
 * name, and loads it once while the cache is on. A segment's style references
 * resolve through the same index, so SegTheme1's 39 bars share the styles
 * MusicThread already holds.
 *
 * A GetObject with DMUS_OBJ_MEMORY loads the blob it is given and ignores the
 * name. That settles musicthread.c's slot-7 bug: the Egypt -> Inca transition
 * reads "ietran2.sgt" but asks for L"eitran2", and there is no EItran2.sgt on
 * the disc to ask for, so the transition out of Egypt into Inca plays the
 * Inca-to-Egypt segment (IEtran2). docs/QUIRKS.md Q6.
 *
 * DOWNLOADS
 * -------------------------------------------------------------------------
 * GUID_Download is recorded and otherwise a no-op: the whole collection is
 * loaded up front. MusicThread never downloads world 0 (the LEGOLAND theme)
 * or its outbound transitions; on Windows those still sounded through the
 * instruments the other worlds' bands had downloaded, and here every GM/GS
 * program is always present.
 *
 * OUTPUT
 * -------------------------------------------------------------------------
 * The performance renders at the AudioContext's own rate (so Web Audio never
 * resamples between chunks) from ll_dmusic_pump, which ll_host_yield calls on
 * the main stack before every browser yield. The pump keeps DM_LEAD_S of music
 * scheduled ahead of the context's clock. Notifications fire as the render head
 * passes them and wake MusicThread through its event; kernel32.c hosts the
 * thread on a fiber, so it runs and waits again before the pump carries on.
 * The port's DirectSound buffer carries the game's music volume (UpdateSoundVols
 * sets it from the slider), and the pump applies it to the output gain.
 */
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ll_dmusic.h"
#include "ll_host.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define DM_S_OK                 0L
#define DM_S_FALSE              1L
#define DM_E_NOTIMPL            ((long)0x80004001)
#define DM_E_POINTER            ((long)0x80004003)
#define DM_E_FAIL               ((long)0x80004005)
#define DM_E_OUTOFMEMORY        ((long)0x8007000E)
#define DM_E_INVALIDARG         ((long)0x80070057)
#define DM_REGDB_E_CLASSNOTREG  ((long)0x80040154)

#define DMUS_OBJ_OBJECT   0x001
#define DMUS_OBJ_CLASS    0x002
#define DMUS_OBJ_NAME     0x004
#define DMUS_OBJ_FILENAME 0x010
#define DMUS_OBJ_MEMORY   0x400

#define DM_LEAD_S    0.25
#define DM_CHUNK     1024
#define DM_VOICES    32          /* the DirectX 6 software synth's default */
#define DM_MASTER    2.0f

/* ---- GUIDs, byte for byte as the exe holds them --------------------------- */

#define DM_D2AC28(x) { (x), 0x28, 0xac, 0xd2, 0x9b, 0xb3, 0xd1, 0x11, \
                       0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd }

static const unsigned char kClsidPerformance[16] = DM_D2AC28(0x81);
static const unsigned char kClsidSegment[16]     = DM_D2AC28(0x82);
static const unsigned char kClsidStyle[16]       = DM_D2AC28(0x8a);
static const unsigned char kClsidComposer[16]    = DM_D2AC28(0x90);
static const unsigned char kClsidLoader[16]      = DM_D2AC28(0x92);
static const unsigned char kGuidAllTypes[16]     = DM_D2AC28(0x93);
static const unsigned char kGuidNotifySegment[16] = DM_D2AC28(0x99);
static const unsigned char kGuidNotifyBeat[16]   = DM_D2AC28(0x9a);
static const unsigned char kGuidDownload[16]     = DM_D2AC28(0xa7);
static const unsigned char kGuidGroove[16]       = DM_D2AC28(0xb2);
static const unsigned char kClsidBand[16] = { 0x00, 0x9e, 0xba, 0x79, 0xee, 0xb6, 0xd1, 0x11,
                                              0x86, 0xbe, 0x00, 0xc0, 0x4f, 0xbf, 0x8f, 0xef };

static int dm_guid_is(const void* g, const unsigned char* want)
{
    return g && memcmp(g, want, 16) == 0;
}

/* ---- the structs the game hands over (wasm32 = the original layouts) ------ */

typedef struct DmObjectDesc {
    uint32_t       dwSize;           /* +0x000 */
    uint32_t       dwValidData;      /* +0x004 */
    unsigned char  guidObject[16];   /* +0x008 */
    unsigned char  guidClass[16];    /* +0x018 */
    uint32_t       ftDate[2];        /* +0x028 */
    uint32_t       vVersion[2];      /* +0x030 */
    uint16_t       wszName[64];      /* +0x038 */
    uint16_t       wszCategory[64];  /* +0x0b8 */
    uint16_t       wszFileName[260]; /* +0x138 */
    int64_t        llMemLength;      /* +0x340 */
    unsigned char* pbMemData;        /* +0x348 */
    void*          pStream;          /* +0x34c */
} DmObjectDesc;

/* DMUS_NOTIFICATION_PMSG: the DMUS_PMSG_PART header, then the notification. */
typedef struct DmNotifyMsg {
    uint32_t      dwSize;            /* +0x00 */
    uint32_t      pad04;
    int64_t       rtTime;            /* +0x08 */
    int32_t       mtTime;            /* +0x10 */
    uint32_t      dwFlags;           /* +0x14 */
    uint32_t      dwPChannel;        /* +0x18 */
    uint32_t      dwVirtualTrackID;  /* +0x1c */
    void*         pTool;             /* +0x20 */
    void*         pGraph;            /* +0x24 */
    uint32_t      dwType;            /* +0x28 */
    uint32_t      dwVoiceID;         /* +0x2c */
    uint32_t      dwGroupID;         /* +0x30 */
    void*         punkUser;          /* +0x34 */
    unsigned char guidNotificationType[16]; /* +0x38 */
    uint32_t      dwNotificationOption;     /* +0x48 */
    uint32_t      dwField1;                 /* +0x4c */
    uint32_t      dwField2;                 /* +0x50 */
} DmNotifyMsg;

/* The offsets the game reads, not the sizes: the rtTime int64 rounds
 * DmNotifyMsg's tail padding up to 0x58 here, which nothing reads. */
#if defined(__wasm32__) || defined(__i386__)
_Static_assert(sizeof(DmObjectDesc) == 0x350, "DMUS_OBJECTDESC is 0x350 bytes");
_Static_assert(offsetof(DmObjectDesc, llMemLength) == 0x340, "llMemLength at +0x340");
_Static_assert(offsetof(DmObjectDesc, pbMemData) == 0x348, "pbMemData at +0x348");
_Static_assert(offsetof(DmNotifyMsg, guidNotificationType) == 0x38, "notification GUID at +0x38");
_Static_assert(offsetof(DmNotifyMsg, dwNotificationOption) == 0x48, "option at +0x48");
_Static_assert(offsetof(DmNotifyMsg, dwField1) == 0x4c, "field1 at +0x4c");
_Static_assert(offsetof(DmNotifyMsg, dwField2) == 0x50, "field2 at +0x50");
#endif

/* ---- the objects ---------------------------------------------------------- */

typedef struct DmUnknown DmUnknown;
typedef struct DmUnknownVtbl {
    long          (*QueryInterface)(DmUnknown*, const void*, void**);
    unsigned long (*AddRef)(DmUnknown*);
    unsigned long (*Release)(DmUnknown*);
} DmUnknownVtbl;
struct DmUnknown {
    const DmUnknownVtbl* lpVtbl;
    int                  refs;
};

static unsigned long dm_addref(void* obj)
{
    DmUnknown* u = (DmUnknown*)obj;
    return u ? u->lpVtbl->AddRef(u) : 0;
}

static unsigned long dm_release(void* obj)
{
    DmUnknown* u = (DmUnknown*)obj;
    return u ? u->lpVtbl->Release(u) : 0;
}

typedef struct DmSegment     DmSegment;
typedef struct DmStyle       DmStyle;
typedef struct DmLoader      DmLoader;
typedef struct DmPerformance DmPerformance;
typedef struct DmPort        DmPort;
typedef struct DmMusic       DmMusic;
typedef struct DmComposer    DmComposer;

/* ---- the module state ------------------------------------------------------ */

static struct {
    int            checked;
    int            ok;
    LLDls*         dls;
    char           dls_path[1024];
    DmPerformance* active;
    int            pumping;
    long           volume_cb;
    int            have_volume;
    double         last_stats_ms;
} g_dm;

static double dm_now_ms(void)
{
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    return 0.0;
#endif
}

/* ---- the page ------------------------------------------------------------- */

#ifdef __EMSCRIPTEN__
EM_JS(int, ll_dmusic_js_off, (void), {
    var l = globalThis['location'];
    return (l && /[?&]music=(0|off|no)(&|$)/.test(l.search)) ? 1 : 0;
});

EM_JS(void, ll_dmusic_js_status, (int ok, int why_ptr), {
    var g = globalThis, s = "";
    for (var i = 0; why_ptr && i < 256; i++) {
        var c = HEAPU8[why_ptr + i];
        if (!c) break;
        s += String.fromCharCode(c);
    }
    g.__llMusic = { available: ok ? true : false, reason: s };
    g.llMusic = function () { return Object.assign({}, g.__llMusic); };
});

EM_JS(void, ll_dmusic_js_stats, (int notes, int voices, int patterns, int segments,
                                 double tick, int measure, int beat, int groove,
                                 int modifier, int seg_ptr, int style_ptr, int pat_ptr,
                                 int missing, double gain), {
    var g = globalThis;
    function str(p) {
        var s = "";
        for (var i = 0; p && i < 64; i++) {
            var c = HEAPU8[p + i];
            if (!c) break;
            s += String.fromCharCode(c);
        }
        return s;
    }
    var m = g.__llMusic || (g.__llMusic = { available: true, reason: "" });
    m.notes = notes;
    m.voices = voices;
    m.patterns = patterns;
    m.segmentsStarted = segments;
    m.tick = tick;
    m.measure = measure;
    m.beat = beat;
    m.groove = groove;
    m.grooveModifier = modifier;
    m.segment = str(seg_ptr);
    m.style = str(style_ptr);
    m.pattern = str(pat_ptr);
    m.missingPrograms = missing;
    m.gain = gain;
    var a = g.__llAudio;
    if (a && a.music) {
        m.chunks = a.music.chunks;
        m.underruns = a.music.underruns;
        m.lead = a.ctx ? a.music.next - a.ctx.currentTime : 0;
    }
});
#else
static int  ll_dmusic_js_off(void) { return 0; }
static void ll_dmusic_js_status(int ok, int why) { (void)ok; (void)why; }
static void ll_dmusic_js_stats(int notes, int voices, int patterns, int segments, double tick,
                               int measure, int beat, int groove, int modifier, int seg_ptr,
                               int style_ptr, int pat_ptr, int missing, double gain)
{
    (void)notes; (void)voices; (void)patterns; (void)segments; (void)tick; (void)measure;
    (void)beat; (void)groove; (void)modifier; (void)seg_ptr; (void)style_ptr; (void)pat_ptr;
    (void)missing; (void)gain;
}
#endif

static void dm_status(int ok, const char* why)
{
    ll_host_trace("DirectMusic: %s", why);
    ll_dmusic_js_status(ok, (int)(__UINTPTR_TYPE__)why);
}

/* ---- small helpers ---------------------------------------------------------- */

/* A wide string from the game, to 7-bit ASCII, whichever width it has.
 *
 * On Windows L"imusic" is UTF-16, and every structure the game declares
 * (DMUS_OBJECTDESC's wszName, the loader's path arguments) is `unsigned short`.
 * But the portable build compiles musicthread.c with the toolchain's own
 * wchar_t, which is 4 bytes on wasm32 and on the native hosts, and its wcscpy
 * is libc's -- so the strings MusicThread passes are UTF-32 LE. Read as UTF-16,
 * L"imusic" is "i": the search directory did not exist, every scan failed, the
 * 30 segments were never built, and MusicThread's unguarded download loop read
 * a vtable through a null segment. Both widths occur here (EnumObject writes
 * UTF-16 into the descriptors it hands back), so the width is read off the
 * string: an ASCII character followed by three zero bytes is UTF-32. A
 * one-character string reads the same either way. */
static void dm_wstr(const void* src, size_t max_bytes, char* out, size_t cap)
{
    const unsigned char* p = (const unsigned char*)src;
    size_t               i, n = 0;
    int                  wide4 = max_bytes >= 8 && p[0] && !p[1] && !p[2] && !p[3];
    size_t               unit = wide4 ? 4 : 2;
    if (!cap)
        return;
    for (i = 0; i + unit <= max_bytes && n + 1 < cap; i += unit) {
        uint32_t ch = wide4 ? (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) |
                              ((uint32_t)p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24)
                            : (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8);
        if (!ch)
            break;
        out[n++] = ch < 0x80 ? (char)ch : '?';
    }
    out[n] = 0;
}

static void dm_a2w(const char* s, uint16_t* w, size_t max)
{
    size_t i = 0;
    for (; s && s[i] && i + 1 < max; i++)
        w[i] = (unsigned char)s[i];
    w[i] = 0;
}

static unsigned char* dm_read_file(const char* path, size_t* size)
{
    FILE*          f = fopen(path, "rb");
    unsigned char* data;
    long           n;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return 0;
    }
    data = (unsigned char*)malloc((size_t)n);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = 0;
    }
    fclose(f);
    if (data)
        *size = (size_t)n;
    return data;
}

static const char* dm_base(const char* path)
{
    const char* s = strrchr(path, '/');
    const char* b = strrchr(path, '\\');
    if (b && (!s || b > s))
        s = b;
    return s ? s + 1 : path;
}

static int dm_has_ext(const char* name, const char* ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e + 1 && name[n - e - 1] == '.' && strcasecmp(name + n - e, ext) == 0;
}

/* ======================================================================== */
/* Segment                                                                  */
/* ======================================================================== */

typedef struct DmSegmentVtbl {
    long          (*QueryInterface)(DmSegment*, const void*, void**);             /* +0x00 */
    unsigned long (*AddRef)(DmSegment*);                                          /* +0x04 */
    unsigned long (*Release)(DmSegment*);                                         /* +0x08 */
    long          (*GetLength)(DmSegment*, long*);                                /* +0x0c */
    long          (*SetLength)(DmSegment*, long);                                 /* +0x10 */
    long          (*GetRepeats)(DmSegment*, unsigned long*);                      /* +0x14 */
    long          (*SetRepeats)(DmSegment*, unsigned long);                       /* +0x18 */
    long          (*GetDefaultResolution)(DmSegment*, unsigned long*);            /* +0x1c */
    long          (*SetDefaultResolution)(DmSegment*, unsigned long);             /* +0x20 */
    long          (*GetTrack)(DmSegment*, const void*, unsigned long, unsigned long,
                              void**);                                            /* +0x24 */
    long          (*GetTrackGroup)(DmSegment*, void*, unsigned long*);            /* +0x28 */
    long          (*InsertTrack)(DmSegment*, void*, unsigned long);               /* +0x2c */
    long          (*RemoveTrack)(DmSegment*, void*);                              /* +0x30 */
    long          (*InitPlay)(DmSegment*, void**, void*, unsigned long);          /* +0x34 */
    long          (*GetGraph)(DmSegment*, void**);                                /* +0x38 */
    long          (*SetGraph)(DmSegment*, void*);                                 /* +0x3c */
    long          (*AddNotificationType)(DmSegment*, const void*);                /* +0x40 */
    long          (*RemoveNotificationType)(DmSegment*, const void*);             /* +0x44 */
    long          (*GetParam)(DmSegment*, const void*, unsigned long, unsigned long, long,
                              long*, void*);                                      /* +0x48 */
    long          (*SetParam)(DmSegment*, const void*, unsigned long, unsigned long, long,
                              void*);                                             /* +0x4c */
    long          (*Clone)(DmSegment*, long, long, void**);                       /* +0x50 */
    long          (*SetStartPoint)(DmSegment*, long);                             /* +0x54 */
    long          (*GetStartPoint)(DmSegment*, long*);                            /* +0x58 */
    long          (*SetLoopPoints)(DmSegment*, long, long);                       /* +0x5c */
    long          (*GetLoopPoints)(DmSegment*, long*, long*);                     /* +0x60 */
    long          (*SetPChannelsUsed)(DmSegment*, unsigned long, unsigned long*); /* +0x64 */
} DmSegmentVtbl;

struct DmSegment {
    const DmSegmentVtbl* lpVtbl;
    int                  refs;
    LLDmSegmentData      data;
    uint32_t             repeats;
    int                  downloaded;
};

static long SG_QueryInterface(DmSegment* s, const void* iid, void** out)
{
    (void)iid;
    if (!out)
        return DM_E_POINTER;
    *out = s;
    s->refs++;
    return DM_S_OK;
}
static unsigned long SG_AddRef(DmSegment* s) { return (unsigned long)++s->refs; }
static unsigned long SG_Release(DmSegment* s)
{
    int i;
    if (--s->refs > 0)
        return (unsigned long)s->refs;
    for (i = 0; i < s->data.nstyles; i++)
        dm_release(s->data.styles[i].owner);
    ll_dm_free_segment(&s->data);
    free(s);
    return 0;
}
static long SG_GetLength(DmSegment* s, long* len)
{
    if (!len) return DM_E_POINTER;
    *len = s->data.length;
    return DM_S_OK;
}
static long SG_SetLength(DmSegment* s, long len)
{
    if (len <= 0) return DM_E_INVALIDARG;
    s->data.length = (int32_t)len;
    return DM_S_OK;
}
static long SG_GetRepeats(DmSegment* s, unsigned long* n)
{
    if (!n) return DM_E_POINTER;
    *n = s->repeats;
    return DM_S_OK;
}
static long SG_SetRepeats(DmSegment* s, unsigned long n) { s->repeats = (uint32_t)n; return DM_S_OK; }
static long SG_GetDefaultResolution(DmSegment* s, unsigned long* r)
{
    if (!r) return DM_E_POINTER;
    *r = s->data.resolution;
    return DM_S_OK;
}
static long SG_SetDefaultResolution(DmSegment* s, unsigned long r)
{
    s->data.resolution = (uint32_t)r;
    return DM_S_OK;
}
static long SG_GetTrack(DmSegment* s, const void* cls, unsigned long group, unsigned long index,
                        void** out)
{
    (void)s; (void)cls; (void)group; (void)index;
    if (out) *out = 0;
    return DM_E_NOTIMPL;
}
static long SG_GetTrackGroup(DmSegment* s, void* track, unsigned long* group)
{ (void)s; (void)track; (void)group; return DM_E_NOTIMPL; }
static long SG_InsertTrack(DmSegment* s, void* track, unsigned long group)
{ (void)s; (void)track; (void)group; return DM_E_NOTIMPL; }
static long SG_RemoveTrack(DmSegment* s, void* track) { (void)s; (void)track; return DM_E_NOTIMPL; }
static long SG_InitPlay(DmSegment* s, void** state, void* perf, unsigned long flags)
{ (void)s; (void)state; (void)perf; (void)flags; return DM_E_NOTIMPL; }
static long SG_GetGraph(DmSegment* s, void** g) { (void)s; if (g) *g = 0; return DM_E_NOTIMPL; }
static long SG_SetGraph(DmSegment* s, void* g) { (void)s; (void)g; return DM_E_NOTIMPL; }
static long SG_AddNotificationType(DmSegment* s, const void* g) { (void)s; (void)g; return DM_S_OK; }
static long SG_RemoveNotificationType(DmSegment* s, const void* g) { (void)s; (void)g; return DM_S_OK; }
static long SG_GetParam(DmSegment* s, const void* type, unsigned long group, unsigned long index,
                        long time, long* next, void* param)
{
    (void)s; (void)type; (void)group; (void)index; (void)time; (void)param;
    if (next) *next = 0;
    return DM_E_NOTIMPL;
}
/* MusicThread sends GUID_Download with the performance; the collection is
 * already loaded whole, so this only records that it asked. */
static long SG_SetParam(DmSegment* s, const void* type, unsigned long group, unsigned long index,
                        long time, void* param)
{
    (void)group; (void)index; (void)time; (void)param;
    if (dm_guid_is(type, kGuidDownload)) {
        s->downloaded = 1;
        return DM_S_OK;
    }
    return DM_S_OK;
}
static long SG_Clone(DmSegment* s, long start, long end, void** out)
{ (void)s; (void)start; (void)end; if (out) *out = 0; return DM_E_NOTIMPL; }
static long SG_SetStartPoint(DmSegment* s, long t)
{
    if (t < 0 || t >= s->data.length) return DM_E_INVALIDARG;
    s->data.play_start = (int32_t)t;
    return DM_S_OK;
}
static long SG_GetStartPoint(DmSegment* s, long* t)
{
    if (!t) return DM_E_POINTER;
    *t = s->data.play_start;
    return DM_S_OK;
}
static long SG_SetLoopPoints(DmSegment* s, long start, long end)
{
    s->data.loop_start = (int32_t)start;
    s->data.loop_end = (int32_t)end;
    return DM_S_OK;
}
static long SG_GetLoopPoints(DmSegment* s, long* start, long* end)
{
    if (start) *start = s->data.loop_start;
    if (end) *end = s->data.loop_end;
    return DM_S_OK;
}
static long SG_SetPChannelsUsed(DmSegment* s, unsigned long n, unsigned long* list)
{ (void)s; (void)n; (void)list; return DM_S_OK; }

static const DmSegmentVtbl kSegmentVtbl = {
    SG_QueryInterface, SG_AddRef, SG_Release, SG_GetLength, SG_SetLength, SG_GetRepeats,
    SG_SetRepeats, SG_GetDefaultResolution, SG_SetDefaultResolution, SG_GetTrack,
    SG_GetTrackGroup, SG_InsertTrack, SG_RemoveTrack, SG_InitPlay, SG_GetGraph, SG_SetGraph,
    SG_AddNotificationType, SG_RemoveNotificationType, SG_GetParam, SG_SetParam, SG_Clone,
    SG_SetStartPoint, SG_GetStartPoint, SG_SetLoopPoints, SG_GetLoopPoints, SG_SetPChannelsUsed,
};

/* ======================================================================== */
/* Style                                                                    */
/* ======================================================================== */

typedef struct DmStyleVtbl {
    long          (*QueryInterface)(DmStyle*, const void*, void**);                    /* +0x00 */
    unsigned long (*AddRef)(DmStyle*);                                                 /* +0x04 */
    unsigned long (*Release)(DmStyle*);                                                /* +0x08 */
    long          (*GetBand)(DmStyle*, unsigned short*, void**);                       /* +0x0c */
    long          (*EnumBand)(DmStyle*, unsigned long, unsigned short*);               /* +0x10 */
    long          (*GetDefaultBand)(DmStyle*, void**);                                 /* +0x14 */
    long          (*EnumMotif)(DmStyle*, unsigned long, unsigned short*);              /* +0x18 */
    long          (*GetMotif)(DmStyle*, unsigned short*, void**);                      /* +0x1c */
    long          (*GetDefaultChordMap)(DmStyle*, void**);                             /* +0x20 */
    long          (*EnumChordMap)(DmStyle*, unsigned long, unsigned short*);           /* +0x24 */
    long          (*GetChordMap)(DmStyle*, unsigned short*, void**);                   /* +0x28 */
    long          (*GetTimeSignature)(DmStyle*, void*);                                /* +0x2c */
    long          (*GetEmbellishmentLength)(DmStyle*, unsigned long, unsigned long,
                                            unsigned long*, unsigned long*);           /* +0x30 */
    long          (*GetTempo)(DmStyle*, double*);                                      /* +0x34 */
} DmStyleVtbl;

struct DmStyle {
    const DmStyleVtbl* lpVtbl;
    int                refs;
    LLDmStyle          style;
};

static long ST_QueryInterface(DmStyle* s, const void* iid, void** out)
{
    (void)iid;
    if (!out) return DM_E_POINTER;
    *out = s;
    s->refs++;
    return DM_S_OK;
}
static unsigned long ST_AddRef(DmStyle* s) { return (unsigned long)++s->refs; }
static unsigned long ST_Release(DmStyle* s)
{
    if (--s->refs > 0)
        return (unsigned long)s->refs;
    ll_dm_free_style(&s->style);
    free(s);
    return 0;
}
static long ST_GetBand(DmStyle* s, unsigned short* name, void** band)
{ (void)s; (void)name; if (band) *band = 0; return DM_E_NOTIMPL; }
static long ST_EnumBand(DmStyle* s, unsigned long i, unsigned short* name)
{ (void)s; (void)i; (void)name; return DM_S_FALSE; }
static long ST_GetDefaultBand(DmStyle* s, void** band) { (void)s; if (band) *band = 0; return DM_E_NOTIMPL; }
static long ST_EnumMotif(DmStyle* s, unsigned long i, unsigned short* name)
{ (void)s; (void)i; (void)name; return DM_S_FALSE; }
static long ST_GetMotif(DmStyle* s, unsigned short* name, void** seg)
{ (void)s; (void)name; if (seg) *seg = 0; return DM_E_NOTIMPL; }
static long ST_GetDefaultChordMap(DmStyle* s, void** cm) { (void)s; if (cm) *cm = 0; return DM_E_NOTIMPL; }
static long ST_EnumChordMap(DmStyle* s, unsigned long i, unsigned short* name)
{ (void)s; (void)i; (void)name; return DM_S_FALSE; }
static long ST_GetChordMap(DmStyle* s, unsigned short* name, void** cm)
{ (void)s; (void)name; if (cm) *cm = 0; return DM_E_NOTIMPL; }
static long ST_GetTimeSignature(DmStyle* s, void* ts)
{
    unsigned char* p = (unsigned char*)ts;
    if (!p) return DM_E_POINTER;
    memset(p, 0, 8);             /* DMUS_TIMESIGNATURE: mtTime, beats, beat, grids */
    p[4] = s->style.ts.beats_per_measure;
    p[5] = s->style.ts.beat;
    p[6] = (unsigned char)(s->style.ts.grids_per_beat & 0xff);
    p[7] = (unsigned char)(s->style.ts.grids_per_beat >> 8);
    return DM_S_OK;
}
static long ST_GetEmbellishmentLength(DmStyle* s, unsigned long type, unsigned long level,
                                      unsigned long* min, unsigned long* max)
{ (void)s; (void)type; (void)level; if (min) *min = 0; if (max) *max = 0; return DM_S_FALSE; }
static long ST_GetTempo(DmStyle* s, double* tempo)
{
    if (!tempo) return DM_E_POINTER;
    *tempo = s->style.tempo;
    return DM_S_OK;
}

static const DmStyleVtbl kStyleVtbl = {
    ST_QueryInterface, ST_AddRef, ST_Release, ST_GetBand, ST_EnumBand, ST_GetDefaultBand,
    ST_EnumMotif, ST_GetMotif, ST_GetDefaultChordMap, ST_EnumChordMap, ST_GetChordMap,
    ST_GetTimeSignature, ST_GetEmbellishmentLength, ST_GetTempo,
};

/* ======================================================================== */
/* Loader                                                                   */
/* ======================================================================== */

typedef struct DmEntry {
    int        kind;             /* LL_DM_KIND_* */
    LLDmGuid   guid;
    int        have_guid;
    char       name[64];
    char       path[1024];
    DmUnknown* obj;              /* the cache's reference */
} DmEntry;

typedef struct DmLoaderVtbl {
    long          (*QueryInterface)(DmLoader*, const void*, void**);                     /* +0x00 */
    unsigned long (*AddRef)(DmLoader*);                                                  /* +0x04 */
    unsigned long (*Release)(DmLoader*);                                                 /* +0x08 */
    long          (*GetObject)(DmLoader*, void*, const void*, void**);                   /* +0x0c */
    long          (*SetObject)(DmLoader*, void*);                                        /* +0x10 */
    long          (*SetSearchDirectory)(DmLoader*, const void*, const unsigned short*, int); /* +0x14 */
    long          (*ScanDirectory)(DmLoader*, const void*, const unsigned short*,
                                   const unsigned short*);                               /* +0x18 */
    long          (*CacheObject)(DmLoader*, void*);                                      /* +0x1c */
    long          (*ReleaseObject)(DmLoader*, void*);                                    /* +0x20 */
    long          (*ClearCache)(DmLoader*, const void*);                                 /* +0x24 */
    long          (*EnableCache)(DmLoader*, const void*, int);                           /* +0x28 */
    long          (*EnumObject)(DmLoader*, const void*, unsigned long, void*);           /* +0x2c */
} DmLoaderVtbl;

struct DmLoader {
    const DmLoaderVtbl* lpVtbl;
    int                 refs;
    char                dir[1024];
    int                 have_dir;
    int                 cache;
    DmEntry*            entries;
    int                 nentries;
    int                 cap;
};

static int dm_kind_of_class(const void* cls)
{
    if (dm_guid_is(cls, kClsidSegment)) return LL_DM_KIND_SEGMENT;
    if (dm_guid_is(cls, kClsidStyle))   return LL_DM_KIND_STYLE;
    if (dm_guid_is(cls, kClsidBand))    return LL_DM_KIND_BAND;
    return LL_DM_KIND_NONE;
}

static DmEntry* dm_entry_by_path(DmLoader* ld, const char* path)
{
    int i;
    for (i = 0; i < ld->nentries; i++)
        if (strcmp(ld->entries[i].path, path) == 0)
            return &ld->entries[i];
    return 0;
}

/* Index one file: its kind, GUID and name come from the file itself. */
static DmEntry* dm_entry_add(DmLoader* ld, const char* path)
{
    DmEntry*       e = dm_entry_by_path(ld, path);
    size_t         size = 0;
    unsigned char* data;
    int            kind;
    if (e)
        return e;
    data = dm_read_file(path, &size);
    if (!data)
        return 0;
    if (ld->nentries == ld->cap) {
        int      cap = ld->cap ? ld->cap * 2 : 256;
        DmEntry* n = (DmEntry*)realloc(ld->entries, (size_t)cap * sizeof *n);
        if (!n) {
            free(data);
            return 0;
        }
        ld->entries = n;
        ld->cap = cap;
    }
    e = &ld->entries[ld->nentries];
    memset(e, 0, sizeof *e);
    kind = ll_dm_peek(data, size, &e->guid, &e->have_guid, e->name, sizeof e->name);
    free(data);
    if (kind == LL_DM_KIND_NONE)
        return 0;
    e->kind = kind;
    snprintf(e->path, sizeof e->path, "%s", path);
    ld->nentries++;
    return e;
}

static DmUnknown* dm_segment_from(DmLoader* ld, const unsigned char* data, size_t size);
static DmUnknown* dm_style_from(const unsigned char* data, size_t size);

/* The object for an index entry, with a reference for the caller. */
static DmUnknown* dm_entry_object(DmLoader* ld, DmEntry* e)
{
    unsigned char* data;
    size_t         size = 0;
    DmUnknown*     obj = 0;
    if (e->obj) {
        dm_addref(e->obj);
        return e->obj;
    }
    data = dm_read_file(e->path, &size);
    if (!data)
        return 0;
    if (e->kind == LL_DM_KIND_STYLE)
        obj = dm_style_from(data, size);
    else if (e->kind == LL_DM_KIND_SEGMENT)
        obj = dm_segment_from(ld, data, size);
    free(data);
    if (obj && ld->cache) {
        /* A segment can load a style through this same entry while it is
         * being built; keep whichever object got here first. */
        if (e->obj) {
            dm_release(obj);
            dm_addref(e->obj);
            return e->obj;
        }
        e->obj = obj;
        dm_addref(obj);
    }
    return obj;
}

static DmEntry* dm_find(DmLoader* ld, int kind, const LLDmGuid* guid, const char* name,
                        const char* file)
{
    int i;
    if (guid)
        for (i = 0; i < ld->nentries; i++)
            if (ld->entries[i].kind == kind && ld->entries[i].have_guid &&
                ll_dm_guid_equal(&ld->entries[i].guid, guid))
                return &ld->entries[i];
    if (name && *name)
        for (i = 0; i < ld->nentries; i++)
            if (ld->entries[i].kind == kind && strcasecmp(ld->entries[i].name, name) == 0)
                return &ld->entries[i];
    if (file && *file) {
        for (i = 0; i < ld->nentries; i++)
            if (ld->entries[i].kind == kind &&
                strcasecmp(dm_base(ld->entries[i].path), dm_base(file)) == 0)
                return &ld->entries[i];
        if (ld->have_dir) {
            char want[1024], path[1024];
            snprintf(want, sizeof want, "%s/%s", ld->dir, dm_base(file));
            ll_host_resolve_path(path, sizeof path, want, 0);
            {
                DmEntry* e = dm_entry_add(ld, path);
                if (e && e->kind == kind)
                    return e;
            }
        }
    }
    return 0;
}

static const LLDmStyle* dm_resolve_style(void* ctx, const LLDmStyleRef* ref, void** owner)
{
    DmLoader*  ld = (DmLoader*)ctx;
    DmEntry*   e = dm_find(ld, LL_DM_KIND_STYLE, ref->have_guid ? &ref->guid : 0, ref->name,
                           ref->file);
    DmUnknown* obj;
    *owner = 0;
    if (!e)
        return 0;
    obj = dm_entry_object(ld, e);
    if (!obj)
        return 0;
    *owner = obj;
    return &((DmStyle*)obj)->style;
}

static DmUnknown* dm_segment_from(DmLoader* ld, const unsigned char* data, size_t size)
{
    DmSegment* s = (DmSegment*)calloc(1, sizeof *s);
    int        missing;
    if (!s)
        return 0;
    if (ll_dm_parse_segment(data, size, &s->data) != 0) {
        free(s);
        return 0;
    }
    s->lpVtbl = &kSegmentVtbl;
    s->refs = 1;
    s->repeats = s->data.repeats;
    missing = ld ? ll_dm_segment_resolve(&s->data, dm_resolve_style, ld) : s->data.nstyles;
    if (missing)
        ll_host_trace("DirectMusic: segment \"%s\": %d of %d style references did not resolve",
                      s->data.name, missing, s->data.nstyles);
    return (DmUnknown*)s;
}

static DmUnknown* dm_style_from(const unsigned char* data, size_t size)
{
    DmStyle* s = (DmStyle*)calloc(1, sizeof *s);
    if (!s)
        return 0;
    if (ll_dm_parse_style(data, size, &s->style) != 0) {
        free(s);
        return 0;
    }
    s->lpVtbl = &kStyleVtbl;
    s->refs = 1;
    return (DmUnknown*)s;
}

static long LD_QueryInterface(DmLoader* ld, const void* iid, void** out)
{
    (void)iid;
    if (!out) return DM_E_POINTER;
    *out = ld;
    ld->refs++;
    return DM_S_OK;
}
static unsigned long LD_AddRef(DmLoader* ld) { return (unsigned long)++ld->refs; }
static long LD_ClearCache(DmLoader* ld, const void* cls);
static unsigned long LD_Release(DmLoader* ld)
{
    if (--ld->refs > 0)
        return (unsigned long)ld->refs;
    LD_ClearCache(ld, kGuidAllTypes);
    free(ld->entries);
    free(ld);
    return 0;
}

static long LD_GetObject(DmLoader* ld, void* desc_in, const void* iid, void** out)
{
    DmObjectDesc* desc = (DmObjectDesc*)desc_in;
    DmUnknown*    obj = 0;
    int           kind;
    (void)iid;
    if (!out || !desc)
        return DM_E_POINTER;
    *out = 0;
    kind = (desc->dwValidData & DMUS_OBJ_CLASS) ? dm_kind_of_class(desc->guidClass)
                                                : LL_DM_KIND_NONE;

    if (desc->dwValidData & DMUS_OBJ_MEMORY) {
        const unsigned char* data = desc->pbMemData;
        size_t               size = (size_t)desc->llMemLength;
        LLDmGuid             guid;
        int                  have_guid = 0;
        char                 name[64];
        int                  file_kind;
        if (!data || !size)
            return DM_E_INVALIDARG;
        file_kind = ll_dm_peek(data, size, &guid, &have_guid, name, sizeof name);
        if (kind == LL_DM_KIND_NONE)
            kind = file_kind;
        if (file_kind != kind)
            return DM_E_FAIL;
        if (kind == LL_DM_KIND_SEGMENT)
            obj = dm_segment_from(ld, data, size);
        else if (kind == LL_DM_KIND_STYLE)
            obj = dm_style_from(data, size);
        if (!obj)
            return DM_E_FAIL;
        *out = obj;
        return DM_S_OK;
    }

    {
        char     name[64], file[520];
        LLDmGuid guid;
        DmEntry* e;
        name[0] = file[0] = 0;
        if (desc->dwValidData & DMUS_OBJ_OBJECT)
            memcpy(guid.b, desc->guidObject, 16);
        if (desc->dwValidData & DMUS_OBJ_NAME)
            dm_wstr(desc->wszName, sizeof desc->wszName, name, sizeof name);
        if (desc->dwValidData & DMUS_OBJ_FILENAME)
            dm_wstr(desc->wszFileName, sizeof desc->wszFileName, file, sizeof file);
        if (kind == LL_DM_KIND_NONE)
            return DM_E_INVALIDARG;
        e = dm_find(ld, kind, (desc->dwValidData & DMUS_OBJ_OBJECT) ? &guid : 0, name, file);
        if (!e)
            return DM_E_FAIL;
        obj = dm_entry_object(ld, e);
        if (!obj)
            return DM_E_FAIL;
        *out = obj;
        return DM_S_OK;
    }
}

static long LD_SetObject(DmLoader* ld, void* desc) { (void)ld; (void)desc; return DM_E_NOTIMPL; }

static long LD_SetSearchDirectory(DmLoader* ld, const void* cls, const unsigned short* path,
                                  int clear)
{
    char want[520];
    DIR* d;
    (void)cls;
    if (!path)
        return DM_E_POINTER;
    if (clear)
        LD_ClearCache(ld, kGuidAllTypes);
    dm_wstr(path, 4 * 260, want, sizeof want);
    ll_host_resolve_path(ld->dir, sizeof ld->dir, want, 0);
    d = opendir(ld->dir);
    ld->have_dir = d != 0;
    if (d)
        closedir(d);
    ll_host_trace("DirectMusic loader: search directory \"%s\" -> %s%s", want, ld->dir,
                  ld->have_dir ? "" : " (missing)");
    return ld->have_dir ? DM_S_OK : DM_E_FAIL;
}

static long LD_ScanDirectory(DmLoader* ld, const void* cls, const unsigned short* ext,
                             const unsigned short* cache_file)
{
    char           want_ext[32];
    DIR*           d;
    struct dirent* ent;
    int            kind = dm_kind_of_class(cls), found = 0;
    (void)cache_file;
    if (!ext)
        return DM_E_POINTER;
    if (!ld->have_dir)
        return DM_E_FAIL;
    dm_wstr(ext, 4 * 16, want_ext, sizeof want_ext);
    d = opendir(ld->dir);
    if (!d)
        return DM_E_FAIL;
    while ((ent = readdir(d)) != 0) {
        char     path[1024];
        DmEntry* e;
        if (!dm_has_ext(ent->d_name, want_ext))
            continue;
        snprintf(path, sizeof path, "%s/%s", ld->dir, ent->d_name);
        e = dm_entry_add(ld, path);
        if (e && (kind == LL_DM_KIND_NONE || e->kind == kind))
            found++;
    }
    closedir(d);
    ll_host_trace("DirectMusic loader: scanned %d *.%s", found, want_ext);
    return found ? DM_S_OK : DM_S_FALSE;
}

static long LD_CacheObject(DmLoader* ld, void* obj) { (void)ld; (void)obj; return DM_S_OK; }
static long LD_ReleaseObject(DmLoader* ld, void* obj)
{
    int i;
    for (i = 0; i < ld->nentries; i++) {
        if (ld->entries[i].obj == obj) {
            ld->entries[i].obj = 0;
            dm_release(obj);
            return DM_S_OK;
        }
    }
    return DM_S_FALSE;
}

static long LD_ClearCache(DmLoader* ld, const void* cls)
{
    int i, kind = dm_kind_of_class(cls);
    int all = dm_guid_is(cls, kGuidAllTypes) || !cls;
    for (i = 0; i < ld->nentries; i++) {
        DmEntry* e = &ld->entries[i];
        if (e->obj && (all || e->kind == kind)) {
            DmUnknown* obj = e->obj;
            e->obj = 0;
            dm_release(obj);
        }
    }
    return DM_S_OK;
}

static long LD_EnableCache(DmLoader* ld, const void* cls, int on)
{
    (void)cls;
    if (!on)
        LD_ClearCache(ld, cls);
    ld->cache = on ? 1 : 0;
    return DM_S_OK;
}

static long LD_EnumObject(DmLoader* ld, const void* cls, unsigned long index, void* desc_in)
{
    DmObjectDesc* desc = (DmObjectDesc*)desc_in;
    int           kind = dm_kind_of_class(cls), i;
    unsigned long n = 0;
    if (!desc)
        return DM_E_POINTER;
    for (i = 0; i < ld->nentries; i++) {
        DmEntry* e = &ld->entries[i];
        if (kind != LL_DM_KIND_NONE && e->kind != kind)
            continue;
        if (n++ != index)
            continue;
        desc->dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_NAME | DMUS_OBJ_FILENAME |
                            (e->have_guid ? DMUS_OBJ_OBJECT : 0);
        memcpy(desc->guidObject, e->guid.b, 16);
        memcpy(desc->guidClass,
               e->kind == LL_DM_KIND_STYLE ? kClsidStyle :
               e->kind == LL_DM_KIND_SEGMENT ? kClsidSegment : kClsidBand, 16);
        dm_a2w(e->name, desc->wszName, 64);
        dm_a2w(dm_base(e->path), desc->wszFileName, 260);
        return DM_S_OK;
    }
    return DM_S_FALSE;
}

static const DmLoaderVtbl kLoaderVtbl = {
    LD_QueryInterface, LD_AddRef, LD_Release, LD_GetObject, LD_SetObject, LD_SetSearchDirectory,
    LD_ScanDirectory, LD_CacheObject, LD_ReleaseObject, LD_ClearCache, LD_EnableCache,
    LD_EnumObject,
};

/* ======================================================================== */
/* Port                                                                     */
/* ======================================================================== */

typedef struct DmPortVtbl {
    long          (*QueryInterface)(DmPort*, const void*, void**);                /* +0x00 */
    unsigned long (*AddRef)(DmPort*);                                             /* +0x04 */
    unsigned long (*Release)(DmPort*);                                            /* +0x08 */
    long          (*PlayBuffer)(DmPort*, void*);                                  /* +0x0c */
    long          (*SetReadNotificationHandle)(DmPort*, void*);                   /* +0x10 */
    long          (*Read)(DmPort*, void*);                                        /* +0x14 */
    long          (*DownloadInstrument)(DmPort*, void*, void**, void*, unsigned long); /* +0x18 */
    long          (*UnloadInstrument)(DmPort*, void*);                            /* +0x1c */
    long          (*GetLatencyClock)(DmPort*, void**);                            /* +0x20 */
    long          (*GetRunningStats)(DmPort*, void*);                             /* +0x24 */
    long          (*Compact)(DmPort*);                                            /* +0x28 */
    long          (*GetCaps)(DmPort*, void*);                                     /* +0x2c */
    long          (*DeviceIoControl)(DmPort*, unsigned long, void*, unsigned long, void*,
                                     unsigned long, unsigned long*, void*);       /* +0x30 */
    long          (*SetNumChannelGroups)(DmPort*, unsigned long);                 /* +0x34 */
    long          (*GetNumChannelGroups)(DmPort*, unsigned long*);                /* +0x38 */
    long          (*Activate)(DmPort*, int);                                      /* +0x3c */
    long          (*SetChannelPriority)(DmPort*, unsigned long, unsigned long, unsigned long); /* +0x40 */
    long          (*GetChannelPriority)(DmPort*, unsigned long, unsigned long, unsigned long*); /* +0x44 */
    long          (*SetDirectSound)(DmPort*, void*, void*);                       /* +0x48 */
    long          (*GetFormat)(DmPort*, void*, unsigned long*, unsigned long*);   /* +0x4c */
} DmPortVtbl;

struct DmPort {
    const DmPortVtbl* lpVtbl;
    int               refs;
    void*             dsb;
    int               active;
};

static long PT_QueryInterface(DmPort* p, const void* iid, void** out)
{
    (void)iid;
    if (!out) return DM_E_POINTER;
    *out = p;
    p->refs++;
    return DM_S_OK;
}
static unsigned long PT_AddRef(DmPort* p) { return (unsigned long)++p->refs; }
static unsigned long PT_Release(DmPort* p)
{
    if (--p->refs > 0)
        return (unsigned long)p->refs;
    free(p);
    return 0;
}
static long PT_PlayBuffer(DmPort* p, void* b) { (void)p; (void)b; return DM_E_NOTIMPL; }
static long PT_SetReadNotificationHandle(DmPort* p, void* h) { (void)p; (void)h; return DM_E_NOTIMPL; }
static long PT_Read(DmPort* p, void* b) { (void)p; (void)b; return DM_E_NOTIMPL; }
static long PT_DownloadInstrument(DmPort* p, void* i, void** dl, void* r, unsigned long n)
{ (void)p; (void)i; (void)r; (void)n; if (dl) *dl = 0; return DM_S_OK; }
static long PT_UnloadInstrument(DmPort* p, void* dl) { (void)p; (void)dl; return DM_S_OK; }
static long PT_GetLatencyClock(DmPort* p, void** c) { (void)p; if (c) *c = 0; return DM_E_NOTIMPL; }
static long PT_GetRunningStats(DmPort* p, void* s) { (void)p; (void)s; return DM_E_NOTIMPL; }
static long PT_Compact(DmPort* p) { (void)p; return DM_S_OK; }
static long PT_GetCaps(DmPort* p, void* c) { (void)p; (void)c; return DM_E_NOTIMPL; }
static long PT_DeviceIoControl(DmPort* p, unsigned long code, void* in, unsigned long in_size,
                               void* out, unsigned long out_size, unsigned long* ret, void* ov)
{
    (void)p; (void)code; (void)in; (void)in_size; (void)out; (void)out_size; (void)ov;
    if (ret) *ret = 0;
    return DM_E_NOTIMPL;
}
static long PT_SetNumChannelGroups(DmPort* p, unsigned long n) { (void)p; return n == 1 ? DM_S_OK : DM_E_INVALIDARG; }
static long PT_GetNumChannelGroups(DmPort* p, unsigned long* n) { (void)p; if (n) *n = 1; return DM_S_OK; }
static long PT_Activate(DmPort* p, int on)
{
    p->active = on ? 1 : 0;
    if (on)
        ll_audio_music_open();
    return DM_S_OK;
}
static long PT_SetChannelPriority(DmPort* p, unsigned long g, unsigned long c, unsigned long prio)
{ (void)p; (void)g; (void)c; (void)prio; return DM_S_OK; }
static long PT_GetChannelPriority(DmPort* p, unsigned long g, unsigned long c, unsigned long* prio)
{ (void)p; (void)g; (void)c; if (prio) *prio = 0x80000000u; return DM_S_OK; }
static long PT_SetDirectSound(DmPort* p, void* ds, void* dsb)
{
    (void)ds;
    p->dsb = dsb;
    return DM_S_OK;
}

/* The DirectX 6 synth's default output: 22.05 kHz, 16-bit stereo. MusicThread
 * sizes its DirectSound buffer from `bufsize`; the PCM this port actually
 * renders goes straight to Web Audio at the context's rate. */
static long PT_GetFormat(DmPort* p, void* wfx_in, unsigned long* wfx_size, unsigned long* buf_size)
{
    unsigned char* wfx = (unsigned char*)wfx_in;
    (void)p;
    if (wfx_size) {
        if (wfx && *wfx_size >= 18) {
            memset(wfx, 0, 18);
            wfx[0] = 1;                          /* WAVE_FORMAT_PCM */
            wfx[2] = 2;                          /* channels */
            wfx[4] = 0x22; wfx[5] = 0x56;        /* 22050 */
            wfx[8] = 0x88; wfx[9] = 0x58; wfx[10] = 0x01; /* 88200 bytes/s */
            wfx[12] = 4;                         /* block align */
            wfx[14] = 16;                        /* bits */
        }
        *wfx_size = 18;
    }
    if (buf_size)
        *buf_size = 88200;
    return DM_S_OK;
}

static const DmPortVtbl kPortVtbl = {
    PT_QueryInterface, PT_AddRef, PT_Release, PT_PlayBuffer, PT_SetReadNotificationHandle,
    PT_Read, PT_DownloadInstrument, PT_UnloadInstrument, PT_GetLatencyClock, PT_GetRunningStats,
    PT_Compact, PT_GetCaps, PT_DeviceIoControl, PT_SetNumChannelGroups, PT_GetNumChannelGroups,
    PT_Activate, PT_SetChannelPriority, PT_GetChannelPriority, PT_SetDirectSound, PT_GetFormat,
};

/* ======================================================================== */
/* IDirectMusic                                                             */
/* ======================================================================== */

typedef struct DmMusicVtbl {
    long          (*QueryInterface)(DmMusic*, const void*, void**);          /* +0x00 */
    unsigned long (*AddRef)(DmMusic*);                                       /* +0x04 */
    unsigned long (*Release)(DmMusic*);                                      /* +0x08 */
    long          (*EnumPort)(DmMusic*, unsigned long, void*);               /* +0x0c */
    long          (*CreateMusicBuffer)(DmMusic*, void*, void**, void*);      /* +0x10 */
    long          (*CreatePort)(DmMusic*, const void*, void*, void**, void*); /* +0x14 */
    long          (*EnumMasterClock)(DmMusic*, unsigned long, void*);        /* +0x18 */
    long          (*GetMasterClock)(DmMusic*, void*, void**);                /* +0x1c */
    long          (*SetMasterClock)(DmMusic*, const void*);                  /* +0x20 */
    long          (*Activate)(DmMusic*, int);                                /* +0x24 */
    long          (*GetDefaultPort)(DmMusic*, void*);                        /* +0x28 */
    long          (*SetDirectSound)(DmMusic*, void*, void*);                 /* +0x2c */
} DmMusicVtbl;

struct DmMusic {
    const DmMusicVtbl* lpVtbl;
    int                refs;
};

static long DM_QueryInterface(DmMusic* m, const void* iid, void** out)
{
    (void)iid;
    if (!out) return DM_E_POINTER;
    *out = m;
    m->refs++;
    return DM_S_OK;
}
static unsigned long DM_AddRef(DmMusic* m) { return (unsigned long)++m->refs; }
static unsigned long DM_Release(DmMusic* m)
{
    if (--m->refs > 0)
        return (unsigned long)m->refs;
    free(m);
    return 0;
}
static long DM_EnumPort(DmMusic* m, unsigned long i, void* caps) { (void)m; (void)i; (void)caps; return DM_S_FALSE; }
static long DM_CreateMusicBuffer(DmMusic* m, void* desc, void** buf, void* outer)
{ (void)m; (void)desc; (void)outer; if (buf) *buf = 0; return DM_E_NOTIMPL; }
static long DM_CreatePort(DmMusic* m, const void* clsid, void* params, void** port, void* outer)
{
    DmPort* p;
    (void)m; (void)clsid; (void)params; (void)outer;
    if (!port) return DM_E_POINTER;
    p = (DmPort*)calloc(1, sizeof *p);
    if (!p) {
        *port = 0;
        return DM_E_OUTOFMEMORY;
    }
    p->lpVtbl = &kPortVtbl;
    p->refs = 1;
    *port = p;
    return DM_S_OK;
}
static long DM_EnumMasterClock(DmMusic* m, unsigned long i, void* info) { (void)m; (void)i; (void)info; return DM_S_FALSE; }
static long DM_GetMasterClock(DmMusic* m, void* guid, void** clock)
{ (void)m; (void)guid; if (clock) *clock = 0; return DM_E_NOTIMPL; }
static long DM_SetMasterClock(DmMusic* m, const void* guid) { (void)m; (void)guid; return DM_S_OK; }
static long DM_Activate(DmMusic* m, int on) { (void)m; (void)on; return DM_S_OK; }
static long DM_GetDefaultPort(DmMusic* m, void* guid) { (void)m; if (guid) memset(guid, 0, 16); return DM_S_OK; }
static long DM_SetDirectSound(DmMusic* m, void* ds, void* hwnd) { (void)m; (void)ds; (void)hwnd; return DM_S_OK; }

static const DmMusicVtbl kMusicVtbl = {
    DM_QueryInterface, DM_AddRef, DM_Release, DM_EnumPort, DM_CreateMusicBuffer, DM_CreatePort,
    DM_EnumMasterClock, DM_GetMasterClock, DM_SetMasterClock, DM_Activate, DM_GetDefaultPort,
    DM_SetDirectSound,
};

/* ======================================================================== */
/* Composer (created by MusicThread, never used)                            */
/* ======================================================================== */

typedef struct DmComposerVtbl {
    long          (*QueryInterface)(DmComposer*, const void*, void**);                  /* +0x00 */
    unsigned long (*AddRef)(DmComposer*);                                               /* +0x04 */
    unsigned long (*Release)(DmComposer*);                                              /* +0x08 */
    long          (*ComposeSegmentFromTemplate)(DmComposer*, void*, void*, unsigned short,
                                                void*, void**);                         /* +0x0c */
    long          (*ComposeSegmentFromShape)(DmComposer*, void*, unsigned short, unsigned short,
                                             unsigned short, int, int, void*, void**);  /* +0x10 */
    long          (*ComposeTransition)(DmComposer*, void*, void*, long, unsigned short,
                                       unsigned long, void*, void**);                   /* +0x14 */
    long          (*AutoTransition)(DmComposer*, void*, void*, unsigned short, unsigned long,
                                    void*, void**, void**, void**);                     /* +0x18 */
    long          (*ComposeTemplateFromShape)(DmComposer*, unsigned short, unsigned short, int,
                                              int, unsigned short, void**);             /* +0x1c */
    long          (*ChangeChordMap)(DmComposer*, void*, int, void*);                    /* +0x20 */
} DmComposerVtbl;

struct DmComposer {
    const DmComposerVtbl* lpVtbl;
    int                   refs;
};

static long CP_QueryInterface(DmComposer* c, const void* iid, void** out)
{
    (void)iid;
    if (!out) return DM_E_POINTER;
    *out = c;
    c->refs++;
    return DM_S_OK;
}
static unsigned long CP_AddRef(DmComposer* c) { return (unsigned long)++c->refs; }
static unsigned long CP_Release(DmComposer* c)
{
    if (--c->refs > 0)
        return (unsigned long)c->refs;
    free(c);
    return 0;
}
static long CP_FromTemplate(DmComposer* c, void* s, void* t, unsigned short a, void* cm, void** out)
{ (void)c; (void)s; (void)t; (void)a; (void)cm; if (out) *out = 0; return DM_E_NOTIMPL; }
static long CP_FromShape(DmComposer* c, void* s, unsigned short m, unsigned short sh,
                         unsigned short a, int i, int e, void* cm, void** out)
{ (void)c; (void)s; (void)m; (void)sh; (void)a; (void)i; (void)e; (void)cm; if (out) *out = 0; return DM_E_NOTIMPL; }
static long CP_Transition(DmComposer* c, void* f, void* t, long mt, unsigned short cmd,
                          unsigned long fl, void* cm, void** out)
{ (void)c; (void)f; (void)t; (void)mt; (void)cmd; (void)fl; (void)cm; if (out) *out = 0; return DM_E_NOTIMPL; }
static long CP_AutoTransition(DmComposer* c, void* p, void* t, unsigned short cmd, unsigned long fl,
                              void* cm, void** tr, void** ts, void** trs)
{
    (void)c; (void)p; (void)t; (void)cmd; (void)fl; (void)cm;
    if (tr) *tr = 0;
    if (ts) *ts = 0;
    if (trs) *trs = 0;
    return DM_E_NOTIMPL;
}
static long CP_TemplateFromShape(DmComposer* c, unsigned short m, unsigned short sh, int i, int e,
                                 unsigned short el, void** out)
{ (void)c; (void)m; (void)sh; (void)i; (void)e; (void)el; if (out) *out = 0; return DM_E_NOTIMPL; }
static long CP_ChangeChordMap(DmComposer* c, void* s, int t, void* cm)
{ (void)c; (void)s; (void)t; (void)cm; return DM_E_NOTIMPL; }

static const DmComposerVtbl kComposerVtbl = {
    CP_QueryInterface, CP_AddRef, CP_Release, CP_FromTemplate, CP_FromShape, CP_Transition,
    CP_AutoTransition, CP_TemplateFromShape, CP_ChangeChordMap,
};

/* ======================================================================== */
/* Performance                                                              */
/* ======================================================================== */

typedef struct DmPerformanceVtbl {
    long          (*QueryInterface)(DmPerformance*, const void*, void**);               /* +0x00 */
    unsigned long (*AddRef)(DmPerformance*);                                            /* +0x04 */
    unsigned long (*Release)(DmPerformance*);                                           /* +0x08 */
    long          (*Init)(DmPerformance*, void**, void*, void*);                        /* +0x0c */
    long          (*PlaySegment)(DmPerformance*, void*, unsigned long, long long, void**); /* +0x10 */
    long          (*Stop)(DmPerformance*, void*, void*, long, unsigned long);           /* +0x14 */
    long          (*GetSegmentState)(DmPerformance*, void**, long);                     /* +0x18 */
    long          (*SetPrepareTime)(DmPerformance*, unsigned long);                     /* +0x1c */
    long          (*GetPrepareTime)(DmPerformance*, unsigned long*);                    /* +0x20 */
    long          (*SetBumperLength)(DmPerformance*, unsigned long);                    /* +0x24 */
    long          (*GetBumperLength)(DmPerformance*, unsigned long*);                   /* +0x28 */
    long          (*SendPMsg)(DmPerformance*, void*);                                   /* +0x2c */
    long          (*MusicToReferenceTime)(DmPerformance*, long, long long*);            /* +0x30 */
    long          (*ReferenceToMusicTime)(DmPerformance*, long long, long*);            /* +0x34 */
    long          (*IsPlaying)(DmPerformance*, void*, void*);                           /* +0x38 */
    long          (*GetTime)(DmPerformance*, long long*, long*);                        /* +0x3c */
    long          (*AllocPMsg)(DmPerformance*, unsigned long, void**);                  /* +0x40 */
    long          (*FreePMsg)(DmPerformance*, void*);                                   /* +0x44 */
    long          (*GetGraph)(DmPerformance*, void**);                                  /* +0x48 */
    long          (*SetGraph)(DmPerformance*, void*);                                   /* +0x4c */
    long          (*SetNotificationHandle)(DmPerformance*, void*, long long);           /* +0x50 */
    long          (*GetNotificationPMsg)(DmPerformance*, void**);                       /* +0x54 */
    long          (*AddNotificationType)(DmPerformance*, const void*);                  /* +0x58 */
    long          (*RemoveNotificationType)(DmPerformance*, const void*);               /* +0x5c */
    long          (*AddPort)(DmPerformance*, void*);                                    /* +0x60 */
    long          (*RemovePort)(DmPerformance*, void*);                                 /* +0x64 */
    long          (*AssignPChannelBlock)(DmPerformance*, unsigned long, void*, unsigned long); /* +0x68 */
    long          (*AssignPChannel)(DmPerformance*, unsigned long, void*, unsigned long,
                                    unsigned long);                                     /* +0x6c */
    long          (*PChannelInfo)(DmPerformance*, unsigned long, void**, unsigned long*,
                                  unsigned long*);                                      /* +0x70 */
    long          (*DownloadInstrument)(DmPerformance*, void*, unsigned long, void**, void*,
                                        unsigned long, void**, unsigned long*,
                                        unsigned long*);                                /* +0x74 */
    long          (*Invalidate)(DmPerformance*, long, unsigned long);                   /* +0x78 */
    long          (*GetParam)(DmPerformance*, const void*, unsigned long, unsigned long, long,
                              long*, void*);                                            /* +0x7c */
    long          (*SetParam)(DmPerformance*, const void*, unsigned long, unsigned long, long,
                              void*);                                                   /* +0x80 */
    long          (*GetGlobalParam)(DmPerformance*, const void*, void*, unsigned long); /* +0x84 */
    long          (*SetGlobalParam)(DmPerformance*, const void*, void*, unsigned long); /* +0x88 */
    long          (*GetLatencyTime)(DmPerformance*, long long*);                        /* +0x8c */
    long          (*GetQueueTime)(DmPerformance*, long long*);                          /* +0x90 */
    long          (*AdjustTime)(DmPerformance*, long long);                             /* +0x94 */
    long          (*CloseDown)(DmPerformance*);                                         /* +0x98 */
    long          (*GetResolvedTime)(DmPerformance*, long long, long long*, unsigned long); /* +0x9c */
    long          (*MIDIToMusic)(DmPerformance*, unsigned char, void*, unsigned char,
                                 unsigned char, unsigned short*);                       /* +0xa0 */
    long          (*MusicToMIDI)(DmPerformance*, unsigned short, void*, unsigned char,
                                 unsigned char, unsigned char*);                        /* +0xa4 */
    long          (*TimeToRhythm)(DmPerformance*, long, void*, unsigned short*, unsigned char*,
                                  unsigned char*, short*);                              /* +0xa8 */
    long          (*RhythmToTime)(DmPerformance*, unsigned short, unsigned char, unsigned char,
                                  short, void*, long*);                                 /* +0xac */
} DmPerformanceVtbl;

struct DmPerformance {
    const DmPerformanceVtbl* lpVtbl;
    int                      refs;
    LLDmPerf*                perf;
    LLSynth*                 synth;
    double                   rate;
    void*                    notify_handle;
    DmPort*                  port;
    int                      closed;
    signed char              groove;
};

static void dm_seg_retain(void* owner) { dm_addref(owner); }
static void dm_seg_release(void* owner) { dm_release(owner); }

/* Notifications are waiting: wake MusicThread. SetEvent runs the thread's
 * fiber at once when it is waiting on this handle (kernel32.c). */
static void dm_notify_hook(void* ctx)
{
    DmPerformance* p = (DmPerformance*)ctx;
    if (p->notify_handle)
        SetEvent((HANDLE)p->notify_handle);
}

static long PF_CloseDown(DmPerformance* p);

static long PF_QueryInterface(DmPerformance* p, const void* iid, void** out)
{
    (void)iid;
    if (!out) return DM_E_POINTER;
    *out = p;
    p->refs++;
    return DM_S_OK;
}
static unsigned long PF_AddRef(DmPerformance* p) { return (unsigned long)++p->refs; }
static unsigned long PF_Release(DmPerformance* p)
{
    if (--p->refs > 0)
        return (unsigned long)p->refs;
    PF_CloseDown(p);
    ll_dmperf_free(p->perf);
    ll_synth_free(p->synth);
    free(p);
    return 0;
}

static long PF_Init(DmPerformance* p, void** dm, void* ds, void* hwnd)
{
    (void)ds; (void)hwnd;
    if (p->perf)
        return DM_S_OK;
    p->rate = ll_audio_music_rate();
    if (p->rate <= 0.0)
        p->rate = 44100.0;
    p->synth = ll_synth_new(g_dm.dls, p->rate, DM_VOICES);
    p->perf = ll_dmperf_new(p->rate, (uint32_t)dm_now_ms() ^ 0x5ca1ab1eu);
    if (!p->synth || !p->perf)
        return DM_E_OUTOFMEMORY;
    ll_dmperf_set_synth(p->perf, p->synth);
    ll_dmperf_enable_notifications(p->perf, 0, 0);
    ll_dmperf_set_owner_hooks(p->perf, dm_seg_retain, dm_seg_release);
    ll_dmperf_set_notify_hook(p->perf, dm_notify_hook, p);
    if (dm && !*dm) {
        DmMusic* m = (DmMusic*)calloc(1, sizeof *m);
        if (!m)
            return DM_E_OUTOFMEMORY;
        m->lpVtbl = &kMusicVtbl;
        m->refs = 1;
        *dm = m;
    }
    g_dm.active = p;
    g_dm.have_volume = 0;
    ll_host_trace("DirectMusic: performance up at %.0f Hz, %d voices", p->rate, DM_VOICES);
    return DM_S_OK;
}

static long PF_PlaySegment(DmPerformance* p, void* seg_in, unsigned long flags, long long start,
                           void** state)
{
    DmSegment* seg = (DmSegment*)seg_in;
    (void)start;
    if (state)
        *state = 0;
    if (!seg)
        return DM_E_POINTER;
    if (!p->perf || p->closed)
        return DM_E_FAIL;
    ll_host_trace("DirectMusic: PlaySegment \"%s\" flags 0x%lx repeats %u", seg->data.name,
                  (unsigned long)flags, (unsigned)seg->repeats);
    return ll_dmperf_play(p->perf, &seg->data, seg, seg->repeats, (uint32_t)flags) == 0
               ? DM_S_OK : DM_E_FAIL;
}

static long PF_Stop(DmPerformance* p, void* seg, void* state, long time, unsigned long flags)
{
    (void)seg; (void)state; (void)time; (void)flags;
    if (p->perf)
        ll_dmperf_stop(p->perf);
    return DM_S_OK;
}

static long PF_GetSegmentState(DmPerformance* p, void** state, long time)
{ (void)p; (void)time; if (state) *state = 0; return DM_E_NOTIMPL; }
static long PF_SetPrepareTime(DmPerformance* p, unsigned long ms) { (void)p; (void)ms; return DM_S_OK; }
static long PF_GetPrepareTime(DmPerformance* p, unsigned long* ms) { (void)p; if (ms) *ms = 1000; return DM_S_OK; }
static long PF_SetBumperLength(DmPerformance* p, unsigned long ms) { (void)p; (void)ms; return DM_S_OK; }
static long PF_GetBumperLength(DmPerformance* p, unsigned long* ms) { (void)p; if (ms) *ms = 50; return DM_S_OK; }
static long PF_SendPMsg(DmPerformance* p, void* msg) { (void)p; (void)msg; return DM_E_NOTIMPL; }
static long PF_MusicToReferenceTime(DmPerformance* p, long mt, long long* rt)
{
    LLDmPerfStats st;
    if (!rt) return DM_E_POINTER;
    ll_dmperf_stats(p->perf, &st);
    *rt = (long long)(mt / (double)LL_DM_PPQ * 60.0 / (st.tempo > 0 ? st.tempo : 120.0) * 1e7);
    return DM_S_OK;
}
static long PF_ReferenceToMusicTime(DmPerformance* p, long long rt, long* mt)
{
    LLDmPerfStats st;
    if (!mt) return DM_E_POINTER;
    ll_dmperf_stats(p->perf, &st);
    *mt = (long)(rt / 1e7 / 60.0 * (st.tempo > 0 ? st.tempo : 120.0) * LL_DM_PPQ);
    return DM_S_OK;
}
static long PF_IsPlaying(DmPerformance* p, void* seg, void* state)
{
    (void)seg; (void)state;
    return ll_dmperf_playing(p->perf) ? DM_S_OK : DM_S_FALSE;
}
static long PF_GetTime(DmPerformance* p, long long* rt, long* mt)
{
    LLDmPerfStats st;
    ll_dmperf_stats(p->perf, &st);
    if (mt) *mt = (long)st.tick;
    if (rt) *rt = (long long)(dm_now_ms() * 1e4);
    return DM_S_OK;
}
static long PF_AllocPMsg(DmPerformance* p, unsigned long size, void** msg)
{
    (void)p;
    if (!msg) return DM_E_POINTER;
    *msg = size >= 4 ? calloc(1, size) : 0;
    if (!*msg) return DM_E_OUTOFMEMORY;
    *(uint32_t*)*msg = (uint32_t)size;
    return DM_S_OK;
}
static long PF_FreePMsg(DmPerformance* p, void* msg)
{
    (void)p;
    free(msg);
    return DM_S_OK;
}
static long PF_GetGraph(DmPerformance* p, void** g) { (void)p; if (g) *g = 0; return DM_E_NOTIMPL; }
static long PF_SetGraph(DmPerformance* p, void* g) { (void)p; (void)g; return DM_E_NOTIMPL; }
static long PF_SetNotificationHandle(DmPerformance* p, void* h, long long minimum)
{
    (void)minimum;
    p->notify_handle = h;
    return DM_S_OK;
}

static long PF_GetNotificationPMsg(DmPerformance* p, void** out)
{
    LLDmNotify   n;
    DmNotifyMsg* m;
    if (!out)
        return DM_E_POINTER;
    *out = 0;
    if (!p->perf || !ll_dmperf_pop_notification(p->perf, &n))
        return DM_S_FALSE;
    m = (DmNotifyMsg*)calloc(1, sizeof *m);
    if (!m)
        return DM_E_OUTOFMEMORY;
    m->dwSize = (uint32_t)sizeof *m;
    m->mtTime = (int32_t)n.tick;
    m->dwType = 17;              /* DMUS_PMSGT_NOTIFICATION */
    m->punkUser = n.segment;
    memcpy(m->guidNotificationType, n.kind == 0 ? kGuidNotifySegment : kGuidNotifyBeat, 16);
    m->dwNotificationOption = (uint32_t)n.option;
    m->dwField1 = (uint32_t)n.field1;
    m->dwField2 = (uint32_t)n.field2;
    *out = m;
    return DM_S_OK;
}

static long PF_AddNotificationType(DmPerformance* p, const void* guid)
{
    if (!p->perf) return DM_E_FAIL;
    if (dm_guid_is(guid, kGuidNotifySegment))
        ll_dmperf_enable_notifications(p->perf, 1, -1);
    else if (dm_guid_is(guid, kGuidNotifyBeat))
        ll_dmperf_enable_notifications(p->perf, -1, 1);
    return DM_S_OK;
}
static long PF_RemoveNotificationType(DmPerformance* p, const void* guid)
{
    if (!p->perf) return DM_E_FAIL;
    if (dm_guid_is(guid, kGuidNotifySegment))
        ll_dmperf_enable_notifications(p->perf, 0, -1);
    else if (dm_guid_is(guid, kGuidNotifyBeat))
        ll_dmperf_enable_notifications(p->perf, -1, 0);
    return DM_S_OK;
}

static long PF_AddPort(DmPerformance* p, void* port_in)
{
    DmPort* port = (DmPort*)port_in;
    if (!port) return DM_E_POINTER;
    if (p->port)
        dm_release(p->port);
    p->port = port;
    dm_addref(port);
    return DM_S_OK;
}
static long PF_RemovePort(DmPerformance* p, void* port)
{
    if (p->port && (void*)p->port == port) {
        dm_release(p->port);
        p->port = 0;
    }
    return DM_S_OK;
}
static long PF_AssignPChannelBlock(DmPerformance* p, unsigned long block, void* port,
                                   unsigned long group)
{ (void)p; (void)block; (void)port; (void)group; return DM_S_OK; }
static long PF_AssignPChannel(DmPerformance* p, unsigned long pch, void* port, unsigned long group,
                              unsigned long mch)
{ (void)p; (void)pch; (void)port; (void)group; (void)mch; return DM_S_OK; }
static long PF_PChannelInfo(DmPerformance* p, unsigned long pch, void** port, unsigned long* group,
                            unsigned long* mch)
{
    if (port) *port = p->port;
    if (group) *group = 1;
    if (mch) *mch = pch & 15;
    return DM_S_OK;
}
static long PF_DownloadInstrument(DmPerformance* p, void* inst, unsigned long pch, void** dl,
                                  void* ranges, unsigned long nranges, void** port,
                                  unsigned long* group, unsigned long* mch)
{
    (void)inst; (void)ranges; (void)nranges;
    if (dl) *dl = 0;
    return PF_PChannelInfo(p, pch, port, group, mch);
}
static long PF_Invalidate(DmPerformance* p, long mt, unsigned long flags) { (void)p; (void)mt; (void)flags; return DM_S_OK; }
static long PF_GetParam(DmPerformance* p, const void* guid, unsigned long group, unsigned long index,
                        long mt, long* next, void* param)
{
    (void)p; (void)guid; (void)group; (void)index; (void)mt; (void)param;
    if (next) *next = 0;
    return DM_E_NOTIMPL;
}
static long PF_SetParam(DmPerformance* p, const void* guid, unsigned long group, unsigned long index,
                        long mt, void* param)
{ (void)p; (void)guid; (void)group; (void)index; (void)mt; (void)param; return DM_E_NOTIMPL; }
static long PF_GetGlobalParam(DmPerformance* p, const void* guid, void* param, unsigned long size)
{
    if (dm_guid_is(guid, kGuidGroove) && param && size >= 1) {
        *(signed char*)param = p->groove;
        return DM_S_OK;
    }
    return DM_E_NOTIMPL;
}
/* GUID_PerfMasterGrooveLevel is the only global MusicThread and
 * SetMusicGrooveLevel set: one signed byte added to every groove level. */
static long PF_SetGlobalParam(DmPerformance* p, const void* guid, void* param, unsigned long size)
{
    if (dm_guid_is(guid, kGuidGroove) && param && size >= 1) {
        p->groove = *(signed char*)param;
        if (p->perf)
            ll_dmperf_set_groove_modifier(p->perf, p->groove);
        return DM_S_OK;
    }
    return DM_S_OK;
}
static long PF_GetLatencyTime(DmPerformance* p, long long* rt)
{
    (void)p;
    if (rt) *rt = (long long)(dm_now_ms() * 1e4);
    return DM_S_OK;
}
static long PF_GetQueueTime(DmPerformance* p, long long* rt) { return PF_GetLatencyTime(p, rt); }
static long PF_AdjustTime(DmPerformance* p, long long rt) { (void)p; (void)rt; return DM_S_OK; }

static long PF_CloseDown(DmPerformance* p)
{
    if (p->closed)
        return DM_S_OK;
    p->closed = 1;
    if (p->perf)
        ll_dmperf_stop(p->perf);
    if (g_dm.active == p) {
        g_dm.active = 0;
        ll_audio_music_stop();
    }
    if (p->port) {
        dm_release(p->port);
        p->port = 0;
    }
    ll_host_trace("DirectMusic: performance closed down");
    return DM_S_OK;
}

static long PF_GetResolvedTime(DmPerformance* p, long long rt, long long* out, unsigned long flags)
{ (void)p; (void)flags; if (out) *out = rt; return DM_S_OK; }
static long PF_MIDIToMusic(DmPerformance* p, unsigned char midi, void* chord, unsigned char mode,
                           unsigned char level, unsigned short* value)
{ (void)p; (void)midi; (void)chord; (void)mode; (void)level; if (value) *value = 0; return DM_E_NOTIMPL; }
static long PF_MusicToMIDI(DmPerformance* p, unsigned short value, void* chord, unsigned char mode,
                           unsigned char level, unsigned char* midi)
{ (void)p; (void)value; (void)chord; (void)mode; (void)level; if (midi) *midi = 0; return DM_E_NOTIMPL; }
static long PF_TimeToRhythm(DmPerformance* p, long mt, void* ts, unsigned short* measure,
                            unsigned char* beat, unsigned char* grid, short* offset)
{
    (void)p; (void)mt; (void)ts;
    if (measure) *measure = 0;
    if (beat) *beat = 0;
    if (grid) *grid = 0;
    if (offset) *offset = 0;
    return DM_E_NOTIMPL;
}
static long PF_RhythmToTime(DmPerformance* p, unsigned short measure, unsigned char beat,
                            unsigned char grid, short offset, void* ts, long* mt)
{ (void)p; (void)measure; (void)beat; (void)grid; (void)offset; (void)ts; if (mt) *mt = 0; return DM_E_NOTIMPL; }

static const DmPerformanceVtbl kPerformanceVtbl = {
    PF_QueryInterface, PF_AddRef, PF_Release, PF_Init, PF_PlaySegment, PF_Stop,
    PF_GetSegmentState, PF_SetPrepareTime, PF_GetPrepareTime, PF_SetBumperLength,
    PF_GetBumperLength, PF_SendPMsg, PF_MusicToReferenceTime, PF_ReferenceToMusicTime,
    PF_IsPlaying, PF_GetTime, PF_AllocPMsg, PF_FreePMsg, PF_GetGraph, PF_SetGraph,
    PF_SetNotificationHandle, PF_GetNotificationPMsg, PF_AddNotificationType,
    PF_RemoveNotificationType, PF_AddPort, PF_RemovePort, PF_AssignPChannelBlock,
    PF_AssignPChannel, PF_PChannelInfo, PF_DownloadInstrument, PF_Invalidate, PF_GetParam,
    PF_SetParam, PF_GetGlobalParam, PF_SetGlobalParam, PF_GetLatencyTime, PF_GetQueueTime,
    PF_AdjustTime, PF_CloseDown, PF_GetResolvedTime, PF_MIDIToMusic, PF_MusicToMIDI,
    PF_TimeToRhythm, PF_RhythmToTime,
};

/* ======================================================================== */
/* Availability, creation, the pump                                         */
/* ======================================================================== */

static int dm_dir_has_segments(const char* dir)
{
    DIR*           d = opendir(dir);
    struct dirent* ent;
    int            n = 0;
    if (!d)
        return 0;
    while ((ent = readdir(d)) != 0)
        if (dm_has_ext(ent->d_name, "sgt"))
            n++;
    closedir(d);
    return n;
}

static int dm_available(void)
{
    static const char* dls_names[] = { "dls/gm.dls", "gm.dls", "imusic/gm.dls" };
    char               path[1024];
    const char*        env;
    int                i;

    if (g_dm.checked)
        return g_dm.ok;
    g_dm.checked = 1;
    if (ll_dmusic_js_off()) {
        dm_status(0, "switched off by the page (?music=0)");
        return 0;
    }
    if (!ll_audio_enabled() || ll_audio_music_rate() <= 0.0) {
        dm_status(0, "no Web Audio here, so no music (MusicThread takes its no-DirectMusic path)");
        return 0;
    }
    ll_host_resolve_path(path, sizeof path, "imusic", 0);
    if (!dm_dir_has_segments(path)) {
        dm_status(0, "no imusic directory with segments next to the game");
        return 0;
    }
    env = getenv("LL_MUSIC_DLS");
    if (env && *env) {
        size_t         size = 0;
        unsigned char* data = dm_read_file(env, &size);
        if (data) {
            g_dm.dls = ll_dls_load(data, size);
            free(data);
            if (g_dm.dls)
                snprintf(g_dm.dls_path, sizeof g_dm.dls_path, "%s", env);
        }
    }
    for (i = 0; !g_dm.dls && i < (int)(sizeof dls_names / sizeof dls_names[0]); i++) {
        size_t         size = 0;
        unsigned char* data;
        ll_host_resolve_path(path, sizeof path, dls_names[i], 0);
        data = dm_read_file(path, &size);
        if (!data)
            continue;
        g_dm.dls = ll_dls_load(data, size);
        free(data);
        if (g_dm.dls)
            snprintf(g_dm.dls_path, sizeof g_dm.dls_path, "%s", path);
    }
    if (!g_dm.dls) {
        dm_status(0, "no DLS instrument collection (dls/gm.dls); the music has no instruments to play");
        return 0;
    }
    {
        static char why[1200];
        snprintf(why, sizeof why, "available: %d instruments, %d waves from %s",
                 ll_dls_instrument_count(g_dm.dls), ll_dls_wave_count(g_dm.dls), g_dm.dls_path);
        dm_status(1, why);
    }
    g_dm.ok = 1;
    return 1;
}

long ll_dmusic_create(const void* clsid, const void* iid, void** out)
{
    (void)iid;
    if (out)
        *out = 0;
    if (!out)
        return DM_E_POINTER;
    if (!dm_available())
        return DM_REGDB_E_CLASSNOTREG;
    if (dm_guid_is(clsid, kClsidPerformance)) {
        DmPerformance* p = (DmPerformance*)calloc(1, sizeof *p);
        if (!p) return DM_E_OUTOFMEMORY;
        p->lpVtbl = &kPerformanceVtbl;
        p->refs = 1;
        *out = p;
        return DM_S_OK;
    }
    if (dm_guid_is(clsid, kClsidLoader)) {
        DmLoader* l = (DmLoader*)calloc(1, sizeof *l);
        if (!l) return DM_E_OUTOFMEMORY;
        l->lpVtbl = &kLoaderVtbl;
        l->refs = 1;
        *out = l;
        return DM_S_OK;
    }
    if (dm_guid_is(clsid, kClsidComposer)) {
        DmComposer* c = (DmComposer*)calloc(1, sizeof *c);
        if (!c) return DM_E_OUTOFMEMORY;
        c->lpVtbl = &kComposerVtbl;
        c->refs = 1;
        *out = c;
        return DM_S_OK;
    }
    return DM_REGDB_E_CLASSNOTREG;
}

/* The output's soft knee: nothing below 0.75 is touched. */
static float dm_knee(float x)
{
    if (x > 0.75f)
        return 0.75f + 0.25f * (float)tanh((x - 0.75f) / 0.25f);
    if (x < -0.75f)
        return -0.75f - 0.25f * (float)tanh((-x - 0.75f) / 0.25f);
    return x;
}

static void dm_publish(DmPerformance* p)
{
    LLDmPerfStats st;
    double        now = dm_now_ms();
    if (now - g_dm.last_stats_ms < 250.0)
        return;
    g_dm.last_stats_ms = now;
    ll_dmperf_stats(p->perf, &st);
    ll_dmusic_js_stats((int)st.notes_on, ll_synth_active_voices(p->synth), (int)st.patterns,
                       (int)st.segments_started, st.tick, st.measure, st.beat, st.groove,
                       st.groove_modifier, (int)(__UINTPTR_TYPE__)st.segment,
                       (int)(__UINTPTR_TYPE__)st.style, (int)(__UINTPTR_TYPE__)st.pattern,
                       (int)ll_synth_missing_programs(p->synth),
                       g_dm.have_volume ? (g_dm.volume_cb <= -10000 ? 0.0
                                           : pow(10.0, g_dm.volume_cb / 2000.0)) : 1.0);
}

void ll_dmusic_pump(void)
{
    static float   L[DM_CHUNK], R[DM_CHUNK];
    DmPerformance* p = g_dm.active;
    double         lead;
    int            chunks = 0;

    if (!p || p->closed || !p->perf || g_dm.pumping || ll_host_in_thread())
        return;
    if (!p->port || !p->port->active || ll_audio_state() != 2)
        return;
    g_dm.pumping = 1;

    /* The game's music volume, from the port's DirectSound buffer. */
    if (p->port->dsb) {
        long vol = ll_dsound_buffer_volume(p->port->dsb);
        if (!g_dm.have_volume || vol != g_dm.volume_cb) {
            g_dm.volume_cb = vol;
            g_dm.have_volume = 1;
            ll_audio_music_gain(vol <= -10000 ? 0.0 : pow(10.0, vol / 2000.0));
        }
    }

    lead = ll_audio_music_lead();
    while (lead >= 0.0 && lead < DM_LEAD_S && chunks < 32) {
        int k;
        ll_dmperf_render(p->perf, L, R, DM_CHUNK);
        if (p->closed)
            break;               /* MusicThread shut the music down mid-render */
        for (k = 0; k < DM_CHUNK; k++) {
            L[k] = dm_knee(L[k] * DM_MASTER);
            R[k] = dm_knee(R[k] * DM_MASTER);
        }
        if (!ll_audio_music_push(L, R, DM_CHUNK, p->rate))
            break;
        lead += DM_CHUNK / p->rate;
        chunks++;
    }
    g_dm.pumping = 0;
    if (!p->closed)
        dm_publish(p);
}
