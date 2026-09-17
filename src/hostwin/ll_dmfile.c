/* LEGOLAND portable build -- the DirectMusic file forms the game ships
 * (the DirectMusic lane).
 *
 * imusic\ holds 30 segments (RIFF DMSG), 212 styles (RIFF DMST) and one band
 * (RIFF DMBD), all written by DirectMusic Producer against the DirectX 6
 * dmusicf.h. The layouts below are that header's, and the sizes are checked
 * against the chunks rather than assumed: every array chunk (notes, curves,
 * commands, tempos, mutes, chords) starts with its own element size, and the
 * DirectX 8 editions of those structs are longer, so reading by the stored size
 * keeps a DX8-authored file working too.
 *
 * WHAT THE GAME'S DATA ACTUALLY USES (census of all 242 files, 2026-09-15):
 *   segments   chord track (a key header, then either nothing or one "M"
 *              chord at tick 0), command track (one groove-1 command), tempo
 *              track (60 BPM), style track (a style per bar), band track (one
 *              GM/GS band at time -1), and in SegEgypt2 one mute track;
 *   styles     4/4 with 8 grids per beat (one each of 2 and 6), 475 patterns
 *              with groove ranges 1, 2, 3 and 1-100 (9 of them motifs), 3002
 *              parts, 57,798 notes (48,805 chord-relative, 8,993 fixed), 371
 *              curve chunks (pitch bend, CC7 and channel pressure); no
 *              inversion groups and no humanize ranges.
 * Anything else a DirectMusic file can carry (sequence, signpost, chordmap,
 * motif and marker tracks, DX8 containers) is skipped, not rejected.
 */
#include <stdlib.h>
#include <string.h>

#include "ll_dmusic.h"

/* ---- RIFF walking -------------------------------------------------------- */

#define DM_FCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

typedef struct DmChunk {
    uint32_t             id;
    uint32_t             form;
    const unsigned char* data;
    uint32_t             size;
} DmChunk;

