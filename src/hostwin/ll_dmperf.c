/* LEGOLAND portable build -- the DirectMusic performance (the DirectMusic
 * lane): segment states, the style engine, and the notifications
 * musicthread.c's state machine runs on.
 *
 * WHAT MUSICTHREAD NEEDS FROM A PERFORMANCE (musicthread.c 0x00492db0)
 * -------------------------------------------------------------------------
 *   PlaySegment(seg, DMUS_SEGF_MEASURE)   start at the next bar line of the
 *                                         primary segment now playing, cutting
 *                                         it there; at once if nothing plays
 *   SetGlobalParam(GrooveLevel, 0 or 1)   the master groove modifier: +1 while a
 *                                         world change is pending, which picks
 *                                         the styles' groove-2 "t1" patterns
 *   SEGMENT notifications                 2 "almost end" chains a world's
 *                                         segments; 4 "abort" means the
 *                                         transition has taken over
 *   MEASUREANDBEAT notifications          field1 = beat, field2 = measure since
 *                                         the segment started; the transition
 *                                         is queued on a downbeat and the new
 *                                         world on beat 3 of its measure 1
 *   Stop(all)                             the :STOP cheat and PlayMovie
 *
 * TIME
 * -------------------------------------------------------------------------
 * The render head `tick` is performance music time (768 per quarter note) and
 * advances by the current tempo. Every event carries its tick; the render loop
 * renders exactly up to the next one, so events land on their sample. A queued
 * segment asks for "the next bar line strictly after the head", which is what
 * DirectMusic's queue time (the head plus latency) produces: queued on a
 * downbeat, it starts one bar later. DMUS_NOTIFICATION_SEGALMOSTEND is sent at
 * the end minus the prepare time (DirectMusic's default, 1000 ms).
 *
 * THE STYLE ENGINE, ONE BAR AT A TIME
 * -------------------------------------------------------------------------
 * At each bar line of the primary segment:
 *   1. the style is the last style-track reference at or before the bar;
 *   2. beat notifications for the bar are queued;
 *   3. if no pattern is still running, a pattern is chosen: the groove is the
 *      command track's plus the master modifier (clamped 1..100); patterns
 *      whose range holds it are candidates, else those at the nearest range --
 *      the fallback that keeps the transitions audible, since the game plays
 *      them at groove 2 and they only have groove-1 patterns -- and one is
 *      picked at random; motifs (embellishment 16) never play here;
 *   4. each part reference picks a variation from the part's enabled ones
 *      (random, or sequential when bRandomVariation is 0; parts sharing a
 *      variation lock id share the pick), and its notes and curves for the
 *      whole pattern are queued. A part shorter than its pattern repeats.
 *
 * NOTES: MUSIC VALUES
 * -------------------------------------------------------------------------
 * 48,805 of the game's notes are music values (octave, chord position, scale
 * position, accidental) played in DMUS_PLAYMODE_ALWAYSPLAY. ll_dm_music_to_midi
 * is DirectMusic's MusicToMIDI as GothicKit's dmusic (MIT) recovered it, whose
 * one surprising rule decides these tunes: a chord position past the chord's
 * last tone walks the SCALE, 2 * position + scale position steps from the root,
 * so position 3 on the "M" triad is B -- the major seventh the notes were
 * written against. The census proves the authoring chord: under C major 7 the
 * twelve (position, step, accidental) triples in the data spell the twelve
 * pitch classes exactly once each. Segments with an empty chord track play
 * against C major 7 on the track header's key (root 12, C major scale); every
 * chord that could stand there gives the same pitches for these values except
 * C6, which the census rules out.
 *
 * Nothing here knows about COM (ll_dmusic.c) or the synth's insides (ll_dls.c).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ll_dmusic.h"

#define DM_EPS             1e-6
#define DM_PREPARE_MS      1000.0
#define DM_CURVE_STEP      8          /* ticks between curve points (~10 ms at 60 BPM) */
#define DM_MAX_NOTIFY      256

#define DMUS_PLAYMODE_KEY_ROOT        0x01
#define DMUS_PLAYMODE_CHORD_ROOT      0x02
#define DMUS_PLAYMODE_SCALE_INTERVALS 0x04
#define DMUS_PLAYMODE_CHORD_INTERVALS 0x08
#define DMUS_PLAYMODE_NONE            0x10

enum {
    EV_NOTE_OFF = 1,
    EV_TEMPO,
    EV_BAND,
    EV_NOTIFY,
    EV_CC,
    EV_BEND,
    EV_CURVE_RESET,
    EV_NOTE_ON
};

/* Same-tick order: tempo and bands, notifications, note-offs, controllers,
 * note-ons -- so a retriggered key is released before it is struck again. */
static int ev_prio(int kind)
{
    switch (kind) {
    case EV_TEMPO: case EV_BAND: return 0;
    case EV_NOTIFY:              return 1;
    case EV_NOTE_OFF:            return 2;
    case EV_CC: case EV_BEND: case EV_CURVE_RESET: return 3;
    default:                     return 4;
    }
}

typedef struct DmEvent {
    double          tick;
    uint32_t        seq;
    uint8_t         kind;
    uint8_t         prio;
    uint8_t         ch;
    uint8_t         first;       /* EV_CC/EV_BEND: the curve's first point */
    int             state;       /* owning segment state, 0 = survives cuts */
    int             a, b, c;
    double          d;           /* curve start tick, or tempo */
    const LLDmBand* band;
    void*           owner;
} DmEvent;

typedef struct DmState {
    int                    id;
    const LLDmSegmentData* seg;
    void*                  owner;
    uint32_t               repeats_left;
    double                 start_tick;
    double                 end_tick;
    double                 pass_tick0;   /* tick where this pass began */
    int32_t                pass_seg0;    /* segment time at pass_tick0 */
    int32_t                pass_end;     /* segment time where this pass ends */
    int                    pass_loops;   /* this pass ends in a loop */
    int32_t                next_bar;     /* segment time of the next bar line */
    int32_t                inst_end;     /* segment time the running pattern ends */
    int                    measure;      /* bars since the state started */
    uint32_t               mute_map[16]; /* live PChannel map, 0xffffffff muted */
    const LLDmStyle*       style;
    const LLDmPattern*     pattern;
} DmState;

