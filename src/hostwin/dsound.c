/* LEGOLAND portable build -- the DirectSound host shim (scopes PORT-B, PORT-B4).
 *
 * WHY THIS IS AN OBJECT AND NOT A FAILURE CODE
 *
 * PORT-B shipped `DirectSoundCreate` returning DSERR_NODRIVER, on the reasoning
 * -- correct as far as it went -- that a failed create leaves `g_dsound` null
 * and `g_samples_ready` 0, and that every later sample entry point is guarded by
 * `g_samples_ready`. What that missed is the ONE caller that is not a sample
 * entry point:
 *
 *   lifecycle.c 0x004964f0 InitSoundSystem
 *       ok = InitSoundSampleSystem(g_snd_hwnd);
 *       if (!ok) return ok;                    <-- bails out HERE
 *       return InitMusicSystem(g_snd_hwnd) != 0;
 *
 *   gamemain.c 0x00459520 RunGame
 *       InitSoundSystem();                     -- result ignored, fine
 *       ...
 *       while (g_music_disabled == 0) { PeekMessageA(..); Sleep(100); }
 *
 * `g_music_disabled` is set by exactly two pieces of code: sysstubs.c's
 * `InitMusicSystem` (0x00495a10) when `g_music_sys` is 0, and musicthread.c's
 * `MusicThread` (0x00492db0) on every one of its failure exits. A sample system
 * that fails therefore makes the -nomusic switch UNREACHABLE: InitSoundSystem
 * returns before InitMusicSystem, nothing ever writes the flag, and RunGame
 * spins on it for ever. That is what the title screen stopped at (PORT-B3).
 *
 * So the sample system has to SUCCEED. InitSoundSampleSystem (audio4.c
 * 0x00492130) needs three calls to work:
 *
 *       if (DirectSoundCreate(0, &g_dsound, 0) != 0) goto fail;
 *       if (g_dsound->lpVtbl->SetCooperativeLevel(g_dsound, hwnd, 1) != 0) goto release;
 *       memset(&g_dsound_caps, 0, sizeof(g_dsound_caps));
 *       g_dsound_caps.size = sizeof(g_dsound_caps);
 *       if (g_dsound->lpVtbl->GetCaps(g_dsound, &g_dsound_caps) != 0) { ... }
 *       g_samples_ready = 1;
 *
 * and `g_samples_ready = 1` then un-guards the whole sample layer, so the object
 * has to be a real -- if silent -- DirectSound, not three stubs.
 *
 * WHAT "REAL" HAS TO MEAN HERE: THE PLAY CURSOR
 *
 * Two loops in the game are driven by the play cursor, and a frozen cursor hangs
 * both of them:
 *
 *   input2.c 0x004963f0 KLIBAUDIO_LockAVISoundBuffer
 *       hr = GetStatus(buf, &status);
 *       if (hr == 0 && (status & DSBSTATUS_PLAYING)) {
 *           hr = GetCurrentPosition(buf, &play, &write);
 *           while (hr == 0 && play >= offset && play < offset + len)
 *               hr = GetCurrentPosition(buf, &play, &write);   <-- spins
 *       }
 *
 *   narration2.c 0x00498b40 PumpNarration
 *       do { ... if (--g_speech_blocks_ready == 0) { stop; return 0; } }
 *       while (!(GetCurrentPosition(..) == 0 && play in the next fill block));
 *
 * The second is worse than it looks: `--g_speech_blocks_ready == 0` is an
 * equality test, so a cursor that never reaches the fill block drives the
 * counter NEGATIVE and the loop never ends.
 *
 * The fix is not to special-case either loop, it is to give every buffer an
 * honest cursor: a buffer that has been Play()ed advances its play position by
 * wall clock times its byte rate, wraps if it was started looping, and stops at
 * the end if it was not. That is what a real sound card does with the speaker
 * unplugged, and it makes both loops terminate for the same reason they
 * terminate on Windows. `ll_host_trace` reports the buffers created and played
 * so a silent run can still be read in the trace.
 *
 * Nothing here produces audio. The PCM the game writes through Lock is kept
 * (so GetCurrentPosition has a length to run against and a future Web Audio /
 * SDL3 backend has the samples to submit) and never played.
 *
 * THE SHAPES. Checked against the declarations in the game's own sources, which
 * are the ABI contract here: IDirectSound's vtable in audio4.c:20 (Release
 * +0x08, CreateSoundBuffer +0x0c, GetCaps +0x10, DuplicateSoundBuffer +0x14,
 * SetCooperativeLevel +0x18) and the full IDirectSoundBuffer in audio3.c:61
 * (QueryInterface +0x00 .. Restore +0x50). Both are the real DirectX 5 layouts.
 * DSBUFFERDESC and WAVEFORMATEX are data2.c:544/554. On wasm32 and i386 every
 * field of those is the same width it was on the original, so they are copied
 * here as declared.
 *
 * THE CALLERS, swept for this lane (every `lpVtbl->` on a sound buffer or on
 * g_dsound in LEGOLAND/*.c), because each one is a slot that must work:
 *   IDirectSound    SetCooperativeLevel audio4.c:166; GetCaps audio4.c:170;
 *                   Release audio4.c:172, lifecycle.c:178; CreateSoundBuffer
 *                   data2.c:674 (sample defs), input2.c:451 (the AVI/speech
 *                   streaming buffer), musicthread.c:540 (the DirectMusic port
 *                   buffer -- unreachable, see the ole32 note below);
 *                   DuplicateSoundBuffer audio3.c:368 (every playable instance
 *                   is a duplicate of its definition's master).
 *   IDSBuffer       Lock data2.c:676 (DSBLOCK_ENTIREBUFFER), input2.c:478,
 *                   narration2.c:646, movie.c:599; Unlock audio2.c:227,
 *                   data2.c:679, narration2.c:657, movie.c:610; Play
 *                   audio2.c:219, input2.c:353/357, sysmisc2.c:91/94; Stop
 *                   audio4.c:125, audiomisc.c:58, narration2.c:250, movie.c:596,
 *                   savemisc2.c:267; SetCurrentPosition audio2.c:216,
 *                   input2.c:350, movie.c:597; GetStatus audio5.c:109,
 *                   input2.c:472, narration2.c:218/300 and AutoKillSamples;
 *                   GetCurrentPosition input2.c:474/476, narration2.c:641/666;
 *                   SetVolume audio3.c:175, audio4.c:222, audio5.c:90,
 *                   audiomisc.c:65, narration2.c:254, sysmisc2.c:219;
 *                   GetVolume audio3.c:313, narration2.c:240; SetPan
 *                   audio3.c:188, sysmisc2.c:221; SetFrequency audio3.c:200;
 *                   GetFrequency audio3.c:215; Release audio3.c:392/460,
 *                   audiomisc.c:51, data2.c:692.
 * Two results are read as numbers rather than as HRESULTs and so are not free
 * to be arbitrary: `Release` is returned through audiomisc.c:51's
 * `Release(buf) == 0`, so it must be the remaining reference count, and
 * AutoKillSamples (narration2.c 0x00496920) reaps a sample when GetStatus
 * reports status 0 -- which is how a finished silent sample gets freed here.
 *
 * ole32: CoInitialize / CoCreateInstance live here too (PORT-B4). They are
 * DirectMusic's door and nothing else in the program uses COM
 * (portable/tools/win32_imports.txt lists exactly these two ole32 imports), so
 * they belong with the audio shim rather than in a file of their own. See the
 * block at the bottom for what the game does with the failure, and
 * docs/lanes/scope-port-b4.md for why MusicThread does not run at all yet.
 *
 * Ownership: PORT-B (docs/SCOPE_PORT_WAVE.md). Declarations: ll_host.h.
 */
