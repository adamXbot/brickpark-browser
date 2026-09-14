/* LEGOLAND portable build -- the AVIFIL32 host shim (scope PORT-B): Video for
 * Windows' AVIFile API. The FMV reports "this file will not open", honestly;
 * the six ADVISOR clips play, from frames decoded at build time (PARK-2).
 *
 * WHAT THIS IS AND IS NOT.  The game's FMV and its on-screen advisor are both
 * played through AVIFile (movie.c, movie2.c, advisor.c, screens3.c), and the
 * clips are Indeo 5 (Ir50_32.dll -- RunGame even LoadLibraryA's it,
 * gamemain.c:398).  There is no Indeo 5 decoder in this port.  So this file is
 * not a decoder: it is the sixteen entry points the import table names
 * (portable/tools/win32_imports.txt), answering for two different kinds of
 * file, told apart by whether portable/tools/advisor_frames.py left decoded
 * frames for it in the working directory's advisor/ (browser.cmake runs it over
 * the user's gamedata with ffmpeg and preloads the result at /gamedata/advisor).
 *
 *     Before PORT-B6:  TRAP AVIFIL32.dll AVIFileInit  from advisor.c, movie.c
 *                      TRAP AVIFIL32.dll AVIFileOpenA ...   (six of them)
 *     PORT-B6:         every open fails; the page runs with no traps at all.
 *     PARK-2:          the advisor clips open and play; the FMV still fails.
 *
 * THE FMV: AVIFileOpenA FAILS.  Intro.avi, lmi.avi, Billund/California/
 * Windsor.avi have no frames file, and for them the failure stands.  It was
 * chosen over a synthetic zero-length stream after reading both callers; the
 * failure is the path the game is BUILT for, the synthetic stream a path it has
 * never taken:
 *
 *   movie.c 0x00476460 OpenMovie
 *       if (AVIFileOpenA(&pfile, path, 0, 0) != 0) {
 *           if (g_avi_open_count == 0) AVIFileExit();
 *           return 0;                        <-- the whole of it
 *       }
 *     and uimisc2.c 0x004771f0 PlayMovie, its only caller, answers a null
 *     movie by trying the second prefix (g_res_path) and then `return 1` --
 *     WITHOUT entering the player: no PauseAllSamples, no StopMusic, no
 *     PushRenderingStatusAndUnlockVideoSurface, no RunMovie, no CloseMovie.
 *     So the intro, lmi.avi, the three park movies (uimisc3.c), the level-end
 *     movie (gameframe.c:517) and the report movie (uimisc.c:734) each cost one
 *     DebugPrintf and nothing else.  A SYNTHETIC stream would instead take the
 *     game into RunMovie with frames == 0, which unlocks and relocks the video
 *     surface around a loop that never runs, ducks and restores the whole audio
 *     stack, and spins until all three mouse buttons are up -- a great deal of
 *     machinery to arrive at the same blank screen.  Real FMV would need its
 *     audio half too (movie2.c, and msacm32.c -- see the header of that file).
 *
 * THE ADVISOR: THE CLIPS OPEN, BECAUSE THE WINDOW IS A HOLE (PARK-2).  The
 * in-game panel's 112x96 advisor window at game (522, 378) is left TRANSPARENT
 * by InterfaceBG.lls, and the only thing that ever paints it is
 *
 *       screens3.c 0x00443e30 RenderAdvisorIcon
 *           if (!ScriptRunning()) {
 *               if (g_vidanim) {              /. 0x00665f5c == advisor.c's
 *                   dib = AVIStreamGetFrame(g_vidanim->pgf, g_vid_frame);
 *                   BltAdvisor(dib, p->x, p->y);   g_advisor_clip ./
 *                   if (<cursor inside dib->width x dib->height>)
 *                       g_hit_info = ctx;
 *
 *     every frame.  With every open failing, g_vidanim was null for the whole
 *     run, so nothing repainted the window: the cursor FlipPrimary stamps into
 *     the back surface and any bubble help drawn over the window stayed there
 *     for the rest of the session -- the red "?" query pointer smeared into a
 *     trail and the bubble boxes stacked up.  The same dead branch holds the
 *     hand-written hit test (BltAdvisor bypasses PrintSprite, which is where
 *     every other icon's ownership comes from), so the window's pixels were
 *     also read as map squares.  docs/lanes/scope-port-b11.md §4 has the whole
 *     trace; one cause, both symptoms.
 *
 *     So an advisor clip opens as a one-stream video file whose frames are
 *     already decoded, and the game's own advisor code -- the pose machine,
 *     the end-of-clip callback, the blit and the hit test -- runs as shipped.
 *     The frames are exactly the DIB the real AVIStreamGetFrame returns for the
 *     format advisor.c's InitAdvisorBmi asks for (0x004b7d70: 112 x 96, 16 bpp,
 *     BI_RGB): a BITMAPINFOHEADER, then X1R5G5B5 rows bottom-up at +0x28, which
 *     is what BltAdvisor (blitmisc.c) reads, widening 555 to 565 itself.
 *
 *     Without the frames (no ffmpeg at build time, the node targets, CI) an
 *     advisor clip fails like the FMV and the window is a hole again.
 *
 * NOTHING UNKNOWN IS EVER FOLLOWED.  A PAVIFILE, PAVISTREAM or PGETFRAME is
 * dereferenced only after a walk of this file's own registry has found it,
 * and an out parameter is written only by a call that succeeds.  That is not
 * tidiness: the game reaches these functions with garbage.
 *
 *   advisor.c 0x00443dc0 SetVidAnim(clip) reads `clip->stop` with no null test
 *   (a shipped bug, scope PORT-M6).  `InitAdvisorMovies` ends with
 *   `SetVidAnim(g_ad_blink)`, and g_ad_blink is 0 whenever AD_Blink.avi did
 *   not open.  Before the portable arm hoisted the test, clang used that read
 *   to prove clip != null and called AVIStreamGetFrameOpen with whatever word
 *   sits at address 0x1c -- the observed behaviour of the -O2 browser build.
 *
 *   movie.c OpenMovie leaves `pfile` UNINITIALISED when AVIFileOpenA fails (it
 *   only writes *ppfile on success, as the real AVIFile does), and on the
 *   no-video path calls `AVIFileRelease(pfile)` with that garbage.
 *
 * RESULT CODES.  MAKE_AVIERR(n) is MAKE_SCODE(SEVERITY_ERROR, FACILITY_ITF,
 * 0x4000 + n), i.e. 0x8004_4000 + n, so AVIERR_FILEOPEN (111) is 0x8004406F and
 * AVIERR_BADHANDLE (108) is 0x8004406C.  AVIFileInit/AVIFileExit and the
 * AddRef/Release family return counts, not HRESULTs, and the game reads
 * AVIFileRelease's and AVIStreamRelease's results nowhere (unlike the
 * DirectSound buffer Release, which audiomisc.c:51 compares to 0 -- see
 * dsound.c).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ll_host.h"

/* MAKE_AVIERR(n) == 0x80044000 + n. */
#define AVIERR_BADSIZE     0x8004406Bl   /* MAKE_AVIERR(107) */
#define AVIERR_BADHANDLE   0x8004406Cl   /* MAKE_AVIERR(108) */
#define AVIERR_FILEOPEN    0x8004406Fl   /* MAKE_AVIERR(111) */
#define AVIERR_NODATA      0x80044073l   /* MAKE_AVIERR(115) */

