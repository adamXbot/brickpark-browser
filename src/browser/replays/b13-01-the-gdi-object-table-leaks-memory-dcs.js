/* PORT-B13 replay 01 — P4-3: the three text boxes that fill solid black, and
 * why. Also the two harness findings, P4-4 and P4-5, measured on the same page.
 *
 *   Serve portable/build-wasm and open
 *     legoland.html?args=-nointro+WINDEBUG&awake=1
 *   await fetch('replays/p4-00-prelude.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/b13-01-the-gdi-object-table-leaks-memory-dcs.js')
 *           .then(r=>r.text()).then(eval);
 *   await B13.all();            // ~6 minutes: it plays the leak out in full
 *
 * WHAT THE DEFECT IS.  Nothing in the game is wrong and nothing in ll_font.c is
 * wrong.  `SetBkMode(TRANSPARENT)` was honoured all along — P4's first guess is
 * refuted by llGdi(): setBkMode.opaque is 0 and setBkMode.transparent counts
 * every call.  The chain is:
 *
 *   1. gdi32.c's DeleteDC released the DC's ATTRIBUTE entry and not the OBJECT
 *      slot CreateCompatibleDC took, so every balanced CreateCompatibleDC /
 *      DeleteDC pair the game makes leaked a slot out of 256.  fpui2.c's
 *      HTBubbleHelp makes one per tooltip.
 *   2. With the table full, obj_new handed back a handle with NO SLOT in it —
 *      0x4c475000 for a brush — which obj_get rejects.
 *   3. ll_host_brush_colour then falls back to BLACK, so FillRect writes 0x0000
 *      across the whole cell of a cached-text sprite.
 *   4. bubblecache.c:419 DrawCachedTextSprite fills that cell with
 *      GetNearestColor(ink) and then makes THAT SAME INK the sprite's colour
 *      key (line 459), because the fill is meant to be invisible.  The key is
 *      still the ink (0x001f for the money readout's 0xff0000); the fill is now
 *      black; the key misses it; RenderSprite blits the cell OPAQUE.
 *   5. The text is printed in `paper` on top of the fill, so it survives — which
 *      is exactly what PORT-P4 saw: legible text on a solid black box, in the
 *      money readout, the objective bubble and the info pop-up's body at once,
 *      surviving a MAP round trip because the table never recovers.
 *
 * THE MEASUREMENT, on one build, lesson 5, identical protocol either side:
 *
 *   | | before | after |
 *   | objs.byClass.dc after 281 HUD hovers | 255 (table full) | 4 |
 *   | objs.exhausted                       | 4                | 0 |
 *   | fill.noBrush                         | 3                | 0 |
 *   | money readout x220..430 y8..30, pure black | 51.7%      | 0.0% |
 *   | fillLast.colorref / ck.lastLow        | 0x0 / 0x1f      | 0xff0000 / 0x1f |
 *
 * The last row is the whole finding in two numbers: the fill and the colour key
 * have to be the SAME colour, and a full object table made them different.
 */

