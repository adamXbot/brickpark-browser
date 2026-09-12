/* PORT-M16 replay — P2-2, the LEGOLAND theme button lost for good.
 *
 * PORT-P2 recorded this as a RACE between the side panel's scroll animation
 * and MAP mode, reproducing at 1300 ms between clicks and not at 1500.  It is
 * neither a race nor a timing question: it is THREE CLICKS, at any speed, and
 * the cause is the `g_popup` name collision (PORT-P2 P2-5 row 7 = PORT-P3
 * P3-2).  See docs/lanes/scope-port-m16.md.
 *
 *   M16.repro()   the three clicks, before/after flags and pill colour
 *   M16.dead()    the permanence check (P2.themeButtonDead's shape)
 *   M16.fuzz(n,s) randomised click sequences with a per-tick flag watch
 *   M16.realtime() re-installs a MessageChannel setTimeout so the game runs at
 *                 35 fps in a HIDDEN tab -- without it a background tab is
 *                 clamped to ~0.3 fps and no timing measurement means anything
 *
 * WHAT GOES WRONG (pre-fix).  `g_popup` names two objects: bighelp.c:456 and
 * popup.c:661 give it the 376-byte `PopUpUI` at 0x007fdea4, fpui2.c:1443 gives
 * it the 0x100-byte request block at 0x007fdec0 -- PopUpUI's own +0x1c.  One
 * namespace keeps one address (0x007fdea4), so every store in `PopUpInfoSetUp`
 * lands 0x1c LOW:
 *     g_popup.type -> 0x007fdea4  g_info_icon_g / g_pu_icon_mech
 *     g_popup.obj  -> 0x007fdea8  g_info_icon_d / g_cb_icon_ok
 * which are two of the ten pop-up ICON POINTERS that `DisableInfoPopUPIcons`
 * (iconui.c:233) dereferences with `flags |= 0x400` EVERY FRAME.
 * `HandleMapClick` (gameframe.c:1258) hands `PopUpInfoSetUp` the whole hit
 * record, and the hit record's `obj` is still the last ICON the mouse hit
 * whenever the cell branch takes type 0x109 / 0x10a / 0x10d without
 * re-assigning it.  So: touch the LEGOLAND button, click bare ground, and that
 * Icon* is parked in `g_info_icon_d` and hidden on the next frame and every
 * frame after.  No bubble help, no click, editState stuck at 0, the panel
 * never reopens -- the level is over.
 *
 * Run from the page console in Lesson 2's park (any park with the theme row
 * will do; Lesson 1 reproduces identically).
 */

window.M16 = window.M16 || {};

/* ---- a tab that actually runs -------------------------------------------
 * emscripten_sleep is setTimeout, and Chrome clamps a hidden tab's chained
 * timers to ~1 s, so a background tab runs the game at 0.3-1 fps.  Every
 * "1300 ms vs 1500 ms" measurement taken that way is measuring the browser.
 * This re-implements setTimeout on a MessageChannel chain, which is not
 * clamped: 35 fps and accurate sleeps with document.hidden true. */
M16.realtime = function () {
  if (window.__M16_TIMER) return 'already installed';
  window.__M16_TIMER = true;
  const real = window.setTimeout.bind(window);
  const cancelled = new Set();
  let counter = 1;
  const tick = fn => { const ch = new MessageChannel();
                       ch.port1.onmessage = () => fn(); ch.port2.postMessage(0); };
  window.__realSetTimeout = real;
  window.setTimeout = function (fn, ms) {
    const extra = Array.prototype.slice.call(arguments, 2);
    const id = ++counter, dl = performance.now() + (ms || 0);
    const step = () => { if (cancelled.has(id)) { cancelled.delete(id); return; }
                         if (performance.now() >= dl) fn.apply(null, extra); else tick(step); };
    tick(step);
    return id;
  };
  window.clearTimeout = id => cancelled.add(id);
  return 'installed';
};

/* Simulate the clamp instead, to show what PORT-P2's tab was measuring:
   every timer shorter than 50 ms is pushed out to `floor` ms. */
M16.setFloor = function (floor) {
  const real = window.__realSetTimeout || window.setTimeout;
  if (!floor) { window.setTimeout = real; return 'off'; }
  window.setTimeout = function (fn, ms) {
    const extra = Array.prototype.slice.call(arguments, 2);
    return real(function () { fn.apply(null, extra); }, (ms || 0) < 50 ? floor : ms);
  };
  return 'floor=' + floor;
};

M16.sleep = ms => new Promise(r => setTimeout(r, ms));

/* ---- finding the four theme icons without a LL_DBG_TABLE entry -----------
 * main.c is another lane's file, so nothing here is named: the four theme
 * buttons are found by SIGNATURE in the heap -- an Icon record (0x40 bytes)
 * with group 0x9a at +0x14, y = 379 at +0x0e and a plausible input slot at
 * +0x2c (wasm function pointers are small table indices, so > 40000 is not
 * one).  g_theme_icon[4] is then the one place in memory holding all four
 * addresses in a row. */
M16.scanThemeIcons = function () {
  const U16 = HEAPU16, U32 = HEAPU32, I16 = HEAP16, n = HEAPU8.length, out = [];
  for (let off = 0x14; off + 0x40 < n; off += 4) {
    if (U16[off >> 1] !== 0x9a) continue;
    const b = off - 0x14;
    if (I16[(b + 0x0e) >> 1] !== 379) continue;
    const inp = U32[(b + 0x2c) >> 2];
    if (!inp || inp > 40000) continue;
    out.push(b);
  }
  return out.sort((a, c) => I16[(a + 0x0c) >> 1] - I16[(c + 0x0c) >> 1]);
};