#include <stdlib.h>
#include <string.h>

#include "ll_host.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <time.h>
#endif

/* ---- HRESULTs and flags, as the game's call sites use them --------------- */
#define DS_OK                   ((long)0)
#define DSERR_ALLOCATED         ((long)0x8878000a)
#define DSERR_INVALIDPARAM      ((long)0x80070057)
#define DSERR_OUTOFMEMORY       ((long)0x8007000e)
#define E_NOINTERFACE           ((long)0x80004002)
#define REGDB_E_CLASSNOTREG     ((long)0x80040154)

#define DSBCAPS_PRIMARYBUFFER   0x00000001u
#define DSBCAPS_CTRL3D          0x00000010u
#define DSBCAPS_CTRLFREQUENCY   0x00000020u
#define DSBCAPS_CTRLPAN         0x00000040u
#define DSBCAPS_CTRLVOLUME      0x00000080u

#define DSBSTATUS_PLAYING       0x00000001u
#define DSBSTATUS_BUFFERLOST    0x00000002u
#define DSBSTATUS_LOOPING       0x00000004u

#define DSBPLAY_LOOPING         0x00000001u
#define DSBLOCK_FROMWRITECURSOR 0x00000001u
#define DSBLOCK_ENTIREBUFFER    0x00000002u

#define DSBVOLUME_MIN           (-10000L)
#define DSBVOLUME_MAX           0L

/* ---- the structs the game passes in ------------------------------------- */
/* WAVEFORMATEX (data2.c:554). */
typedef struct LLWaveFormat {
    unsigned short wFormatTag;
    unsigned short nChannels;
    unsigned int   nSamplesPerSec;
    unsigned int   nAvgBytesPerSec;
    unsigned short nBlockAlign;
    unsigned short wBitsPerSample;
    unsigned short cbSize;
} LLWaveFormat;

/* DSBUFFERDESC (data2.c:544). musicthread.c's DSBufferDesc is the same first
 * five fields and claims dwSize 0x24 while leaving guid3DAlgorithm
 * uninitialised -- which is why nothing here reads past lpwfxFormat. */
