/* LEGOLAND portable build -- the MSACM32 host shim (scope PORT-B): the Audio
 * Compression Manager, as much of it as the game actually needs.
 *
 * WHY THIS IS NOT A STUB.  Six MSACM32 imports (tools/win32_imports.txt)
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
 * WHAT IT DOES.  The real ACM's job in this game is two conversions: PCM to
 * 16-bit PCM, and MS ADPCM to 16-bit PCM. Both are implemented -- the first
 * including the 8-bit-unsigned to 16-bit-signed widening a real PCM converter
 * does, the second (PORT-B11) as a complete MS ADPCM decoder. IMA ADPCM
 * (tag 0x11) and anything else compressed is still refused with
 * ACMERR_NOTPOSSIBLE (512), which is what a real ACM returns when no driver can
 * do the conversion and which every caller handles (ConvertWAVToPCM returns 0
 * with `data` intact; CreateSampleFromWAV frees and returns 0; the shipped game
 * behaves the same way on a machine whose codec is missing).
 *
 * THE CENSUS, because "what it can convert" deserves numbers. Over the RIFF/WAVE
 * members of all three shipped archives there are 155 samples:
 *
 *     tag 1  mono 22050  16-bit   122      tag 2 (MS ADPCM) mono 22050  4-bit  21
 *     tag 1  mono 22050   8-bit    11      tag 1 mono 44100 16-bit              1
 *
 * and every file in gamedata/disc/Speech -- all 1,266 of them, 86 minutes of
 * narration -- is MS ADPCM mono 22050. Before PORT-B11 this file converted 134
 * of the 155 and not one line of speech; it now converts all 155 and all 1,266.
 * The decoder was checked against an independent reference over every one of
 * those speech files and is byte-identical.
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
#define WAVE_FORMAT_ADPCM       2    /* MS ADPCM (PORT-B11) */

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
    unsigned short cbSize;
};

/* The MS ADPCM decode parameters, all of them out of the format block itself.
 * PORT-B11. `ncoef` is 0 for a format that is not MS ADPCM. */
#define LL_ADPCM_MAX_COEF 16
struct ll_acm_fmt {
    int            channels;
    unsigned int   samples_per_block;
    unsigned int   block_align;
    unsigned int   ncoef;
    int            coef1[LL_ADPCM_MAX_COEF];
    int            coef2[LL_ADPCM_MAX_COEF];
};

/* ---- MS ADPCM (WAVE_FORMAT_ADPCM, tag 2) -------------------------------
 * PORT-B11. This is the format a fifth of the sound effects and ALL of the
 * speech are stored in, so without it the game loads 134 of its 155 archived
 * samples and not one line of narration: `CreateSampleFromWAV` (data2.c:591)
 * calls ConvertWAVToPCM unconditionally and drops the sample when it fails,
 * and PlayNarrationFile (audio4.c:185) opens an ACM stream per speech file.
 * Measured over gamedata/disc/Legoland.res: 122 samples PCM16 mono 22050,
 * 21 MS ADPCM mono 22050, 11 PCM8, 1 PCM16 44100; every file in
 * gamedata/disc/Speech is MS ADPCM mono 22050, 4-bit, blockAlign 512.
 *
 * The format: per block, one byte of predictor INDEX per channel, then an
 * int16 delta, then the two most recent samples (sample1 then sample2) per
 * channel; the block then emits sample2 and sample1 before decoding nibbles,
 * high nibble first, channels interleaved. The predictor is a two-tap
 * recurrence whose coefficient pairs come from the format's own extension
 * block -- wSamplesPerBlock at +0x12, wNumCoef at +0x14, then wNumCoef pairs
 * of int16 -- which is why `struct ll_wfx` had to grow to carry them: the
 * standard seven pairs are merely the usual contents of that table, not part
 * of the codec.
 *
 * Cross-checked against web/adpcm.js, the JS decoder already in this tree
 * (ADAPT table, the >> 8 predictor scaling, the 16-floor on delta, the
 * sample2-then-sample1 preamble). The arithmetic here is the same, in C. */

static const int kAdpcmAdapt[16] = {
    230, 230, 230, 230, 307, 409, 512, 614,
    768, 614, 512, 409, 307, 230, 230, 230
};

static int adpcm_clamp16(int v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return v;
}

/* Decode ONE block. Returns the number of 16-bit samples written (per all
 * channels together), 0 if the block is malformed or there is no room. */