#define OF_WRITE           0x0001u
#define OF_READWRITE       0x0002u
#define OF_CREATE          0x1000u

#define streamtypeVIDEO    0x73646976ul  /* 'vids', advisor.c's si.fccType test */
#define FCC_IV50           0x30355649ul  /* 'IV50', what the clips carry */
#define AVIGETFRAMEF_BESTDISPLAYFMT ((const void*)1)

/* ---- the three records this file fills --------------------------------------
 * In the ILP32 layout advisor.c and movie.c declare them in (every `long` is 4
 * bytes there). Fixed-width members, so the native 64-bit compile of this file
 * lays them out the same way. */
typedef struct LLAviFileInfo {          /* AVIFILEINFOA */
    uint32_t dwMaxBytesPerSec;          /* +0x00 */
    uint32_t dwFlags;                   /* +0x04 */
    uint32_t dwCaps;                    /* +0x08 */
    int32_t  dwStreams;                 /* +0x0c  the one field advisor.c reads */
    uint32_t dwSuggestedBufferSize;     /* +0x10 */
    uint32_t dwWidth;                   /* +0x14 */
    uint32_t dwHeight;                  /* +0x18 */
    uint32_t dwScale;                   /* +0x1c */
    uint32_t dwRate;                    /* +0x20 */
    uint32_t dwLength;                  /* +0x24 */
    uint32_t dwEditCount;               /* +0x28 */
    char     szFileType[64];            /* +0x2c */
} LLAviFileInfo;