typedef struct LLDSBufferDesc {
    unsigned int  dwSize;
    unsigned int  dwFlags;
    unsigned int  dwBufferBytes;
    unsigned int  dwReserved;
    LLWaveFormat* lpwfxFormat;
    unsigned char guid3DAlgorithm[16];
} LLDSBufferDesc;

/* DSCAPS: audio4.c:18 declares it as `{ unsigned long size; char data[0x5c]; }`
 * -- 0x60 bytes, the DirectX 5 size -- and only ever writes the leading size.
 * The field names are the real ones so the values below can be read. */
typedef struct LLDSCaps {
    unsigned int dwSize;
    unsigned int dwFlags;
    unsigned int dwMinSecondarySampleRate;
    unsigned int dwMaxSecondarySampleRate;
    unsigned int dwPrimaryBuffers;
    unsigned int dwMaxHwMixingAllBuffers;
    unsigned int dwMaxHwMixingStaticBuffers;
    unsigned int dwMaxHwMixingStreamingBuffers;
    unsigned int dwFreeHwMixingAllBuffers;
    unsigned int dwFreeHwMixingStaticBuffers;
    unsigned int dwFreeHwMixingStreamingBuffers;
    unsigned int dwMaxHw3DAllBuffers;
    unsigned int dwMaxHw3DStaticBuffers;
    unsigned int dwMaxHw3DStreamingBuffers;
    unsigned int dwFreeHw3DAllBuffers;
    unsigned int dwFreeHw3DStaticBuffers;
    unsigned int dwFreeHw3DStreamingBuffers;
    unsigned int dwTotalHwMemBytes;
    unsigned int dwFreeHwMemBytes;
    unsigned int dwMaxContigFreeHwMemBytes;
    unsigned int dwUnlockTransferRateHwBuffers;
    unsigned int dwPlayCpuOverheadSwBuffers;
    unsigned int dwReserved1;
    unsigned int dwReserved2;
} LLDSCaps;

/* DSBCAPS: 20 bytes. No call site in the game reads one; filled for form. */
typedef struct LLDSBCaps {
    unsigned int dwSize;
    unsigned int dwFlags;
    unsigned int dwBufferBytes;
    unsigned int dwUnlockTransferRate;
    unsigned int dwPlayCpuOverhead;
} LLDSBCaps;

/* ---- the objects -------------------------------------------------------- */
typedef struct LLDSBuffer LLDSBuffer;
typedef struct LLDSound   LLDSound;

/* The sample memory, shared by a definition's master buffer and every
 * duplicate of it: DuplicateSoundBuffer on Windows shares the sample data, and
 * audio3.c 0x00492370 makes one duplicate per playable INSTANCE, so copying
 * would multiply the game's whole sound set by its polyphony. */
typedef struct LLSoundData {
    int            refs;
    unsigned int   bytes;
    unsigned char* pcm;
    LLWaveFormat   fmt;
    int            have_fmt;
} LLSoundData;

typedef struct LLDSBufferVtbl {
    long          (*QueryInterface)(LLDSBuffer*, const void*, void**);
    unsigned long (*AddRef)(LLDSBuffer*);
    unsigned long (*Release)(LLDSBuffer*);
    long          (*GetCaps)(LLDSBuffer*, void*);
    long          (*GetCurrentPosition)(LLDSBuffer*, unsigned long*, unsigned long*);
    long          (*GetFormat)(LLDSBuffer*, void*, unsigned long, unsigned long*);
    long          (*GetVolume)(LLDSBuffer*, long*);
    long          (*GetPan)(LLDSBuffer*, long*);
    long          (*GetFrequency)(LLDSBuffer*, unsigned long*);
    long          (*GetStatus)(LLDSBuffer*, unsigned long*);
    long          (*Initialize)(LLDSBuffer*, void*, void*);
    long          (*Lock)(LLDSBuffer*, unsigned long, unsigned long, void**,
                          unsigned long*, void**, unsigned long*, unsigned long);
    long          (*Play)(LLDSBuffer*, unsigned long, unsigned long, unsigned long);
    long          (*SetCurrentPosition)(LLDSBuffer*, unsigned long);
    long          (*SetFormat)(LLDSBuffer*, const void*);
    long          (*SetVolume)(LLDSBuffer*, long);
    long          (*SetPan)(LLDSBuffer*, long);
    long          (*SetFrequency)(LLDSBuffer*, unsigned long);
    long          (*Stop)(LLDSBuffer*);
    long          (*Unlock)(LLDSBuffer*, void*, unsigned long, void*, unsigned long);
    long          (*Restore)(LLDSBuffer*);
} LLDSBufferVtbl;