typedef struct DmSeqCounter {
    const LLDmPattern* pattern;
    uint32_t           count;
} DmSeqCounter;

struct LLDmPerf {
    double    rate;
    LLSynth*  synth;
    double    tick;
    double    tempo;
    uint32_t  rng;
    int       groove_mod;
    int       notify_segment;
    int       notify_beat;

    DmState*  primary;
    DmState*  pending;
    int       next_id;

    DmEvent*  heap;
    int       nheap;
    int       cap_heap;
    uint32_t  seq;

    LLDmNotify notes[DM_MAX_NOTIFY];
    int        note_head;
    int        note_count;
    int        notify_pending;
    void     (*notify_hook)(void*);
    void*      notify_ctx;

    void     (*retain)(void*);
    void     (*release)(void*);

    int        transpose[16];
    double     curve_start[16][129];   /* per channel: CC 0..127, 128 = pitch bend */

    DmSeqCounter seqs[64];
    int          nseqs;

    LLDmTraceFn trace;
    void*       trace_ctx;

    LLDmPerfStats stats;
    int           last_beat;
    int           last_measure;
};

/* ---- small helpers ------------------------------------------------------- */

static uint32_t dm_rand(LLDmPerf* p)
{
    uint32_t x = p->rng ? p->rng : 0x2545f491u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    p->rng = x;
    return x;
}

static int dm_popcount(uint32_t v)
{
    int n = 0;
    while (v) {
        n += (int)(v & 1u);
        v >>= 1;
    }
    return n;
}

static int32_t dm_beat_ticks(const LLDmTimeSig* ts)
{
    int beat = ts->beat ? ts->beat : 256;
    int32_t t = (LL_DM_PPQ * 4) / beat;
    return t > 0 ? t : 1;
}

static int32_t dm_bar_ticks(const LLDmTimeSig* ts)
{
    int32_t t = dm_beat_ticks(ts) * (ts->beats_per_measure ? ts->beats_per_measure : 4);
    return t > 0 ? t : 1;
}

static void dm_trace(LLDmPerf* p, double tick, int ch, int kind, int a, int b)
{
    if (p->trace)
        p->trace(p->trace_ctx, tick, ch, kind, a, b);
}

/* ---- the event heap ------------------------------------------------------ */

static int ev_before(const DmEvent* x, const DmEvent* y)
{
    if (x->tick != y->tick)
        return x->tick < y->tick;
    if (x->prio != y->prio)
        return x->prio < y->prio;
    return x->seq < y->seq;
}

static void heap_up(LLDmPerf* p, int i)
{
    while (i > 0) {
        int     parent = (i - 1) / 2;
        DmEvent t;
        if (!ev_before(&p->heap[i], &p->heap[parent]))
            break;
        t = p->heap[i];
        p->heap[i] = p->heap[parent];
        p->heap[parent] = t;
        i = parent;
    }
}

static void heap_down(LLDmPerf* p, int i)
{
    for (;;) {
        int     l = 2 * i + 1, r = l + 1, m = i;
        DmEvent t;
        if (l < p->nheap && ev_before(&p->heap[l], &p->heap[m]))
            m = l;
        if (r < p->nheap && ev_before(&p->heap[r], &p->heap[m]))
            m = r;
        if (m == i)
            break;
        t = p->heap[i];
        p->heap[i] = p->heap[m];
        p->heap[m] = t;
        i = m;
    }
}

static void heap_push(LLDmPerf* p, DmEvent* e)
{
    if (p->nheap == p->cap_heap) {
        int      cap = p->cap_heap ? p->cap_heap * 2 : 1024;
        DmEvent* n = (DmEvent*)realloc(p->heap, (size_t)cap * sizeof *n);
        if (!n)
            return;
        p->heap = n;
        p->cap_heap = cap;
    }
    e->seq = p->seq++;
    e->prio = (uint8_t)ev_prio(e->kind);
    p->heap[p->nheap] = *e;
    heap_up(p, p->nheap++);
}

static void heap_pop(LLDmPerf* p, DmEvent* out)
{
    *out = p->heap[0];
    p->heap[0] = p->heap[--p->nheap];
    if (p->nheap)
        heap_down(p, 0);
}

static void heap_rebuild(LLDmPerf* p)
{
    int i;
    for (i = p->nheap / 2 - 1; i >= 0; i--)
        heap_down(p, i);
}

/* ---- notifications ------------------------------------------------------- */

static void notify_push(LLDmPerf* p, int kind, int option, int f1, int f2, double tick,
                        void* owner)
{
    LLDmNotify* n;
    if (kind == 0 && !p->notify_segment)
        return;
    if (kind == 1 && !p->notify_beat)
        return;
    if (p->note_count == DM_MAX_NOTIFY) {       /* nobody is reading: drop the oldest */
        p->note_head = (p->note_head + 1) % DM_MAX_NOTIFY;
        p->note_count--;
    }
    n = &p->notes[(p->note_head + p->note_count) % DM_MAX_NOTIFY];
    n->kind = kind;
    n->option = option;
    n->field1 = f1;
    n->field2 = f2;
    n->tick = tick;
    n->segment = owner;
    p->note_count++;
    p->notify_pending = 1;
    p->stats.notifications++;
}

int ll_dmperf_pop_notification(LLDmPerf* p, LLDmNotify* out)
{
    if (!p || !p->note_count)
        return 0;
    *out = p->notes[p->note_head];
    p->note_head = (p->note_head + 1) % DM_MAX_NOTIFY;
    p->note_count--;
    return 1;
}

/* ---- MusicToMIDI --------------------------------------------------------- */