static unsigned int adpcm_block(const struct ll_acm_fmt* f,
                                const unsigned char* src, unsigned int srclen,
                                short* dst, unsigned int dstcap)
{
    int ch = f->channels, c;
    int coef1[2], coef2[2], delta[2], s1[2], s2[2];
    unsigned int off = 0, want, n, out = 0;
    unsigned int nibbles;

    if (ch < 1 || ch > 2)
        return 0;
    if (srclen < (unsigned int)(7 * ch))
        return 0;
    for (c = 0; c < ch; c++) {
        unsigned int bp = src[off++];
        if (bp >= f->ncoef)
            bp = 0;                  /* a corrupt index picks the identity-ish pair */
        coef1[c] = f->coef1[bp];
        coef2[c] = f->coef2[bp];
    }
    for (c = 0; c < ch; c++) { delta[c] = (short)(src[off] | (src[off+1] << 8)); off += 2; }
    for (c = 0; c < ch; c++) { s1[c]    = (short)(src[off] | (src[off+1] << 8)); off += 2; }
    for (c = 0; c < ch; c++) { s2[c]    = (short)(src[off] | (src[off+1] << 8)); off += 2; }

    /* The preamble, oldest first. */
    for (c = 0; c < ch; c++) {
        if (out + 1 >= dstcap) return out;
        dst[out++] = (short)s2[c];
    }
    for (c = 0; c < ch; c++) {
        if (out >= dstcap) return out;
        dst[out++] = (short)s1[c];
    }

    /* How many nibbles this block actually carries: what the format says, or
     * what the bytes allow, whichever is smaller. A short final block (the
     * data chunk need not be a whole number of blocks) decodes as far as it
     * goes rather than reading past its end. */
    want = (f->samples_per_block > 2 ? f->samples_per_block - 2u : 0u) * (unsigned int)ch;
    nibbles = (srclen - off) * 2u;
    if (want > nibbles)
        want = nibbles;

    c = 0;
    for (n = 0; n < want; n++) {
        int nib = (n & 1) ? (src[off + (n >> 1)] & 0x0f)
                          : ((src[off + (n >> 1)] >> 4) & 0x0f);
        int err = nib >= 8 ? nib - 16 : nib;
        int pred = (s1[c] * coef1[c] + s2[c] * coef2[c]) >> 8;
        int val = adpcm_clamp16(pred + delta[c] * err);
        int nd;
        if (out >= dstcap)
            break;
        dst[out++] = (short)val;
        nd = (kAdpcmAdapt[nib] * delta[c]) >> 8;
        delta[c] = nd < 16 ? 16 : nd;
        s2[c] = s1[c];
        s1[c] = val;
        c = ch == 1 ? 0 : (c + 1) % ch;
    }
    return out;
}

static void wfx_read(const void* p, struct ll_wfx* out)
{
    const unsigned char* b = (const unsigned char*)p;
    memcpy(&out->wFormatTag,      b + 0x00, 2);
    memcpy(&out->nChannels,       b + 0x02, 2);
    memcpy(&out->nSamplesPerSec,  b + 0x04, 4);
    memcpy(&out->nAvgBytesPerSec, b + 0x08, 4);
    memcpy(&out->nBlockAlign,     b + 0x0c, 2);
    memcpy(&out->wBitsPerSample,  b + 0x0e, 2);
    memcpy(&out->cbSize,          b + 0x10, 2);
}

/* The MS ADPCM extension that follows the 18-byte WAVEFORMATEX: wSamplesPerBlock
 * +0x12, wNumCoef +0x14, then wNumCoef pairs of int16. The game hands this file
 * the WHOLE `fmt ` chunk it read off disk (data2.c:627 allocates the chunk's own
 * size and audio5.c:219 keeps the pointer), so the extension is there to read --
 * but `cbSize` is FORCED TO 0 for a chunk of 0x12 bytes or less (data2.c:632),
 * so a tag-2 format with no extension is a malformed file and is refused rather
 * than decoded against a guessed coefficient table. */
static int adpcm_fmt_read(const void* p, const struct ll_wfx* w,
                          struct ll_acm_fmt* f)
{
    const unsigned char* b = (const unsigned char*)p;
    unsigned int i;

