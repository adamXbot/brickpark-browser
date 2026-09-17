#!/usr/bin/env node
// The narration stream, sample for sample, through the REAL scheduler code.
//
//   node tests/web/test_narration_feed.mjs [--source path/to/ll_audio.c]
//
// ll_audio_js_feed, ll_audio_js_stop and ll_audio_js_written are read out of
// src/hostwin/ll_audio.c (their EM_JS bodies are plain JavaScript) and
// run over a fake AudioContext whose clock the test owns. Around them, a
// line-for-line model of the three things they sit between:
//
//   dsound.c        the wall-clock play cursor (44100 bytes/s, looping over
//                   0xa000 bytes); every GetCurrentPosition runs the feed, every
//                   Unlock reports the write head, and a Play stops the voice and
//                   feeds at once
//   movie.c         RewindNarrationBuffer: ten 0x1000 blocks primed, then Play
//   narration2.c    PumpNarration, once a frame: overwrite the fill block while
//                   the cursor is not in it; Stop when blocks_ready reaches 0
//
// The "speech" is a long-period pseudo-random stream, so any splice, repeat,
// gap or read past the ring shows up as a wrong sample. Asset-free: CI runs it.

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const args = process.argv.slice(2);
const sourceArg = args.indexOf('--source');
const SOURCE = sourceArg >= 0 ? args[sourceArg + 1] : new URL('../../src/hostwin/ll_audio.c', import.meta.url);
const SRC = readFileSync(SOURCE, 'utf8');

// An EM_JS body as a callable taking its C parameters BY NAME, so the harness
// runs against an ll_audio.c whose signature differs (--source an older one).
function emjs(name, optional = false) {
  const m = new RegExp('EM_JS\\((\\w+), ' + name + ', \\(([^)]*)\\), \\{').exec(SRC);
  if (!m) {
    if (optional) return null;
    throw new Error('EM_JS ' + name + ' not found in ' + SOURCE);
  }
  const start = m.index + m[0].length;
  let depth = 1, i = start;
  for (; i < SRC.length && depth; i++) {
    if (SRC[i] === '{') depth++;
    else if (SRC[i] === '}') depth--;
  }
  const params = m[2].split(',').map((p) => p.trim().split(/[\s*]+/).pop()).filter(Boolean);
  // eslint-disable-next-line no-new-func
  const fn = new Function('globalThis', 'HEAPU8', 'HEAP16', ...params, SRC.slice(start, i - 1));
  return (g, HEAPU8, HEAP16, named) => fn(g, HEAPU8, HEAP16, ...params.map((p) => named[p]));
}
const feed = emjs('ll_audio_js_feed');
const stop = emjs('ll_audio_js_stop');
const written = emjs('ll_audio_js_written', true);

const RING = 0xa000, BLOCK = 0x1000, RATE = 22050, BYTE_RATE = RATE * 2, CHUNK = 2560;
const CTX0 = 3.25;

function prng(seed) {
  let s = seed >>> 0 || 1;
  return () => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s / 4294967296; };
}