struct LLDSBuffer {
    const LLDSBufferVtbl* lpVtbl;      /* +0x00, as the game expects */
    int           refs;
    LLSoundData*  data;
    unsigned int  flags;               /* the DSBCAPS_* the desc asked for */
    long          volume;              /* hundredths of a dB, 0 .. -10000 */
    long          pan;
    unsigned int  frequency;           /* 0 = the format's own rate */
    int           playing;
    int           looping;
    double        play_t0;             /* wall clock at the last (re)start */
    unsigned int  play_base;           /* cursor byte offset at that moment */
    unsigned int  cursor;              /* the cursor when stopped */
    int           primary;
};

typedef struct LLDSoundVtbl {
    long          (*QueryInterface)(LLDSound*, const void*, void**);
    unsigned long (*AddRef)(LLDSound*);
    unsigned long (*Release)(LLDSound*);
    long          (*CreateSoundBuffer)(LLDSound*, const void*, LLDSBuffer**, void*);
    long          (*GetCaps)(LLDSound*, void*);
    long          (*DuplicateSoundBuffer)(LLDSound*, LLDSBuffer*, LLDSBuffer**);
    long          (*SetCooperativeLevel)(LLDSound*, void*, unsigned long);
    long          (*Compact)(LLDSound*);
    long          (*GetSpeakerConfig)(LLDSound*, unsigned long*);
    long          (*SetSpeakerConfig)(LLDSound*, unsigned long);
    long          (*Initialize)(LLDSound*, const void*);
} LLDSoundVtbl;

struct LLDSound {
    const LLDSoundVtbl* lpVtbl;
    int refs;
    int live_buffers;
};

/* ---- the clock ---------------------------------------------------------- */
/* The same monotonic clock winmm.c and kernel32.c use, so a cursor and
 * timeGetTime cannot disagree about how long a sound has been playing. */
static double ds_now_ms(void)
{
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

/* ---- the simulated play cursor ------------------------------------------ */

/* Bytes per second for this buffer, honouring SetFrequency. Never 0: a 0 rate
 * would freeze the cursor, which is the hang this whole file exists to avoid,
 * so an unknown format falls back to 22050 Hz 16-bit mono (the game's own
 * narration default, audio4.c's g_speech_pcm). */
static unsigned int ds_byte_rate(const LLDSBuffer* b)
{
    const LLWaveFormat* f = &b->data->fmt;
    unsigned int align = 2;
    unsigned int rate;

    if (!b->data->have_fmt)
        return 22050u * 2u;

    if (f->nBlockAlign)
        align = f->nBlockAlign;
    else if (f->nChannels && f->wBitsPerSample)
        align = (unsigned int)f->nChannels * ((unsigned int)f->wBitsPerSample / 8u);

    rate = b->frequency ? b->frequency : f->nSamplesPerSec;
    if (!rate) {
        if (f->nAvgBytesPerSec)
            return f->nAvgBytesPerSec;
        rate = 22050u;
    }
    if (align == 0)
        align = 2;
    return rate * align;
}

/* Advance `cursor` to where wall clock says it is, and clear `playing` when a
 * one-shot has run off the end. Every slot that reports a position or a status
 * calls this first; nothing else moves the cursor. */
static void ds_tick(LLDSBuffer* b)
{
    double elapsed;
    unsigned long long played;
    unsigned int total = b->data->bytes;
    unsigned int pos;

    if (!b->playing || total == 0)
        return;

    elapsed = ds_now_ms() - b->play_t0;
    if (elapsed < 0.0)
        elapsed = 0.0;
    played = (unsigned long long)((elapsed * (double)ds_byte_rate(b)) / 1000.0);

    if (b->looping) {
        pos = (unsigned int)((b->play_base + played % total) % total);
        b->cursor = pos;
        return;
    }
    if (played + b->play_base >= (unsigned long long)total) {
        /* A one-shot that has run out: DirectSound stops the buffer and leaves
         * the play cursor at 0. AutoKillSamples (narration2.c 0x00496920) reaps
         * the instance on the next frame because GetStatus now reports 0. */
        b->playing = 0;
        b->cursor = 0;
        return;
    }
    b->cursor = (unsigned int)(b->play_base + played);
}

/* ---- IDirectSoundBuffer ------------------------------------------------- */

static long LL_DSB_QueryInterface(LLDSBuffer* b, const void* iid, void** out)
{
    (void)b; (void)iid;
    if (out)
        *out = 0;
    return E_NOINTERFACE;   /* no 3D, no notify: nothing in the game asks */
}

static unsigned long LL_DSB_AddRef(LLDSBuffer* b)
{
    return (unsigned long)++b->refs;
}

static unsigned long LL_DSB_Release(LLDSBuffer* b)
{
    int refs = --b->refs;

    if (refs > 0)
        return (unsigned long)refs;

    /* The sample memory outlives the buffer when a duplicate still holds it. */
    if (--b->data->refs <= 0) {
        free(b->data->pcm);
        free(b->data);
    }
    free(b);
    /* audiomisc.c:51 is `return buf->lpVtbl->Release(buf) == 0;`, so the final
     * release must report 0 or DestroyAVISoundBuffer reports failure. */
    return 0;
}

static long LL_DSB_GetCaps(LLDSBuffer* b, void* caps)
{
    LLDSBCaps* c = (LLDSBCaps*)caps;

    if (!c || c->dwSize < sizeof(LLDSBCaps))
        return DSERR_INVALIDPARAM;
    c->dwFlags = b->flags;
    c->dwBufferBytes = b->data->bytes;
    c->dwUnlockTransferRate = 0;
    c->dwPlayCpuOverhead = 0;
    return DS_OK;
}

static long LL_DSB_GetCurrentPosition(LLDSBuffer* b, unsigned long* play,
                                      unsigned long* write)
{
    if (ll_host_beating()) ll_host_beat("dsound.GetCurrentPosition");
    ds_tick(b);
    if (play)
        *play = b->cursor;
    /* The write cursor leads the play cursor by the mixahead on real hardware.
     * No caller reads it (input2.c:474 and narration2.c:641 both discard it),
     * so it is reported equal to the play cursor rather than invented. */
    if (write)
        *write = b->cursor;
    return DS_OK;
}

static long LL_DSB_GetFormat(LLDSBuffer* b, void* fmt, unsigned long size,
                             unsigned long* written)
{
    unsigned long n = (unsigned long)sizeof(LLWaveFormat);

    if (fmt && size) {
        if (n > size)
            n = size;
        memcpy(fmt, &b->data->fmt, n);
    }
    if (written)
        *written = (unsigned long)sizeof(LLWaveFormat);
    return DS_OK;
}

static long LL_DSB_GetVolume(LLDSBuffer* b, long* vol)
{
    if (!vol)
        return DSERR_INVALIDPARAM;
    *vol = b->volume;
    return DS_OK;
}

static long LL_DSB_GetPan(LLDSBuffer* b, long* pan)
{
    if (!pan)
        return DSERR_INVALIDPARAM;
    *pan = b->pan;
    return DS_OK;
}

static long LL_DSB_GetFrequency(LLDSBuffer* b, unsigned long* freq)
{
    if (!freq)
        return DSERR_INVALIDPARAM;
    /* GetSampleFrequency (audio3.c 0x00492a60) returns this to the caller and
     * SetSampleFrequency scales it, so an unset frequency must read back as the
     * format's rate, not as 0. */
    *freq = b->frequency ? b->frequency : b->data->fmt.nSamplesPerSec;
    return DS_OK;
}

static long LL_DSB_GetStatus(LLDSBuffer* b, unsigned long* status)
{
    if (ll_host_beating()) ll_host_beat("dsound.GetStatus");
    if (!status)
        return DSERR_INVALIDPARAM;
    ds_tick(b);
    *status = b->playing
        ? (DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0u))
        : 0u;
    return DS_OK;
}

