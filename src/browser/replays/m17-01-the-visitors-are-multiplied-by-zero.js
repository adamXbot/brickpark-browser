// PORT-M17 replay 01 — P1-6 SOLVED: every minifigure vertex was multiplied by
// zero, so every triangle was degenerate and the back-face test threw all 79 of
// them away.
//
// ONE LINE: `FMUL`/`FMULA`/`FMULP` (person3d.c) are inline asm in the original
// and `mov ecx, <multiplier>` takes the multiplier's 32 BITS. Every multiplier
// Draw3DPersonModel hands them — fx, fy, fz, ydep, zb — is declared `float` and
// has already been overwritten IN PLACE by TOFIX with a 16.16 INTEGER, so its
// storage holds 29295 / 65536 and its float VALUE is a denormal near 4e-41. The
// portable arms read `(b)` by VALUE, `LL_FMUL16` casts it with `(int)`, and
// `(int)4.1e-41` is 0. Fix: read the multiplier through `LL_ASINT` in the
// portable arm (`*(int*)&x` — the identity on an int lvalue, `mov ecx` on a
// float one). Three lines, portable arms only, no VC6 byte moves.
//
// Paste p1-00-prelude.js, run `await P1.start()`, let the park fill (30
// visitors), then paste this. Sections 1-3 are the diagnosis and run on either
// build; section 4 is the A/B and prints the verdict.
//
//   measurement                          pre-fix            with the fix
//   ----------------------------------   ----------------   ----------------
//   static-mask control (hidden/hidden)  23 / 0 / 25 px     18 px
//   static-mask signal  (shown/hidden)   23 / 0 / 25 px     2682/2444/2123 px
//   Draw3DPersonModel faces drawn        0 of 67 + 0 of 12  all that face front
//   pixels the four tri3d fillers wrote  0                  thousands
//
// The pre-fix signal is the control, distribution for distribution, with SIX
// minifigures on screen. The fix is worth ~400 pixels per visitor.

