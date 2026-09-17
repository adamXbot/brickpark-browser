/* LEGOLAND portable build -- a DLS Level 1 instrument collection and the
 * software synthesizer that plays it (the DirectMusic lane).
 *
 * WHY A SYNTH AT ALL
 * -------------------------------------------------------------------------
 * The game's music is not audio. imusic\ holds DirectMusic segments and styles
 * whose bands name General MIDI / GS programs ("patch 0x49 on PChannel 2"), and
 * the game ships no .dls of its own: on Windows those programs were the Roland
 * GS Sound Set in %WINDIR%\system32\drivers\gm.dls, played by the Microsoft
 * Software Synthesizer that DirectX 6.1 installed -- a DLS Level 1 synth. So
 * the thing that makes the tunes audible is a DLS1 synth over a GS collection.
 *
 * WHICH COLLECTION
 * -------------------------------------------------------------------------
 * Any DLS Level 1 or 2 file works; only Level 1 articulation is honoured,
 * which is all the DirectX 6 synth had. macOS carries a GS collection at
 * /System/Library/Components/CoreAudio.component/Contents/Resources/
 * gs_instruments.dls (235 instruments -- 226 melodic including the GS
 * variations and 9 drum kits, the same count as Windows' gm.dls -- 495 waves,
 * 8-bit 22 kHz). browser.cmake preloads whichever file LL_MUSIC_DLS names. The
 * file is somebody else's copyright, exactly like gamedata/: it is used where
 * it is, never committed.
 *
 * UNITS (DLS Level 1, every connection's lScale >> 16)
 * -------------------------------------------------------------------------
 *   time        timecents, seconds = 2^(tc/1200); 0x80000000 is "zero seconds"
 *   pitch       cents
 *   gain        centibels (tenths of a dB), negative is quieter
 *   percent     tenths of a percent (sustain level 0..1000, pan -500..500)
 *   LFO rate    absolute pitch cents, Hz = 8.176 * 2^(cents/1200)
 * Region articulation replaces the instrument's (the drum kits carry theirs per
 * region). A region's wsmp overrides its wave's.
 *
 * THE VOICE (what DLS Level 1 defines, nothing more)
 * -------------------------------------------------------------------------
 *   sample      linear interpolation, forward loop or one-shot
 *   pitch       (key - unity) * 100 + fine tune + pitch bend + EG2 * depth
 *               + LFO * (depth + mod wheel depth)
 *   EG1         attack is linear in amplitude; decay and release move at 96 dB
 *               per their time and stop at the sustain level, which is a
 *               fraction of a 100 dB range (1000 = full level)
 *   EG2         the same shape, linear 0..1, routed to pitch only
 *   gain        EG1 * region gain * velocity * CC7 * CC11 * LFO tremolo; the
 *               velocity, CC7 and CC11 curves are DLS's concave one, i.e. 40 dB
 *               per decade (amplitude squared), bottoming out at -96 dB
 *   pan         region pan + CC10, constant power
 * Control values update every LL_SYNTH_CTRL frames and the output gains ramp
 * across the block, so a zero-time attack is a sub-millisecond ramp rather
 * than a click.
 *
 * Nothing here knows about DirectMusic, COM or the browser (ll_dmusic.h).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ll_dmusic.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define LL_SYNTH_CTRL      32
#define LL_TC_ZERO         (-32768)
#define LL_DB_FLOOR        (-96.0)

/* ---- little-endian RIFF walking ---------------------------------------- */