static uint32_t dm_scale_fixup(uint32_t scale, uint8_t scale_root)
{
    static const uint32_t fallback[12] = {
        0xab5ab5, 0x6ad6ad, 0x5ab5ab, 0xad5ad5, 0x6b56b5, 0x5ad5ad,
        0x56b56b, 0xd5ad5a, 0xb56b56, 0xd6ad6a, 0xb5ab5a, 0xad6ad6,
    };
    /* Two octaves of the scale, rotated onto its root. */
    scale = (scale & 0x0fffu) | (scale << 12);
    scale >>= 12 - (scale_root % 12);
    scale = (scale & 0x0fffu) | (scale << 12);
    /* A "scale" of four notes or fewer is replaced by the closest real one. */
    if (dm_popcount(scale & 0x0fffu) <= 4) {
        uint32_t best = fallback[0];
        int      best_score = 0, i;
        for (i = 0; i < 12; i++) {
            int score = dm_popcount(fallback[i] & scale & 0x0fffu);
            if (score > best_score) {
                best = fallback[i];
                best_score = score;
            }
        }
        scale = best;
    }
    if (!(scale & 0xff000000u))
        scale |= (scale & 0x00fff000u) << 12;
    return scale;
}

int ll_dm_music_to_midi(const LLDmSubChord* chord, uint8_t mode, uint16_t value)
{
    int      offset = 0;
    int      root = 0, root_oct, chord_pos, scale_pos, acc, bits;
    int      nv = 0, noff, npos, octave;
    uint32_t scale, cpat, pat;

    if (mode == 0)
        return value & 0x7f;
    /* Octaves 14 and 15 are -2 and -1. */
    while (value >= 0xE000) {
        value = (uint16_t)(value + 0x1000);
        offset -= 12;
    }
    /* Scale positions 9..15 are -7..-1: seven steps up, an octave down. */
    {
        uint16_t t = (uint16_t)((value & 0x00F0) + 0x0070);
        if (t & 0x0F00) {
            value = (uint16_t)((value & 0xFF0F) | (t & 0x00F0));
            offset -= 12;
        }
    }
    if (mode & DMUS_PLAYMODE_CHORD_ROOT)
        root = chord->chord_root;
    else if (mode & DMUS_PLAYMODE_KEY_ROOT)
        root = chord->scale_root;
    if (!(mode & (DMUS_PLAYMODE_CHORD_INTERVALS | DMUS_PLAYMODE_SCALE_INTERVALS)))
        return (value & 0x7f) + root % 12;

    scale = dm_scale_fixup(chord->scale_pattern ? chord->scale_pattern : 0xab5ab5u,
                           chord->scale_root);
    cpat = chord->chord_pattern ? chord->chord_pattern : 1u;
    chord_pos = (value >> 8) & 0x0F;
    scale_pos = (value >> 4) & 0x07;
    acc = value & 0x0F;
    if (acc > 8)
        acc -= 16;
    root_oct = root % 12;
    bits = dm_popcount(cpat);

    if ((mode & DMUS_PLAYMODE_CHORD_INTERVALS) && scale_pos == 0 && chord_pos < bits) {
        /* A chord tone: walk the chord pattern. */
        noff = root + acc;
        pat = cpat;
        npos = chord_pos;
    } else if ((mode & DMUS_PLAYMODE_CHORD_INTERVALS) && chord_pos < bits) {
        /* A chord tone, then scale steps above it. */
        pat = cpat;
        npos = chord_pos;
        while (pat && !(pat & 1u)) {
            pat >>= 1;
            nv++;
        }
        while (npos > 0) {
            pat >>= 1;
            nv++;
            if (pat & 1u)
                npos--;
            if (!pat) {
                nv += npos;
                break;
            }
        }
        nv += root_oct;
        noff = acc + root - root_oct;
        pat = scale >> (nv % 12);
        npos = scale_pos;
    } else if (mode & DMUS_PLAYMODE_SCALE_INTERVALS) {
        /* Past the chord's last tone (or no chord intervals): stacked thirds
         * in the scale. */
        nv = root_oct;
        noff = acc + root - root_oct;
        pat = scale >> root_oct;
        npos = chord_pos * 2 + scale_pos;
    } else {
        return -1;
    }

    for (npos += 1; npos > 0; pat >>= 1) {
        nv++;
        if (pat & 1u)
            npos--;
        if (!pat) {
            nv += npos;
            break;
        }
    }
    nv = nv - 1 + noff + offset;
    octave = (value >> 12) & 0x0F;
    if (mode & DMUS_PLAYMODE_CHORD_ROOT)
        return octave * 12 + nv - 12;
    return octave * 12 + nv;
}

/* ---- segment-track lookups ----------------------------------------------- */

static const LLDmStyle* dm_style_at(const LLDmSegmentData* seg, int32_t t)
{
    const LLDmStyle* sty = 0;
    int              i;
    for (i = 0; i < seg->nstyles; i++) {
        if (seg->styles[i].time > t && sty)
            break;
        if (seg->styles[i].style)
            sty = seg->styles[i].style;
    }
    return sty;
}

static const LLDmCommand* dm_command_at(const LLDmSegmentData* seg, int32_t t)
{
    const LLDmCommand* c = 0;
    int                i;
    for (i = 0; i < seg->ncommands; i++) {
        if (seg->commands[i].time > t)
            break;
        c = &seg->commands[i];
    }
    return c;
}

static const LLDmSubChord* dm_subchord_at(const LLDmSegmentData* seg, int32_t t, int level,
                                          LLDmSubChord* tmp)
{
    const LLDmChord* ch = 0;
    int              i;
    for (i = 0; i < seg->nchords; i++) {
        if (seg->chords[i].time > t && ch)
            break;
        ch = &seg->chords[i];
    }
    if (ch && ch->nsub > 0) {
        for (i = 0; i < ch->nsub; i++)
            if (ch->sub[i].levels & (1u << (level & 31)))
                return &ch->sub[i];
        return &ch->sub[0];
    }
    /* No chord: C major 7 on the chord track header's key. */
    memset(tmp, 0, sizeof *tmp);
    tmp->chord_pattern = 0x891;
    tmp->scale_pattern = seg->have_key ? (seg->key & 0x00ffffffu) : 0xab5ab5u;
    if (!tmp->scale_pattern)
        tmp->scale_pattern = 0xab5ab5u;
    tmp->inversion_points = 0xffffff;
    tmp->levels = 0xffffffffu;
    tmp->chord_root = seg->have_key ? (uint8_t)(seg->key >> 24) : 12;
    tmp->scale_root = (uint8_t)(tmp->chord_root % 12);
    return tmp;
}

