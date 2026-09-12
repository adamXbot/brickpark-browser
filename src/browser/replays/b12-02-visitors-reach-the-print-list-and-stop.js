// PORT-B12 replay 02 — P1-6 CONFIRMED, and narrowed to one function.
//
// ONE LINE: the visitors are sorted into the print list, the print list is
// walked, `Render3DPerson` (rin.c:535) IS entered for 27 of 30 of them every
// frame — and `Draw3DPersonModel` (person3d.c:1092, the 63.3% WIP body)
// rasterises nothing. Hiding all thirty blokes changes the frame by fewer
// pixels than the scenery's own animation noise.
//
// Owner: a matching / render lane on person3d.c's Draw3DPersonModel (and the
// tri3d.c triangle fillers it calls). NOT shim-side: every shim object on the
// path (IntersectRect, the locked surface, the print-list arena) checks out.
//
// Paste p1-00-prelude.js, run P1.start(), let the park fill, then this.

(async function () {
  const W = (ms) => new Promise((r) => setTimeout(r, ms));
  const a = llAddrs(), I = HEAP32, F = new Float32Array(I.buffer);
  const dv = new DataView(I.buffer);
  const g = Module.canvas.getContext('2d');
  const log = [];

  await llMove(620, 460, 120);          // cursor off the park

  // ---- the chain, with the flag that decides whether it is even sorted ---
  // RenderPeople (renderlist.c:148) skips `flags62 & 0xa0`: 0x80 = suppressed,
  // 0x20 = inside a ride vehicle (the ride draws its own occupants later).
  const P = [];
  { let p = I[a.g_people_head >> 2], n = 0;
    while (p && n < 400) {
      const q = I[(p + 4) >> 2];
      if (q) P.push({ b: p, q, f: dv.getUint16(p + 0x62, true),
                      sx: I[(q + 0x1c) >> 2], sy: I[(q + 0x20) >> 2] });
      p = I[p >> 2]; n++; } }
  const sorted = P.filter((r) => !(r.f & 0xa0));
  log.push({ chain: P.length, sortedThisFrame: sorted.length,
             flags62: P.reduce((h, r) => (h['0x' + r.f.toString(16)] =
                                          (h['0x' + r.f.toString(16)] || 0) + 1, h), {}) });

  // ---- 1. they DO reach the print list ----------------------------------
  // SortPerson (blokeai.c:218) bump-allocates a 0x20-byte node, stamps
  // type 0x2000 at +0x0c and the Person3D at +0x1c. DrawAndClearPrintList
  // resets the CURSOR, not the arena, so last frame's nodes are still there.
  // Signature scan: a word 0x2000 whose +0x10 neighbour is a live Person3D.
  const live = new Set(P.map((r) => r.q));
  const nodes = [];
  for (let w = 0; w < I.length - 8; w++)
    if (I[w] === 0x2000) {
      const base = (w << 2) - 0x0c;
      if (base > 0 && live.has(I[(base + 0x1c) >> 2])) nodes.push(base);
    }
  log.push({ printNodesType0x2000: nodes.length,          // 23 of 30, keys sane
             arena: nodes.length ? [Math.min(...nodes), Math.max(...nodes)] : null,
             sample: nodes.slice(0, 3).map((b) => ({
               key: I[(b + 8) >> 2], person: I[(b + 0x1c) >> 2] })) });

  // ---- 2. and Render3DPerson IS entered for them -------------------------
  // THE PROBE. rin.c:537-539 writes scale_x/y/z = 1.0f as the FIRST three
  // statements, before the IntersectRect and GetVideoSurface early-outs. Poke
  // a sentinel into scale_x and see which ones come back 1.0.
  for (const r of P) F[(r.q + 0x10) >> 2] = 7.0;
  await W(420);
  const called = P.filter((r) => F[(r.q + 0x10) >> 2] === 1).length;
  log.push({ Render3DPerson_entered: called, of: P.length,
             note: 'the print list is threaded and walked; the persons arrive' });

  // ---- 3. the records are healthy ---------------------------------------
  // Person3D (person3d.c:484): frame +0x4c, faces +0x50, depth +0x54,
  // matrix[9] +0x58 (16.16), tint +0x34, ydepth +0x38.
  const onScreen = P.filter((r) => r.sx > -160 && r.sx < 640 && r.sy > -120 && r.sy < 480);
  log.push({ onScreenPersons: onScreen.map((r) => ({
      flags62: '0x' + r.f.toString(16), sx: r.sx, sy: r.sy,
      frame: I[(r.q + 0x4c) >> 2], faces: I[(r.q + 0x50) >> 2],
      depth: I[(r.q + 0x54) >> 2], tint: I[(r.q + 0x34) >> 2],
      ydepth: F[(r.q + 0x38) >> 2],
      matrix: [0,1,2,3,4,5,6,7,8].map((k) => I[(r.q + 0x58 + 4*k) >> 2]) })) });
  //  matrix is a proper 16.16 rotation (65536 / 46341 = cos 45), frame 4-6,
  //  faces non-null, depth == sy + 45, tint 0xff, ydepth 0 — nothing wrong.

  // ---- 4. THE MEASUREMENT: hide every bloke and the frame does not care --
  // Setting flags62 |= 0x80 makes RenderPeople skip all of them. If any bloke
  // were drawn, the two frames would differ by hundreds of pixels (a minifigure
  // is ~30x60). Three noise baselines are taken so the park's own animation
  // (the entrance flag, the banners, a ride's ornament) is visible for what
  // it is.
  const S = P.map((r) => [r.b, dv.getUint16(r.b + 0x62, true)]);
  const hide = () => { for (const [b, f] of S) dv.setUint16(b + 0x62, f | 0x80, true); };
  const shownAgain = () => { for (const [b, f] of S) dv.setUint16(b + 0x62, f, true); };
  const grab = () => g.getImageData(0, 0, 640, 480).data.slice();
  const diff = (X, Y) => { let c = 0, bx = [1e9, 1e9, -1, -1];
    for (let i = 0; i < X.length; i += 4)
      if (X[i] !== Y[i] || X[i+1] !== Y[i+1] || X[i+2] !== Y[i+2]) {
        c++; const px = (i >> 2) % 640, py = (i >> 2) / 640 | 0;
        if (px < bx[0]) bx[0] = px; if (py < bx[1]) bx[1] = py;
        if (px > bx[2]) bx[2] = px; if (py > bx[3]) bx[3] = py; }
    return { pixels: c, box: bx }; };

  hide();        await W(500); const H1 = grab(); await W(500); const H2 = grab();
  shownAgain();  await W(500); const V1 = grab(); await W(500); const V2 = grab();
  hide();        await W(500); const H3 = grab();
  shownAgain();
  log.push({
    noise_hidden_vs_hidden: diff(H1, H2),   // ~1500 px, box = the animating scenery
    noise_shown_vs_shown:   diff(V1, V2),   // ~1550 px, the SAME box
    hidden_vs_shown:        diff(H2, V1),   // ~1600 px, the SAME box
    shown_vs_hidden:        diff(V2, H3),   // ~1520 px, the SAME box
    verdict: 'thirty blokes are worth nothing: every diff is the scenery'
  });

  // ---- 5. and the geometry consumer is never reached ---------------------
  // Draw3DPersonModel copies EVERY vertex of every person it draws into two
  // plain .bss scratch arrays -- g_xverts (x86 0x00643ee8, 3 ints per vertex)
  // and g_vert_key (0x00641004) -- and rewrites them per person, per frame.
  // So diff the whole static-data region and look for that churn. The highest
  // address any recovered global resolves to is ~3.05 MB, so 0..6 MB covers
  // all of it; above ~11 MB is the allocator's.
  const scan = (span) => {
    const HI = span >> 2, A = I.slice(0, HI);
    return new Promise((res) => setTimeout(() => {
      const B = I.slice(0, HI); const runs = []; let s = -1, ch = 0;
      for (let w = 0; w < A.length; w++) {
        const c = A[w] !== B[w]; if (c) ch++;
        if (c && s < 0) s = w;
        else if (!c && s >= 0) { if (w - s >= 16) runs.push([s * 4, w - s]); s = -1; }
      }
      res({ changedWords: ch, runs });
    }, 180));
  };
  const withBlokes = await scan(6000000);
  hide(); await W(300);
  const without = await scan(3300000);
  shownAgain();
  log.push({ staticDataChurn_withBlokes: withBlokes,     // ~739 words, runs at 2413xxx only
             staticDataChurn_blokesHidden: without,      // the 2413xxx runs vanish
             sampleOfTheOneRun: withBlokes.runs.length
               ? Array.from(I.slice(withBlokes.runs[0][0] >> 2,
                                    (withBlokes.runs[0][0] >> 2) + 12)) : null,
             //  6979, 6966, 10990, 6966 ... = 8.8 walking positions
             //  (printlist.c:455 does `b->fx >> 8` for the cell), i.e. the
             //  bloke AI's own state. NO array anywhere in static data
             //  receives per-vertex data.
             verdict: 'Render3DPerson is entered (step 2) and its geometry '
                    + 'consumer is never reached: the bail is at one of the '
                    + 'two early-outs in rin.c:546-554' });

  return log;
})();