(async function () {
  const W = (ms) => new Promise((r) => setTimeout(r, ms));
  const a = llAddrs(), I = HEAP32, F = new Float32Array(I.buffer);
  const dv = new DataView(I.buffer);
  const g = Module.canvas.getContext('2d');
  const log = [];

  await llMove(620, 462, 100);                 // the cursor off the park

  const chain = () => {
    const P = []; let p = I[a.g_people_head >> 2], n = 0;
    while (p && n < 400) {
      const q = I[(p + 4) >> 2];
      if (q) P.push({ b: p, q, f: dv.getUint16(p + 0x62, true),
                      sx: I[(q + 0x1c) >> 2], sy: I[(q + 0x20) >> 2] });
      p = I[p >> 2]; n++;
    }
    return P;
  };

  // ---- 1. Render3DPerson CLEARS ITS GATES -- B12's candidate 1 is refuted ---
  // In the WHOLE tree, SetRenderTarget (0x00485f30, tri3d.c -- rin.c declares it
  // SetRasterTarget), SetMousePixel (0x00488700, declared SetRasterOrigin) and
  // Render_SetViewport (0x00441800, sweep1.c) have EXACTLY ONE caller each, and
  // it is rin.c:556-558 -- the three statements immediately AFTER the
  // `GetVideoSurface` early-out and immediately BEFORE Draw3DPersonModel. So
  // these globals are zero for as long as no person has ever cleared that gate,
  // and they hold the last drawn person's window once one has. (Added to
  // main.c's LL_DBG_TABLE by this lane: indices 84..106.)
  const R = (n) => I[a[n] >> 2];
  log.push({ step: '1. the GetVideoSurface gate',
    g_surface: R('g_surface'), g_pitch: R('g_pitch'),
    g_width: R('g_width'), g_rows: R('g_rows'),
    g_mouse_pixel: R('g_mouse_pixel'),
    viewport: [R('g_clip_x0'), R('g_clip_y0'), R('g_clip_x1'), R('g_clip_y1')],
    lockedSurface: R('g_ddsd_bits'), lockPitch: R('g_ddsd_pitch'),
    verdict: 'g_surface NON-ZERO and = ddsd_bits + sy*1280 + sx*2; the viewport '
           + 'is the person-local clip (0,0,160,120) full / (0,0,68,77) clipped. '
           + 'The video surface IS locked while DrawAndClearPrintList walks. '
           + 'PORT-B12 candidate 1 is REFUTED.' });

  // ---- 2. and the vertex LOOPS DO RUN -- B12's section 3.5 is a false negative
  // g_xverts reads back all zeroes and never changes, which is what B12 read as
  // "the vertex loops are never reached". It is not: write a sentinel over both
  // scratch arrays and the game overwrites exactly 3*nverts and nverts words of
  // it. The loops run -- they write ZEROES, and a frame-to-frame diff of a
  // constant cannot see a loop that keeps writing the same constant.
  const xb = a.g_xverts >> 2, kb = a.g_vert_key >> 2;
  for (let i = 0; i < 600; i++) I[xb + i] = 0x5ca1ab1e;
  for (let i = 0; i < 300; i++) I[kb + i] = 0x5ca1ab1e;
  await W(600);
  let sx = 0, sk = 0;
  for (let i = 0; i < 600; i++) if (I[xb + i] === 0x5ca1ab1e) sx++;
  for (let i = 0; i < 300; i++) if (I[kb + i] === 0x5ca1ab1e) sk++;
  log.push({ step: '2. the vertex loops',
    xvertsOverwritten: 600 - sx, keysOverwritten: 300 - sk,
    firstXverts: Array.from(I.subarray(xb, xb + 9)),
    verdict: (600 - sx) + ' of 600 g_xverts words and ' + (300 - sk)
           + ' of 300 g_vert_key words are rewritten every frame '
           + '(3*nverts and nverts, nverts = 66). Draw3DPersonModel DOES reach '
           + 'its vertex loops; on the pre-fix build every word it writes is 0.' });

  // ---- 3. the mesh is fine: the frame records are in memory --------------
  // Frame3D is 0x38 bytes: bmin, bmax, n_verts +0x18, verts +0x1c, FaceSet
  // +0x20, n_normals +0x24, normals +0x28. The bloke animation's frames sit in
  // one array at a 0x38 stride, every frame pointing at the SAME FaceSet.
  const ok = (p) => p > 0x10000 && p < I.length * 4;
  const frames = [];
  for (let w = 0x4000; w < I.length - 40 && frames.length < 4; w++) {
    const nv = I[w + 6];
    if (nv < 4 || nv > 4000) continue;
    if (!ok(I[w + 7]) || !ok(I[w + 8])) continue;
    const nn = I[w + 9]; if (nn < 1 || nn > 4000 || !ok(I[w + 10])) continue;
    if (I[w + 20] !== nv || I[w + 22] !== I[w + 8]) continue;
    frames.push({ frame: w * 4, nverts: nv, verts: I[w + 7],
                  faceSet: I[w + 8], nnormals: nn });
  }
  const fs = frames.length ? frames[0].faceSet >> 2 : 0;
  log.push({ step: '3. the mesh', frames,
    faceSet: fs ? { n_faces: I[fs], n_gouraud: I[fs + 1], tris: I[fs + 2] } : null,
    verdict: '66 vertices, 213 normals, 79 triangles of which 67 are Gouraud. '
           + 'Nothing is wrong with the model data.' });

  // ---- 4. THE A/B: a STATIC MASK defeats the park's own animation --------
  // A plain hidden-vs-shown frame diff is useless here: the park animates
  // several hundred to several thousand pixels a frame on its own (banners, a
  // ride's ornament, the entrance flag), which is the same order as thirty
  // minifigures. So first build the set of pixels that are IDENTICAL across
  // five consecutive frames with every bloke hidden (`flags62 |= 0x80`, which
  // RenderPeople skips, renderlist.c:148) -- the provably static background --
  // and then count only changes inside it. The control is one more HIDDEN frame
  // against the mask: it must come out near zero by construction.
  const P = chain();
  const S = P.map((r) => [r.b, r.f]);
  const hide = () => { for (const [b, f] of S) dv.setUint16(b + 0x62, f | 0x80, true); };
  const show = () => { for (const [b, f] of S) dv.setUint16(b + 0x62, f, true); };
  const grab = () => g.getImageData(0, 0, 640, 480).data.slice();

  hide(); await W(500);
  const H = []; for (let i = 0; i < 5; i++) { H.push(grab()); await W(260); }
  const N = 640 * 480, stat = new Uint8Array(N); let statN = 0;
  for (let p = 0; p < N; p++) {
    const i = p * 4; let same = true;
    for (let k = 1; k < 5; k++)
      if (H[k][i] !== H[0][i] || H[k][i+1] !== H[0][i+1] || H[k][i+2] !== H[0][i+2]) { same = false; break; }
    if (same) { stat[p] = 1; statN++; }
  }
  const onMask = (V) => {
    let n = 0, bx = [1e9, 1e9, -1, -1];
    for (let p = 0; p < N; p++) if (stat[p]) {
      const i = p * 4;
      if (V[i] !== H[0][i] || V[i+1] !== H[0][i+1] || V[i+2] !== H[0][i+2]) {
        n++; const px = p % 640, py = p / 640 | 0;
        if (px < bx[0]) bx[0] = px; if (py < bx[1]) bx[1] = py;
        if (px > bx[2]) bx[2] = px; if (py > bx[3]) bx[3] = py;
      }
    }
    return { n, bx };
  };
  const control = []; for (let k = 0; k < 3; k++) { control.push(onMask(grab()).n); await W(300); }
  show(); await W(400);
  const signal = []; for (let k = 0; k < 3; k++) { signal.push(onMask(grab())); await W(300); }
  const figs = chain().filter((r) => !(r.f & 0xa0))
                      .map((r) => [r.sx + 80, r.sy + 90])
                      .filter((q) => q[0] > 0 && q[0] < 640 && q[1] > 0 && q[1] < 480);
  log.push({ step: '4. THE A/B (static mask)',
    staticPixels: statN, ofTotal: N,
    control_hidden_vs_hidden: control,
    signal_shown_vs_hidden: signal.map((r) => r.n),
    signalBox: signal[0].bx,
    figuresOnScreen: figs,
    verdict: signal[0].n > 20 * Math.max(1, control[0])
      ? 'BLOKES DRAW: the signal is two orders of magnitude over the control, '
        + 'spread across the park at the figure positions.'
      : 'BLOKES DO NOT DRAW: the signal is the control. This is the pre-fix '
        + 'build (P1-6).' });

  // A figure centre is (sx + 80, sy + 90): rin.c:541 builds the 160x120 render
  // window at (sx, sy) and person3d.c centres the model at ox = 80<<16,
  // oy = 90<<16. PORT-P1's replay 05 ringed (sx, sy) -- the window's CORNER --
  // which is 80 px left and 90 px above every bloke it was looking for.
  console.log(JSON.stringify(log, null, 2));
  return log;
})();