typedef struct LLAviStreamInfo {        /* AVISTREAMINFOA */
    uint32_t fccType;                   /* +0x00 */
    uint32_t fccHandler;                /* +0x04 */
    uint32_t dwFlags;                   /* +0x08 */
    uint32_t dwCaps;                    /* +0x0c */
    uint16_t wPriority;                 /* +0x10 */
    uint16_t wLanguage;                 /* +0x12 */
    uint32_t dwScale;                   /* +0x14 */
    uint32_t dwRate;                    /* +0x18  fps = dwRate / dwScale */
    uint32_t dwStart;                   /* +0x1c */
    uint32_t dwLength;                  /* +0x20  frames */
    uint32_t dwInitialFrames;           /* +0x24 */
    uint32_t dwSuggestedBufferSize;     /* +0x28 */
    uint32_t dwQuality;                 /* +0x2c */
    uint32_t dwSampleSize;              /* +0x30 */
    int32_t  rcFrame[4];                /* +0x34  left, top, right, bottom */
    uint32_t dwEditCount;               /* +0x44 */
    uint32_t dwFormatChangeCount;       /* +0x48 */
    char     szName[64];                /* +0x4c */
} LLAviStreamInfo;

typedef struct LLBitmapInfoHeader {     /* BITMAPINFOHEADER */
    uint32_t biSize;                    /* +0x00 */
    int32_t  biWidth;                   /* +0x04 */
    int32_t  biHeight;                  /* +0x08  positive: rows bottom-up */
    uint16_t biPlanes;                  /* +0x0c */
    uint16_t biBitCount;                /* +0x0e */
    uint32_t biCompression;             /* +0x10  BI_RGB == 0 */
    uint32_t biSizeImage;               /* +0x14 */
    int32_t  biXPelsPerMeter;           /* +0x18 */
    int32_t  biYPelsPerMeter;           /* +0x1c */
    uint32_t biClrUsed;                 /* +0x20 */
    uint32_t biClrImportant;            /* +0x24 */
} LLBitmapInfoHeader;

_Static_assert(sizeof(LLAviFileInfo) == 0x6c, "AVIFILEINFOA is 0x6c bytes");
_Static_assert(sizeof(LLAviStreamInfo) == 0x8c, "AVISTREAMINFOA is 0x8c bytes");
_Static_assert(sizeof(LLBitmapInfoHeader) == 0x28, "BITMAPINFOHEADER is 0x28 bytes");

/* ---- the advisor clips ------------------------------------------------------
 *
 * One record per clip, keyed by the lower-cased file stem. A PAVIFILE is the
 * address of `file` and a PAVISTREAM the address of `stream`, so a handle's
 * kind is which member it points at.
 *
 * Records are never freed. KillAdvisorMovies (advisor.c) releases each clip's
 * STREAM but never its FILE, and the reference AVIFileGetStream hands out is
 * never released at all -- so no count ever reaches zero on the game's own
 * books, and a record freed on one would be one the game still holds. Six
 * clips of 64 frames are 8 MB for the life of the page, and a second open of
 * the same name finds the loaded record instead of reading it again. */
#define LLV_MAGIC        "LLV1"
#define LLV_HEADER_SIZE  0x14
#define LLV_MAX_FRAMES   100000u