function runLine({ seconds, frameMs, driftPpm = 0 }) {
  // The wasm heap: the ring at PTR, and noise after it, so a read past the end
  // of the ring is a wrong sample rather than a lucky zero.
  const heap = new ArrayBuffer(4 * RING);
  const HEAPU8 = new Uint8Array(heap), HEAP16 = new Int16Array(heap);
  const PTR = RING;
  for (let i = 2 * RING; i < 3 * RING; i++) HEAPU8[i] = (i * 131 + 17) & 255;
  const ring = HEAPU8.subarray(PTR, PTR + RING);

  const lcg = prng(0x5eed);
  const clip = new Int16Array(Math.round(seconds * RATE)).map(() => Math.floor(lcg() * 65536) - 32768);
  const clipBytes = new Uint8Array(clip.buffer);

  let t = 50000;                                     // wall clock, ms
  const T0 = t;
  const clock = { ctx: () => CTX0 + ((t - T0) / 1000) * (1 + driftPpm / 1e6) };
  const sources = [];
  const ctx = {
    state: 'running',
    get currentTime() { return clock.ctx(); },
    createBuffer(ch, frames, rate) {
      const data = Array.from({ length: ch }, () => new Float32Array(frames));
      return { numberOfChannels: ch, length: frames, sampleRate: rate, getChannelData: (c) => data[c] };
    },
    createBufferSource() {
      const s = {
        buffer: null, playbackRate: { value: 1 }, startAt: null, stopAt: Infinity,
        connect() {}, disconnect() {},
        start(when) { this.startAt = when === undefined ? clock.ctx() : when; },
        stop(when) { this.stopAt = Math.min(this.stopAt, when === undefined ? clock.ctx() : when); }
      };
      sources.push(s);
      return s;
    }
  };
  const g = { __llAudio: { ctx, voices: { 1: { gain: {} } }, created: 1, started: 0, blocked: 0,
                           fed_chunks: 0, fed_bytes: 0, underruns: 0, refused: 0, formats: {} } };
  const call = (fn, named) => fn(g, HEAPU8, HEAP16, named);

  // dsound.c
  let playing = false, playT0 = 0;
  const cursor = () => Math.floor(((t - playT0) * BYTE_RATE) / 1000) % RING;
  const getPos = () => {
    const c = cursor();
    if (playing)
      call(feed, { v: 1, ptr: PTR, bytes: RING, cursor: c, chunk: CHUNK, rate: RATE, ch: 1, bits: 16, rate_mul: 1 });
    return c;
  };
  const unlock = (end, full) => { if (written) call(written, { v: 1, end, full: full ? 1 : 0 }); };

  // movie.c RewindNarrationBuffer, then Play
  let dpos = 0, fill = 0, ready = 0;
  const writeBlock = (b) => {
    const n = Math.min(BLOCK, clipBytes.length - dpos);
    ring.set(clipBytes.subarray(dpos, dpos + n), b * BLOCK);
    ring.fill(0, b * BLOCK + n, (b + 1) * BLOCK);
    dpos += n;
    if (n) ready++;
  };
  for (fill = 0; fill < 10; fill++) writeBlock(fill);
  unlock(0, true);                                   // the whole ring in one Lock
  fill = 0;
  playing = true;
  playT0 = t;
  call(stop, { v: 1 });
  getPos();

  // narration2.c PumpNarration, once a frame
  const inFill = (c) => c >= fill * BLOCK && c < (fill + 1) * BLOCK;
  let stoppedAt = null;
  for (let frame = 0; playing && frame < 1e6; frame++) {
    t += frameMs(frame);
    if (inFill(getPos())) continue;
    do {
      writeBlock(fill);
      unlock(((fill + 1) * BLOCK) % RING, false);
      fill = (fill + 1) % 10;
      if (--ready === 0) {
        getPos();                                    // LL_DSB_Stop ticks first
        playing = false;
        stoppedAt = clock.ctx();
        call(stop, { v: 1 });
        break;
      }
    } while (!inFill(getPos()));
  }
  assert.ok(stoppedAt !== null, 'the line ended');

  // The output as heard.
  const heard = new Map();                           // output sample index -> value
  for (const s of sources) {
    if (s.startAt === null || !s.buffer) continue;
    const data = s.buffer.getChannelData(0);
    const at = Math.round((s.startAt - CTX0) * RATE);
    const len = Math.min(data.length, Math.round((s.stopAt - s.startAt) * RATE));
    for (let i = 0; i < len; i++) {
      assert.ok(!heard.has(at + i), 'two sources overlap at output sample ' + (at + i));
      heard.set(at + i, Math.round(data[i] * 32768));
    }
  }
  const idx = [...heard.keys()].sort((x, y) => x - y);
  // Every contiguous run of output must be a contiguous run of the clip, and
  // the runs must move forward through it. A run is located by its first 16
  // samples: one 16-bit value recurs every ~65k samples of noise, 16 do not.
  const PROBE = 16;
  let wrong = 0, gaps = 0, inOrder = 0, clipPos = 0, firstWrong = null;
  for (let i = 0; i < idx.length;) {
    let j = i + 1;
    while (j < idx.length && idx[j] === idx[j - 1] + 1) j++;
    const run = idx.slice(i, j).map((o) => heard.get(o));
    if (i > 0) gaps++;
    let start = -1;
    const probe = Math.min(PROBE, run.length);
    for (let p = clipPos; p + probe <= clip.length && start < 0; p++) {
      let ok = true;
      for (let q = 0; q < probe; q++) if (clip[p + q] !== run[q]) { ok = false; break; }
      if (ok) start = p;
    }
    for (let q = 0; q < run.length; q++) {
      const inClip = start >= 0 && start + q < clip.length;
      if (inClip && clip[start + q] === run[q]) inOrder++;
      else if (inClip || run[q] !== 0) {                    // zero padding past the clip is fine
        wrong++;
        if (!firstWrong) firstWrong = { outSample: idx[i + q], runStart: start, offset: q };
      }
    }
    if (start >= 0) clipPos = start + run.length;
    i = j;
  }
  return { clip: clip.length, heard: idx.length, inOrder, wrong, gaps, firstWrong,
           underruns: g.__llAudio.underruns, tailMs: ((clip.length - inOrder) * 1000) / RATE };
}