/* 0xffffffff when muted, else the PChannel the note plays on. */
static uint32_t dm_mute_at(const LLDmSegmentData* seg, uint32_t pchannel, int32_t t)
{
    uint32_t map = pchannel;
    int      i;
    for (i = 0; i < seg->nmutes; i++) {
        if (seg->mutes[i].time > t)
            break;
        if (seg->mutes[i].pchannel == pchannel)
            map = seg->mutes[i].map == 0xffffffffu ? 0xffffffffu : seg->mutes[i].map;
    }
    return map;
}

/* ---- pattern and variation choice ---------------------------------------- */

static const LLDmPattern* dm_choose_pattern(LLDmPerf* p, const LLDmStyle* sty, int groove,
                                            int emb)
{
    const LLDmPattern* cand[64];
    int                ncand = 0, best = 0x7fffffff, i, pass;

    for (pass = 0; pass < 2 && !ncand; pass++) {
        int want = pass == 0 ? emb : 0;
        if (pass == 1 && emb == 0)
            break;
        /* Exact groove range first. */
        for (i = 0; i < sty->npatterns && ncand < 64; i++) {
            const LLDmPattern* pat = &sty->patterns[i];
            int ok_emb = want ? (pat->embellishment & want) != 0 : pat->embellishment == 0;
            if (ok_emb && groove >= pat->groove_lo && groove <= pat->groove_hi)
                cand[ncand++] = pat;
        }
        /* Else the nearest range. */
        if (!ncand) {
            for (i = 0; i < sty->npatterns; i++) {
                const LLDmPattern* pat = &sty->patterns[i];
                int ok_emb = want ? (pat->embellishment & want) != 0 : pat->embellishment == 0;
                int d;
                if (!ok_emb)
                    continue;
                d = groove < pat->groove_lo ? pat->groove_lo - groove : groove - pat->groove_hi;
                if (d < best) {
                    best = d;
                    ncand = 0;
                }
                if (d == best && ncand < 64)
                    cand[ncand++] = pat;
            }
        }
    }
    if (!ncand)
        return 0;
    return cand[ncand == 1 ? 0 : dm_rand(p) % (uint32_t)ncand];
}

static uint32_t dm_seq_next(LLDmPerf* p, const LLDmPattern* pat)
{
    int i;
    for (i = 0; i < p->nseqs; i++)
        if (p->seqs[i].pattern == pat)
            return p->seqs[i].count++;
    if (p->nseqs < (int)(sizeof p->seqs / sizeof p->seqs[0])) {
        p->seqs[p->nseqs].pattern = pat;
        p->seqs[p->nseqs].count = 1;
        p->nseqs++;
    }
    return 0;
}

/* ---- generation ---------------------------------------------------------- */

static double dm_curve_value(const LLDmCurve* cv, double phase)
{
    double s = cv->start, e = cv->end;
    switch (cv->shape) {
    case 1:  return e;                                              /* instant */
    case 2:  return s + (e - s) * pow(phase, 4.0);                   /* exp */
    case 3:  return s + (e - s) * sqrt(phase);                       /* log */
    case 4:  return s + (e - s) * (sin((phase - 0.5) * M_PI) + 1.0) * 0.5;   /* sine */
    default: return s + (e - s) * phase;                             /* linear */
    }
}

static void dm_gen_curve(LLDmPerf* p, DmState* st, const LLDmCurve* cv, int ch, double t0,
                         double clip_tick)
{
    DmEvent e;
    int     kind, key, maxv, k, last = -1, steps;
    if (cv->type == 3) {
        kind = EV_BEND;
        key = 128;
        maxv = 16383;
    } else if (cv->type == 4) {
        kind = EV_CC;
        key = cv->cc & 0x7f;
        maxv = 127;
    } else {
        return;                  /* channel/poly pressure: no DLS1 routing */
    }
    memset(&e, 0, sizeof e);
    e.kind = (uint8_t)kind;
    e.ch = (uint8_t)ch;
    e.state = st->id;
    e.d = t0;
    steps = cv->shape == 1 || cv->duration <= 0 ? 0 : cv->duration / DM_CURVE_STEP;
    for (k = 0; k <= steps; k++) {
        double tick = t0 + (steps ? (double)cv->duration * k / steps : 0.0);
        double phase = steps ? (double)k / steps : 1.0;
        int    v = (int)lround(dm_curve_value(cv, phase));
        if (tick >= clip_tick)
            break;
        if (v < 0) v = 0;
        if (v > maxv) v = maxv;
        if (v == last)
            continue;
        e.tick = tick;
        e.a = key;
        e.b = v;
        e.first = (uint8_t)(last < 0);
        heap_push(p, &e);
        last = v;
    }
    if ((cv->flags & 1) && last >= 0) {
        int r = cv->reset < 0 ? 0 : cv->reset > maxv ? maxv : cv->reset;
        e.kind = EV_CURVE_RESET;
        e.tick = t0 + cv->duration + cv->reset_duration;
        e.a = key;
        e.b = r;
        e.first = 0;
        e.state = st->id;
        heap_push(p, &e);
    }
}