static long LL_DSB_Initialize(LLDSBuffer* b, void* ds, void* desc)
{
    (void)b; (void)ds; (void)desc;
    return DS_OK;           /* already initialised; nothing calls this */
}

static long LL_DSB_Lock(LLDSBuffer* b, unsigned long offset, unsigned long bytes,
                        void** ptr1, unsigned long* len1,
                        void** ptr2, unsigned long* len2, unsigned long flags)
{
    if (ll_host_beating()) ll_host_beat("dsound.Lock");
    unsigned int total = b->data->bytes;
    unsigned int first;

    if (!ptr1 || !len1)
        return DSERR_INVALIDPARAM;

    /* DSBLOCK_ENTIREBUFFER is how data2.c 0x0048a0a0 uploads a whole sample:
     * `Lock(buf, 0, 0, &ptr1, &bytes1, 0, 0, 2)` and then
     * `memcpy(ptr1, conv, bytes1)`, so bytes1 has to come back as the FULL
     * buffer length or the sample is uploaded short. */
    if (flags & DSBLOCK_ENTIREBUFFER) {
        offset = 0;
        bytes = total;
    } else if (flags & DSBLOCK_FROMWRITECURSOR) {
        ds_tick(b);
        offset = b->cursor;
    }
    if (bytes == 0)
        bytes = total;

    if (total == 0 || offset >= total)
        return DSERR_INVALIDPARAM;
    if (bytes > total)
        bytes = total;

    first = total - offset;
    if (first > bytes)
        first = (unsigned int)bytes;

    *ptr1 = b->data->pcm + offset;
    *len1 = first;

    /* The wrapped remainder. Three of the four call sites pass NULL for the
     * second region (data2.c:676, narration2.c:646, movie.c:599 all do), so it
     * is only reported when asked for -- and the region is then simply not
     * handed out, exactly as DirectSound does with a null pointer pair. */
    if (ptr2 && len2) {
        unsigned int rest = (unsigned int)bytes - first;
        *ptr2 = rest ? b->data->pcm : 0;
        *len2 = rest;
    }
    return DS_OK;
}

