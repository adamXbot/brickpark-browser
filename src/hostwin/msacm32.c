/* LEGOLAND portable build -- the MSACM32 host shim (scope PORT-B): the Audio
 * Compression Manager, as much of it as the game actually needs.
 *
 * WHY THIS IS NOT A STUB.  Six MSACM32 imports (portable/tools/win32_imports.txt)
 * and three callers, and one of the three is on the path of EVERY sound the
 * game loads:
 *
 *   resaudio2.c 0x004921c0 ConvertWAVToPCM -- called UNCONDITIONALLY by
 *     data2.c 0x00492380 CreateSampleFromWAV (data2.c:664) for every RIFF/WAVE
 *     resource in the archives, whatever its format tag, and
 *     `if (!conv) goto free_data;` two lines later. A shim that refuses
 *     everything therefore makes CreateSampleFromWAV return 0 for every sample
 *     in the game -- no Sample records, no DirectSound buffers, nothing for the
 *     sample layer to own. That is not "silent", it is "the sample loader
 *     fails", and it would be a regression dressed as a stub.
 *   audio4.c 0x00492... OpenNarrationStream -- the speech stream. Ignores
 *     acmStreamOpen's AND acmStreamSize's results and `malloc(output_size)`
 *     straight from acmStreamSize's out parameter (audio4.c:212-213), so that
 *     parameter has to be written even on failure. See acmStreamSize.
 *   movie2.c 0x004... StartMovieAudio -- the FMV audio track, unreachable while
 *     avifil32.c reports AVIERR_FILEOPEN (that file's header explains).
 *
 * WHAT IT DOES.  The real ACM's job in this game is two conversions:
 * IMA/MS ADPCM (tag 0x11 / 0x02) to 16-bit PCM, and PCM to PCM. The second one
 * is a genuine, complete conversion and it is implemented here -- including the
 * 8-bit-unsigned to 16-bit-signed widening a real PCM converter does, which is
 * the one case where the bytes actually change. The first needs an ADPCM
 * decoder; it is refused with ACMERR_NOTPOSSIBLE (512), which is what a real
 * ACM returns when no driver can do the conversion, and which every caller
 * handles (ConvertWAVToPCM returns 0 with `data` intact; CreateSampleFromWAV
 * frees and returns 0; the shipped game behaves the same way on a machine
 * whose ADPCM codec is missing).
 *
 * So: PCM samples load and play (silently -- dsound.c is a silent device with a
 * wall-clock cursor), ADPCM samples do not load. Which of the archives' samples
 * are which is a question for a lane with an asset census; the honest statement
 * is that the shim converts what it can convert and says so when it cannot.
 *
 * FORMAT STRUCT.  WAVEFORMATEX, as resaudio2.c:128 (packed, 0x12) and audio4.c:42
 * (natural alignment, 0x14) both spell it -- the field OFFSETS are identical and
 * are the only thing read here: wFormatTag +0x00, nChannels +0x02,
 * nSamplesPerSec +0x04, nAvgBytesPerSec +0x08, nBlockAlign +0x0c,
 * wBitsPerSample +0x0e, cbSize +0x10.
 *
 * HEADER STRUCT.  ACMSTREAMHEADER, 0x54 bytes, resaudio2.c:140 spells every
 * field: cbStruct +0x00, fdwStatus +0x04, dwUser +0x08, pbSrc +0x0c,
 * cbSrcLength +0x10, cbSrcLengthUsed +0x14, dwSrcUser +0x18, pbDst +0x1c,
 * cbDstLength +0x20, cbDstLengthUsed +0x24, dwDstUser +0x28, reserved +0x2c.
 *
 * HANDLES LEAK, ON PURPOSE.  resaudio2.c's own comment: "ORIGINAL BUG,
 * reproduced: the ACM stream is never acmStreamClose()d, so a stream handle
 * leaks per converted sample." A fixed pool would run out after a few samples
 * and start refusing conversions that a real ACM would do, so each open is a
 * malloc and a leaked stream is 24 bytes, exactly as it is on Windows.
 */