static void dm_gen_instance(LLDmPerf* p, DmState* st, const LLDmStyle* sty,
                            const LLDmPattern* pat, int32_t s, double t0, int32_t plen)
{
    const LLDmSegmentData* seg = st->seg;
    int32_t                clip = s + plen < st->pass_end ? s + plen : st->pass_end;
    double                 clip_tick = t0 + (clip - s);
    int64_t                locks[256];
    int                    r;

    for (r = 0; r < 256; r++)
        locks[r] = -1;

    for (r = 0; r < pat->nrefs; r++) {
        const LLDmPartRef* ref = &pat->refs[r];
        const LLDmPart*    part;
        int                valid[32], nvalid = 0, v, rep;
        uint32_t           pick, varbit;
        int32_t            part_len, grid_ticks;

        if (ref->part < 0 || ref->part >= sty->nparts)
            continue;
        part = &sty->parts[ref->part];

        for (v = 0; v < 32; v++)
            if (part->var_choices[v] & 0x0fffffffu)
                valid[nvalid++] = v;
        if (!nvalid)
            valid[nvalid++] = 0;
        if (ref->random) {
            if (ref->lock) {
                if (locks[ref->lock] < 0)
                    locks[ref->lock] = dm_rand(p);
                pick = (uint32_t)locks[ref->lock];
            } else {
                pick = dm_rand(p);
            }
        } else {
            pick = dm_seq_next(p, pat);
        }
        varbit = 1u << valid[pick % (uint32_t)nvalid];

        part_len = dm_bar_ticks(&part->ts) * (part->measures ? part->measures : 1);
        grid_ticks = dm_beat_ticks(&part->ts) / (part->ts.grids_per_beat ? part->ts.grids_per_beat : 1);
        if (grid_ticks < 1)
            grid_ticks = 1;

        for (rep = 0; (int64_t)rep * part_len < plen; rep++) {
            int n;
            for (n = 0; n < part->nnotes; n++) {
                const LLDmNote* note = &part->notes[n];
                int32_t         in_part = note->grid * grid_ticks + note->offset;
                int32_t         rel = rep * part_len + (in_part < 0 ? 0 : in_part);
                uint32_t        map;
                int             midi;
                uint8_t         pm;
                DmEvent         e;
                if (!(note->variation & varbit) || in_part >= part_len || s + rel >= clip)
                    continue;
                map = dm_mute_at(seg, ref->pchannel, s + rel);
                if (map == 0xffffffffu || map > 15)
                    continue;
                pm = note->playmode == DMUS_PLAYMODE_NONE ? part->playmode : note->playmode;
                if (pm == 0) {
                    midi = note->music_value <= 127 ? note->music_value : -1;
                } else {
                    LLDmSubChord tmp;
                    midi = ll_dm_music_to_midi(dm_subchord_at(seg, s + rel, ref->subchord, &tmp),
                                               pm, note->music_value);
                }
                if (midi < -48 || midi > 175)
                    continue;
                memset(&e, 0, sizeof e);
                e.kind = EV_NOTE_ON;
                e.tick = t0 + rel;
                e.ch = (uint8_t)map;
                e.state = st->id;
                e.a = midi;
                e.b = note->velocity ? note->velocity : 1;
                e.c = note->duration > 0 ? note->duration : 1;
                heap_push(p, &e);
            }
            for (n = 0; n < part->ncurves; n++) {
                const LLDmCurve* cv = &part->curves[n];
                int32_t          in_part = cv->grid * grid_ticks + cv->offset;
                int32_t          rel = rep * part_len + (in_part < 0 ? 0 : in_part);
                uint32_t         map;
                if (!(cv->variation & varbit) || in_part >= part_len || s + rel >= clip)
                    continue;
                map = dm_mute_at(seg, ref->pchannel, s + rel);
                if (map == 0xffffffffu || map > 15)
                    continue;
                dm_gen_curve(p, st, cv, (int)map, t0 + rel, clip_tick + 4.0 * LL_DM_PPQ);
            }
        }
    }
    p->stats.patterns++;
    st->pattern = pat;
    p->stats.pattern = pat->name;
    dm_trace(p, t0, -1, LL_DM_TRACE_PATTERN, pat->groove_lo, pat->groove_hi);
}

static void dm_apply_band(LLDmPerf* p, const LLDmBand* band, double tick)
{
    int i;
    for (i = 0; i < band->nins; i++) {
        const LLDmInstrument* ins = &band->ins[i];
        int                   ch = (int)ins->pchannel;
        if (ins->pchannel > 15)
            continue;
        if (ins->flags & 0x01) {
            uint32_t patch = (ins->flags & 0x02) ? ins->patch : (ins->patch & 0x800000ffu);
            ll_synth_program(p->synth, ch, patch);
            dm_trace(p, tick, ch, LL_DM_TRACE_PROGRAM, (int)(patch & 0x7fffffff),
                     (patch & 0x80000000u) ? 1 : 0);
        }
        if (ins->flags & 0x40)
            ll_synth_cc(p->synth, ch, 7, ins->volume);
        if (ins->flags & 0x20)
            ll_synth_cc(p->synth, ch, 10, ins->pan);
        if (ins->flags & 0x80)
            p->transpose[ch] = ins->transpose;
    }
}