static long LL_DSB_Play(LLDSBuffer* b, unsigned long reserved1,
                        unsigned long reserved2, unsigned long flags)
{
    if (ll_host_beating()) ll_host_beat("dsound.Play");
    (void)reserved1; (void)reserved2;

    /* A Play on an already-playing buffer only updates the looping flag on
     * Windows; it does not rewind. SetCurrentPosition is what rewinds, and
     * input2.c:350 / audio2.c:216 / movie.c:597 all call it first. */
    if (!b->playing) {
        b->playing = 1;
        b->play_base = b->cursor;
        b->play_t0 = ds_now_ms();
    }
    b->looping = (flags & DSBPLAY_LOOPING) ? 1 : 0;
    ll_host_trace("DSOUND Play %p %u bytes%s (silent)", (void*)b,
                  b->data->bytes, b->looping ? " looping" : "");
    return DS_OK;
}

static long LL_DSB_SetCurrentPosition(LLDSBuffer* b, unsigned long pos)
{
    unsigned int total = b->data->bytes;

    if (total && pos >= total)
        pos %= total;
    b->cursor = (unsigned int)pos;
    b->play_base = (unsigned int)pos;
    b->play_t0 = ds_now_ms();
    return DS_OK;
}

static long LL_DSB_SetFormat(LLDSBuffer* b, const void* fmt)
{
    /* Only legal on a primary buffer, and the game never creates one. */
    if (fmt && b->primary) {
        memcpy(&b->data->fmt, fmt, sizeof(LLWaveFormat));
        b->data->have_fmt = 1;
    }
    return DS_OK;
}

static long LL_DSB_SetVolume(LLDSBuffer* b, long vol)
{
    if (vol > DSBVOLUME_MAX)
        vol = DSBVOLUME_MAX;
    if (vol < DSBVOLUME_MIN)
        vol = DSBVOLUME_MIN;
    /* FadeSamples (narration2.c 0x004967f0) reads this back every frame and
     * steps it, so it has to be stored, clamped the way DirectSound clamps. */
    b->volume = vol;
    return DS_OK;
}

static long LL_DSB_SetPan(LLDSBuffer* b, long pan)
{
    if (pan > 10000L)
        pan = 10000L;
    if (pan < -10000L)
        pan = -10000L;
    b->pan = pan;
    return DS_OK;
}

static long LL_DSB_SetFrequency(LLDSBuffer* b, unsigned long freq)
{
    /* 0 means "back to the format's rate" in DirectSound. A changed rate
     * changes the byte rate, so the cursor is re-based here: without that the
     * elapsed time already banked would be re-scaled by the new rate. */
    ds_tick(b);
    b->frequency = (unsigned int)freq;
    b->play_base = b->cursor;
    b->play_t0 = ds_now_ms();
    return DS_OK;
}

static long LL_DSB_Stop(LLDSBuffer* b)
{
    /* DirectSound leaves the play cursor where it stopped. */
    ds_tick(b);
    b->playing = 0;
    b->looping = 0;
    return DS_OK;
}

static long LL_DSB_Unlock(LLDSBuffer* b, void* ptr1, unsigned long len1,
                          void* ptr2, unsigned long len2)
{
    (void)b; (void)ptr1; (void)len1; (void)ptr2; (void)len2;
    return DS_OK;           /* the game wrote straight into the PCM block */
}

static long LL_DSB_Restore(LLDSBuffer* b)
{
    (void)b;
    return DS_OK;           /* buffers are never lost here */
}

static const LLDSBufferVtbl ll_dsbuffer_vtbl = {
    LL_DSB_QueryInterface,
    LL_DSB_AddRef,
    LL_DSB_Release,
    LL_DSB_GetCaps,
    LL_DSB_GetCurrentPosition,
    LL_DSB_GetFormat,
    LL_DSB_GetVolume,
    LL_DSB_GetPan,
    LL_DSB_GetFrequency,
    LL_DSB_GetStatus,
    LL_DSB_Initialize,
    LL_DSB_Lock,
    LL_DSB_Play,
    LL_DSB_SetCurrentPosition,
    LL_DSB_SetFormat,
    LL_DSB_SetVolume,
    LL_DSB_SetPan,
    LL_DSB_SetFrequency,
    LL_DSB_Stop,
    LL_DSB_Unlock,
    LL_DSB_Restore
};

/* ---- IDirectSound ------------------------------------------------------- */

static long LL_DS_QueryInterface(LLDSound* ds, const void* iid, void** out)
{
    (void)ds; (void)iid;
    if (out)
        *out = 0;
    return E_NOINTERFACE;
}

static unsigned long LL_DS_AddRef(LLDSound* ds)
{
    return (unsigned long)++ds->refs;
}

static unsigned long LL_DS_Release(LLDSound* ds)
{
    int refs = --ds->refs;

    if (refs > 0)
        return (unsigned long)refs;
    ll_host_trace("DSOUND IDirectSound released (%d buffer(s) outstanding)",
                  ds->live_buffers);
    free(ds);
    return 0;
}