static uint32_t dm_u32(const unsigned char* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t dm_u16(const unsigned char* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static double dm_f64(const unsigned char* p)
{
    double d;
    memcpy(&d, p, 8);            /* both hosts are little-endian IEEE */
    return d;
}

static int dm_next(const unsigned char** cur, const unsigned char* end, DmChunk* c)
{
    const unsigned char* p = *cur;
    uint32_t             size;
    if (end - p < 8)
        return 0;
    c->id = dm_u32(p);
    size = dm_u32(p + 4);
    if (size > (uint32_t)(end - p - 8))
        size = (uint32_t)(end - p - 8);
    c->form = 0;
    c->data = p + 8;
    c->size = size;
    if ((c->id == DM_FCC('L', 'I', 'S', 'T') || c->id == DM_FCC('R', 'I', 'F', 'F')) && size >= 4) {
        c->form = dm_u32(p + 8);
        c->data = p + 12;
        c->size = size - 4;
    }
    p += 8 + size + (size & 1);
    *cur = p > end ? end : p;
    return 1;
}

/* Walk the children of `parent`. */
#define DM_EACH(parent, child)                                           \
    for (const unsigned char *dm_cur_ = (parent)->data,                  \
                             *dm_end_ = (parent)->data + (parent)->size; \
         dm_next(&dm_cur_, dm_end_, (child));)

/* UTF-16LE (NUL-terminated or chunk-bounded) to a lossy 7-bit string. */
static void dm_wide(const unsigned char* p, uint32_t bytes, char* out, size_t out_size)
{
    size_t   n = 0;
    uint32_t i;
    if (!out_size)
        return;
    for (i = 0; i + 1 < bytes && n + 1 < out_size; i += 2) {
        uint16_t ch = dm_u16(p + i);
        if (!ch)
            break;
        out[n++] = ch < 0x80 ? (char)ch : '?';
    }
    out[n] = 0;
}

static const unsigned char kClsChord[16] =   { 0x8b, 0x28, 0xac, 0xd2, 0x9b, 0xb3, 0xd1, 0x11,
                                                0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd };
static const unsigned char kClsCommand[16] = { 0x8c, 0x28, 0xac, 0xd2, 0x9b, 0xb3, 0xd1, 0x11,
                                                0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd };
static const unsigned char kClsTempo[16] =   { 0x85, 0x28, 0xac, 0xd2, 0x9b, 0xb3, 0xd1, 0x11,
                                                0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd };
static const unsigned char kClsStyle[16] =   { 0x8d, 0x28, 0xac, 0xd2, 0x9b, 0xb3, 0xd1, 0x11,
                                                0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd };
static const unsigned char kClsBand[16] =    { 0x94, 0x28, 0xac, 0xd2, 0x9b, 0xb3, 0xd1, 0x11,
                                                0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd };
static const unsigned char kClsMute[16] =    { 0x98, 0x28, 0xac, 0xd2, 0x9b, 0xb3, 0xd1, 0x11,
                                                0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd };

int ll_dm_guid_equal(const LLDmGuid* a, const LLDmGuid* b)
{
    return memcmp(a->b, b->b, 16) == 0;
}

/* ---- identity ------------------------------------------------------------ */

static void dm_unfo_name(const DmChunk* list, char* name, size_t name_size)
{
    DmChunk c;
    DM_EACH(list, &c) {
        if (c.id == DM_FCC('U', 'N', 'A', 'M')) {
            dm_wide(c.data, c.size, name, name_size);
            return;
        }
    }
}

int ll_dm_peek(const unsigned char* data, size_t size, LLDmGuid* guid, int* have_guid,
               char* name, size_t name_size)
{
    const unsigned char* cur = data;
    DmChunk              riff, c;
    int                  kind;

    if (name && name_size)
        name[0] = 0;
    if (have_guid)
        *have_guid = 0;
    if (!data || size < 12 || !dm_next(&cur, data + size, &riff) ||
        riff.id != DM_FCC('R', 'I', 'F', 'F'))
        return LL_DM_KIND_NONE;
    if (riff.form == DM_FCC('D', 'M', 'S', 'G'))
        kind = LL_DM_KIND_SEGMENT;
    else if (riff.form == DM_FCC('D', 'M', 'S', 'T'))
        kind = LL_DM_KIND_STYLE;
    else if (riff.form == DM_FCC('D', 'M', 'B', 'D'))
        kind = LL_DM_KIND_BAND;
    else
        return LL_DM_KIND_NONE;
    DM_EACH(&riff, &c) {
        if (c.id == DM_FCC('g', 'u', 'i', 'd') && c.size >= 16 && guid) {
            memcpy(guid->b, c.data, 16);
            if (have_guid)
                *have_guid = 1;
        } else if (c.form == DM_FCC('U', 'N', 'F', 'O') && name) {
            dm_unfo_name(&c, name, name_size);
        }
    }
    return kind;
}

/* ---- bands --------------------------------------------------------------- */

static int dm_parse_band_body(const DmChunk* riff, LLDmBand* band)
{
    DmChunk c;
    int     cap = 0;
    memset(band, 0, sizeof *band);
    DM_EACH(riff, &c) {
        if (c.form == DM_FCC('U', 'N', 'F', 'O')) {
            dm_unfo_name(&c, band->name, sizeof band->name);
        } else if (c.form == DM_FCC('l', 'b', 'i', 'l')) {
            DmChunk in;
            DM_EACH(&c, &in) {
                DmChunk b;
                if (in.form != DM_FCC('l', 'b', 'i', 'n'))
                    continue;
                DM_EACH(&in, &b) {
                    LLDmInstrument* ins;
                    if (b.id != DM_FCC('b', 'i', 'n', 's') || b.size < 40)
                        continue;
                    if (band->nins == cap) {
                        int             ncap = cap ? cap * 2 : 16;
                        LLDmInstrument* n = (LLDmInstrument*)realloc(band->ins,
                                                                     (size_t)ncap * sizeof *n);
                        if (!n)
                            return -1;
                        band->ins = n;
                        cap = ncap;
                    }
                    ins = &band->ins[band->nins++];
                    ins->patch = dm_u32(b.data);
                    ins->pchannel = dm_u32(b.data + 24);
                    ins->flags = dm_u32(b.data + 28);
                    ins->pan = b.data[32];
                    ins->volume = b.data[33];
                    ins->transpose = (int16_t)dm_u16(b.data + 34);
                    ins->priority = dm_u32(b.data + 36);
                }
            }
        }
    }
    return 0;
}

int ll_dm_parse_band(const unsigned char* data, size_t size, LLDmBand* out)
{
    const unsigned char* cur = data;
    DmChunk              riff;
    memset(out, 0, sizeof *out);
    if (!data || size < 12 || !dm_next(&cur, data + size, &riff) ||
        riff.id != DM_FCC('R', 'I', 'F', 'F') || riff.form != DM_FCC('D', 'M', 'B', 'D'))
        return -1;
    return dm_parse_band_body(&riff, out);
}

void ll_dm_free_band(LLDmBand* band)
{
    if (!band)
        return;
    free(band->ins);
    band->ins = 0;
    band->nins = 0;
}

/* ---- styles -------------------------------------------------------------- */

static void dm_timesig(const unsigned char* p, LLDmTimeSig* ts)
{
    ts->beats_per_measure = p[0];
    ts->beat = p[1];
    ts->grids_per_beat = dm_u16(p + 2);
    if (!ts->beats_per_measure)
        ts->beats_per_measure = 4;
    if (!ts->grids_per_beat)
        ts->grids_per_beat = 1;
}

static int dm_parse_part(const DmChunk* list, LLDmPart* part)
{
    DmChunk c;
    memset(part, 0, sizeof *part);
    part->ts.beats_per_measure = 4;
    part->ts.beat = 4;
    part->ts.grids_per_beat = 4;
    part->measures = 1;
    DM_EACH(list, &c) {
        if (c.id == DM_FCC('p', 'r', 't', 'h') && c.size >= 153) {
            int i;
            dm_timesig(c.data, &part->ts);
            for (i = 0; i < 32; i++)
                part->var_choices[i] = dm_u32(c.data + 4 + i * 4);
            memcpy(part->guid.b, c.data + 132, 16);
            part->measures = dm_u16(c.data + 148);
            part->playmode = c.data[150];
            part->invert_hi = c.data[151];
            part->invert_lo = c.data[152];
            if (!part->measures)
                part->measures = 1;
        } else if (c.id == DM_FCC('n', 'o', 't', 'e') && c.size >= 4) {
            uint32_t cb = dm_u32(c.data);
            uint32_t n, i;
            if (cb < 22)
                continue;
            n = (c.size - 4) / cb;
            part->notes = (LLDmNote*)calloc(n ? n : 1, sizeof(LLDmNote));
            if (!part->notes)
                return -1;
            for (i = 0; i < n; i++) {
                const unsigned char* p = c.data + 4 + i * cb;
                LLDmNote*            note = &part->notes[i];
                note->grid = (int32_t)dm_u32(p);
                note->variation = dm_u32(p + 4);
                note->duration = (int32_t)dm_u32(p + 8);
                note->offset = (int16_t)dm_u16(p + 12);
                note->music_value = dm_u16(p + 14);
                note->velocity = p[16];
                note->time_range = p[17];
                note->dur_range = p[18];
                note->vel_range = p[19];
                note->inversion = p[20];
                note->playmode = p[21];
            }
            part->nnotes = (int)n;
        } else if (c.id == DM_FCC('c', 'r', 'v', 'e') && c.size >= 4) {
            uint32_t cb = dm_u32(c.data);
            uint32_t n, i;
            if (cb < 28)
                continue;
            n = (c.size - 4) / cb;
            part->curves = (LLDmCurve*)calloc(n ? n : 1, sizeof(LLDmCurve));
            if (!part->curves)
                return -1;
            for (i = 0; i < n; i++) {
                const unsigned char* p = c.data + 4 + i * cb;
                LLDmCurve*           cv = &part->curves[i];
                cv->grid = (int32_t)dm_u32(p);
                cv->variation = dm_u32(p + 4);
                cv->duration = (int32_t)dm_u32(p + 8);
                cv->reset_duration = (int32_t)dm_u32(p + 12);
                cv->offset = (int16_t)dm_u16(p + 16);
                cv->start = (int16_t)dm_u16(p + 18);
                cv->end = (int16_t)dm_u16(p + 20);
                cv->reset = (int16_t)dm_u16(p + 22);
                cv->type = p[24];
                cv->shape = p[25];
                cv->cc = p[26];
                cv->flags = p[27];
            }
            part->ncurves = (int)n;
        }
    }
    return 0;
}

static int dm_parse_pattern(const DmChunk* list, LLDmPattern* pat)
{
    DmChunk c;
    int     cap = 0;
    memset(pat, 0, sizeof *pat);
    pat->ts.beats_per_measure = 4;
    pat->ts.beat = 4;
    pat->ts.grids_per_beat = 4;
    pat->groove_lo = 1;
    pat->groove_hi = 100;
    pat->measures = 1;
    DM_EACH(list, &c) {
        if (c.id == DM_FCC('p', 't', 'n', 'h') && c.size >= 10) {
            dm_timesig(c.data, &pat->ts);
            pat->groove_lo = c.data[4];
            pat->groove_hi = c.data[5];
            pat->embellishment = dm_u16(c.data + 6);
            pat->measures = dm_u16(c.data + 8);
            if (!pat->measures)
                pat->measures = 1;
        } else if (c.form == DM_FCC('U', 'N', 'F', 'O')) {
            dm_unfo_name(&c, pat->name, sizeof pat->name);
        } else if (c.form == DM_FCC('p', 'r', 'e', 'f')) {
            DmChunk r;
            DM_EACH(&c, &r) {
                LLDmPartRef* ref;
                if (r.id != DM_FCC('p', 'r', 'f', 'c') || r.size < 22)
                    continue;
                if (pat->nrefs == cap) {
                    int          ncap = cap ? cap * 2 : 16;
                    LLDmPartRef* n = (LLDmPartRef*)realloc(pat->refs, (size_t)ncap * sizeof *n);
                    if (!n)
                        return -1;
                    pat->refs = n;
                    cap = ncap;
                }
                ref = &pat->refs[pat->nrefs++];
                memcpy(ref->guid.b, r.data, 16);
                ref->part = -1;
                ref->pchannel = dm_u16(r.data + 16);
                ref->lock = r.data[18];
                ref->subchord = r.data[19];
                ref->priority = r.data[20];
                ref->random = r.data[21];
                /* DirectX 8 widened the PChannel to a DWORD at +24. */
                if (r.size >= 28)
                    ref->pchannel = (uint16_t)dm_u32(r.data + 24);
            }
        }
    }
    return 0;
}

int ll_dm_parse_style(const unsigned char* data, size_t size, LLDmStyle* out)
{
    const unsigned char* cur = data;
    DmChunk              riff, c;
    int                  cap_parts = 0, cap_pats = 0, cap_bands = 0;
    int                  i, j, k;

    memset(out, 0, sizeof *out);
    out->ts.beats_per_measure = 4;
    out->ts.beat = 4;
    out->ts.grids_per_beat = 4;
    out->tempo = 120.0;
    if (!data || size < 12 || !dm_next(&cur, data + size, &riff) ||
        riff.id != DM_FCC('R', 'I', 'F', 'F') || riff.form != DM_FCC('D', 'M', 'S', 'T'))
        return -1;

    DM_EACH(&riff, &c) {
        if (c.id == DM_FCC('s', 't', 'y', 'h') && c.size >= 12) {
            dm_timesig(c.data, &out->ts);
            out->tempo = dm_f64(c.data + 4);
        } else if (c.id == DM_FCC('g', 'u', 'i', 'd') && c.size >= 16) {
            memcpy(out->guid.b, c.data, 16);
        } else if (c.form == DM_FCC('U', 'N', 'F', 'O')) {
            dm_unfo_name(&c, out->name, sizeof out->name);
        } else if (c.id == DM_FCC('R', 'I', 'F', 'F') && c.form == DM_FCC('D', 'M', 'B', 'D')) {
            if (out->nbands == cap_bands) {
                int       ncap = cap_bands ? cap_bands * 2 : 2;
                LLDmBand* n = (LLDmBand*)realloc(out->bands, (size_t)ncap * sizeof *n);
                if (!n)
                    goto fail;
                out->bands = n;
                cap_bands = ncap;
            }
            if (dm_parse_band_body(&c, &out->bands[out->nbands++]) != 0)
                goto fail;
        } else if (c.form == DM_FCC('p', 'a', 'r', 't')) {
            if (out->nparts == cap_parts) {
                int       ncap = cap_parts ? cap_parts * 2 : 16;
                LLDmPart* n = (LLDmPart*)realloc(out->parts, (size_t)ncap * sizeof *n);
                if (!n)
                    goto fail;
                out->parts = n;
                cap_parts = ncap;
            }
            if (dm_parse_part(&c, &out->parts[out->nparts++]) != 0)
                goto fail;
        } else if (c.form == DM_FCC('p', 't', 't', 'n')) {
            if (out->npatterns == cap_pats) {
                int          ncap = cap_pats ? cap_pats * 2 : 4;
                LLDmPattern* n = (LLDmPattern*)realloc(out->patterns, (size_t)ncap * sizeof *n);
                if (!n)
                    goto fail;
                out->patterns = n;
                cap_pats = ncap;
            }
            if (dm_parse_pattern(&c, &out->patterns[out->npatterns++]) != 0)
                goto fail;
        }
    }

    /* Part references to part indices. */
    for (i = 0; i < out->npatterns; i++) {
        for (j = 0; j < out->patterns[i].nrefs; j++) {
            LLDmPartRef* ref = &out->patterns[i].refs[j];
            for (k = 0; k < out->nparts; k++) {
                if (ll_dm_guid_equal(&ref->guid, &out->parts[k].guid)) {
                    ref->part = k;
                    break;
                }
            }
        }
    }
    return 0;

fail:
    ll_dm_free_style(out);
    return -1;
}

void ll_dm_free_style(LLDmStyle* sty)
{
    int i;
    if (!sty)
        return;
    for (i = 0; i < sty->nparts; i++) {
        free(sty->parts[i].notes);
        free(sty->parts[i].curves);
    }
    for (i = 0; i < sty->npatterns; i++)
        free(sty->patterns[i].refs);
    for (i = 0; i < sty->nbands; i++)
        ll_dm_free_band(&sty->bands[i]);
    free(sty->parts);
    free(sty->patterns);
    free(sty->bands);
    memset(sty, 0, sizeof *sty);
}

/* ---- segments ------------------------------------------------------------ */

static void dm_parse_ref(const DmChunk* list, LLDmStyleRef* ref)
{
    DmChunk c;
    DM_EACH(list, &c) {
        if (c.id == DM_FCC('g', 'u', 'i', 'd') && c.size >= 16) {
            memcpy(ref->guid.b, c.data, 16);
            ref->have_guid = 1;
        } else if (c.id == DM_FCC('n', 'a', 'm', 'e')) {
            dm_wide(c.data, c.size, ref->name, sizeof ref->name);
        } else if (c.id == DM_FCC('f', 'i', 'l', 'e')) {
            dm_wide(c.data, c.size, ref->file, sizeof ref->file);
        }
    }
}

static int dm_grow(void** arr, int* cap, int count, size_t elem)
{
    if (count < *cap)
        return 0;
    {
        int   ncap = *cap ? *cap * 2 : 8;
        void* n = realloc(*arr, (size_t)ncap * elem);
        if (!n)
            return -1;
        *arr = n;
        *cap = ncap;
    }
    return 0;
}

static int dm_parse_track(const DmChunk* dmtk, LLDmSegmentData* seg, int* caps)
{
    DmChunk              c;
    const unsigned char* cls = 0;
    uint32_t             ckid = 0, fcc = 0;

    DM_EACH(dmtk, &c) {
        if (c.id == DM_FCC('t', 'r', 'k', 'h') && c.size >= 32) {
            cls = c.data;
            ckid = dm_u32(c.data + 24);
            fcc = dm_u32(c.data + 28);
            continue;
        }
        if (!cls)
            continue;
        if (!(ckid && c.id == ckid) && !(fcc && c.form == fcc))
            continue;

        if (memcmp(cls, kClsTempo, 16) == 0 && c.size >= 4) {
            uint32_t cb = dm_u32(c.data), i, n;
            if (cb < 16)
                continue;
            n = (c.size - 4) / cb;
            for (i = 0; i < n; i++) {
                const unsigned char* p = c.data + 4 + i * cb;
                if (dm_grow((void**)&seg->tempos, &caps[0], seg->ntempos, sizeof *seg->tempos))
                    return -1;
                seg->tempos[seg->ntempos].time = (int32_t)dm_u32(p);
                seg->tempos[seg->ntempos].bpm = dm_f64(p + 8);
                seg->ntempos++;
            }
        } else if (memcmp(cls, kClsCommand, 16) == 0 && c.size >= 4) {
            uint32_t cb = dm_u32(c.data), i, n;
            if (cb < 9)
                continue;
            n = (c.size - 4) / cb;
            for (i = 0; i < n; i++) {
                const unsigned char* p = c.data + 4 + i * cb;
                if (dm_grow((void**)&seg->commands, &caps[1], seg->ncommands, sizeof *seg->commands))
                    return -1;
                seg->commands[seg->ncommands].time = (int32_t)dm_u32(p);
                seg->commands[seg->ncommands].command = p[7];
                seg->commands[seg->ncommands].groove = p[8];
                seg->commands[seg->ncommands].groove_range = cb >= 10 ? p[9] : 0;
                seg->ncommands++;
            }
        } else if (memcmp(cls, kClsChord, 16) == 0) {
            DmChunk k;
            DM_EACH(&c, &k) {
                if (k.id == DM_FCC('c', 'r', 'd', 'h') && k.size >= 4) {
                    seg->key = dm_u32(k.data);
                    seg->have_key = 1;
                } else if (k.id == DM_FCC('c', 'r', 'd', 'b') && k.size >= 4) {
                    uint32_t   cb = dm_u32(k.data), nsub, cbsub, i;
                    LLDmChord* ch;
                    if (cb < 36 || 4 + cb + 8 > k.size)
                        continue;
                    if (dm_grow((void**)&seg->chords, &caps[2], seg->nchords, sizeof *seg->chords))
                        return -1;
                    ch = &seg->chords[seg->nchords++];
                    memset(ch, 0, sizeof *ch);
                    dm_wide(k.data + 4, 32, ch->name, sizeof ch->name);
                    ch->time = (int32_t)dm_u32(k.data + 36);
                    nsub = dm_u32(k.data + 4 + cb);
                    cbsub = dm_u32(k.data + 8 + cb);
                    if (cbsub < 18)
                        nsub = 0;
                    for (i = 0; i < nsub && i < LL_DM_MAX_SUBCHORDS; i++) {
                        const unsigned char* s = k.data + 12 + cb + i * cbsub;
                        if (s + 18 > k.data + k.size)
                            break;
                        ch->sub[i].chord_pattern = dm_u32(s);
                        ch->sub[i].scale_pattern = dm_u32(s + 4);
                        ch->sub[i].inversion_points = dm_u32(s + 8);
                        ch->sub[i].levels = dm_u32(s + 12);
                        ch->sub[i].chord_root = s[16];
                        ch->sub[i].scale_root = s[17];
                        ch->nsub++;
                    }
                }
            }
        } else if (memcmp(cls, kClsStyle, 16) == 0) {
            DmChunk f;
            DM_EACH(&c, &f) {
                DmChunk       e;
                LLDmStyleRef  ref;
                int           have = 0;
                if (f.form != DM_FCC('s', 't', 'r', 'f'))
                    continue;
                memset(&ref, 0, sizeof ref);
                DM_EACH(&f, &e) {
                    if (e.id == DM_FCC('s', 't', 'm', 'p') && e.size >= 4) {
                        ref.time = (int32_t)dm_u32(e.data);
                    } else if (e.form == DM_FCC('D', 'M', 'R', 'F')) {
                        dm_parse_ref(&e, &ref);
                        have = 1;
                    }
                }
                if (!have)
                    continue;
                if (dm_grow((void**)&seg->styles, &caps[3], seg->nstyles, sizeof *seg->styles))
                    return -1;
                seg->styles[seg->nstyles++] = ref;
            }
        } else if (memcmp(cls, kClsBand, 16) == 0) {
            DmChunk l;
            DM_EACH(&c, &l) {
                DmChunk b;
                if (l.form != DM_FCC('l', 'b', 'd', 'l'))
                    continue;
                DM_EACH(&l, &b) {
                    DmChunk       e;
                    LLDmBandItem  item;
                    int           have = 0;
                    if (b.form != DM_FCC('l', 'b', 'n', 'd'))
                        continue;
                    memset(&item, 0, sizeof item);
                    DM_EACH(&b, &e) {
                        if (e.id == DM_FCC('b', 'd', 'i', 'h') && e.size >= 4) {
                            item.time = (int32_t)dm_u32(e.data);
                        } else if (e.id == DM_FCC('b', 'd', '2', 'h') && e.size >= 4) {
                            item.time = (int32_t)dm_u32(e.data);
                        } else if (e.id == DM_FCC('R', 'I', 'F', 'F') &&
                                   e.form == DM_FCC('D', 'M', 'B', 'D')) {
                            if (dm_parse_band_body(&e, &item.band) != 0)
                                return -1;
                            have = 1;
                        }
                    }
                    if (!have)
                        continue;
                    if (dm_grow((void**)&seg->bands, &caps[4], seg->nbands, sizeof *seg->bands)) {
                        ll_dm_free_band(&item.band);
                        return -1;
                    }
                    seg->bands[seg->nbands++] = item;
                }
            }
        } else if (memcmp(cls, kClsMute, 16) == 0 && c.size >= 4) {
            uint32_t cb = dm_u32(c.data), i, n;
            if (cb < 12)
                continue;
            n = (c.size - 4) / cb;
            for (i = 0; i < n; i++) {
                const unsigned char* p = c.data + 4 + i * cb;
                if (dm_grow((void**)&seg->mutes, &caps[5], seg->nmutes, sizeof *seg->mutes))
                    return -1;
                seg->mutes[seg->nmutes].time = (int32_t)dm_u32(p);
                seg->mutes[seg->nmutes].pchannel = dm_u32(p + 4);
                seg->mutes[seg->nmutes].map = dm_u32(p + 8);
                seg->nmutes++;
            }
        }
    }
    return 0;
}

/* Stable insertion sort by the int32 time every track item starts with. */
#define DM_SORT_BY_TIME(arr, n, type)                                \
    do {                                                             \
        int i_, j_;                                                  \
        for (i_ = 1; i_ < (n); i_++) {                               \
            type t_ = (arr)[i_];                                     \
            for (j_ = i_; j_ > 0 && (arr)[j_ - 1].time > t_.time; j_--) \
                (arr)[j_] = (arr)[j_ - 1];                           \
            (arr)[j_] = t_;                                          \
        }                                                            \
    } while (0)

int ll_dm_parse_segment(const unsigned char* data, size_t size, LLDmSegmentData* out)
{
    const unsigned char* cur = data;
    DmChunk              riff, c;
    int                  caps[6] = { 0, 0, 0, 0, 0, 0 };

    memset(out, 0, sizeof *out);
    if (!data || size < 12 || !dm_next(&cur, data + size, &riff) ||
        riff.id != DM_FCC('R', 'I', 'F', 'F') || riff.form != DM_FCC('D', 'M', 'S', 'G'))
        return -1;

    DM_EACH(&riff, &c) {
        if (c.id == DM_FCC('s', 'e', 'g', 'h') && c.size >= 24) {
            out->repeats = dm_u32(c.data);
            out->length = (int32_t)dm_u32(c.data + 4);
            out->play_start = (int32_t)dm_u32(c.data + 8);
            out->loop_start = (int32_t)dm_u32(c.data + 12);
            out->loop_end = (int32_t)dm_u32(c.data + 16);
            out->resolution = dm_u32(c.data + 20);
        } else if (c.id == DM_FCC('g', 'u', 'i', 'd') && c.size >= 16) {
            memcpy(out->guid.b, c.data, 16);
        } else if (c.form == DM_FCC('U', 'N', 'F', 'O')) {
            dm_unfo_name(&c, out->name, sizeof out->name);
        } else if (c.form == DM_FCC('t', 'r', 'k', 'l')) {
            DmChunk t;
            DM_EACH(&c, &t) {
                if (t.id == DM_FCC('R', 'I', 'F', 'F') && t.form == DM_FCC('D', 'M', 'T', 'K')) {
                    if (dm_parse_track(&t, out, caps) != 0) {
                        ll_dm_free_segment(out);
                        return -1;
                    }
                }
            }
        }
    }
    DM_SORT_BY_TIME(out->tempos, out->ntempos, LLDmTempo);
    DM_SORT_BY_TIME(out->commands, out->ncommands, LLDmCommand);
    DM_SORT_BY_TIME(out->chords, out->nchords, LLDmChord);
    DM_SORT_BY_TIME(out->styles, out->nstyles, LLDmStyleRef);
    DM_SORT_BY_TIME(out->bands, out->nbands, LLDmBandItem);
    DM_SORT_BY_TIME(out->mutes, out->nmutes, LLDmMute);
    if (out->length <= 0)
        out->length = LL_DM_PPQ * 4;
    return 0;
}

void ll_dm_free_segment(LLDmSegmentData* seg)
{
    int i;
    if (!seg)
        return;
    for (i = 0; i < seg->nbands; i++)
        ll_dm_free_band(&seg->bands[i].band);
    free(seg->chords);
    free(seg->commands);
    free(seg->tempos);
    free(seg->styles);
    free(seg->bands);
    free(seg->mutes);
    memset(seg, 0, sizeof *seg);
}

int ll_dm_segment_resolve(LLDmSegmentData* seg, LLDmResolveStyle resolve, void* ctx)
{
    int i, missing = 0;
    for (i = 0; i < seg->nstyles; i++) {
        LLDmStyleRef* ref = &seg->styles[i];
        if (!ref->style && resolve)
            ref->style = resolve(ctx, ref, &ref->owner);
        if (!ref->style)
            missing++;
    }
    return missing;
}