/* One bar line of the primary segment. */
static void dm_gen_bar(LLDmPerf* p, DmState* st)
{
    const LLDmSegmentData* seg = st->seg;
    int32_t                s = st->next_bar;
    const LLDmStyle*       sty = dm_style_at(seg, s);
    LLDmTimeSig            ts = { 4, 4, 4 };
    int32_t                beat, bar, b;
    double                 t0 = st->pass_tick0 + (s - st->pass_seg0);
    DmEvent                e;
    int                    i;

    if (sty)
        ts = sty->ts;
    beat = dm_beat_ticks(&ts);
    bar = dm_bar_ticks(&ts);
    st->style = sty;
    p->stats.style = sty ? sty->name : 0;

    /* Tempo and band changes inside this bar (time -1 bands went out at start). */
    for (i = 0; i < seg->ntempos; i++) {
        if (seg->tempos[i].time >= s && seg->tempos[i].time < s + bar &&
            seg->tempos[i].time > st->pass_seg0 && seg->tempos[i].bpm > 0.0) {
            memset(&e, 0, sizeof e);
            e.kind = EV_TEMPO;
            e.tick = t0 + (seg->tempos[i].time - s);
            e.d = seg->tempos[i].bpm;
            e.state = st->id;
            heap_push(p, &e);
        }
    }
    for (i = 0; i < seg->nbands; i++) {
        if (seg->bands[i].time >= s && seg->bands[i].time < s + bar &&
            seg->bands[i].time > st->pass_seg0) {
            memset(&e, 0, sizeof e);
            e.kind = EV_BAND;
            e.tick = t0 + (seg->bands[i].time - s);
            e.band = &seg->bands[i].band;
            e.state = st->id;
            heap_push(p, &e);
        }
    }

    /* Beats. */
    for (b = 0; b < ts.beats_per_measure; b++) {
        if (s + b * beat >= st->pass_end)
            break;
        memset(&e, 0, sizeof e);
        e.kind = EV_NOTIFY;
        e.tick = t0 + b * beat;
        e.state = st->id;
        e.a = 1;                 /* measure-and-beat */
        e.b = b;
        e.c = st->measure;
        e.owner = st->owner;
        if (p->notify_beat)
            heap_push(p, &e);
    }

    /* A new pattern when none is running. */
    if (s >= st->inst_end) {
        if (sty) {
            const LLDmCommand* cmd = dm_command_at(seg, s);
            int                groove = (cmd ? cmd->groove : 1) + p->groove_mod;
            int                emb = 0;
            const LLDmPattern* pat;
            if (groove < 1) groove = 1;
            if (groove > 100) groove = 100;
            if (cmd) {
                switch (cmd->command) {
                case 1: emb = 1; break;      /* fill */
                case 2: emb = 4; break;      /* intro */
                case 3: emb = 2; break;      /* break */
                case 4: emb = 8; break;      /* end */
                default: emb = 0; break;
                }
            }
            p->stats.groove = groove;
            pat = dm_choose_pattern(p, sty, groove, emb);
            if (pat) {
                int32_t plen = bar * (pat->measures ? pat->measures : 1);
                dm_gen_instance(p, st, sty, pat, s, t0, plen);
                st->inst_end = s + plen;
            } else {
                st->inst_end = s + bar;
            }
        } else {
            st->inst_end = s + bar;
        }
    }
    st->next_bar = s + bar;
    st->measure++;
}

/* ---- segment states ------------------------------------------------------ */

static int32_t dm_loop_end(const LLDmSegmentData* seg)
{
    return seg->loop_end > seg->loop_start && seg->loop_end <= seg->length ? seg->loop_end
                                                                            : seg->length;
}

static double dm_tempo_at(const LLDmSegmentData* seg, int32_t t, double fallback)
{
    double bpm = fallback;
    int    i;
    for (i = 0; i < seg->ntempos; i++) {
        if (seg->tempos[i].time > t && i > 0)
            break;
        if (seg->tempos[i].bpm > 0.0)
            bpm = seg->tempos[i].bpm;
    }
    return bpm;
}

static void dm_state_free(LLDmPerf* p, DmState* st)
{
    if (!st)
        return;
    if (p->release && st->owner)
        p->release(st->owner);
    free(st);
}

/* Drop what a state had queued past `tick`. Note-offs always survive (they end
 * notes that already sounded); pending curve resets fire at the cut. */
static void dm_cut(LLDmPerf* p, DmState* st, double tick)
{
    int i, j = 0;
    for (i = 0; i < p->nheap; i++) {
        DmEvent* e = &p->heap[i];
        if (e->state == st->id && e->kind != EV_NOTE_OFF && e->tick >= tick - DM_EPS) {
            if (e->kind == EV_CURVE_RESET) {
                e->tick = tick;
                e->state = 0;
            } else {
                continue;
            }
        }
        p->heap[j++] = *e;
    }
    p->nheap = j;
    heap_rebuild(p);
}

static void dm_start_pending(LLDmPerf* p)
{
    DmState*               st = p->pending;
    const LLDmSegmentData* seg = st->seg;
    double                 bpm, prepare;
    int32_t                loop_end = dm_loop_end(seg);
    int32_t                play_start = seg->play_start >= 0 && seg->play_start < seg->length
                                            ? seg->play_start : 0;
    double                 total;
    DmEvent                e;
    int                    i;

    p->pending = 0;
    if (p->primary) {
        DmState* old = p->primary;
        dm_cut(p, old, st->start_tick);
        notify_push(p, 0, LL_DMUS_SEGABORT, 0, 0, st->start_tick, old->owner);
        p->primary = 0;
        dm_state_free(p, old);
    }
    p->primary = st;
    st->pass_tick0 = st->start_tick;
    st->pass_seg0 = play_start;
    st->next_bar = play_start;
    st->inst_end = play_start;
    st->measure = 0;
    if (st->repeats_left > 0 && play_start < loop_end) {
        st->pass_end = loop_end;
        st->pass_loops = 1;
    } else {
        st->pass_end = seg->length;
        st->pass_loops = 0;
        st->repeats_left = 0;
    }
    if (st->pass_loops)
        total = (double)(loop_end - play_start) +
                (double)st->repeats_left * (double)(loop_end - seg->loop_start) +
                (double)(seg->length - loop_end);
    else
        total = (double)(seg->length - play_start);
    st->end_tick = st->start_tick + total;

    bpm = dm_tempo_at(seg, play_start, p->tempo);
    if (bpm > 0.0)
        p->tempo = bpm;
    for (i = 0; i < seg->nbands; i++)
        if (seg->bands[i].time <= play_start)
            dm_apply_band(p, &seg->bands[i].band, st->start_tick);

    notify_push(p, 0, LL_DMUS_SEGSTART, 0, 0, st->start_tick, st->owner);
    p->stats.segments_started++;
    p->stats.segment = seg->name;

    /* "Almost end": the end minus the prepare time at the segment's tempo. */
    prepare = DM_PREPARE_MS / 1000.0 * dm_tempo_at(seg, loop_end, p->tempo) / 60.0 * LL_DM_PPQ;
    memset(&e, 0, sizeof e);
    e.kind = EV_NOTIFY;
    e.tick = st->end_tick - prepare > st->start_tick ? st->end_tick - prepare : st->start_tick;
    e.state = st->id;
    e.a = 0;
    e.b = LL_DMUS_SEGALMOSTEND;
    e.owner = st->owner;
    heap_push(p, &e);
}