typedef struct LLVClip LLVClip;

typedef struct LLVRef {
    LLVClip* clip;
    int      refs;
} LLVRef;

struct LLVClip {
    LLVClip*       next;
    char           stem[64];
    int            width, height;
    uint32_t       frames, rate, scale;
    unsigned char* pixels;              /* frames x width x height x 2 */
    LLVRef         file;
    LLVRef         stream;
};

/* A PGETFRAME: the DIB AVIStreamGetFrame returns, valid until the next
 * GetFrame on the same handle or its GetFrameClose -- the real API's rule. */
typedef struct LLVGetFrame {
    struct LLVGetFrame* next;
    LLVClip*            clip;
    long                shown;          /* frame the DIB holds, -1 before the first */
    unsigned char*      dib;            /* BITMAPINFOHEADER + width x height x 2 */
} LLVGetFrame;

static LLVClip*     g_llv_clips;
static LLVGetFrame* g_llv_getframes;

static LLVClip* llv_file_of(const void* p)
{
    LLVClip* c;
    for (c = g_llv_clips; c; c = c->next)
        if (p == (const void*)&c->file)
            return c;
    return 0;
}

static LLVClip* llv_stream_of(const void* p)
{
    LLVClip* c;
    for (c = g_llv_clips; c; c = c->next)
        if (p == (const void*)&c->stream)
            return c;
    return 0;
}

static LLVGetFrame* llv_getframe_of(const void* p)
{
    LLVGetFrame* g;
    for (g = g_llv_getframes; g; g = g->next)
        if (p == (const void*)g)
            return g;
    return 0;
}

static size_t llv_frame_bytes(const LLVClip* c)
{
    return (size_t)c->width * (size_t)c->height * 2;
}

/* "AD_Blink.avi", ".\AD_Blink.avi", "D:\LEGOLAND\AD_Blink.avi" -> "ad_blink". */
static int llv_stem(const char* name, char* out, size_t cap)
{
    const char* base = name;
    const char* dot = 0;
    const char* p;
    size_t n = 0;

    for (p = name; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            base = p + 1;
    for (p = base; *p; p++)
        if (*p == '.')
            dot = p;
    if (!dot)
        dot = p;
    for (p = base; p < dot; p++) {
        if (n + 1 >= cap)
            return 0;
        out[n++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p - 'A' + 'a') : *p;
    }
    out[n] = 0;
    return n > 0;
}

static uint32_t llv_u16(const unsigned char* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t llv_u32(const unsigned char* p) { return llv_u16(p) | llv_u16(p + 2) << 16; }

/* advisor/<stem>.llv, relative to the working directory (the page chdir's to
 * /gamedata). A file that is missing, short or implausible is a failed open,
 * never a half-loaded clip: RenderAdvisorIcon blits whatever GetFrame returns
 * without a null test, so a clip that opens has to be able to serve every
 * frame it claims. */
static LLVClip* llv_load(const char* stem)
{
    char           path[96];
    unsigned char  hdr[LLV_HEADER_SIZE];
    FILE*          f;
    LLVClip*       c = 0;
    uint32_t       width, height, frames, rate, scale;
    size_t         bytes;

    snprintf(path, sizeof path, "advisor/%s.llv", stem);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, LLV_MAGIC, 4) != 0)
        goto bad;
    width = llv_u16(hdr + 0x04);
    height = llv_u16(hdr + 0x06);
    frames = llv_u32(hdr + 0x08);
    rate = llv_u32(hdr + 0x0c);
    scale = llv_u32(hdr + 0x10);
    if (!width || !height || !frames || frames > LLV_MAX_FRAMES || !rate || !scale)
        goto bad;

    c = (LLVClip*)calloc(1, sizeof *c);
    if (!c)
        goto bad;
    snprintf(c->stem, sizeof c->stem, "%s", stem);
    c->width = (int)width;
    c->height = (int)height;
    c->frames = frames;
    c->rate = rate;
    c->scale = scale;
    bytes = llv_frame_bytes(c) * frames;
    c->pixels = (unsigned char*)malloc(bytes);
    if (!c->pixels || fread(c->pixels, 1, bytes, f) != bytes)
        goto bad;
    fclose(f);

    c->file.clip = c;
    c->stream.clip = c;
    c->next = g_llv_clips;
    g_llv_clips = c;
    return c;

bad:
    ll_host_trace("AVIFileOpenA: %s is not a usable advisor frames file", path);
    if (c)
        free(c->pixels);
    free(c);
    fclose(f);
    return 0;
}