#include <stdlib.h>
#include <string.h>

#include "ll_host.h"

#define MMSYSERR_NOERROR        0
#define MMSYSERR_INVALHANDLE    5
#define MMSYSERR_INVALPARAM     11
#define ACMERR_NOTPOSSIBLE      512
#define ACMERR_UNPREPARED       514

#define WAVE_FORMAT_PCM         1

/* ACM_STREAMOPENF_*: the game passes 4 (NONREALTIME) at all three call sites. */
#define ACM_STREAMOPENF_QUERY   1

#define ACM_STREAM_MAGIC        0x4c4c4143u   /* 'CALL' -- a handle this file made */

/* WAVEFORMATEX, by offset. Read through a byte pointer so neither the packed
 * nor the padded spelling of the struct can change what is read. */
struct ll_wfx {
    unsigned short wFormatTag;
    unsigned short nChannels;
    unsigned int   nSamplesPerSec;
    unsigned int   nAvgBytesPerSec;
    unsigned short nBlockAlign;
    unsigned short wBitsPerSample;
};

static void wfx_read(const void* p, struct ll_wfx* out)
{
    const unsigned char* b = (const unsigned char*)p;
    memcpy(&out->wFormatTag,      b + 0x00, 2);
    memcpy(&out->nChannels,       b + 0x02, 2);
    memcpy(&out->nSamplesPerSec,  b + 0x04, 4);
    memcpy(&out->nAvgBytesPerSec, b + 0x08, 4);
    memcpy(&out->nBlockAlign,     b + 0x0c, 2);
    memcpy(&out->wBitsPerSample,  b + 0x0e, 2);
}

/* ACMSTREAMHEADER, by offset, for the same reason. */
#define LL_ACMH_SRC          0x0c
#define LL_ACMH_SRCLEN       0x10
#define LL_ACMH_SRCUSED      0x14
#define LL_ACMH_DST          0x1c
#define LL_ACMH_DSTLEN       0x20
#define LL_ACMH_DSTUSED      0x24

static unsigned int h_u32(const void* h, int off)
{
    unsigned int v;
    memcpy(&v, (const unsigned char*)h + off, 4);
    return v;
}

static void h_set_u32(void* h, int off, unsigned int v)
{
    memcpy((unsigned char*)h + off, &v, 4);
}

static void* h_ptr(const void* h, int off)
{
    void* p;
    memcpy(&p, (const unsigned char*)h + off, sizeof p);
    return p;
}

/* One open conversion stream. Only PCM -> PCM16 exists, so all the state is
 * the source width and the channel count. */
struct ll_acm {
    unsigned int   magic;
    unsigned short src_bits;     /* 8 or 16 */
    unsigned short channels;
    unsigned int   rate;
};

static struct ll_acm* acm_of(void* has)
{
    struct ll_acm* s = (struct ll_acm*)has;
    /* Pointer-blind where it matters: the game reaches acmStreamSize and
     * acmStreamPrepareHeader with whatever acmStreamOpen left in
     * g_speech_acm even when the open FAILED (audio4.c:211-220 checks
     * nothing), so a handle has to be validated, not trusted. A null or a
     * non-magic pointer is not followed. */
    if (!s)
        return 0;
    if (s->magic != ACM_STREAM_MAGIC)
        return 0;
    return s;
}

/* How many destination bytes `srclen` source bytes become. */
static unsigned int acm_dst_bytes(const struct ll_acm* s, unsigned int srclen)
{
    return s->src_bits == 8 ? srclen * 2u : srclen;
}