/* The primary's pass has run out: loop or end. */
static void dm_pass_end(LLDmPerf* p)
{
    DmState*               st = p->primary;
    const LLDmSegmentData* seg = st->seg;
    double                 t = st->pass_tick0 + (st->pass_end - st->pass_seg0);

    if (st->pass_loops && st->repeats_left > 0) {
        int32_t loop_end = dm_loop_end(seg);
        st->repeats_left--;
        st->pass_tick0 = t;
        st->pass_seg0 = seg->loop_start;
        st->next_bar = seg->loop_start;
        st->inst_end = seg->loop_start;
        if (st->repeats_left > 0) {
            st->pass_end = loop_end;
            st->pass_loops = 1;
        } else {
            st->pass_end = seg->length;
            st->pass_loops = 0;
        }
        notify_push(p, 0, LL_DMUS_SEGLOOP, 0, 0, t, st->owner);
        return;
    }
    notify_push(p, 0, LL_DMUS_SEGEND, 0, 0, t, st->owner);
    dm_cut(p, st, t);
    p->primary = 0;
    dm_state_free(p, st);
}

static void dm_dispatch(LLDmPerf* p, DmEvent* e)
{
    switch (e->kind) {
    case EV_NOTE_ON: {
        int     note = e->a + p->transpose[e->ch & 15];
        DmEvent off;
        while (note < 0) note += 12;
        while (note > 127) note -= 12;
        ll_synth_note_on(p->synth, e->ch, note, e->b);
        dm_trace(p, e->tick, e->ch, LL_DM_TRACE_NOTE_ON, note, e->b);
        memset(&off, 0, sizeof off);
        off.kind = EV_NOTE_OFF;
        off.tick = e->tick + e->c;
        off.ch = e->ch;
        off.a = note;
        heap_push(p, &off);
        p->stats.notes_on++;
        break;
    }
    case EV_NOTE_OFF:
        ll_synth_note_off(p->synth, e->ch, e->a);
        dm_trace(p, e->tick, e->ch, LL_DM_TRACE_NOTE_OFF, e->a, 0);
        break;
    case EV_CC:
    case EV_BEND:
        if (e->first)
            p->curve_start[e->ch & 15][e->a & 0xff] = e->d;
        if (e->kind == EV_CC) {
            ll_synth_cc(p->synth, e->ch, e->a, e->b);
            dm_trace(p, e->tick, e->ch, LL_DM_TRACE_CC, e->a, e->b);
        } else {
            ll_synth_bend(p->synth, e->ch, e->b);
            dm_trace(p, e->tick, e->ch, LL_DM_TRACE_BEND, e->b, 0);
        }
        break;
    case EV_CURVE_RESET:
        /* Only if no later curve on the same controller has started since. */
        if (p->curve_start[e->ch & 15][e->a & 0xff] <= e->d + DM_EPS) {
            if (e->a == 128)
                ll_synth_bend(p->synth, e->ch, e->b);
            else
                ll_synth_cc(p->synth, e->ch, e->a, e->b);
        }
        break;
    case EV_TEMPO:
        if (e->d > 0.0)
            p->tempo = e->d;
        break;
    case EV_BAND:
        if (e->band)
            dm_apply_band(p, e->band, e->tick);
        break;
    case EV_NOTIFY:
        if (e->a == 1) {
            p->last_beat = e->b;
            p->last_measure = e->c;
        }
        notify_push(p, e->a, e->a == 1 ? LL_DMUS_MEASUREBEAT : e->b, e->a == 1 ? e->b : 0,
                    e->a == 1 ? e->c : 0, e->tick, e->owner);
        break;
    default:
        break;
    }
}

/* Everything due at the head, in DirectMusic's order: a queued segment
 * starting, a pass ending, the bar line, then the events.
 *
 * The queued segment goes FIRST, and that decides a notification MusicThread
 * cannot do without. DirectMusic aborts the primary segment a new one replaces
 * when the new one is queued, while the old one is still playing -- so a
 * replacement that lands exactly on the old segment's own end is still
 * SEGABORT, not SEGEND. MusicThread moves from state 6 (transition queued) to 7
 * only on SEGABORT; a world change asked for in the last bar of a segment
 * queues its transition at that segment's end, and with the end processed first
 * the thread stayed in state 6, never queued the destination, and the music
 * stopped when the transition did. */
static void dm_process(LLDmPerf* p)
{
    int guard;
    for (guard = 0; guard < 100000; guard++) {
        double now = p->tick + DM_EPS;
        if (p->pending && p->pending->start_tick <= now) {
            dm_start_pending(p);
            continue;
        }
        if (p->primary &&
            p->primary->pass_tick0 + (p->primary->pass_end - p->primary->pass_seg0) <= now) {
            dm_pass_end(p);
            continue;
        }
        if (p->primary && p->primary->next_bar < p->primary->pass_end &&
            p->primary->pass_tick0 + (p->primary->next_bar - p->primary->pass_seg0) <= now) {
            dm_gen_bar(p, p->primary);
            continue;
        }
        if (p->nheap && p->heap[0].tick <= now) {
            DmEvent e;
            heap_pop(p, &e);
            dm_dispatch(p, &e);
            continue;
        }
        break;
    }
}

static double dm_next_tick(const LLDmPerf* p)
{
    double next = 1e300;
    if (p->primary) {
        const DmState* st = p->primary;
        double         end = st->pass_tick0 + (st->pass_end - st->pass_seg0);
        if (end < next)
            next = end;
        if (st->next_bar < st->pass_end) {
            double bar = st->pass_tick0 + (st->next_bar - st->pass_seg0);
            if (bar < next)
                next = bar;
        }
    }
    if (p->pending && p->pending->start_tick < next)
        next = p->pending->start_tick;
    if (p->nheap && p->heap[0].tick < next)
        next = p->heap[0].tick;
    return next;
}