// ---------------------------------------------------------------------------
// WHAT THIS RULES OUT, AND THE ONE CORRECTION TO P1's REPLAY 05
//
//   * NOT the print list. 23 type-0x2000 nodes with real depth keys sit in the
//     arena, and step 2 proves the walk reaches them.
//   * NOT the records. Step 3: matrix, frame, faces, depth, tint all sane.
//   * NOT the shim's rect maths. user32.c's IntersectRect computes into locals
//     before storing, so `IntersectRect(&r, &v, &r)` (rin.c:549 aliases dst and
//     src2) is safe, and g_clip_rect reads (0, 0, 640, 480) in the park.
//   * NOT the locked surface. Every sprite in the frame — terrain, buildings,
//     banners, the entrance — is blitted into the same locked bits the raster
//     target points at, and they all appear.
//   * NOT "they are all off camera". Two to three of the chain are inside the
//     viewport at any instant. It is fewer than P1's eleven because P1 counted
//     the whole chain: THIRTEEN of the thirty carry flags62 & 0x20 and are
//     never sorted at all (they are inside rides/queues), and those are exactly
//     the ones whose stale screen positions cluster on-camera.
//
//   * CORRECTION to p1-05: the ring overlay was drawn at (sx, sy), which is the
//     TOP-LEFT CORNER of the person's 160x120 render window, not the figure.
//     person3d.c:107 centres the model at ox = (80 << 16) - width/2,
//     oy = (90 << 16) - height/2, so a drawn bloke would appear around
//     (sx + 80, sy + 90). Step 4 does not depend on knowing that.
//
// WHERE IT DIES
//
// Step 2 says Render3DPerson is ENTERED; step 5 says its geometry consumer is
// NEVER REACHED. So the bail is at one of the two early-outs between them
// (rin.c:546-554), and the first is already cleared:
//
//     r = g_clip_rect; r.right--; r.bottom--;
//     if (!IntersectRect(&r, &v, &r)) return;   /* shim side is alias-safe,
//                                                  and v is on-screen for 2-3 */
//     OffsetRect(&r, -p->sx, -p->sy);
//     if (!GetVideoSurface(&vs)) return;        /* <-- the candidate */
//
// GetVideoSurface (surface.c:310) returns 0 whenever g_video_locked == 0 — the
// sources call it "the can I draw? test" in as many words. So test FIRST that
// the video surface is not locked while DrawAndClearPrintList walks the list:
// every 3D person would silently return while every SPRITE still drew, because
// sprites go through PrintSprite -> RenderSprite, which pushes and pops its own
// lock around each blit. That is exactly the symptom — a fully drawn park with
// no people in it, no trap, healthy records.
//
// g_video_locked is a GAME global and cannot be sampled mid-frame from the
// console (it reads 0 between frames, which says nothing). It is already in
// main.c's table as llAddrs().g_video_locked; one host-side trace, or one read
// from inside the walk, settles it in ten minutes.
//
// If the surface IS locked, the second candidate is Draw3DPersonModel itself
// (person3d.c:1092, the 1023-instruction WIP body at 63.3%) bailing before its
// vertex copy, and its portable arms are the place to look: FMUL / FMULA /
// FMULP / TOFIX and the SHADE macro (person3d.c:529-596) are hand-written
// replacements for the original's inline asm, and TOFIX is what turns the 0.447
// isometric foreshortening into 16.16 — a model whose scale comes out 0
// rasterises to nothing, silently.
//
// STILL OWED (this lane ran out of clock): the same probe on the TUTORIAL park,
// where PORT-M10's merge note says blokes walk the paths. If they do not draw
// there either on this build, this is a regression after M11 and the bisect is
// short; if they do, the difference is in what StartFreePlayPark (uimisc2.c:404)
// sets up. Steps 1-4 above are the whole probe and need no free-play state.
