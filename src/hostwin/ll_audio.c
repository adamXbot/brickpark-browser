/* LEGOLAND portable build -- the Web Audio back end behind the DirectSound
 * shim (scope PORT-B11).
 *
 * WHAT THIS IS FOR
 * -------------------------------------------------------------------------
 * PORT-B4 built a complete but SILENT IDirectSound: every buffer the game
 * creates is real memory, every cursor advances by wall clock, every status and
 * volume round-trips, and nothing is ever heard. dsound.c's own header says so
 * -- "the PCM the game writes through Lock is kept ... and never played". This
 * file is what plays it. dsound.c keeps all of the DirectSound semantics; this
 * file knows only about PCM blocks, gains and time.
 *
 * WHY IT IS A SEPARATE FILE. dsound.c is 950 lines of DirectX 5 ABI and
 * cursor arithmetic and it has to keep compiling, unchanged in behaviour, on
 * the native 64-bit toolchain where there is no Web Audio at all. Everything
 * browser-specific is therefore here, behind an eight-function C interface
 * whose native implementation is a set of counters. `ninja -C portable/build`
 * still builds the whole shim and `llAudio()` still reports on a node run; it
 * just reports zero voices.
 *
 * WHY EM_JS AND NOT A --js-library
 * -------------------------------------------------------------------------
 * ll_canvas.js reaches the shim through four `ll_js_*` symbols that are
 * resolved by `--js-library` in the browser link and by
 * src/headless/node_shim.c in the node links. Adding a fifth family that way
 * would mean editing node_shim.c and tests.cmake, which this lane does not own,
 * and would break `legoland_tests` and `legoland_headless` the moment it landed.
 * EM_JS compiles the JavaScript INTO this object file, so it links in every
 * Emscripten target with no library and no stub -- at the cost of having to be
 * node-safe itself, which is the next paragraph.
 *
 * NODE SAFETY, AND THE OPTIMIZER TRAP
 * -------------------------------------------------------------------------
 * `legoland_tests` and `legoland_headless` run this code under node, where
 * there is no AudioContext and no `window`. Every lookup below therefore goes
 * through `globalThis['Name']` rather than a bare identifier, for the reason
 * ll_canvas.js documents at length: emcc's -O2 JS optimizer constant-folds
 * `typeof AudioContext === 'undefined'` at BUILD time, with no DOM in scope,
 * and bakes in the wrong answer. A property access on globalThis is opaque to
 * it. And nothing here throws: a failure to make a context leaves
 * `ll_audio_enabled()` at 0 and the port is exactly as silent as it was.
 *
 * THE AUTOPLAY POLICY
 * -------------------------------------------------------------------------
 * A browser will not let a page make noise before the user has interacted with
 * it: the AudioContext is created `suspended` and has to be `resume()`d from
 * inside a real input event. The game's own first click is such an event, so
 * the context is resumed from one-shot capture-phase listeners on
 * pointerdown/keydown/touchend, installed when the context is made. Until that
 * happens, Play calls are counted as `blocked` rather than dropped, and
 * `llAudio().state` reads "suspended" so the page can put up a "click to enable
 * sound" affordance (index.html does). Nothing waits: the game never blocks on
 * audio, because dsound.c's cursor is wall clock and not this file's clock.
 *
 * TWO PLAYBACK MODES, AND WHY THE GAME NEEDS BOTH
 * -------------------------------------------------------------------------
 * 1. STATIC. A sound effect is written once, in full, before it is ever played:
 *    CreateSampleFromWAV (data2.c:591) does one
 *    `Lock(0, 0, .., DSBLOCK_ENTIREBUFFER)`, one memcpy and one Unlock at LOAD
 *    time, and PlayInstanceOfSample then duplicates the buffer per voice and
 *    plays it. So the PCM is a snapshot: it becomes one AudioBuffer and one
 *    AudioBufferSourceNode, with `loop` set for DSBPLAY_LOOPING, and the
 *    browser does the rest. This is the path for all 155 archived samples.
 *
 * 2. STREAMED. The narration buffer is 0xa000 bytes of TEN 0x1000 blocks,
 *    played LOOPING for ever while PumpNarration (narration2.c:622) rewrites
 *    the block one ahead of the play cursor, frame after frame; a whole speech
 *    file of any length goes through those 40 KB. A snapshot would play the
 *    first 1.9 seconds of every line on a loop. So a streamed buffer is fed as
 *    small AudioBuffers scheduled back to back, each one copied out of the
 *    game's buffer only AFTER the cursor dsound.c reports has passed over it --
 *    which is the whole trick, because the cursor is what the game fills
 *    against, so "the cursor has passed it" is exactly "the game has written
 *    it". Output therefore lags the reported cursor by one chunk (~46 ms) and
 *    can never read a block the game has not filled.
 *
 *    dsound.c decides which mode a buffer is in, and the rule is the game's own
 *    call pattern: a Lock WITHOUT DSBLOCK_ENTIREBUFFER means a partial write,
 *    which only the streaming writers do (narration2.c:646, movie.c:599,
 *    input2.c:478 -- all of them flags 0; every sample loader passes 2).
 *
 * VOLUME AND PAN
 * -------------------------------------------------------------------------
 * DirectSound volume is hundredths of a dB of ATTENUATION, -10000..0, so the
 * linear gain is 10^(v/2000); the game's own VolToDB (audio3.c:494) maps a
 * 0..100 slider to -4000..0 and snaps 0 to -10000, and SetSampleVolume
 * (audio3.c:167) maps 0..100 to -3200..0. -10000 is treated as true silence
 * rather than as 10^-5, so a muted sample costs nothing to keep scheduled.
 *
 * Pan is hundredths of a dB applied to the opposite channel, ±10000. It is
 * converted to the two channel gains and then to a StereoPannerNode position
 * as their difference, which is exact at the ends and within a dB in between.
 * The game's PanFromOffset (tinystubs.c:258) is `dx * 4` off the viewport
 * centre, so the values that actually occur are a few thousand at most.
 *
 * WHAT IS NOT HERE
 * -------------------------------------------------------------------------
 * DirectMusic. musicthread.c's port buffer is created through the same
 * CreateSoundBuffer and will arrive here if it is ever reached, but nothing
 * writes PCM into it -- the music is .sgt segments a DirectMusic performance
 * would render -- so it stays silent and stays out of scope (dsound.c's ole32
 * note explains why the whole path is unreachable today).
 *
 * Ownership: PORT-B (docs/SCOPE_PORT_WAVE.md). Declarations: ll_host.h.
 */