const tests = [];
const test = (name, fn) => tests.push({ name, fn });
const r = prng(42);

test('steady 35 fps: every sample, in order, no gap', () => {
  const s = runLine({ seconds: 12, frameMs: () => 1000 / 35 });
  assert.equal(s.wrong, 0, 'samples that are not the clip');
  assert.equal(s.gaps, 0);
  assert.equal(s.underruns, 0);
  assert.ok(s.tailMs < 60, 'only the last few ms are cut by the Stop (' + s.tailMs.toFixed(1) + ' ms)');
});

test('frames of 18-45 ms and a 150 ppm clock difference', () => {
  const s = runLine({ seconds: 12, frameMs: () => 18 + r() * 27, driftPpm: 150 });
  assert.equal(s.wrong, 0, 'samples that are not the clip');
  assert.equal(s.gaps, 0);
  assert.equal(s.underruns, 0);
});

test('2% of frames stall for 150-240 ms, shorter than the lead', () => {
  const s = runLine({ seconds: 12, frameMs: () => (r() < 0.02 ? 150 + r() * 90 : 20 + r() * 20) });
  assert.equal(s.wrong, 0, 'samples that are not the clip');
  assert.equal(s.gaps, 0);
  assert.equal(s.underruns, 0);
});

test('a 700 ms stall is one underrun: a skip, never a splice', () => {
  const s = runLine({ seconds: 8, frameMs: (f) => (f === 90 ? 700 : 1000 / 35) });
  assert.equal(s.wrong, 0, 'after the stall the output resumes at the cursor, in order');
  assert.equal(s.underruns, 1);
  assert.equal(s.gaps, 1);
});

test('stalls of 300-500 ms again and again', () => {
  const s = runLine({ seconds: 20, frameMs: (f) => (f % 70 === 35 ? 300 + r() * 200 : 25 + r() * 10) });
  assert.equal(s.wrong, 0, 'samples that are not the clip');
  assert.ok(s.underruns >= 1, 'the long stalls do drain the lead');
});

let failed = 0;
for (const t of tests) {
  try {
    t.fn();
    console.log('ok   ' + t.name);
  } catch (e) {
    failed++;
    console.log('FAIL ' + t.name + '\n     ' + String(e.message || e).split('\n')[0]);
  }
}
console.log(`\n${tests.length - failed}/${tests.length} passed (${SOURCE})`);
process.exit(failed ? 1 : 0);