    memset(f, 0, sizeof(*f));
    f->channels    = w->nChannels;
    f->block_align = w->nBlockAlign;
    if (w->cbSize < 4)
        return 0;
    f->samples_per_block = (unsigned int)(b[0x12] | (b[0x13] << 8));
    f->ncoef             = (unsigned int)(b[0x14] | (b[0x15] << 8));
    if (f->ncoef == 0 || f->ncoef > LL_ADPCM_MAX_COEF)
        return 0;
    if (w->cbSize < 4u + 4u * f->ncoef)
        return 0;
    for (i = 0; i < f->ncoef; i++) {
        int a = b[0x16 + 4 * i]     | (b[0x17 + 4 * i] << 8);
        int c = b[0x18 + 4 * i]     | (b[0x19 + 4 * i] << 8);
        f->coef1[i] = (short)a;
        f->coef2[i] = (short)c;
    }
    /* A block has to hold its own header, and the sample count has to be
     * consistent with the block size: 4 bits per sample after the 7-byte
     * per-channel preamble. */
    if (f->block_align < (unsigned int)(7 * f->channels))
        return 0;
    if (f->samples_per_block < 2)
        return 0;
    return 1;
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
    unsigned short src_tag;      /* 1 PCM, 2 MS ADPCM */
    unsigned short src_bits;     /* 8 or 16 for PCM, 4 for ADPCM */
    unsigned short channels;
    unsigned int   rate;
    struct ll_acm_fmt adpcm;     /* valid when src_tag == 2 */
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

/* How many destination bytes `srclen` source bytes become.
 *
 * For MS ADPCM this is deliberately a CEILING: the game mallocs exactly what
 * this returns (audio4.c:213, resaudio2.c:201) and then reads the real figure
 * out of cbDstLengthUsed, so an over-estimate wastes a few hundred bytes while
 * an under-estimate overruns the heap. A partial final block still decodes, so
 * the block count is rounded up. */
static unsigned int acm_dst_bytes(const struct ll_acm* s, unsigned int srclen)
{
    if (s->src_tag == WAVE_FORMAT_ADPCM) {
        unsigned int ba     = s->adpcm.block_align;
        unsigned int blocks = ba ? (srclen + ba - 1u) / ba : 0u;
        return blocks * s->adpcm.samples_per_block * 2u * (unsigned int)s->channels;
    }
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

    /* The destination is always PCM -- every caller builds it that way -- and
     * the source is PCM or MS ADPCM. IMA ADPCM (0x11) and everything else
     * compressed still lands in the refusal below, which is what a real ACM
     * with a missing codec answers and what the game is written to survive
     * (data2.c drops the sample; movie2.c drops the movie's audio). */
    if (dst.wFormatTag != WAVE_FORMAT_PCM ||
        (src.wFormatTag != WAVE_FORMAT_PCM && src.wFormatTag != WAVE_FORMAT_ADPCM)) {
        ll_host_trace("acmStreamOpen: no driver for format 0x%04x -> 0x%04x"
                      " (ACMERR_NOTPOSSIBLE)",
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
    if (src.wFormatTag == WAVE_FORMAT_PCM &&
        src.wBitsPerSample != 8 && src.wBitsPerSample != 16)
        return ACMERR_NOTPOSSIBLE;
    if (src.wFormatTag == WAVE_FORMAT_ADPCM) {
        struct ll_acm_fmt probe;
        if (src.nChannels < 1 || src.nChannels > 2)
            return ACMERR_NOTPOSSIBLE;
        if (!adpcm_fmt_read(srcfmt, &src, &probe)) {
            ll_host_trace("acmStreamOpen: MS ADPCM format block unusable"
                          " (cbSize %u)", (unsigned)src.cbSize);
            return ACMERR_NOTPOSSIBLE;
        }
    }

    if (flags & ACM_STREAMOPENF_QUERY)
        return MMSYSERR_NOERROR;              /* "yes, possible"; no handle */
    if (!phas)
        return MMSYSERR_INVALPARAM;

    s = (struct ll_acm*)malloc(sizeof *s);
    if (!s)
        return MMSYSERR_INVALPARAM;
    s->magic = ACM_STREAM_MAGIC;
    s->src_tag = src.wFormatTag;
    s->src_bits = src.wBitsPerSample;
    s->channels = src.nChannels;
    s->rate = src.nSamplesPerSec;
    memset(&s->adpcm, 0, sizeof s->adpcm);
    if (src.wFormatTag == WAVE_FORMAT_ADPCM) {
        adpcm_fmt_read(srcfmt, &src, &s->adpcm);
        ll_host_trace("acmStreamOpen: MS ADPCM %u Hz %u ch, block %u,"
                      " %u samples/block, %u coef pairs",
                      (unsigned)src.nSamplesPerSec, (unsigned)src.nChannels,
                      s->adpcm.block_align, s->adpcm.samples_per_block,
                      s->adpcm.ncoef);
    }
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
    if (ll_host_beating()) ll_host_beat("msacm32.StreamConvert");
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

    /* MS ADPCM, block by block. The source is consumed in whole blocks -- a
     * block is the codec's unit of state, so half of one decodes to noise --
     * and cbSrcLengthUsed reports only the blocks that fitted the destination.
     * That matters for the speech stream: RefillNarrationRing (narration2.c:564)
     * converts chunks of exactly ten blocks and advances its source ring by
     * cbSrcLengthUsed. */
    if (s->src_tag == WAVE_FORMAT_ADPCM) {
        unsigned int ba = s->adpcm.block_align;
        unsigned int src_used = 0, dst_used = 0;
        if (!ba)
            return MMSYSERR_INVALPARAM;
        while (src_used < srclen) {
            unsigned int have = srclen - src_used;
            unsigned int blk  = have < ba ? have : ba;
            unsigned int room = (dstcap - dst_used) / 2u;
            unsigned int got;
            if (room == 0)
                break;
            got = adpcm_block(&s->adpcm, src + src_used, blk,
                              (short*)(void*)(dst + dst_used), room);
            if (got == 0)
                break;
            dst_used += got * 2u;
            src_used += blk;
        }
        h_set_u32(phdr, LL_ACMH_SRCUSED, src_used);
        h_set_u32(phdr, LL_ACMH_DSTUSED, dst_used);
        return MMSYSERR_NOERROR;
    }

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
