/* LEGOLAND portable build -- the DirectMusic engine's internal interface.
 *
 * Three layers, one header, no game types:
 *
 *   ll_dls.c     a DLS Level 1 instrument collection and the software synth
 *                that plays it (voices, envelopes, LFO, channels, mixing);
 *   ll_dmfile.c  the DirectX 6 RIFF forms the game ships in imusic\ --
 *                segments (.sgt), styles (.sty) and bands (.bnd);
 *   ll_dmperf.c  the performance: segment states, the style engine that turns
 *                patterns + chords + groove into notes, curves, bands, and the
 *                SEGMENT / MEASUREANDBEAT notifications MusicThread waits on.
 *
 * ll_dmusic.c wraps all three in the COM objects musicthread.c calls through
 * vtable offsets, and feeds the rendered PCM to ll_audio.c. Nothing in these
 * three files knows about COM, Win32 or Emscripten, so the same code builds
 * into the native render tool (src/tools/dmusic_tool.c) and its
 * selftest.
 *
 * Music time is DirectMusic's: 768 ticks to a quarter note (DMUS_PPQ).
 */
#ifndef LL_DMUSIC_H
#define LL_DMUSIC_H

#include <stddef.h>
#include <stdint.h>

#define LL_DM_PPQ 768

/* ======================================================================== */
/* DLS collection and synth (ll_dls.c)                                      */
/* ======================================================================== */

typedef struct LLDls LLDls;
typedef struct LLSynth LLSynth;

/* Parse a DLS collection held in memory. The PCM is converted to float and
 * copied, so `data` may be freed afterwards. NULL on a malformed file. */
LLDls* ll_dls_load(const unsigned char* data, size_t size);
void   ll_dls_free(LLDls* dls);
int    ll_dls_instrument_count(const LLDls* dls);
int    ll_dls_wave_count(const LLDls* dls);

/* A synth over one collection: 16 MIDI channels (DirectMusic's one channel
 * group) and `voices` voices of polyphony, rendering at `rate` Hz. */
LLSynth* ll_synth_new(const LLDls* dls, double rate, int voices);
void     ll_synth_free(LLSynth* s);

/* DirectMusic's dwPatch: bank select MSB << 16 | LSB << 8 | program, with
 * bit 31 set for a drum kit. Missing GS variations fall back to the capital
 * tone, as the Microsoft synthesizer's GS set does. */
void ll_synth_program(LLSynth* s, int ch, uint32_t patch);
void ll_synth_cc(LLSynth* s, int ch, int cc, int value);
void ll_synth_bend(LLSynth* s, int ch, int value);           /* 0..16383 */
void ll_synth_note_on(LLSynth* s, int ch, int note, int velocity);
void ll_synth_note_off(LLSynth* s, int ch, int note);
void ll_synth_all_notes_off(LLSynth* s);                     /* release every voice */
void ll_synth_reset(LLSynth* s);                             /* silence + default controllers */

/* Add `frames` of stereo audio into the two planar buffers (not cleared). */
void ll_synth_render(LLSynth* s, float* left, float* right, int frames);
int  ll_synth_active_voices(const LLSynth* s);
uint32_t ll_synth_missing_programs(const LLSynth* s);        /* program changes with no instrument */

/* ======================================================================== */
/* Segment / style / band data (ll_dmfile.c)                                */
/* ======================================================================== */

typedef struct LLDmGuid { unsigned char b[16]; } LLDmGuid;

typedef struct LLDmTimeSig {
    uint8_t  beats_per_measure;
    uint8_t  beat;               /* 4 = quarter note; 0 = 256th (DirectMusic's rule) */
    uint16_t grids_per_beat;
} LLDmTimeSig;

/* DMUS_IO_INSTRUMENT (40 bytes in DirectX 6). */
typedef struct LLDmInstrument {
    uint32_t patch;
    uint32_t pchannel;
    uint32_t flags;              /* DMUS_IO_INST_* */
    uint8_t  pan;
    uint8_t  volume;
    int16_t  transpose;
    uint32_t priority;
} LLDmInstrument;

typedef struct LLDmBand {
    LLDmInstrument* ins;
    int             nins;
    char            name[64];
} LLDmBand;

/* DMUS_IO_STYLENOTE (22 bytes). */
typedef struct LLDmNote {
    int32_t  grid;
    uint32_t variation;
    int32_t  duration;
    int16_t  offset;
    uint16_t music_value;
    uint8_t  velocity;
    uint8_t  time_range;
    uint8_t  dur_range;
    uint8_t  vel_range;
    uint8_t  inversion;
    uint8_t  playmode;
} LLDmNote;