int acmStreamOpen(void** phas, void* hdrv, void* srcfmt, void* dstfmt,
                  void* wfltr, unsigned long callback, unsigned long inst,
                  unsigned long flags)
{
    struct ll_wfx src, dst;
    struct ll_acm* s;

    (void)hdrv;
    (void)wfltr;
    (void)callback;
    (void)inst;

    if (!srcfmt || !dstfmt)
        return MMSYSERR_INVALPARAM;
    wfx_read(srcfmt, &src);
    wfx_read(dstfmt, &dst);

    /* The conversion this shim does not have. Both ADPCM tags the game could
     * carry (0x02 MS ADPCM, 0x11 IMA ADPCM) land here, and so would anything
     * else compressed. */
    if (src.wFormatTag != WAVE_FORMAT_PCM || dst.wFormatTag != WAVE_FORMAT_PCM) {
        ll_host_trace("acmStreamOpen: no driver for format 0x%04x -> 0x%04x"
                      " (ACMERR_NOTPOSSIBLE; no ADPCM decoder in this port)",
                      (unsigned)src.wFormatTag, (unsigned)dst.wFormatTag);
        return ACMERR_NOTPOSSIBLE;
    }
    /* A real PCM converter resamples and remixes; this one does not, and every
     * caller asks for the same rate and channel count (resaudio2.c:180-186 and
     * audio4.c:203-211 both copy them across from the source). */
    if (src.nChannels != dst.nChannels || src.nSamplesPerSec != dst.nSamplesPerSec)
        return ACMERR_NOTPOSSIBLE;
    if (dst.wBitsPerSample != 16)
        return ACMERR_NOTPOSSIBLE;
    if (src.wBitsPerSample != 8 && src.wBitsPerSample != 16)
        return ACMERR_NOTPOSSIBLE;

    if (flags & ACM_STREAMOPENF_QUERY)
        return MMSYSERR_NOERROR;              /* "yes, possible"; no handle */
    if (!phas)
        return MMSYSERR_INVALPARAM;

    s = (struct ll_acm*)malloc(sizeof *s);
    if (!s)
        return MMSYSERR_INVALPARAM;
    s->magic = ACM_STREAM_MAGIC;
    s->src_bits = src.wBitsPerSample;
    s->channels = src.nChannels;
    s->rate = src.nSamplesPerSec;
    *phas = s;
    return MMSYSERR_NOERROR;
}

/* ACM_STREAMSIZEF_SOURCE (0) is the only flag the game passes, and it passes
 * it as a literal 0 at all three sites -- "given this many SOURCE bytes, how
 * many destination bytes".
 *
 * *pdwOutput IS WRITTEN EVEN ON FAILURE, which a real acmStreamSize does not
 * do. audio4.c:212 is
 *
 *     acmStreamOpen(&g_speech_acm, ...);            // result ignored
 *     acmStreamSize(g_speech_acm, g_speech_chunk_size, &output_size, 0);
 *     g_speech_decoded = malloc(output_size);       // result ignored
 *
 * so a failed open leaves `output_size` an uninitialised stack word and the
 * game mallocs it. On Windows that is a latent bug the working ADPCM codec
 * hides; here it would be a malloc of an arbitrary 32-bit number on the first
 * line of speech. Writing 0 makes it malloc(0) -- a valid pointer to nothing,
 * which the zero-length PrepareHeader that follows is consistent with. */
int acmStreamSize(void* has, unsigned long srclen, unsigned long* pdwOutput,
                  unsigned long flags)
{
    struct ll_acm* s = acm_of(has);

    (void)flags;
    if (pdwOutput)
        *pdwOutput = 0;
    if (!s)
        return MMSYSERR_INVALHANDLE;
    if (!pdwOutput)
        return MMSYSERR_INVALPARAM;
    *pdwOutput = acm_dst_bytes(s, (unsigned int)srclen);
    return MMSYSERR_NOERROR;
}

/* Nothing to pin or lock in this port: the buffers are ordinary heap. Setting
 * ACMSTREAMHEADER_STATUSF_PREPARED (0x00000001) in fdwStatus is what a real ACM
 * does and what acmStreamConvert checks for, so it is set here too. */
#define ACMSTREAMHEADER_STATUSF_PREPARED 0x00000001u