static uint32_t dls_u32(const unsigned char* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t dls_u16(const unsigned char* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

#define DLS_FCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

typedef struct DlsChunk {
    uint32_t             id;
    uint32_t             form;    /* LIST/RIFF type, 0 otherwise */
    const unsigned char* data;    /* after the type for LIST/RIFF */
    uint32_t             size;    /* of `data` */
    const unsigned char* start;   /* the chunk header itself */
} DlsChunk;

/* Step to the next chunk in [*cur, end). 0 at the end or on a truncated
 * header; a body that overruns the parent is clipped to it. */
static int dls_next(const unsigned char** cur, const unsigned char* end, DlsChunk* c)
{
    const unsigned char* p = *cur;
    uint32_t size;
    if (end - p < 8)
        return 0;
    c->start = p;
    c->id = dls_u32(p);
    size = dls_u32(p + 4);
    if (size > (uint32_t)(end - p - 8))
        size = (uint32_t)(end - p - 8);
    c->form = 0;
    c->data = p + 8;
    c->size = size;
    if ((c->id == DLS_FCC('L', 'I', 'S', 'T') || c->id == DLS_FCC('R', 'I', 'F', 'F')) &&
        size >= 4) {
        c->form = dls_u32(p + 8);
        c->data = p + 12;
        c->size = size - 4;
    }
    p += 8 + size + (size & 1);
    *cur = p > end ? end : p;
    return 1;
}

/* ---- the collection ------------------------------------------------------ */

typedef struct DlsArt {
    int32_t eg1_attack, eg1_decay, eg1_release, eg1_sustain;
    int32_t eg1_vel_attack, eg1_key_decay;
    int32_t eg2_attack, eg2_decay, eg2_release, eg2_sustain;
    int32_t eg2_vel_attack, eg2_key_decay;
    int32_t eg2_pitch;
    int32_t lfo_freq, lfo_delay, lfo_pitch, lfo_atten;
    int32_t mw_pitch, mw_atten;
    int32_t vel_atten;
    int32_t pan;
    int32_t key_pitch;          /* cents per key * 128 / 100 scale, 12800 = normal */
} DlsArt;

typedef struct DlsSample {
    int      unity;
    int      fine;               /* cents */
    int32_t  gain;               /* centibels */
    int      loop;
    uint32_t loop_start;
    uint32_t loop_len;
} DlsSample;

typedef struct DlsWave {
    float*    pcm;
    uint32_t  frames;
    uint32_t  rate;
    int       have_smp;
    DlsSample smp;
    uint32_t  offset;            /* of its LIST in the pool, for ptbl */
} DlsWave;

typedef struct DlsRegion {
    uint8_t   key_lo, key_hi, vel_lo, vel_hi;
    uint16_t  options;
    uint16_t  key_group;
    int       wave;              /* index into waves, -1 when the link is bad */
    DlsSample smp;
    int       art;               /* index into arts, -1 for the instrument's */
} DlsRegion;

typedef struct DlsInstrument {
    uint32_t   bank;             /* MIDILOCALE ulBank */
    uint32_t   program;
    DlsRegion* regions;
    int        nregions;
    int        art;              /* index into arts, -1 for defaults */
} DlsInstrument;

struct LLDls {
    DlsInstrument* ins;
    int            nins;
    DlsWave*       waves;
    int            nwaves;
    DlsArt*        arts;
    int            narts;
    int            cap_arts;
};

static void dls_art_defaults(DlsArt* a)
{
    memset(a, 0, sizeof *a);
    a->eg1_attack = a->eg1_decay = a->eg1_release = LL_TC_ZERO;
    a->eg2_attack = a->eg2_decay = a->eg2_release = LL_TC_ZERO;
    a->eg1_sustain = 1000;
    a->eg2_sustain = 1000;
    a->lfo_freq = -851;          /* 5 Hz */
    a->lfo_delay = -7973;        /* 10 ms */
    a->vel_atten = -960;
    a->key_pitch = 12800;
}

static int dls_art_add(LLDls* d, const DlsArt* a)
{
    if (d->narts == d->cap_arts) {
        int     cap = d->cap_arts ? d->cap_arts * 2 : 64;
        DlsArt* n = (DlsArt*)realloc(d->arts, (size_t)cap * sizeof *n);
        if (!n)
            return -1;
        d->arts = n;
        d->cap_arts = cap;
    }
    d->arts[d->narts] = *a;
    return d->narts++;
}

/* One art1/art2 chunk into `a` (on top of whatever it already holds). */
static void dls_parse_art(const DlsChunk* c, DlsArt* a)
{
    uint32_t cb, n, i;
    if (c->size < 8)
        return;
    cb = dls_u32(c->data);
    n = dls_u32(c->data + 4);
    if (cb < 8 || cb > c->size)
        return;
    for (i = 0; i < n && cb + (i + 1) * 12u <= c->size; i++) {
        const unsigned char* b = c->data + cb + i * 12u;
        uint16_t src = dls_u16(b), ctl = dls_u16(b + 2), dst = dls_u16(b + 4);
        int32_t  v = (int32_t)dls_u32(b + 8);
        int32_t  s = v == (int32_t)0x80000000 ? LL_TC_ZERO : (v >> 16);
        if (ctl == 0 && src == 0) {
            switch (dst) {
            case 0x0206: a->eg1_attack = s; break;
            case 0x0207: a->eg1_decay = s; break;
            case 0x0209: a->eg1_release = s; break;
            case 0x020a: a->eg1_sustain = s; break;
            case 0x030a: a->eg2_attack = s; break;
            case 0x030b: a->eg2_decay = s; break;
            case 0x030d: a->eg2_release = s; break;
            case 0x030e: a->eg2_sustain = s; break;
            case 0x0104: a->lfo_freq = s; break;
            case 0x0105: a->lfo_delay = s; break;
            case 0x0004: a->pan = s; break;
            default: break;
            }
        } else if (ctl == 0) {
            if (src == 2 && dst == 0x0206) a->eg1_vel_attack = s;
            else if (src == 3 && dst == 0x0207) a->eg1_key_decay = s;
            else if (src == 2 && dst == 0x030a) a->eg2_vel_attack = s;
            else if (src == 3 && dst == 0x030b) a->eg2_key_decay = s;
            else if (src == 5 && dst == 0x0003) a->eg2_pitch = s;
            else if (src == 1 && dst == 0x0003) a->lfo_pitch = s;
            else if (src == 1 && dst == 0x0001) a->lfo_atten = s;
            else if (src == 2 && dst == 0x0001) a->vel_atten = s;
            else if (src == 3 && dst == 0x0003) a->key_pitch = s;
        } else if (ctl == 0x81 && src == 1) {
            if (dst == 0x0003) a->mw_pitch = s;
            else if (dst == 0x0001) a->mw_atten = s;
        }
    }
}

static void dls_parse_wsmp(const DlsChunk* c, DlsSample* smp)
{
    uint32_t cb, loops;
    memset(smp, 0, sizeof *smp);
    smp->unity = 60;
    if (c->size < 20)
        return;
    cb = dls_u32(c->data);
    smp->unity = dls_u16(c->data + 4) & 0x7f;
    smp->fine = (int16_t)dls_u16(c->data + 6);
    smp->gain = ((int32_t)dls_u32(c->data + 8)) >> 16;
    loops = dls_u32(c->data + 16);
    if (loops && cb >= 20 && cb + 16u <= c->size) {
        const unsigned char* l = c->data + cb;
        smp->loop = 1;
        smp->loop_start = dls_u32(l + 8);
        smp->loop_len = dls_u32(l + 12);
    }
}

static void dls_parse_wave(const DlsChunk* list, DlsWave* w)
{
    const unsigned char* cur = list->data;
    const unsigned char* end = list->data + list->size;
    const unsigned char* pcm = 0;
    uint32_t             pcm_size = 0;
    uint16_t             tag = 0, channels = 1, bits = 8;
    DlsChunk             c;

    memset(w, 0, sizeof *w);
    while (dls_next(&cur, end, &c)) {
        if (c.id == DLS_FCC('f', 'm', 't', ' ') && c.size >= 16) {
            tag = dls_u16(c.data);
            channels = dls_u16(c.data + 2);
            w->rate = dls_u32(c.data + 4);
            bits = dls_u16(c.data + 14);
        } else if (c.id == DLS_FCC('w', 's', 'm', 'p')) {
            dls_parse_wsmp(&c, &w->smp);
            w->have_smp = 1;
        } else if (c.id == DLS_FCC('d', 'a', 't', 'a')) {
            pcm = c.data;
            pcm_size = c.size;
        }
    }
    if (tag != 1 || !pcm || channels < 1 || (bits != 8 && bits != 16) || !w->rate)
        return;
    w->frames = pcm_size / (uint32_t)((bits / 8) * channels);
    w->pcm = (float*)malloc((w->frames + 1) * sizeof(float));
    if (!w->pcm) {
        w->frames = 0;
        return;
    }
    {
        uint32_t i;
        for (i = 0; i < w->frames; i++) {
            /* The first channel of anything wider; DLS waves are mono. */
            if (bits == 8)
                w->pcm[i] = ((int)pcm[i * channels] - 128) / 128.0f;
            else
                w->pcm[i] = (int16_t)dls_u16(pcm + i * 2u * channels) / 32768.0f;
        }
        w->pcm[w->frames] = 0.0f;
    }
}

static void dls_parse_region(LLDls* d, const DlsChunk* list, DlsRegion* r, uint32_t* cue)
{
    const unsigned char* cur = list->data;
    const unsigned char* end = list->data + list->size;
    DlsChunk             c;
    int                  have_smp = 0;

    memset(r, 0, sizeof *r);
    r->key_hi = 127;
    r->vel_hi = 127;
    r->wave = -1;
    r->art = -1;
    *cue = 0xffffffffu;
    while (dls_next(&cur, end, &c)) {
        if (c.id == DLS_FCC('r', 'g', 'n', 'h') && c.size >= 12) {
            r->key_lo = (uint8_t)(dls_u16(c.data) & 0x7f);
            r->key_hi = (uint8_t)(dls_u16(c.data + 2) & 0x7f);
            r->vel_lo = (uint8_t)(dls_u16(c.data + 4) & 0x7f);
            r->vel_hi = (uint8_t)(dls_u16(c.data + 6) & 0x7f);
            r->options = dls_u16(c.data + 8);
            r->key_group = dls_u16(c.data + 10);
            if (r->vel_hi < r->vel_lo || (r->vel_lo == 0 && r->vel_hi == 0)) {
                r->vel_lo = 0;
                r->vel_hi = 127;
            }
        } else if (c.id == DLS_FCC('w', 's', 'm', 'p')) {
            dls_parse_wsmp(&c, &r->smp);
            have_smp = 1;
        } else if (c.id == DLS_FCC('w', 'l', 'n', 'k') && c.size >= 12) {
            *cue = dls_u32(c.data + 8);
        } else if (c.form == DLS_FCC('l', 'a', 'r', 't') ||
                   c.form == DLS_FCC('l', 'a', 'r', '2')) {
            const unsigned char* a = c.data;
            const unsigned char* ae = c.data + c.size;
            DlsChunk             ac;
            DlsArt               art;
            dls_art_defaults(&art);
            while (dls_next(&a, ae, &ac))
                if (ac.id == DLS_FCC('a', 'r', 't', '1') || ac.id == DLS_FCC('a', 'r', 't', '2'))
                    dls_parse_art(&ac, &art);
            r->art = dls_art_add(d, &art);
        }
    }
    if (!have_smp)
        r->smp.unity = -1;       /* take the wave's */
}

LLDls* ll_dls_load(const unsigned char* data, size_t size)
{
    const unsigned char* cur = data;
    const unsigned char* end = data + size;
    const unsigned char* pool = 0;
    const unsigned char* ptbl = 0;
    uint32_t             ptbl_size = 0;
    DlsChunk             riff, c;
    LLDls*               d;
    const unsigned char* lins = 0;
    uint32_t             lins_size = 0;
    uint32_t             pool_size = 0;

    if (!data || size < 12 || !dls_next(&cur, end, &riff) ||
        riff.id != DLS_FCC('R', 'I', 'F', 'F') || riff.form != DLS_FCC('D', 'L', 'S', ' '))
        return 0;
    d = (LLDls*)calloc(1, sizeof *d);
    if (!d)
        return 0;

    cur = riff.data;
    end = riff.data + riff.size;
    while (dls_next(&cur, end, &c)) {
        if (c.form == DLS_FCC('l', 'i', 'n', 's')) {
            lins = c.data;
            lins_size = c.size;
        } else if (c.form == DLS_FCC('w', 'v', 'p', 'l')) {
            pool = c.data;
            pool_size = c.size;
        } else if (c.id == DLS_FCC('p', 't', 'b', 'l')) {
            ptbl = c.data;
            ptbl_size = c.size;
        }
    }

    /* The wave pool, remembering where each LIST sits for the pool table. */
    if (pool) {
        const unsigned char* p = pool;
        const unsigned char* pe = pool + pool_size;
        int                  cap = 0;
        while (dls_next(&p, pe, &c)) {
            if (c.form != DLS_FCC('w', 'a', 'v', 'e'))
                continue;
            if (d->nwaves == cap) {
                int      ncap = cap ? cap * 2 : 256;
                DlsWave* nw = (DlsWave*)realloc(d->waves, (size_t)ncap * sizeof *nw);
                if (!nw)
                    break;
                d->waves = nw;
                cap = ncap;
            }
            dls_parse_wave(&c, &d->waves[d->nwaves]);
            /* ptbl offsets count from the first byte after the 'wvpl' type. */
            d->waves[d->nwaves].offset = (uint32_t)(c.start - pool);
            d->nwaves++;
        }
    }

    /* The instruments. */
    if (lins) {
        const unsigned char* p = lins;
        const unsigned char* pe = lins + lins_size;
        int                  cap = 0;
        while (dls_next(&p, pe, &c)) {
            const unsigned char* q;
            const unsigned char* qe;
            DlsChunk             ic;
            DlsInstrument*       ins;
            if (c.form != DLS_FCC('i', 'n', 's', ' '))
                continue;
            if (d->nins == cap) {
                int            ncap = cap ? cap * 2 : 256;
                DlsInstrument* ni = (DlsInstrument*)realloc(d->ins, (size_t)ncap * sizeof *ni);
                if (!ni)
                    break;
                d->ins = ni;
                cap = ncap;
            }
            ins = &d->ins[d->nins];
            memset(ins, 0, sizeof *ins);
            ins->art = -1;
            q = c.data;
            qe = c.data + c.size;
            while (dls_next(&q, qe, &ic)) {
                if (ic.id == DLS_FCC('i', 'n', 's', 'h') && ic.size >= 12) {
                    ins->bank = dls_u32(ic.data + 4);
                    ins->program = dls_u32(ic.data + 8) & 0x7f;
                } else if (ic.form == DLS_FCC('l', 'r', 'g', 'n')) {
                    const unsigned char* r = ic.data;
                    const unsigned char* re = ic.data + ic.size;
                    DlsChunk             rc;
                    int                  rcap = 0;
                    while (dls_next(&r, re, &rc)) {
                        uint32_t cue;
                        if (rc.form != DLS_FCC('r', 'g', 'n', ' ') &&
                            rc.form != DLS_FCC('r', 'g', 'n', '2'))
                            continue;
                        if (ins->nregions == rcap) {
                            int        ncap = rcap ? rcap * 2 : 8;
                            DlsRegion* nr = (DlsRegion*)realloc(ins->regions,
                                                                (size_t)ncap * sizeof *nr);
                            if (!nr)
                                break;
                            ins->regions = nr;
                            rcap = ncap;
                        }
                        dls_parse_region(d, &rc, &ins->regions[ins->nregions], &cue);
                        /* Resolve the cue through the pool table to a wave. */
                        if (ptbl && ptbl_size >= 8) {
                            uint32_t cb = dls_u32(ptbl);
                            uint32_t ncues = dls_u32(ptbl + 4);
                            if (cue < ncues && cb + (cue + 1) * 4u <= ptbl_size) {
                                uint32_t off = dls_u32(ptbl + cb + cue * 4u);
                                int      w;
                                for (w = 0; w < d->nwaves; w++) {
                                    if (d->waves[w].offset == off) {
                                        ins->regions[ins->nregions].wave = w;
                                        break;
                                    }
                                }
                            }
                        } else if (cue < (uint32_t)d->nwaves) {
                            ins->regions[ins->nregions].wave = (int)cue;
                        }
                        ins->nregions++;
                    }
                } else if (ic.form == DLS_FCC('l', 'a', 'r', 't') ||
                           ic.form == DLS_FCC('l', 'a', 'r', '2')) {
                    const unsigned char* a = ic.data;
                    const unsigned char* ae = ic.data + ic.size;
                    DlsChunk             ac;
                    DlsArt               art;
                    dls_art_defaults(&art);
                    while (dls_next(&a, ae, &ac))
                        if (ac.id == DLS_FCC('a', 'r', 't', '1') ||
                            ac.id == DLS_FCC('a', 'r', 't', '2'))
                            dls_parse_art(&ac, &art);
                    ins->art = dls_art_add(d, &art);
                }
            }
            /* Regions without their own wsmp take the wave's. */
            {
                int k;
                for (k = 0; k < ins->nregions; k++) {
                    DlsRegion* rg = &ins->regions[k];
                    if (rg->smp.unity < 0) {
                        if (rg->wave >= 0 && d->waves[rg->wave].have_smp) {
                            rg->smp = d->waves[rg->wave].smp;
                        } else {
                            memset(&rg->smp, 0, sizeof rg->smp);
                            rg->smp.unity = 60;
                        }
                    }
                }
            }
            d->nins++;
        }
    }

    if (d->nins == 0 || d->nwaves == 0) {
        ll_dls_free(d);
        return 0;
    }
    return d;
}

void ll_dls_free(LLDls* d)
{
    int i;
    if (!d)
        return;
    for (i = 0; i < d->nwaves; i++)
        free(d->waves[i].pcm);
    for (i = 0; i < d->nins; i++)
        free(d->ins[i].regions);
    free(d->waves);
    free(d->ins);
    free(d->arts);
    free(d);
}

int ll_dls_instrument_count(const LLDls* d) { return d ? d->nins : 0; }
int ll_dls_wave_count(const LLDls* d) { return d ? d->nwaves : 0; }

static const DlsInstrument* dls_find_exact(const LLDls* d, uint32_t bank, uint32_t program)
{
    int i;
    for (i = 0; i < d->nins; i++)
        if (d->ins[i].bank == bank && d->ins[i].program == program)
            return &d->ins[i];
    return 0;
}

/* dwPatch -> instrument, with the GS fallbacks: variation -> capital tone,
 * then (drums) the standard kit. */
static const DlsInstrument* dls_find(const LLDls* d, uint32_t patch)
{
    uint32_t drums = patch & 0x80000000u;
    uint32_t msb = (patch >> 16) & 0x7f;
    uint32_t lsb = (patch >> 8) & 0x7f;
    uint32_t prog = patch & 0x7f;
    const DlsInstrument* ins;

    if ((ins = dls_find_exact(d, drums | (msb << 8) | lsb, prog)))
        return ins;
    if ((ins = dls_find_exact(d, drums | (msb << 8), prog)))
        return ins;
    if ((ins = dls_find_exact(d, drums, prog)))
        return ins;
    if (drums && (ins = dls_find_exact(d, drums, 0)))
        return ins;
    return 0;
}

/* ======================================================================== */
/* The synth                                                                */
/* ======================================================================== */

enum { EG_OFF = 0, EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

typedef struct SynthChannel {
    const DlsInstrument* ins;
    uint32_t patch;
    int      volume, expression, pan, modwheel, sustain;
    int      bend, bend_range;   /* cents */
    int      rpn_msb, rpn_lsb;
    uint32_t priority;
} SynthChannel;

typedef struct Voice {
    int              active;
    int              ch, note, velocity;
    const DlsRegion* rgn;
    const DlsWave*   wav;
    const DlsArt*    art;
    double           pos;
    double           base_cents;    /* key, unity and fine tune */
    int              key_down;
    int              sustained;      /* note-off arrived under the pedal */
    int              eg1;
    double           eg1_amp, eg1_db, eg1_attack, eg1_decay, eg1_sus_db, eg1_release;
    int              eg2;
    double           eg2_level, eg2_attack, eg2_decay, eg2_sus, eg2_release;
    double           lfo_phase, lfo_inc, lfo_wait;
    double           static_cb;      /* velocity + region gain */
    double           gl, gr;         /* output gains at the end of the last block */
    uint32_t         age;
    uint32_t         priority;
} Voice;

struct LLSynth {
    const LLDls* dls;
    double       rate;
    int          nvoices;
    Voice*       voices;
    SynthChannel ch[16];
    uint32_t     age;
    uint32_t     missing;
};

static double tc_seconds(int32_t tc)
{
    if (tc <= -12000)
        return 0.0;
    return pow(2.0, tc / 1200.0);
}

/* The DLS concave curve over a 0..127 controller: 40 dB per decade. */
static double concave_cb(int value)
{
    if (value <= 0)
        return -960.0;
    if (value >= 127)
        return 0.0;
    {
        double cb = 400.0 * log10(value / 127.0);
        return cb < -960.0 ? -960.0 : cb;
    }
}

/* DAUD_STANDARD_VOICE_PRIORITY plus the default channel offsets: channel 10
 * (drums) first, then 1, 2, .. 9, 11 .. 16. */
static uint32_t default_priority(int ch)
{
    static const unsigned char off[16] = { 14, 13, 12, 11, 10, 9, 8, 7, 6, 15, 5, 4, 3, 2, 1, 0 };
    return 0x80000000u + off[ch & 15];
}

static void channel_defaults(SynthChannel* c, int ch)
{
    memset(c, 0, sizeof *c);
    c->volume = 100;
    c->expression = 127;
    c->pan = 64;
    c->bend = 8192;
    c->bend_range = 200;
    c->rpn_msb = c->rpn_lsb = 127;
    c->priority = default_priority(ch);
}

LLSynth* ll_synth_new(const LLDls* dls, double rate, int voices)
{
    LLSynth* s;
    int      i;
    if (voices < 1)
        voices = 32;
    s = (LLSynth*)calloc(1, sizeof *s);
    if (!s)
        return 0;
    s->voices = (Voice*)calloc((size_t)voices, sizeof(Voice));
    if (!s->voices) {
        free(s);
        return 0;
    }
    s->dls = dls;
    s->rate = rate > 0 ? rate : 44100.0;
    s->nvoices = voices;
    for (i = 0; i < 16; i++)
        channel_defaults(&s->ch[i], i);
    if (dls) {
        for (i = 0; i < 16; i++) {
            s->ch[i].ins = dls_find(dls, i == 9 ? 0x80000000u : 0);
            s->ch[i].patch = i == 9 ? 0x80000000u : 0;
        }
    }
    return s;
}

void ll_synth_free(LLSynth* s)
{
    if (!s)
        return;
    free(s->voices);
    free(s);
}

void ll_synth_program(LLSynth* s, int ch, uint32_t patch)
{
    if (!s || ch < 0 || ch > 15)
        return;
    s->ch[ch].patch = patch;
    s->ch[ch].ins = s->dls ? dls_find(s->dls, patch) : 0;
    if (!s->ch[ch].ins)
        s->missing++;
}

static void voice_release(LLSynth* s, Voice* v)
{
    (void)s;
    if (!v->active || v->eg1 == EG_RELEASE)
        return;
    if (v->eg1 == EG_ATTACK)
        v->eg1_db = v->eg1_amp > 1e-5 ? 20.0 * log10(v->eg1_amp) : LL_DB_FLOOR;
    v->eg1 = EG_RELEASE;
    if (v->eg2 != EG_OFF)
        v->eg2 = EG_RELEASE;
}

void ll_synth_cc(LLSynth* s, int ch, int cc, int value)
{
    SynthChannel* c;
    int           i;
    if (!s || ch < 0 || ch > 15)
        return;
    c = &s->ch[ch];
    if (value < 0) value = 0;
    if (value > 127) value = 127;
    switch (cc) {
    case 1: c->modwheel = value; break;
    case 7: c->volume = value; break;
    case 10: c->pan = value; break;
    case 11: c->expression = value; break;
    case 64:
        c->sustain = value >= 64;
        if (!c->sustain) {
            for (i = 0; i < s->nvoices; i++) {
                Voice* v = &s->voices[i];
                if (v->active && v->ch == ch && v->sustained) {
                    v->sustained = 0;
                    voice_release(s, v);
                }
            }
        }
        break;
    case 100: c->rpn_lsb = value; break;
    case 101: c->rpn_msb = value; break;
    case 6:
        if (c->rpn_msb == 0 && c->rpn_lsb == 0)
            c->bend_range = value * 100 + c->bend_range % 100;
        break;
    case 38:
        if (c->rpn_msb == 0 && c->rpn_lsb == 0)
            c->bend_range = (c->bend_range / 100) * 100 + value;
        break;
    case 120:
        for (i = 0; i < s->nvoices; i++)
            if (s->voices[i].ch == ch)
                s->voices[i].active = 0;
        break;
    case 121:
        channel_defaults(c, ch);
        c->ins = s->dls ? dls_find(s->dls, c->patch) : 0;
        break;
    case 123:
        for (i = 0; i < s->nvoices; i++)
            if (s->voices[i].active && s->voices[i].ch == ch)
                voice_release(s, &s->voices[i]);
        break;
    default: break;
    }
}

void ll_synth_bend(LLSynth* s, int ch, int value)
{
    if (!s || ch < 0 || ch > 15)
        return;
    if (value < 0) value = 0;
    if (value > 16383) value = 16383;
    s->ch[ch].bend = value;
}

static Voice* voice_alloc(LLSynth* s)
{
    Voice*   best = 0;
    double   best_score = 0.0;
    int      i;
    for (i = 0; i < s->nvoices; i++)
        if (!s->voices[i].active)
            return &s->voices[i];
    /* Steal: a releasing voice before a held one, a lower-priority channel
     * before a higher one, the quieter and then the older before the rest. */
    for (i = 0; i < s->nvoices; i++) {
        Voice* v = &s->voices[i];
        double score = (v->eg1 == EG_RELEASE ? 0.0 : 4.0e9) + (double)v->priority +
                       v->eg1_amp - (double)(s->age - v->age) * 1e-6;
        if (!best || score < best_score) {
            best = v;
            best_score = score;
        }
    }
    return best;
}

void ll_synth_note_on(LLSynth* s, int ch, int note, int velocity)
{
    SynthChannel*        c;
    const DlsInstrument* ins;
    const DlsRegion*     rgn = 0;
    const DlsArt*        art;
    static DlsArt        defaults;
    static int           have_defaults;
    Voice*               v;
    int                  i;
    double               sec;

    if (!s || ch < 0 || ch > 15 || note < 0 || note > 127)
        return;
    if (velocity <= 0) {
        ll_synth_note_off(s, ch, note);
        return;
    }
    if (velocity > 127)
        velocity = 127;
    c = &s->ch[ch];
    ins = c->ins;
    if (!ins)
        return;
    for (i = 0; i < ins->nregions; i++) {
        const DlsRegion* r = &ins->regions[i];
        if (note >= r->key_lo && note <= r->key_hi && velocity >= r->vel_lo &&
            velocity <= r->vel_hi && r->wave >= 0 && s->dls->waves[r->wave].frames) {
            rgn = r;
            break;
        }
    }
    if (!rgn)
        return;
    if (!have_defaults) {
        dls_art_defaults(&defaults);
        have_defaults = 1;
    }
    art = rgn->art >= 0 ? &s->dls->arts[rgn->art]
                        : ins->art >= 0 ? &s->dls->arts[ins->art] : &defaults;

    /* Exclusive key groups (hi-hats) and non-self-exclusive retriggers. */
    for (i = 0; i < s->nvoices; i++) {
        Voice* o = &s->voices[i];
        if (!o->active || o->ch != ch)
            continue;
        if (rgn->key_group && o->rgn && o->rgn->key_group == rgn->key_group &&
            o->eg1 != EG_RELEASE) {
            o->eg1_release = 0.005 * s->rate;
            voice_release(s, o);
        } else if (o->note == note && !(rgn->options & 1) && o->eg1 != EG_RELEASE) {
            voice_release(s, o);
        }
    }

    v = voice_alloc(s);
    if (!v)
        return;
    memset(v, 0, sizeof *v);
    v->active = 1;
    v->ch = ch;
    v->note = note;
    v->velocity = velocity;
    v->rgn = rgn;
    v->wav = &s->dls->waves[rgn->wave];
    v->art = art;
    v->key_down = 1;
    v->age = ++s->age;
    v->priority = c->priority;
    v->base_cents = (note - rgn->smp.unity) * (art->key_pitch / 128.0) + rgn->smp.fine;

    /* EG1, in samples. Velocity scales the attack, the key scales the decay. */
    sec = tc_seconds(art->eg1_attack + (int32_t)(art->eg1_vel_attack * velocity / 128.0));
    v->eg1_attack = sec * s->rate;
    sec = tc_seconds(art->eg1_decay + (int32_t)(art->eg1_key_decay * note / 128.0));
    v->eg1_decay = sec * s->rate;
    v->eg1_release = tc_seconds(art->eg1_release) * s->rate;
    {
        int sus = art->eg1_sustain < 0 ? 0 : art->eg1_sustain > 1000 ? 1000 : art->eg1_sustain;
        v->eg1_sus_db = -(1000 - sus) / 10.0;
        if (v->eg1_sus_db < LL_DB_FLOOR)
            v->eg1_sus_db = LL_DB_FLOOR;
    }
    v->eg1 = EG_ATTACK;
    v->eg1_amp = 0.0;
    v->eg1_db = LL_DB_FLOOR;

    if (art->eg2_pitch) {
        sec = tc_seconds(art->eg2_attack + (int32_t)(art->eg2_vel_attack * velocity / 128.0));
        v->eg2_attack = sec * s->rate;
        sec = tc_seconds(art->eg2_decay + (int32_t)(art->eg2_key_decay * note / 128.0));
        v->eg2_decay = sec * s->rate;
        v->eg2_release = tc_seconds(art->eg2_release) * s->rate;
        v->eg2_sus = (art->eg2_sustain < 0 ? 0 : art->eg2_sustain > 1000 ? 1000
                                                                          : art->eg2_sustain) / 1000.0;
        v->eg2 = EG_ATTACK;
    }

    v->lfo_inc = 2.0 * M_PI * (8.176 * pow(2.0, art->lfo_freq / 1200.0)) / s->rate;
    v->lfo_wait = tc_seconds(art->lfo_delay) * s->rate;

    /* Velocity through its connection (concave, scaled to its depth). */
    v->static_cb = concave_cb(velocity) * (art->vel_atten / -960.0) + rgn->smp.gain;
}

void ll_synth_note_off(LLSynth* s, int ch, int note)
{
    int i;
    if (!s || ch < 0 || ch > 15)
        return;
    for (i = 0; i < s->nvoices; i++) {
        Voice* v = &s->voices[i];
        if (!v->active || v->ch != ch || v->note != note || !v->key_down)
            continue;
        v->key_down = 0;
        if (s->ch[ch].sustain)
            v->sustained = 1;
        else
            voice_release(s, v);
    }
}

void ll_synth_all_notes_off(LLSynth* s)
{
    int i;
    if (!s)
        return;
    for (i = 0; i < s->nvoices; i++)
        if (s->voices[i].active) {
            s->voices[i].key_down = 0;
            voice_release(s, &s->voices[i]);
        }
}

void ll_synth_reset(LLSynth* s)
{
    int i;
    if (!s)
        return;
    for (i = 0; i < s->nvoices; i++)
        s->voices[i].active = 0;
    for (i = 0; i < 16; i++) {
        uint32_t patch = s->ch[i].patch;
        channel_defaults(&s->ch[i], i);
        s->ch[i].patch = patch;
        s->ch[i].ins = s->dls ? dls_find(s->dls, patch) : 0;
    }
}

int ll_synth_active_voices(const LLSynth* s)
{
    int i, n = 0;
    if (!s)
        return 0;
    for (i = 0; i < s->nvoices; i++)
        n += s->voices[i].active;
    return n;
}

uint32_t ll_synth_missing_programs(const LLSynth* s)
{
    return s ? s->missing : 0;
}

/* Advance EG1 by n frames; returns the amplitude at the end. */
static double eg1_step(Voice* v, int n)
{
    switch (v->eg1) {
    case EG_ATTACK:
        v->eg1_amp += v->eg1_attack >= 1.0 ? n / v->eg1_attack : 1.0;
        if (v->eg1_amp >= 1.0) {
            v->eg1_amp = 1.0;
            v->eg1_db = 0.0;
            v->eg1 = EG_DECAY;
        }
        return v->eg1_amp;
    case EG_DECAY:
        v->eg1_db -= v->eg1_decay >= 1.0 ? 96.0 * n / v->eg1_decay : 96.0;
        if (v->eg1_db <= v->eg1_sus_db) {
            v->eg1_db = v->eg1_sus_db;
            v->eg1 = EG_SUSTAIN;
        }
        break;
    case EG_SUSTAIN:
        v->eg1_db = v->eg1_sus_db;
        break;
    case EG_RELEASE:
        v->eg1_db -= v->eg1_release >= 1.0 ? 96.0 * n / v->eg1_release : 96.0;
        if (v->eg1_db <= LL_DB_FLOOR) {
            v->eg1_db = LL_DB_FLOOR;
            v->active = 0;
            return 0.0;
        }
        break;
    default:
        return 0.0;
    }
    v->eg1_amp = pow(10.0, v->eg1_db / 20.0);
    return v->eg1_amp;
}

static double eg2_step(Voice* v, int n)
{
    switch (v->eg2) {
    case EG_ATTACK:
        v->eg2_level += v->eg2_attack >= 1.0 ? n / v->eg2_attack : 1.0;
        if (v->eg2_level >= 1.0) {
            v->eg2_level = 1.0;
            v->eg2 = EG_DECAY;
        }
        break;
    case EG_DECAY:
        v->eg2_level -= v->eg2_decay >= 1.0 ? n / v->eg2_decay : 1.0;
        if (v->eg2_level <= v->eg2_sus) {
            v->eg2_level = v->eg2_sus;
            v->eg2 = EG_SUSTAIN;
        }
        break;
    case EG_RELEASE:
        v->eg2_level -= v->eg2_release >= 1.0 ? n / v->eg2_release : 1.0;
        if (v->eg2_level <= 0.0) {
            v->eg2_level = 0.0;
            v->eg2 = EG_OFF;
        }
        break;
    default:
        break;
    }
    return v->eg2_level;
}

static void voice_render(LLSynth* s, Voice* v, float* L, float* R, int n)
{
    const SynthChannel* c = &s->ch[v->ch];
    const DlsArt*       art = v->art;
    const DlsWave*      w = v->wav;
    const DlsSample*    smp = &v->rgn->smp;
    double              lfo = 0.0, cents, step, amp, cb, gain, pan, angle, gl, gr;
    double              dl, dr;
    int                 i;
    int                 loop = smp->loop && smp->loop_len > 1 &&
                               smp->loop_start + smp->loop_len <= w->frames;
    double              loop_end = loop ? (double)(smp->loop_start + smp->loop_len) : 0.0;

    if (v->lfo_wait > 0.0) {
        v->lfo_wait -= n;
    } else {
        lfo = sin(v->lfo_phase);
        v->lfo_phase += v->lfo_inc * n;
        if (v->lfo_phase > 2.0 * M_PI)
            v->lfo_phase -= 2.0 * M_PI;
    }

    cents = v->base_cents + (c->bend - 8192) / 8192.0 * c->bend_range +
            lfo * (art->lfo_pitch + art->mw_pitch * c->modwheel / 127.0);
    if (v->eg2 != EG_OFF)
        cents += eg2_step(v, n) * art->eg2_pitch;
    step = pow(2.0, cents / 1200.0) * (double)w->rate / s->rate;

    amp = eg1_step(v, n);
    cb = v->static_cb + concave_cb(c->volume) + concave_cb(c->expression) +
         lfo * (art->lfo_atten + art->mw_atten * c->modwheel / 127.0);
    gain = amp * pow(10.0, cb / 200.0);

    pan = art->pan / 1000.0 + (c->pan - 64) / 127.0;
    if (pan < -0.5) pan = -0.5;
    if (pan > 0.5) pan = 0.5;
    angle = (pan + 0.5) * (M_PI / 2.0);
    gl = gain * cos(angle);
    gr = gain * sin(angle);

    dl = (gl - v->gl) / n;
    dr = (gr - v->gr) / n;
    for (i = 0; i < n; i++) {
        uint32_t idx = (uint32_t)v->pos;
        double   frac = v->pos - idx;
        float    a, b, x;
        if (idx >= w->frames) {
            v->active = 0;
            break;
        }
        a = w->pcm[idx];
        if (loop && idx + 1 >= (uint32_t)loop_end)
            b = w->pcm[smp->loop_start];
        else
            b = w->pcm[idx + 1];
        x = (float)(a + (b - a) * frac);
        L[i] += (float)(x * (v->gl + dl * (i + 1)));
        R[i] += (float)(x * (v->gr + dr * (i + 1)));
        v->pos += step;
        if (loop && v->pos >= loop_end)
            v->pos -= smp->loop_len;
    }
    v->gl = gl;
    v->gr = gr;
    if (!v->active)
        v->gl = v->gr = 0.0;
}

void ll_synth_render(LLSynth* s, float* left, float* right, int frames)
{
    int done = 0;
    if (!s)
        return;
    while (done < frames) {
        int n = frames - done;
        int i;
        if (n > LL_SYNTH_CTRL)
            n = LL_SYNTH_CTRL;
        for (i = 0; i < s->nvoices; i++)
            if (s->voices[i].active)
                voice_render(s, &s->voices[i], left + done, right + done, n);
        done += n;
    }
}