/* DMUS_IO_STYLECURVE (28 bytes). */
typedef struct LLDmCurve {
    int32_t  grid;
    uint32_t variation;
    int32_t  duration;
    int32_t  reset_duration;
    int16_t  offset;
    int16_t  start;
    int16_t  end;
    int16_t  reset;
    uint8_t  type;               /* DMUS_CURVET_* */
    uint8_t  shape;              /* DMUS_CURVES_* */
    uint8_t  cc;
    uint8_t  flags;              /* DMUS_CURVE_RESET = 1 */
} LLDmCurve;

typedef struct LLDmPart {
    LLDmGuid    guid;
    LLDmTimeSig ts;
    uint32_t    var_choices[32];
    uint16_t    measures;
    uint8_t     playmode;
    uint8_t     invert_hi;
    uint8_t     invert_lo;
    LLDmNote*   notes;
    int         nnotes;
    LLDmCurve*  curves;
    int         ncurves;
} LLDmPart;

/* DMUS_IO_PARTREF (22 bytes), with the part resolved to an index. */
typedef struct LLDmPartRef {
    LLDmGuid guid;
    int      part;               /* index into LLDmStyle.parts, -1 if unresolved */
    uint16_t pchannel;
    uint8_t  lock;
    uint8_t  subchord;
    uint8_t  priority;
    uint8_t  random;
} LLDmPartRef;

typedef struct LLDmPattern {
    LLDmTimeSig  ts;
    uint8_t      groove_lo;
    uint8_t      groove_hi;
    uint16_t     embellishment;  /* DMUS_EMBELLISHT_*: 0 normal, 16 motif */
    uint16_t     measures;
    LLDmPartRef* refs;
    int          nrefs;
    char         name[64];
} LLDmPattern;

typedef struct LLDmStyle {
    LLDmGuid     guid;
    char         name[64];
    LLDmTimeSig  ts;
    double       tempo;
    LLDmPart*    parts;
    int          nparts;
    LLDmPattern* patterns;
    int          npatterns;
    LLDmBand*    bands;
    int          nbands;
} LLDmStyle;

typedef struct LLDmSubChord {
    uint32_t chord_pattern;
    uint32_t scale_pattern;
    uint32_t inversion_points;
    uint32_t levels;
    uint8_t  chord_root;
    uint8_t  scale_root;
} LLDmSubChord;

#define LL_DM_MAX_SUBCHORDS 8

typedef struct LLDmChord {
    int32_t      time;
    char         name[16];
    int          nsub;
    LLDmSubChord sub[LL_DM_MAX_SUBCHORDS];
} LLDmChord;

typedef struct LLDmCommand {
    int32_t time;
    uint8_t command;             /* DMUS_COMMANDT_*: 0 groove */
    uint8_t groove;
    uint8_t groove_range;
} LLDmCommand;

typedef struct LLDmTempo {
    int32_t time;
    double  bpm;
} LLDmTempo;

typedef struct LLDmStyleRef {
    int32_t          time;
    LLDmGuid         guid;
    int              have_guid;
    char             name[64];
    char             file[260];
    const LLDmStyle* style;      /* resolved by ll_dm_segment_resolve */
    void*            owner;      /* the resolver's handle for it (a COM object) */
} LLDmStyleRef;

typedef struct LLDmBandItem {
    int32_t  time;               /* -1: before the segment's first tick */
    LLDmBand band;
} LLDmBandItem;

typedef struct LLDmMute {
    int32_t  time;
    uint32_t pchannel;
    uint32_t map;                /* 0xffffffff mutes the channel */
} LLDmMute;

typedef struct LLDmSegmentData {
    LLDmGuid      guid;
    char          name[64];
    uint32_t      repeats;       /* as authored; the COM object keeps the live value */
    int32_t       length;
    int32_t       play_start;
    int32_t       loop_start;
    int32_t       loop_end;
    uint32_t      resolution;
    uint32_t      key;           /* chord track header: root << 24 | scale */
    int           have_key;
    LLDmChord*    chords;
    int           nchords;
    LLDmCommand*  commands;
    int           ncommands;
    LLDmTempo*    tempos;
    int           ntempos;
    LLDmStyleRef* styles;
    int           nstyles;
    LLDmBandItem* bands;
    int           nbands;
    LLDmMute*     mutes;
    int           nmutes;
} LLDmSegmentData;

enum { LL_DM_KIND_NONE = 0, LL_DM_KIND_SEGMENT, LL_DM_KIND_STYLE, LL_DM_KIND_BAND };

/* What a file is and what it calls itself, from its RIFF form, its `guid`
 * chunk and its UNFO/UNAM name. Returns the LL_DM_KIND_*. */
int  ll_dm_peek(const unsigned char* data, size_t size, LLDmGuid* guid, int* have_guid,
                char* name, size_t name_size);