#include <string.h>

#include "ll_host.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

/* The shared state lives on globalThis, not in a C struct, because the page's
 * `llAudio()` hook has to be able to read it and because an AudioContext is not
 * a thing C can hold. `ll_audio_js_state` is the one accessor. */
EM_JS(int, ll_audio_js_init, (void), {
    var g = globalThis;
    var a = g.__llAudio;
    if (a)
        return a.ctx ? 1 : 0;
    a = {
        ctx: null, voices: {}, created: 0, started: 0, blocked: 0,
        fed_chunks: 0, fed_bytes: 0, underruns: 0, refused: 0, formats: {}
    };
    g.__llAudio = a;

    /* The page's window into this file. Deliberately a function and not an
     * object, so a console or a driver always gets a fresh reading. */
    g.llAudio = function () {
        var s = g.__llAudio, n = 0, k;
        for (k in s.voices) if (s.voices[k].src) n++;
        return {
            state: s.ctx ? s.ctx.state : 'none',
            rate: s.ctx ? s.ctx.sampleRate : 0,
            voices_created: s.created,
            plays: s.started,
            plays_blocked: s.blocked,
            active: n,
            stream_chunks: s.fed_chunks,
            stream_bytes: s.fed_bytes,
            underruns: s.underruns,
            refused: s.refused,
            formats: s.formats
        };
    };
    /* Let the page ask for the context by hand, from its own click handler. */
    g.llAudioResume = function () {
        var s = g.__llAudio;
        if (s && s.ctx && s.ctx.state !== 'running') { try { s.ctx.resume(); } catch (e) {} }
        return s && s.ctx ? s.ctx.state : 'none';
    };

    var C = g['AudioContext'] || g['webkitAudioContext'];
    if (!C) {
        a.refused++;
        return 0;                      /* node, or a browser with no Web Audio */
    }
    try {
        a.ctx = new C();
    } catch (e) {
        a.refused++;
        return 0;
    }
    /* The autoplay policy. One-shot, capture phase, on the three gestures a
     * browser accepts, and `passive` so the game's own handlers are unaffected. */
    var d = g['document'];
    if (d && d.addEventListener) {
        var wake = function () {
            if (a.ctx && a.ctx.state !== 'running') { try { a.ctx.resume(); } catch (e) {} }
        };
        var opts = { capture: true, passive: true };
        d.addEventListener('pointerdown', wake, opts);
        d.addEventListener('keydown', wake, opts);
        d.addEventListener('touchend', wake, opts);
        d.addEventListener('mousedown', wake, opts);
    }
    try { a.ctx.resume(); } catch (e) {}
    return 1;
});