/* ---- the library bracket ----------------------------------------------------
 * AVIFileInit/AVIFileExit bracket the library. The game reference-counts them
 * itself (movie.c:225's g_avi_open_count, advisor.c's own tally) and calls Init
 * only on the 0 -> 1 edge. Counted here too, and traced once. */
static int avi_init_depth;
static int avi_init_total;

void AVIFileInit(void)
{
    if (avi_init_depth++ == 0 && avi_init_total++ == 0)
        ll_host_trace("AVIFileInit: advisor clips play from advisor/*.llv when"
                      " present; every other open reports AVIERR_FILEOPEN");
}

void AVIFileExit(void)
{
    if (avi_init_depth > 0)
        avi_init_depth--;
}

/* The one that matters. *ppfile is written only on success: a real AVIFile
 * leaves it alone on failure, and both callers test the HRESULT before reading
 * it (movie.c:648, advisor.c:164). */
long AVIFileOpenA(void** ppfile, const char* name, unsigned int mode,
                  const void* handler)
{
    char     stem[64];
    LLVClip* c = 0;

    (void)handler;
    if (ppfile && name && !(mode & (OF_WRITE | OF_READWRITE | OF_CREATE))
        && llv_stem(name, stem, sizeof stem)) {
        for (c = g_llv_clips; c; c = c->next)
            if (strcmp(c->stem, stem) == 0)
                break;
        if (!c)
            c = llv_load(stem);
    }
    if (!c) {
        ll_host_trace("AVIFileOpenA(\"%s\"): AVIERR_FILEOPEN (no AVI decoder)",
                      name ? name : "(null)");
        return AVIERR_FILEOPEN;
    }
    c->file.refs++;
    *ppfile = &c->file;
    ll_host_trace("AVIFileOpenA(\"%s\"): advisor/%s.llv, %dx%d, %u frames at %u/%u",
                  name, c->stem, c->width, c->height,
                  (unsigned)c->frames, (unsigned)c->rate, (unsigned)c->scale);
    return 0;
}

/* LoadAdvisorMovie pre-clears the one field it reads (`fi.dwStreams = 0;`,
 * advisor.c) and checks no result, so an untouched buffer reads as "no
 * streams" -- which is what every handle this file did not open gets. */
long AVIFileInfoA(void* pfile, void* pfi, long size)
{
    LLVClip*      c = llv_file_of(pfile);
    LLAviFileInfo fi;

    if (!c)
        return AVIERR_BADHANDLE;
    if (!pfi || size < 0)
        return AVIERR_BADSIZE;
    memset(&fi, 0, sizeof fi);
    fi.dwMaxBytesPerSec = (uint32_t)(llv_frame_bytes(c) * c->rate / c->scale);
    fi.dwCaps = 0x1;                          /* AVIFILECAPS_CANREAD */
    fi.dwStreams = 1;
    fi.dwSuggestedBufferSize = (uint32_t)llv_frame_bytes(c);
    fi.dwWidth = (uint32_t)c->width;
    fi.dwHeight = (uint32_t)c->height;
    fi.dwScale = c->scale;
    fi.dwRate = c->rate;
    fi.dwLength = c->frames;
    snprintf(fi.szFileType, sizeof fi.szFileType, "LEGOLAND advisor frames");
    memcpy(pfi, &fi, (size_t)size < sizeof fi ? (size_t)size : sizeof fi);
    return 0;
}