int acmStreamPrepareHeader(void* has, void* phdr, unsigned long flags)
{
    struct ll_acm* s = acm_of(has);

    (void)flags;
    if (!phdr)
        return MMSYSERR_INVALPARAM;
    if (!s)
        return MMSYSERR_INVALHANDLE;
    h_set_u32(phdr, 0x04, h_u32(phdr, 0x04) | ACMSTREAMHEADER_STATUSF_PREPARED);
    h_set_u32(phdr, LL_ACMH_SRCUSED, 0);
    h_set_u32(phdr, LL_ACMH_DSTUSED, 0);
    return MMSYSERR_NOERROR;
}

int acmStreamUnprepareHeader(void* has, void* phdr, unsigned long flags)
{
    struct ll_acm* s = acm_of(has);

    (void)flags;
    if (!phdr)
        return MMSYSERR_INVALPARAM;
    if (!s)
        return MMSYSERR_INVALHANDLE;
    h_set_u32(phdr, 0x04, h_u32(phdr, 0x04) & ~ACMSTREAMHEADER_STATUSF_PREPARED);
    return MMSYSERR_NOERROR;
}

/* The conversion. 16-bit source is a copy; 8-bit source is the unsigned-to-
 * signed widening ((b - 128) << 8) a PCM codec does. cbSrcLengthUsed and
 * cbDstLengthUsed are both written: resaudio2.c:206 reads cbDstLengthUsed as
 * the converted length and movie2.c:419 reads it as the decoded block size. */
int acmStreamConvert(void* has, void* phdr, unsigned long flags)
{
    struct ll_acm* s = acm_of(has);
    const unsigned char* src;
    unsigned char* dst;
    unsigned int srclen, dstcap, want, n, i;

    (void)flags;
    if (!phdr)
        return MMSYSERR_INVALPARAM;
    if (!s)
        return MMSYSERR_INVALHANDLE;
    if (!(h_u32(phdr, 0x04) & ACMSTREAMHEADER_STATUSF_PREPARED))
        return ACMERR_UNPREPARED;

    src = (const unsigned char*)h_ptr(phdr, LL_ACMH_SRC);
    dst = (unsigned char*)h_ptr(phdr, LL_ACMH_DST);
    srclen = h_u32(phdr, LL_ACMH_SRCLEN);
    dstcap = h_u32(phdr, LL_ACMH_DSTLEN);
    h_set_u32(phdr, LL_ACMH_SRCUSED, 0);
    h_set_u32(phdr, LL_ACMH_DSTUSED, 0);
    if (!src || !dst)
        return MMSYSERR_INVALPARAM;

    /* Clip to whichever buffer runs out first -- a short destination is a
     * partial conversion, not an error, and that is what the Used counts are
     * for. */
    want = acm_dst_bytes(s, srclen);
    if (want > dstcap)
        want = dstcap;
    if (s->src_bits == 8) {
        n = want / 2u;                         /* source bytes consumed */
        for (i = 0; i < n; i++) {
            int v = ((int)src[i] - 128) << 8;
            dst[i * 2u]      = (unsigned char)(v & 0xff);
            dst[i * 2u + 1u] = (unsigned char)((v >> 8) & 0xff);
        }
        h_set_u32(phdr, LL_ACMH_SRCUSED, n);
        h_set_u32(phdr, LL_ACMH_DSTUSED, n * 2u);
    } else {
        want &= ~1u;                           /* whole 16-bit samples only */
        memcpy(dst, src, want);
        h_set_u32(phdr, LL_ACMH_SRCUSED, want);
        h_set_u32(phdr, LL_ACMH_DSTUSED, want);
    }
    return MMSYSERR_NOERROR;
}

int acmStreamClose(void* has, unsigned long flags)
{
    struct ll_acm* s = acm_of(has);

    (void)flags;
    if (!s)
        return MMSYSERR_INVALHANDLE;
    s->magic = 0;
    free(s);
    return MMSYSERR_NOERROR;
}