/* 0 on success. On failure the output is left empty and safe to free. */
int  ll_dm_parse_segment(const unsigned char* data, size_t size, LLDmSegmentData* out);
int  ll_dm_parse_style(const unsigned char* data, size_t size, LLDmStyle* out);
int  ll_dm_parse_band(const unsigned char* data, size_t size, LLDmBand* out);
void ll_dm_free_segment(LLDmSegmentData* seg);
void ll_dm_free_style(LLDmStyle* sty);
void ll_dm_free_band(LLDmBand* band);

/* Resolve every style reference through `resolve`, which returns the style
 * and may set *owner. Returns the number left unresolved. */
typedef const LLDmStyle* (*LLDmResolveStyle)(void* ctx, const LLDmStyleRef* ref, void** owner);
int  ll_dm_segment_resolve(LLDmSegmentData* seg, LLDmResolveStyle resolve, void* ctx);

int  ll_dm_guid_equal(const LLDmGuid* a, const LLDmGuid* b);

/* ======================================================================== */
/* The performance (ll_dmperf.c)                                            */
/* ======================================================================== */

typedef struct LLDmPerf LLDmPerf;

/* DMUS_SEGF_* bits the game uses. */
#define LL_DMUS_SEGF_SECONDARY 0x80
#define LL_DMUS_SEGF_BEAT      0x1000
#define LL_DMUS_SEGF_MEASURE   0x2000

/* DMUS_NOTIFICATION_* options. */
#define LL_DMUS_SEGSTART     0
#define LL_DMUS_SEGEND       1
#define LL_DMUS_SEGALMOSTEND 2
#define LL_DMUS_SEGLOOP      3
#define LL_DMUS_SEGABORT     4
#define LL_DMUS_MEASUREBEAT  0

typedef struct LLDmNotify {
    int     kind;                /* 0 segment, 1 measure-and-beat */
    int     option;
    int     field1;              /* beat */
    int     field2;              /* measure */
    double  tick;
    void*   segment;             /* the owner handle passed to ll_dmperf_play */
} LLDmNotify;

typedef struct LLDmPerfStats {
    uint32_t    notes_on;
    uint32_t    patterns;
    uint32_t    notifications;
    uint32_t    segments_started;
    double      tick;
    double      tempo;
    int         groove;
    int         groove_modifier;
    int         measure;
    int         beat;
    const char* segment;
    const char* style;
    const char* pattern;
} LLDmPerfStats;

LLDmPerf* ll_dmperf_new(double rate, uint32_t seed);
void      ll_dmperf_free(LLDmPerf* p);
void      ll_dmperf_set_synth(LLDmPerf* p, LLSynth* synth);

/* Segment ownership: the performance retains a segment's owner while a state
 * of it is queued or playing. Both hooks may be NULL. */
void ll_dmperf_set_owner_hooks(LLDmPerf* p, void (*retain)(void*), void (*release)(void*));

/* Play `seg` as the primary segment. `repeats` is the live repeat count.
 * DMUS_SEGF_MEASURE / BEAT align the start to the next boundary of the
 * primary segment now playing; with nothing playing it starts at once. */
int  ll_dmperf_play(LLDmPerf* p, const LLDmSegmentData* seg, void* owner,
                    uint32_t repeats, uint32_t flags);
void ll_dmperf_stop(LLDmPerf* p);
void ll_dmperf_set_groove_modifier(LLDmPerf* p, int modifier);
void ll_dmperf_enable_notifications(LLDmPerf* p, int segment, int measure_beat);

/* Called between render steps whenever notifications are waiting. It may call
 * back into the performance (play, stop, pop). */
void ll_dmperf_set_notify_hook(LLDmPerf* p, void (*hook)(void*), void* ctx);
int  ll_dmperf_pop_notification(LLDmPerf* p, LLDmNotify* out);

/* Render `frames` of stereo into two planar buffers (overwritten). With no
 * synth attached the performance still advances and notifies. */
void ll_dmperf_render(LLDmPerf* p, float* left, float* right, int frames);

/* Optional event trace for the tool: (tick, pchannel, kind, a, b). */
typedef void (*LLDmTraceFn)(void* ctx, double tick, int pchannel, int kind, int a, int b);
enum { LL_DM_TRACE_NOTE_ON = 1, LL_DM_TRACE_NOTE_OFF, LL_DM_TRACE_CC, LL_DM_TRACE_BEND,
       LL_DM_TRACE_PROGRAM, LL_DM_TRACE_PATTERN };
void ll_dmperf_set_trace(LLDmPerf* p, LLDmTraceFn fn, void* ctx);

void ll_dmperf_stats(const LLDmPerf* p, LLDmPerfStats* out);
int  ll_dmperf_playing(const LLDmPerf* p);

/* DirectMusic's MusicToMIDI for one subchord and play mode (exposed for the
 * selftest). Returns -1 when the value cannot be converted. */
int  ll_dm_music_to_midi(const LLDmSubChord* chord, uint8_t playmode, uint16_t value);

#endif /* LL_DMUSIC_H */