EM_JS(int, ll_audio_js_state, (void), {
    var a = globalThis.__llAudio;
    if (!a || !a.ctx) return 0;
    return a.ctx.state === 'running' ? 2 : 1;
});

/* Build an AudioBuffer from `bytes` of interleaved PCM at HEAP offset `ptr`.
 * 16-bit signed is what ConvertWAVToPCM always produces (resaudio2.c:176 sets
 * wBitsPerSample to 16 unconditionally); 8-bit unsigned is handled because
 * SetFormat on a primary buffer could in principle deliver it. */
EM_JS(int, ll_audio_js_decode, (int v, int ptr, int bytes, int rate, int ch, int bits), {
    var a = globalThis.__llAudio;
    if (!a || !a.ctx || bytes <= 0 || ch < 1) return 0;
    var bps = (bits >> 3) * ch;
    if (bps < 1) return 0;
    var frames = (bytes / bps) | 0;
    if (frames < 1) return 0;
    var buf;
    try { buf = a.ctx.createBuffer(ch, frames, rate); } catch (e) { return 0; }
    for (var c = 0; c < ch; c++) {
        var out = buf.getChannelData(c);
        if (bits === 16) {
            var base = (ptr >> 1) + c;
            for (var i = 0; i < frames; i++)
                out[i] = HEAP16[base + i * ch] / 32768;
        } else {
            var b8 = ptr + c;
            for (var j = 0; j < frames; j++)
                out[j] = (HEAPU8[b8 + j * ch] - 128) / 128;
        }
    }
    var st = a.voices[v] || (a.voices[v] = {});
    st.buf = buf;
    st.rate = rate;
    st.ch = ch;
    st.bits = bits;
    st.bytes = bytes;
    return 1;
});

/* DirectSound hundredths-of-a-dB to a linear gain, and a pan to a
 * StereoPanner position. Shared by start and by the live setters. */
EM_JS(void, ll_audio_js_gain, (int v, int vol_cb, int pan_cb), {
    var a = globalThis.__llAudio;
    if (!a || !a.ctx) return;
    var st = a.voices[v];
    if (!st) return;
    st.vol_cb = vol_cb;
    st.pan_cb = pan_cb;
    var g = vol_cb <= -10000 ? 0 : Math.pow(10, vol_cb / 2000);
    if (st.gain) { try { st.gain.gain.value = g; } catch (e) {} }
    if (st.panner) {
        var l = pan_cb > 0 ? Math.pow(10, -pan_cb / 2000) : 1;
        var r = pan_cb < 0 ? Math.pow(10,  pan_cb / 2000) : 1;
        var p = r - l;
        if (p < -1) p = -1;
        if (p >  1) p =  1;
        try { st.panner.pan.value = p; } catch (e) {}
    }
});

/* Tear a voice's nodes down. Stopping a source makes it unusable, which is why
 * every (re)start builds a new one -- that is how Web Audio works, not a
 * shortcut. */
EM_JS(void, ll_audio_js_stop, (int v), {
    var a = globalThis.__llAudio;
    if (!a) return;
    var st = a.voices[v];
    if (!st) return;
    if (st.src) { try { st.src.stop(); } catch (e) {} try { st.src.disconnect(); } catch (e) {} st.src = null; }
    if (st.queue) {
        for (var i = 0; i < st.queue.length; i++) {
            try { st.queue[i].stop(); } catch (e) {}
            try { st.queue[i].disconnect(); } catch (e) {}
        }
        st.queue = [];
    }
    st.next = 0;
    st.read = -1;
});

EM_JS(void, ll_audio_js_release, (int v), {
    var a = globalThis.__llAudio;
    if (!a) return;
    if (a.voices[v]) delete a.voices[v];
});

/* Make (or re-make) the gain/pan chain for a voice. */
EM_JS(void, ll_audio_js_chain, (int v), {
    var a = globalThis.__llAudio;
    if (!a || !a.ctx) return;
    var st = a.voices[v] || (a.voices[v] = {});
    if (st.gain) return;
    try {
        st.gain = a.ctx.createGain();
        if (a.ctx.createStereoPanner) {
            st.panner = a.ctx.createStereoPanner();
            st.gain.connect(st.panner);
            st.panner.connect(a.ctx.destination);
        } else {
            st.gain.connect(a.ctx.destination);
        }
        a.created++;
    } catch (e) { st.gain = null; }
});