window.B13 = {
  W: (window.llSleep || function (ms) {
    return new Promise(function (r) { setTimeout(r, ms); }); }),

  /* The three rectangles PORT-P4 measured, in game pixels. */
  BOX: { money: [220, 8, 210, 22], bubble: [420, 330, 210, 45] },

  /* Percentage of PURE BLACK in a rectangle of the canvas, plus its five
   * commonest colours. A luminance threshold would not do: the question is
   * whether the pixels are 0x000000 — the value an unresolved brush writes —
   * and not whether they are dark. */
  px: function (x, y, w, h) {
    var d = Module.canvas.getContext('2d').getImageData(x, y, w, h).data;
    var m = {}, n = 0, black = 0, i;
    for (i = 0; i < d.length; i += 4) {
      var k = ((d[i] << 16) | (d[i + 1] << 8) | d[i + 2]).toString(16);
      while (k.length < 6) k = '0' + k;
      m[k] = (m[k] || 0) + 1; n++;
      if (d[i] === 0 && d[i + 1] === 0 && d[i + 2] === 0) black++;
    }
    return { blackPct: +(100 * black / n).toFixed(1),
             top: Object.keys(m).sort(function (a, b) { return m[b] - m[a]; })
                    .slice(0, 5)
                    .map(function (k) { return k + ':' + (100 * m[k] / n).toFixed(1) + '%'; }) };
  },
  boxes: function () {
    return { money: this.px.apply(this, this.BOX.money),
             bubble: this.px.apply(this, this.BOX.bubble) };
  },

  /* :COLDHARDCASH. The point here is not the 5000 bricks, it is that the money
   * READOUT's text changes, so PrintCachedText misses its cache and RASTERISES
   * A NEW SPRITE — which is what puts a fresh FillRect through the shim. A
   * money box that is already drawn stays correct however full the table gets;
   * the black arrives with the next value. (Shift is how the game types ':'.) */
  cash: async function () {
    var m0 = llPark().money;
    await llKey('ShiftLeft', '');
    await llType('COLDHARDCASH');
    await this.W(2500);
    return { money: [m0, llPark().money] };
  },

  /* Drain the object table by HOVERING. Every tooltip measure is one
   * CreateCompatibleDC/DeleteDC pair (fpui2.c:1009/1016), i.e. one leaked slot
   * before the fix and none after. Stops when the table is exhausted AND a fill
   * has gone down with an unresolved brush — the two facts the defect needs —
   * or after `max` hovers, which is the pass condition once it is fixed. */
  drain: async function (max) {
    var i, g, t0 = Date.now(), trace = [];
    max = max || 320;
    for (i = 0; i < max; i++) {
      await llMove(30 + (i % 5) * 90, 443, 120);
      g = llGdi();
      if (i % 20 === 0)
        trace.push({ hover: i, live: g.objs.live, dc: g.objs.byClass.dc || 0,
                     cap: g.objs.capacity, exhausted: g.objs.exhausted });
      if (g.objs.exhausted > 0 && g.fill.noBrush > 0) break;
    }
    g = llGdi();
    return { hovers: i, ms: Date.now() - t0, trace: trace,
             objs: g.objs, fill: g.fill, fillLast: g.fillLast, ck: g.ck,
             blt: g.blt, setBkMode: g.setBkMode };
  },

  /* The whole thing: park -> a money change -> the drain -> another money
   * change -> the three boxes. Returns the table the notes quote. */
  all: async function () {
    var L = [];
    L.push({ tag: 'in the park', park: llPark(), gdi: llGdi().objs,
             boxes: this.boxes() });
    L.push({ tag: 'money changes once', cash: await this.cash(),
             boxes: this.boxes(), gdi: llGdi().objs });
    L.push({ tag: 'drain', drain: await this.drain() });
    L.push({ tag: 'money changes again', cash: await this.cash(),
             boxes: this.boxes(), gdi: llGdi(),
             /* The money digits, read back off the canvas: they print either
              * way, which is the half of P4-3 that is NOT broken. */
             ascii: llAscii(240, 10, 96, 13, 40, true) });
    L.push({ tag: 'health', stats: llStats() });
    return L;
  },

  /* ---- P4-4: the canvas is never scaled ---------------------------------
   * A MouseEvent's clientX is a `long`. Dispatch eight adjacent game columns
   * and read back what the page's own inverse makes of them: at 1:1 they come
   * back 320..327, and at any scale below 1 several of them collide. Run it at
   * two pane sizes. */
  point: function () {
    var c = Module.canvas, r = c.getBoundingClientRect(), got = [];
    var h = function (e) { got.push([e.clientX,
                                     +((e.clientX - r.left) * 640 / r.width).toFixed(2)]); };
    c.addEventListener('mousemove', h);
    for (var gx = 320; gx < 328; gx++)
      c.dispatchEvent(new MouseEvent('mousemove',
        { clientX: r.left + gx * r.width / 640, clientY: r.top + 10, bubbles: true }));
    c.removeEventListener('mousemove', h);
    return { vw: innerWidth, rect: [r.width, r.height], scale: r.width / 640,
             sent: '320..327', got: got,
             distinct: new Set(got.map(function (g) { return g[1]; })).size };
  },

  /* ---- P4-5: the driver's waits ------------------------------------------
   * The ambient hidden-tab clamp does not engage in every pane, so this
   * installs Chrome's documented behaviour explicitly — a real timer longer
   * than 1 ms does not fire before the next 1 s tick — and times one llClick
   * and one llType of 20 characters with the waits going through a setTimeout
   * (the pre-B13 driver) and through the keep-awake hop (the current one).
   * The hop's own 0/1 ms timers are left alone, so the GAME is awake for both
   * and the fps column stays at the 28 ms FlipPrimary ceiling either way. */
  waits: async function (withClamp) {
    var base = window.setTimeout, out = { hidden: document.hidden,
                                          awake: llAwake().engaged, clamp: !!withClamp };
    var oldSleep = function (ms) {
      return new Promise(function (r) { base.call(window, r, ms); }); };
    var pt = function (gx, gy) { var c = Module.canvas, r = c.getBoundingClientRect();
      return { c: c, x: r.left + gx * r.width / 640, y: r.top + gy * r.height / 480 }; };
    var ev = function (t, p) { p.c.dispatchEvent(new MouseEvent(t,
      { clientX: p.x, clientY: p.y, bubbles: true, cancelable: true })); };
    var oldClick = async function (gx, gy) { var p = pt(gx, gy);
      ev('mousemove', p); await oldSleep(150);
      ev('mousedown', p); await oldSleep(150);
      ev('mouseup', p);   await oldSleep(500); };
    var oldType = async function (s) { for (var i = 0; i < s.length; i++) {
      window.dispatchEvent(new KeyboardEvent('keydown',
        { code: 'Key' + s[i].toUpperCase(), key: s[i], bubbles: true }));
      await oldSleep(150);
      window.dispatchEvent(new KeyboardEvent('keyup',
        { code: 'Key' + s[i].toUpperCase(), key: s[i], bubbles: true }));
      await oldSleep(120); } };
    var T = async function (f) { var t = performance.now(); await f();
                                 return Math.round(performance.now() - t); };
    if (withClamp)
      window.setTimeout = function (fn, d) {
        if (typeof d === 'number' && d > 1)
          return base.call(window, fn, Math.max(d, 1000));
        return base.apply(window, arguments); };
    try {
      out.fpsStart = llStats().fps;
      out.oldClick = await T(function () { return oldClick(5, 5); });
      out.newClick = await T(function () { return llClick(5, 5); });
      out.newType20 = await T(function () { return llType('abcdefghijklmnopqrst'); });
      out.oldType20 = await T(function () { return oldType('abcdefghijklmnopqrst'); });
      out.fpsEnd = llStats().fps;
    } finally { window.setTimeout = base; }
    return out;
  }
};
'B13 replay installed — B13.all(), B13.point(), B13.waits(true)';