/* ---- public -------------------------------------------------------------- */

LLDmPerf* ll_dmperf_new(double rate, uint32_t seed)
{
    LLDmPerf* p = (LLDmPerf*)calloc(1, sizeof *p);
    if (!p)
        return 0;
    p->rate = rate > 0 ? rate : 44100.0;
    p->tempo = 120.0;
    p->rng = seed ? seed : 0x9e3779b9u;
    p->next_id = 1;
    p->notify_segment = 1;
    p->notify_beat = 1;
    return p;
}

void ll_dmperf_free(LLDmPerf* p)
{
    if (!p)
        return;
    dm_state_free(p, p->primary);
    dm_state_free(p, p->pending);
    free(p->heap);
    free(p);
}

void ll_dmperf_set_synth(LLDmPerf* p, LLSynth* synth)
{
    if (p)
        p->synth = synth;
}

void ll_dmperf_set_owner_hooks(LLDmPerf* p, void (*retain)(void*), void (*release)(void*))
{
    if (!p)
        return;
    p->retain = retain;
    p->release = release;
}

void ll_dmperf_set_notify_hook(LLDmPerf* p, void (*hook)(void*), void* ctx)
{
    if (!p)
        return;
    p->notify_hook = hook;
    p->notify_ctx = ctx;
}

void ll_dmperf_set_trace(LLDmPerf* p, LLDmTraceFn fn, void* ctx)
{
    if (!p)
        return;
    p->trace = fn;
    p->trace_ctx = ctx;
}

void ll_dmperf_enable_notifications(LLDmPerf* p, int segment, int measure_beat)
{
    if (!p)
        return;
    if (segment >= 0)
        p->notify_segment = segment;
    if (measure_beat >= 0)
        p->notify_beat = measure_beat;
}

void ll_dmperf_set_groove_modifier(LLDmPerf* p, int modifier)
{
    if (p)
        p->groove_mod = modifier;
}

int ll_dmperf_play(LLDmPerf* p, const LLDmSegmentData* seg, void* owner, uint32_t repeats,
                   uint32_t flags)
{
    DmState* st;
    double   start = p ? p->tick : 0.0;

    if (!p || !seg)
        return -1;
    if (flags & LL_DMUS_SEGF_SECONDARY)
        return 0;                /* the game never plays one (music.c's callers are dead) */

    if (p->primary && (flags & (LL_DMUS_SEGF_MEASURE | LL_DMUS_SEGF_BEAT))) {
        const DmState*   cur = p->primary;
        double           seg_time = cur->pass_seg0 + (p->tick - cur->pass_tick0);
        const LLDmStyle* sty = dm_style_at(cur->seg, (int32_t)seg_time);
        LLDmTimeSig      ts = { 4, 4, 4 };
        double           grid, k, boundary;
        if (sty)
            ts = sty->ts;
        grid = (flags & LL_DMUS_SEGF_MEASURE) ? dm_bar_ticks(&ts) : dm_beat_ticks(&ts);
        k = floor((seg_time + DM_EPS) / grid) + 1.0;
        boundary = k * grid;
        if (boundary > cur->pass_end)
            boundary = cur->pass_end;
        start = cur->pass_tick0 + (boundary - cur->pass_seg0);
        if (start > cur->end_tick)
            start = cur->end_tick;
    }

    st = (DmState*)calloc(1, sizeof *st);
    if (!st)
        return -1;
    st->id = p->next_id++;
    st->seg = seg;
    st->owner = owner;
    st->repeats_left = repeats;
    st->start_tick = start;
    if (p->retain && owner)
        p->retain(owner);
    if (p->pending)
        dm_state_free(p, p->pending);
    p->pending = st;
    return 0;
}

void ll_dmperf_stop(LLDmPerf* p)
{
    if (!p)
        return;
    if (p->primary) {
        notify_push(p, 0, LL_DMUS_SEGABORT, 0, 0, p->tick, p->primary->owner);
        dm_state_free(p, p->primary);
        p->primary = 0;
    }
    if (p->pending) {
        dm_state_free(p, p->pending);
        p->pending = 0;
    }
    p->nheap = 0;
    ll_synth_all_notes_off(p->synth);
}

void ll_dmperf_render(LLDmPerf* p, float* left, float* right, int frames)
{
    int done = 0, stuck = 0;
    if (!p || frames <= 0)
        return;
    memset(left, 0, (size_t)frames * sizeof(float));
    memset(right, 0, (size_t)frames * sizeof(float));
    while (done < frames) {
        double next, tps;
        int    n;
        dm_process(p);
        if (p->notify_pending && p->notify_hook) {
            p->notify_pending = 0;
            p->notify_hook(p->notify_ctx);
            dm_process(p);
        }
        tps = p->tempo * LL_DM_PPQ / 60.0 / p->rate;
        n = frames - done;
        next = dm_next_tick(p);
        if (next < 1e299) {
            double k = ceil((next - p->tick) / tps - 1e-9);
            if (k < (double)n)
                n = k < 1.0 ? 0 : (int)k;
        }
        if (n <= 0) {
            if (++stuck < 64)
                continue;
            n = 1;               /* an event that will not dispatch: move on */
        }
        stuck = 0;
        if (p->synth)
            ll_synth_render(p->synth, left + done, right + done, n);
        p->tick += n * tps;
        done += n;
    }
}

void ll_dmperf_stats(const LLDmPerf* p, LLDmPerfStats* out)
{
    if (!p) {
        memset(out, 0, sizeof *out);
        return;
    }
    *out = p->stats;
    out->tick = p->tick;
    out->tempo = p->tempo;
    out->groove_modifier = p->groove_mod;
    out->beat = p->last_beat;
    out->measure = p->last_measure;
    if (!p->primary)
        out->segment = 0;
}

int ll_dmperf_playing(const LLDmPerf* p)
{
    return p && (p->primary || p->pending);
}