/* STATIC start. `off` is the byte the DirectSound cursor is at, which
 * SetCurrentPosition has normally just set to 0. */
EM_JS(int, ll_audio_js_start, (int v, int off, int looping, double rate_mul), {
    var a = globalThis.__llAudio;
    if (!a || !a.ctx) return 0;
    var st = a.voices[v];
    if (!st || !st.buf || !st.gain) return 0;
    if (a.ctx.state !== 'running') { a.blocked++; return 0; }
    var bps = (st.bits >> 3) * st.ch;
    var when = bps > 0 ? (off / bps) / st.rate : 0;
    try {
        var s = a.ctx.createBufferSource();
        s.buffer = st.buf;
        s.loop = looping ? true : false;
        if (rate_mul > 0) s.playbackRate.value = rate_mul;
        s.connect(st.gain);
        s.start(0, when);
        st.src = s;
        a.started++;
        var key = st.rate + 'Hz/' + st.ch + 'ch/' + st.bits + 'bit';
        a.formats[key] = (a.formats[key] || 0) + 1;
    } catch (e) { return 0; }
    return 1;
});

/* STREAMED feed. `cursor` is the play position dsound.c reports to the game, so
 * everything BEHIND it has been written; `read` is how far this file has
 * scheduled. One chunk of latency is deliberate -- see the header. */
EM_JS(int, ll_audio_js_feed, (int v, int ptr, int bytes, int cursor, int chunk,
                              int rate, int ch, int bits, double rate_mul), {
    var a = globalThis.__llAudio;
    if (!a || !a.ctx || !bytes || !chunk) return 0;
    var st = a.voices[v];
    if (!st || !st.gain) return 0;
    if (a.ctx.state !== 'running') { a.blocked++; return 0; }
    var bps = (bits >> 3) * ch;
    if (bps < 1) return 0;
    if (st.read === undefined || st.read < 0) {
        /* Start one chunk behind the cursor: the block the game has just
         * finished writing. */
        st.read = ((cursor - chunk) % bytes + bytes) % bytes;
        st.next = 0;
        st.queue = [];
    }
    var avail = ((cursor - st.read) % bytes + bytes) % bytes;
    var n = 0;
    while (avail >= chunk && n < 8) {
        var frames = (chunk / bps) | 0;
        var buf;
        try { buf = a.ctx.createBuffer(ch, frames, rate); } catch (e) { break; }
        for (var c = 0; c < ch; c++) {
            var out = buf.getChannelData(c);
            if (bits === 16) {
                var base = ((ptr + st.read) >> 1) + c;
                for (var i = 0; i < frames; i++) out[i] = HEAP16[base + i * ch] / 32768;
            } else {
                var b8 = ptr + st.read + c;
                for (var j = 0; j < frames; j++) out[j] = (HEAPU8[b8 + j * ch] - 128) / 128;
            }
        }
        var now = a.ctx.currentTime;
        if (!st.next || st.next < now + 0.01) {
            if (st.next) a.underruns++;
            st.next = now + 0.03;       /* re-prime after a gap */
        }
        try {
            var s = a.ctx.createBufferSource();
            s.buffer = buf;
            if (rate_mul > 0) s.playbackRate.value = rate_mul;
            s.connect(st.gain);
            s.start(st.next);
            if (!st.queue) st.queue = [];
            st.queue.push(s);
            if (st.queue.length > 32) st.queue.shift();
        } catch (e) { break; }
        st.next += frames / (rate * (rate_mul > 0 ? rate_mul : 1));
        st.read = (st.read + chunk) % bytes;
        avail -= chunk;
        a.fed_chunks++;
        a.fed_bytes += chunk;
        n++;
    }
    return n;
});

EM_JS(void, ll_audio_js_rate, (int v, double rate_mul), {
    var a = globalThis.__llAudio;
    if (!a) return;
    var st = a.voices[v];
    if (!st) return;
    st.rate_mul = rate_mul;
    if (st.src && rate_mul > 0) { try { st.src.playbackRate.value = rate_mul; } catch (e) {} }
});

#else  /* ---- not Emscripten: the counters, and nothing else --------------- */

static int g_na_voices, g_na_plays, g_na_feeds;