/* Stream 0 is the video; there is no other. */
long AVIFileGetStream(void* pfile, void** ppstream, unsigned long fcc,
                      long lParam)
{
    LLVClip* c = llv_file_of(pfile);

    if (!c)
        return AVIERR_BADHANDLE;
    if (!ppstream)
        return AVIERR_BADSIZE;
    if (lParam != 0 || (fcc != 0 && fcc != streamtypeVIDEO)) {
        *ppstream = 0;
        return AVIERR_NODATA;
    }
    c->stream.refs++;
    *ppstream = &c->stream;
    return 0;
}

/* A reference count, not an HRESULT. Nothing in the game reads it, and the
 * record outlives zero -- see the registry's comment. */
unsigned long AVIFileRelease(void* pfile)
{
    LLVClip* c = llv_file_of(pfile);

    if (!c)
        return 0;
    if (c->file.refs > 0)
        c->file.refs--;
    return (unsigned long)c->file.refs;
}

/* LoadAdvisorMovie takes a stream whose fccType is 'vids' and reads dwLength
 * (frames), dwRate / dwScale (fps) and rcFrame (the size). */
long AVIStreamInfoA(void* pavi, void* psi, long size)
{
    LLVClip*        c = llv_stream_of(pavi);
    LLAviStreamInfo si;

    if (!c)
        return AVIERR_BADHANDLE;
    if (!psi || size < 0)
        return AVIERR_BADSIZE;
    memset(&si, 0, sizeof si);
    si.fccType = streamtypeVIDEO;
    si.fccHandler = FCC_IV50;
    si.dwScale = c->scale;
    si.dwRate = c->rate;
    si.dwLength = c->frames;
    si.dwSuggestedBufferSize = (uint32_t)llv_frame_bytes(c);
    si.dwQuality = 0xffffffffu;               /* "default quality" */
    si.rcFrame[2] = c->width;
    si.rcFrame[3] = c->height;
    snprintf(si.szName, sizeof si.szName, "%s video", c->stem);
    memcpy(psi, &si, (size_t)size < sizeof si ? (size_t)size : sizeof si);
    return 0;
}

unsigned long AVIStreamAddRef(void* pavi)
{
    LLVClip* c = llv_stream_of(pavi);

    return c ? (unsigned long)++c->stream.refs : 1;
}

unsigned long AVIStreamRelease(void* pavi)
{
    LLVClip* c = llv_stream_of(pavi);

    if (!c)
        return 0;
    if (c->stream.refs > 0)
        c->stream.refs--;
    return (unsigned long)c->stream.refs;
}

/* A PGETFRAME for the format the caller wants, or null -- what a real
 * AVIStreamGetFrameOpen returns when no decompressor can produce that format.
 * The frames are 16-bpp BI_RGB bottom-up at the clip's own size, so that is the
 * only format on offer; SetVidAnim asks for exactly it (g_advisor_bmi, 112 x 96
 * x 16). `wanted` is read only once `pavi` has been found in the registry:
 * the stray call SetVidAnim once made with a garbage stream came with a real
 * `wanted`, but nothing guarantees the converse. */
void* AVIStreamGetFrameOpen(void* pavi, const void* wanted)
{
    LLVClip*           c = llv_stream_of(pavi);
    LLVGetFrame*       g;
    LLBitmapInfoHeader bi;

    if (!c)
        return 0;
    if (wanted && wanted != AVIGETFRAMEF_BESTDISPLAYFMT) {
        memcpy(&bi, wanted, sizeof bi);
        if (bi.biBitCount != 16 || bi.biCompression != 0
            || (bi.biWidth != 0 && bi.biWidth != c->width)
            || (bi.biHeight != 0 && bi.biHeight != c->height)) {
            ll_host_trace("AVIStreamGetFrameOpen(%s): no conversion to %dx%d x %u"
                          " compression %u", c->stem, (int)bi.biWidth,
                          (int)bi.biHeight, (unsigned)bi.biBitCount,
                          (unsigned)bi.biCompression);
            return 0;
        }
    }

    g = (LLVGetFrame*)calloc(1, sizeof *g);
    if (!g)
        return 0;
    g->dib = (unsigned char*)calloc(1, sizeof bi + llv_frame_bytes(c));
    if (!g->dib) {
        free(g);
        return 0;
    }
    memset(&bi, 0, sizeof bi);
    bi.biSize = sizeof bi;
    bi.biWidth = c->width;
    bi.biHeight = c->height;
    bi.biPlanes = 1;
    bi.biBitCount = 16;
    bi.biSizeImage = (uint32_t)llv_frame_bytes(c);
    memcpy(g->dib, &bi, sizeof bi);
    g->clip = c;
    g->shown = -1;
    g->next = g_llv_getframes;
    g_llv_getframes = g;
    return g;
}