static LLDSBuffer* ds_new_buffer(LLSoundData* data, unsigned int flags, int primary)
{
    LLDSBuffer* b = (LLDSBuffer*)calloc(1, sizeof(LLDSBuffer));

    if (!b)
        return 0;
    b->lpVtbl = &ll_dsbuffer_vtbl;
    b->refs = 1;
    b->data = data;
    b->flags = flags;
    b->volume = DSBVOLUME_MAX;
    b->pan = 0;
    b->frequency = 0;
    b->primary = primary;
    return b;
}

static long LL_DS_CreateSoundBuffer(LLDSound* ds, const void* desc_in,
                                    LLDSBuffer** out, void* outer)
{
    const LLDSBufferDesc* desc = (const LLDSBufferDesc*)desc_in;
    LLSoundData* data;
    LLDSBuffer*  b;
    unsigned int bytes;
    int          primary;

    (void)outer;
    if (!out)
        return DSERR_INVALIDPARAM;
    *out = 0;
    if (!desc)
        return DSERR_INVALIDPARAM;

    primary = (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) ? 1 : 0;
    bytes = desc->dwBufferBytes;
    if (primary) {
        /* A primary buffer's dwBufferBytes must be 0 and the device owns the
         * memory. The game never asks for one (every desc it builds carries
         * CTRLVOLUME at least), but a 0-length buffer would freeze the cursor,
         * so give it one mixer block. */
        bytes = 4096;
    } else if (bytes == 0) {
        return DSERR_INVALIDPARAM;
    }

    data = (LLSoundData*)calloc(1, sizeof(LLSoundData));
    if (!data)
        return DSERR_OUTOFMEMORY;
    data->refs = 1;
    data->bytes = bytes;
    data->pcm = (unsigned char*)calloc(1, bytes);
    if (!data->pcm) {
        free(data);
        return DSERR_OUTOFMEMORY;
    }
    if (desc->lpwfxFormat) {
        memcpy(&data->fmt, desc->lpwfxFormat, sizeof(LLWaveFormat));
        data->have_fmt = 1;
    }

    b = ds_new_buffer(data, desc->dwFlags, primary);
    if (!b) {
        free(data->pcm);
        free(data);
        return DSERR_OUTOFMEMORY;
    }
    ds->live_buffers++;
    ll_host_trace("DSOUND CreateSoundBuffer %u bytes %uHz %uch %ubit flags 0x%x -> %p",
                  bytes, data->fmt.nSamplesPerSec, data->fmt.nChannels,
                  data->fmt.wBitsPerSample, desc->dwFlags, (void*)b);
    *out = b;
    return DS_OK;
}

static long LL_DS_GetCaps(LLDSound* ds, void* caps_in)
{
    LLDSCaps* caps = (LLDSCaps*)caps_in;
    unsigned int size;

    (void)ds;
    if (!caps)
        return DSERR_INVALIDPARAM;
    /* InitSoundSampleSystem memsets the struct and sets dwSize to 0x60 before
     * calling, and treats any non-zero HRESULT as "no sound card" -- so the
     * only way to fail here is to disagree about the size. */
    size = caps->dwSize;
    if (size < sizeof(unsigned int) * 2u)
        return DSERR_INVALIDPARAM;

    memset(caps, 0, size < sizeof(LLDSCaps) ? size : sizeof(LLDSCaps));
    caps->dwSize = size;
    /* PRIMARY16BIT | PRIMARYSTEREO | SECONDARY16BIT | SECONDARYSTEREO |
     * CONTINUOUSRATE: a plain software-mixed 16-bit stereo device. */
    caps->dwFlags = 0x00000020u | 0x00000010u | 0x00000800u | 0x00000400u
                  | 0x00000010u;
    if (size >= sizeof(LLDSCaps)) {
        caps->dwMinSecondarySampleRate = 100u;
        caps->dwMaxSecondarySampleRate = 100000u;
        caps->dwPrimaryBuffers = 1u;
        caps->dwTotalHwMemBytes = 0u;
        caps->dwFreeHwMemBytes = 0u;
    }
    return DS_OK;
}

static long LL_DS_DuplicateSoundBuffer(LLDSound* ds, LLDSBuffer* orig,
                                       LLDSBuffer** out)
{
    LLDSBuffer* b;

    if (!out)
        return DSERR_INVALIDPARAM;
    *out = 0;
    if (!orig || orig->lpVtbl != &ll_dsbuffer_vtbl)
        return DSERR_INVALIDPARAM;

    /* StartPlayableSample (audio3.c 0x00492370) duplicates a definition's
     * master for every instance, so the PCM is SHARED and the duplicate starts
     * stopped at 0 with the original's volume and pan -- which is what
     * DirectSound gives it. */
    orig->data->refs++;
    b = ds_new_buffer(orig->data, orig->flags, 0);
    if (!b) {
        orig->data->refs--;
        return DSERR_OUTOFMEMORY;
    }
    b->volume = orig->volume;
    b->pan = orig->pan;
    b->frequency = orig->frequency;
    ds->live_buffers++;
    *out = b;
    return DS_OK;
}