static int  ll_audio_js_init(void)   { return 0; }
static int  ll_audio_js_state(void)  { return 0; }
static int  ll_audio_js_decode(int v, int ptr, int bytes, int rate, int ch, int bits)
{ (void)v; (void)ptr; (void)bytes; (void)rate; (void)ch; (void)bits; return 0; }
static void ll_audio_js_gain(int v, int vol, int pan) { (void)v; (void)vol; (void)pan; }
static void ll_audio_js_stop(int v) { (void)v; }
static void ll_audio_js_release(int v) { (void)v; }
static void ll_audio_js_chain(int v) { (void)v; g_na_voices++; }
static int  ll_audio_js_start(int v, int off, int looping, double rate_mul)
{ (void)v; (void)off; (void)looping; (void)rate_mul; g_na_plays++; return 0; }
static int  ll_audio_js_feed(int v, int ptr, int bytes, int cursor, int chunk,
                             int rate, int ch, int bits, double rate_mul)
{ (void)v; (void)ptr; (void)bytes; (void)cursor; (void)chunk; (void)rate;
  (void)ch; (void)bits; (void)rate_mul; g_na_feeds++; return 0; }
static void ll_audio_js_rate(int v, double rate_mul) { (void)v; (void)rate_mul; }

#endif

/* ---- the C interface dsound.c uses ------------------------------------- */

static int g_audio_tried;
static int g_audio_live;
static int g_audio_next_voice = 1;

/* Created on demand, which on the page is the first CreateSoundBuffer --
 * InitSoundSampleSystem's, inside InitSession, long before a frame is drawn and
 * therefore long before the user has clicked. That is fine and is why the
 * autoplay listeners are installed with the context rather than at the first
 * Play. */
int ll_audio_enabled(void)
{
    if (!g_audio_tried) {
        g_audio_tried = 1;
        g_audio_live = ll_audio_js_init();
        ll_host_trace("AUDIO: Web Audio %s",
                      g_audio_live ? "available" : "unavailable (silent)");
    }
    return g_audio_live;
}

/* 0 none, 1 suspended (the autoplay gate), 2 running. */
int ll_audio_state(void)
{
    if (!ll_audio_enabled())
        return 0;
    return ll_audio_js_state();
}

int ll_audio_voice_new(void)
{
    int v;
    if (!ll_audio_enabled())
        return 0;
    v = g_audio_next_voice++;
    ll_audio_js_chain(v);
    return v;
}

void ll_audio_voice_free(int voice)
{
    if (!voice || !g_audio_live)
        return;
    ll_audio_js_stop(voice);
    ll_audio_js_release(voice);
}

void ll_audio_set_levels(int voice, int volume_cb, int pan_cb)
{
    if (!voice || !g_audio_live)
        return;
    ll_audio_js_gain(voice, volume_cb, pan_cb);
}

void ll_audio_set_rate(int voice, double rate_mul)
{
    if (!voice || !g_audio_live)
        return;
    ll_audio_js_rate(voice, rate_mul);
}

void ll_audio_stop(int voice)
{
    if (!voice || !g_audio_live)
        return;
    ll_audio_js_stop(voice);
}

/* A static buffer: snapshot the PCM and start a source node. Returns 1 when a
 * sound is actually audible, 0 when it was refused (no context, or the autoplay
 * gate is still shut) -- the caller does not care, because DirectSound's Play
 * has already succeeded by then; the return is for the trace. */
int ll_audio_play_static(int voice, const void* pcm, unsigned int bytes,
                         unsigned int rate, int channels, int bits,
                         unsigned int offset, int looping,
                         int volume_cb, int pan_cb, double rate_mul)
{
    if (!voice || !g_audio_live || !pcm || !bytes)
        return 0;
    ll_audio_js_stop(voice);
    if (!ll_audio_js_decode(voice, (int)(long)pcm, (int)bytes, (int)rate,
                            channels, bits))
        return 0;
    ll_audio_js_gain(voice, volume_cb, pan_cb);
    return ll_audio_js_start(voice, (int)offset, looping, rate_mul);
}

/* A streamed buffer: hand over the game's buffer and the cursor it is filling
 * against, and let the JS side take whatever is now safely behind it. */
int ll_audio_feed_stream(int voice, const void* pcm, unsigned int bytes,
                         unsigned int cursor, unsigned int chunk,
                         unsigned int rate, int channels, int bits,
                         double rate_mul)
{
    if (!voice || !g_audio_live || !pcm || !bytes || !chunk)
        return 0;
    return ll_audio_js_feed(voice, (int)(long)pcm, (int)bytes, (int)cursor,
                            (int)chunk, (int)rate, channels, bits, rate_mul);
}