M16.find = function () {
  const U32 = HEAPU32, T = M16.scanThemeIcons();
  let block = 0;
  for (let i = 0; i + 3 < U32.length; i++)
    if (U32[i] === T[0] && U32[i + 1] === T[1] && U32[i + 2] === T[2] && U32[i + 3] === T[3]) {
      block = i * 4; break;
    }
  M16.A = { themeIcon: block };
  return { icons: T, g_theme_icon: block };
};

M16.T        = () => [0, 1, 2, 3].map(i => HEAPU32[(M16.A.themeIcon >> 2) + i]);
M16.flags    = () => M16.T().map(p => HEAPU32[(p + 0x34) >> 2].toString(16));
M16.legFlags = () => HEAPU32[(M16.T()[0] + 0x34) >> 2];

/* The LEGOLAND pill's mean colour: [165,168,195] with the button there,
   [128,142,209] when it is one of "four empty pills". */
M16.pill0 = function () {
  const d = Module.canvas.getContext('2d').getImageData(8, 379, 92, 34).data;
  let r = 0, g = 0, b = 0;
  for (let i = 0; i < d.length; i += 4) { r += d[i]; g += d[i + 1]; b += d[i + 2]; }
  const n = d.length / 4;
  return [Math.round(r / n), Math.round(g / n), Math.round(b / n)];
};

/* ---- the repro ----------------------------------------------------------
 * (300,120) must be BARE GROUND: llSel().hit.typeHex has to read 0x109 there.
 * A cell carrying an object gives 0x103 and HandleMapClick re-assigns `obj`,
 * so the stale icon never reaches the pop-up. */
M16.repro = async function (groundX, groundY) {
  groundX = groundX || 300; groundY = groundY || 120;
  if (!M16.A) M16.find();
  const r = { before: M16.flags(), steps: [] };
  await llClick( 53, 394); await M16.sleep(1200); r.steps.push(['theme open',  M16.flags().join(',')]);
  await llClick( 53, 394); await M16.sleep(1200); r.steps.push(['theme close', M16.flags().join(',')]);
  await llClick(groundX, groundY); await M16.sleep(1200);
  r.steps.push(['bare ground', M16.flags().join(',')]);
  r.after   = M16.flags();
  r.pill0   = M16.pill0();
  r.hit     = llSel().hit;
  r.legIcon = M16.T()[0];
  r.lost    = r.after[0] !== '6012';
  return r;
};

/* Permanence: three more clicks and a MAP round trip, as PORT-P2 tried. */
M16.dead = async function () {
  const out = [];
  for (let i = 0; i < 3; i++) {
    await llClick(53, 394); await M16.sleep(1800);
    out.push([i, llFrameHash(), llPark().editState, M16.flags()[0]]);
  }
  await llClick(278, 443); await M16.sleep(1800);
  await llClick(278, 443); await M16.sleep(1800);
  return { clicks: out, afterMapRoundTrip: M16.flags(), editState: llPark().editState,
           pill0: M16.pill0(), dead: String(llDead()), traps: llStats().traps };
};

/* Randomised sequences with a per-tick watch on the LEGOLAND flag word.
   Seed 7 / 3 iterations is what first caught it. */
M16.fuzz = async function (iters, seed) {
  if (!M16.A) M16.find();
  const CLICKS = [[53,394],[27,90],[278,443],[420,300],[40,443],
                  [200,443],[105,394],[300,120],[320,200],[560,250]];
  let s = seed >>> 0;
  const rnd = () => (s = (s * 1103515245 + 12345) >>> 0) / 4294967296;
  const base = M16.legFlags();
  let hits = 0, polls = 0, stop = false;
  const tick = () => { if (stop) return; polls++;
    if (M16.legFlags() !== base) hits++;
    const ch = new MessageChannel(); ch.port1.onmessage = tick; ch.port2.postMessage(0); };
  tick();
  for (let k = 0; k < iters && !hits; k++) {
    const n = 4 + Math.floor(rnd() * 4);
    for (let j = 0; j < n; j++) {
      const c = CLICKS[Math.floor(rnd() * CLICKS.length)];
      await llClick(c[0], c[1]);
      await M16.sleep(100 + Math.floor(rnd() * 1600));
    }
  }
  stop = true;
  return { seed, iters, polls, hits, flags: M16.flags(),
           gameMode: llPark().gameMode, dead: String(llDead()) };
};

/* PORT-P2's own replay, run at a real frame rate over the whole window the
   brief asks for.  Needs p2-01 + p2-03 loaded for P2.themeButtonRace. */
M16.gapSweep = async function () {
  const out = [];
  for (const gap of [1000, 1100, 1200, 1300, 1400, 1500, 1600]) {
    for (let i = 0; i < 3 && llPark().gameMode === 1; i++) { await llClick(278, 443); await M16.sleep(2000); }
    const pre = M16.flags();
    await P2.themeButtonRace(gap);
    await M16.sleep(1500);
    const post = M16.flags();
    out.push({ gap, lost: post[0] !== '6012', pre: pre.join(','), post: post.join(','),
               fps: llStats().fps, dead: String(llDead()) });
  }
  return out;
};

/* M16.realtime(); M16.find(); await M16.repro(); await M16.dead();
   await M16.fuzz(3, 7); await M16.gapSweep(); */