static long LL_DS_SetCooperativeLevel(LLDSound* ds, void* hwnd, unsigned long level)
{
    (void)ds;
    ll_host_trace("DSOUND SetCooperativeLevel hwnd %p level %lu", hwnd, level);
    return DS_OK;
}

static long LL_DS_Compact(LLDSound* ds)              { (void)ds; return DS_OK; }

static long LL_DS_GetSpeakerConfig(LLDSound* ds, unsigned long* cfg)
{
    (void)ds;
    if (!cfg)
        return DSERR_INVALIDPARAM;
    *cfg = 1u;              /* DSSPEAKER_HEADPHONE */
    return DS_OK;
}

static long LL_DS_SetSpeakerConfig(LLDSound* ds, unsigned long cfg)
{
    (void)ds; (void)cfg;
    return DS_OK;
}

static long LL_DS_Initialize(LLDSound* ds, const void* guid)
{
    (void)ds; (void)guid;
    return DS_OK;
}

static const LLDSoundVtbl ll_dsound_vtbl = {
    LL_DS_QueryInterface,
    LL_DS_AddRef,
    LL_DS_Release,
    LL_DS_CreateSoundBuffer,
    LL_DS_GetCaps,
    LL_DS_DuplicateSoundBuffer,
    LL_DS_SetCooperativeLevel,
    LL_DS_Compact,
    LL_DS_GetSpeakerConfig,
    LL_DS_SetSpeakerConfig,
    LL_DS_Initialize
};

/* ---- the entry point --------------------------------------------------- */

long DirectSoundCreate(void* guid, void** out, void* outer)
{
    LLDSound* ds;

    (void)guid; (void)outer;
    if (!out)
        return DSERR_INVALIDPARAM;
    *out = 0;

    ds = (LLDSound*)calloc(1, sizeof(LLDSound));
    if (!ds)
        return DSERR_OUTOFMEMORY;
    ds->lpVtbl = &ll_dsound_vtbl;
    ds->refs = 1;
    ll_host_trace("DirectSoundCreate -> %p (silent device, simulated cursor)",
                  (void*)ds);
    *out = ds;
    return DS_OK;
}

/* ======================================================================== */
/* ole32 -- DirectMusic's door, and why it stays shut                        */
/* ======================================================================== */
/*
 * These are the program's only two COM imports (win32_imports.txt), and the
 * only caller of either is MusicThread (musicthread.c 0x00492db0):
 *
 *       CoInitialize(0);
 *       if (CoCreateInstance(&CLSID_DirectMusicComposer, 0, 3,
 *                            &IID_IDirectMusicComposer, &g_dm_composer) < 0)
 *           goto shutdown;
 *       ...
 *   shutdown:
 *       g_music_ready = 0;
 *       g_music_disabled = 1;
 *       return 0;
 *
 * So a failed CoCreateInstance is the designed "no DirectMusic" path: the
 * thread takes the FIRST rung of the failure ladder, which releases nothing
 * because nothing has been created yet, sets the two flags and returns. That is
 * also what releases RunGame's music wait, which is why failing cleanly here is
 * what the music-on command line needs -- there is nothing to document as
 * missing on the game's side.
 *
 * CoInitialize reports success because its result is discarded at the one call
 * site, and because reporting failure would be a lie about a call that cannot
 * fail: there is no apartment to fail to enter. CoCreateInstance reports
 * REGDB_E_CLASSNOTREG, the HRESULT Windows itself returns for a CLSID with no
 * registered server, which is exactly the truth about this host.
 *
 * CAVEAT, and it is the one thing to know about the music path: MusicThread
 * never runs at all in this port, so neither of these is reached today.
 * kernel32.c's CreateThread refuses (the port is single-threaded), sysstubs.c's
 * InitMusicSystem (0x00495a10) reports success anyway on a null thread handle,
 * and nothing then writes g_music_disabled -- so without -nomusic RunGame waits
 * for ever. The fix is one line in CreateThread (PORT-A's file) and is measured
 * in docs/lanes/scope-port-b4.md §3.
 */
long CoInitialize(void* reserved)
{
    (void)reserved;
    ll_host_trace("CoInitialize: no COM runtime; the result is discarded");
    return DS_OK;
}

long CoCreateInstance(const void* clsid, void* outer, unsigned long context,
                      const void* iid, void** out)
{
    (void)clsid; (void)outer; (void)context; (void)iid;
    if (out)
        *out = 0;
    ll_host_trace("CoCreateInstance: REGDB_E_CLASSNOTREG (no DirectMusic)");
    return REGDB_E_CLASSNOTREG;
}