/* RenderAdvisorIcon counts g_vid_frame up to the clip's dwLength and restarts
 * it through SetVidAnim, so `pos` is in range; it is clamped anyway, because
 * the blit that follows takes the DIB on trust. movie.c:776 treats a null
 * return as "the decompressor gave up" -- only a handle this file never
 * issued gets one. */
void* AVIStreamGetFrame(void* pgf, long pos)
{
    LLVGetFrame* g = llv_getframe_of(pgf);
    LLVClip*     c;
    size_t       n;

    if (!g)
        return 0;
    c = g->clip;
    if (pos < 0)
        pos = 0;
    if ((unsigned long)pos >= c->frames)
        pos = (long)c->frames - 1;
    if (pos != g->shown) {
        n = llv_frame_bytes(c);
        memcpy(g->dib + sizeof(LLBitmapInfoHeader), c->pixels + (size_t)pos * n, n);
        g->shown = pos;
    }
    return g->dib;
}

/* SetVidAnim closes the outgoing clip's PGETFRAME before opening the next one,
 * and FreeAdvisorClip closes it once more at teardown only `if
 * (clip->getframe)`, so a close of a handle not in the registry is not an
 * error worth reporting -- there is simply nothing to free. */
long AVIStreamGetFrameClose(void* pgf)
{
    LLVGetFrame** link = &g_llv_getframes;

    while (*link && (void*)*link != pgf)
        link = &(*link)->next;
    if (*link) {
        LLVGetFrame* g = *link;
        *link = g->next;
        free(g->dib);
        free(g);
    }
    return 0;
}

/* ---- the audio side of a stream (movie2.c) -------------------------------
 *
 * All four are behind `mv->audio`, which only StartMovieAudio reads and which
 * OpenMovie only ever sets from a stream walk that cannot happen: the FMV never
 * opens, and an advisor clip is video only (LoadAdvisorMovie takes no audio
 * stream either). Kept real (not traps) because the import table names them
 * and because a zero-length, zero-start stream is the consistent answer to go
 * with a failed open: StartMovieAudio's `end = AVIStreamStart(mv->audio) +
 * AVIStreamLength(...)` (movie2.c:268) is then 0, and its caller plays nothing. */
long AVIStreamStart(void* pavi)
{
    (void)pavi;
    return 0;
}

long AVIStreamLength(void* pavi)
{
    (void)pavi;
    return 0;
}

long AVIStreamRead(void* pavi, long start, long samples, void* buf,
                   long buflen, long* bytes, long* nsamples)
{
    (void)pavi;
    (void)start;
    (void)samples;
    (void)buf;
    (void)buflen;
    /* The two out counts ARE written when the pointers are non-null: movie2.c
     * hands them stack addresses, and "0 bytes, 0 samples" is what says the
     * read produced nothing. */
    if (bytes)
        *bytes = 0;
    if (nsamples)
        *nsamples = 0;
    return AVIERR_NODATA;
}

/* movie2.c:312 calls this with fmt == 0 to SIZE the format block, then again
 * with a buffer. `*size = 0` on the sizing pass is what makes the second call
 * ask for nothing; the caller checks the HRESULT of neither, but it does test
 * the wave format tag afterwards, which an untouched buffer leaves as whatever
 * the record already held -- all zero in the image. */
long AVIStreamReadFormat(void* pavi, long pos, void* fmt, long* size)
{
    (void)pavi;
    (void)pos;
    (void)fmt;
    if (size)
        *size = 0;
    return AVIERR_BADHANDLE;
}
