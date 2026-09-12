/* PORT-M19 replay: P4-2 — "lesson 3's LEGOLAND panel loses three rides".
 *
 * VERDICT: it does not.  On origin/main 63ee1432 the panel's item list holds
 * every class the script has GIVEn, at every step, and lesson 3 finishes all
 * EIGHT of its player objectives — the Spider Ride included.
 *
 * Serve `portable/build-wasm` and open
 *   legoland.html?args=-nointro+WINDEBUG&awake=1
 * with the pane at >= ~820x760 (P4-4).  `replays/` is not created by the
 * build; link it in first:
 *   ln -sfn <repo>/portable/src/browser/replays portable/build-wasm/replays
 *
 *   await fetch('replays/p4-00-prelude.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/p4-02-lesson3.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/m19-01-the-lesson-3-panel-has-every-class.js')
 *         .then(r=>r.text()).then(eval);
 *   P4.fast();
 *   await P4.toLesson(3);          // reload once if it answers reload:true
 *   M19.watch();                   // start the element-flag watcher FIRST
 *   await M19.cold();              // the whole lesson, one call per step
 *
 * Every await below takes minutes; `javascript_tool` gives up after 45 s.  Run
 * each phase through `M19.bg(name, fn)` and poll `M19.R[name].done` — the page
 * keeps running while the driver call has already returned.
 *
 * ---------------------------------------------------------------------------
 * WHY THE LIST AND NOT THE PANEL
 *
 * PORT-P4 read the panel by arming icon slots.  That reading is not the list:
 * `MakeUpObjectList` (fpui2.c:1196) lays the nodes out 0x42 apart in a 0x154-
 * tall box and then RESTORES `g_list_scroll[g_list_menu & 0xff]`, a per-menu
 * offset that SURVIVES a rebuild, a panel close/open and a MAP round trip.  So
 * what an arming sweep returns depends on the scroll it inherited, and this
 * lane measured that directly: with all EIGHT classes proven in the list, the
 * first `P4L3.panelNames()` returned all eight and the second — run after the
 * first had left `g_list_scroll[0]` at -250 — returned seven, silently losing
 * "Flowers", the cheapest and topmost.  PORT-P4 ran `panelNames()` after every
 * build inside `rides()`, six times, each inheriting the last one's offset.
 *
 * `llPanel()` (this lane's page hook) walks `g_object_list` itself and
 * re-evaluates `ObjectLinkedList`'s own predicate per class, so "the class is
 * not in the panel" and "the icon was not reachable from where the panel was
 * scrolled" stop being the same measurement.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT SHOWS
 *
 * ObjList3.txt ENABLEs three classes at INIT and only LOADs the other five;
 * the five arrive one reward at a time.  `llPanel().count` tracks that exactly:
 *
 *   step  event                                       nodes  elem flags
 *   8     level as loaded (3 ENABLEd)                   3     0x17 / 0x15
 *   8->9  REWARD GIVE "SPACE TOWER RIDE", NOPOPUP       4     0x15 -> 0x20017
 *   9->11 REWARD GIVE "SMALL POWER STATION"             5     0x15 -> 0x20017
 *   11->13 REWARD GIVE "LEGO SHOP 1"/"LEGO SHOP 2"      7     0x15 -> 0x20017
 *   13->14 REWARD FMV Spider.avi + GIVE "SPIDER RIDE"   8     0x15 -> 0x20017
 *
 * 0x1 loaded, 0x2 available, 0x4 class loaded, 0x10 = the LLIDB TYPE nibble for
 * an ODF, 0x20000 "given" (cleared by the first render of its icon).  The
 * builder's predicate is `(flags & 0x13) == 0x13` (fpui2.c:602), so 0x15 is
 * LOADed-but-not-given and 0x17 is in-the-panel; the watcher never once saw a
 * 0x2 go back down.
 *
 * Then step 14 drains (the Spider Ride builds, 80 bricks) and step 15 drains
 * (a second Small Power Station), and `llGoals().stepCount` goes to 0 — the
 * script list is empty, which is the ENDLEVEL wait.
 *
 * ---------------------------------------------------------------------------
 * M19-1, WHICH IS NEW AND IS A BLOCKER
 *
 * With the Spider Ride standing and carrying riders the game loop DIES:
 *
 *   TRAP RUNTIME RuntimeError: divide by zero
 *     Render3DPerson / RenderBlokeIn3D / SpiderRide_Interact /
 *     DrawAndClearPrintList / GameFrame / main
 *
 * (symbolised against `legoland_dbg.wasm`'s name section; the indices in the
 * page's stack are the same in both links).  The only integer division on that
 * path is `Draw3DPersonModel`'s
 *
 *     zscale = 0x40000000 / ((hi - lo) >> 5);            person3d.c:1351
 *
 * inlined into `Render3DPerson`, where hi/lo are the min/max transformed z of
 * the model.  `M19.postMortem()` reads the wreck: `g_xverts` holds (0,0,0) for
 * every vertex of the model that was being drawn, so hi - lo is 0; and of the
 * eleven blokes alive, exactly ONE has an all-zero `p->matrix` (+0x58) — the
 * one whose bloke flags carry 0x80, i.e. the rider `SpiderRide_Interact` draws.
 * `SetPersonRotation` (math3d.c:402) unconditionally writes m[4] = 0x10000, so
 * an all-zero matrix means it was never called for that person, and a zero
 * matrix maps every vertex to the origin.  Its `ydepth` (+0x38) is a quiet NaN
 * as well.  Owner: the person3d / ride lane that owns P1-6.
 */

window.M19 = {
  R: {},
  /* Long phases as background promises: the driver tool times out at 45 s, the
   * page does not.  M19.bg('x', fn) then poll M19.R.x.done. */
  bg: function (name, fn) {
    this.R[name] = { done: false };
    var R = this.R[name];
    (async function () {
      try { R.v = await fn(); } catch (e) { R.e = String(e); }
      R.done = true;
    })();
    return 'started ' + name;
  },

  /* Every loaded class's LLIDB element {flags, refcount}, keyed by name. */
  read: function () {
    var p = llPanel(), I = HEAP32, o = {};
    p.classes.forEach(function (c) {
      o[c.elem] = { f: I[(c.elemAddr + 8) >> 2] >>> 0,
                    rc: I[(c.elemAddr + 0x10) >> 2] };
    });
    return o;
  },
  /* Log every TRANSITION of those two words with the script step it happened
   * on.  This is what turns "the panel lost a class" from a click sweep into a
   * store: MarkElemAvailable sets 0x2, MarkElemUnavailable and MarkElemNew
   * clear it (eventtick.c:325/344, movie3.c:293) and LLIDB_UnLoadData clears it
   * on the last reference (memdb.c:231).  Nothing did, all session. */
  log: [], prev: {}, timer: null,
  tick: function () {
    try {
      var now = M19.read(), p = llPanel();
      var st = llGoals() && llGoals().cur ? llGoals().cur.id : null;
      Object.keys(now).forEach(function (k) {
        var n = now[k], v = M19.prev[k];
        if (!v) { M19.prev[k] = n; return; }
        if (v.f !== n.f || v.rc !== n.rc) {
          M19.log.push({ step: st, elem: k, list: p.count,
                         from: '0x' + v.f.toString(16) + '/rc' + v.rc,
                         to:   '0x' + n.f.toString(16) + '/rc' + n.rc,
                         money: llPark().money });
          M19.prev[k] = n;
        }
      });
    } catch (e) { M19.err = String(e); }
  },
  watch: function () {
    this.prev = {}; this.tick();
    if (!this.timer) this.timer = setInterval(function () { M19.tick(); }, 120);
    return this.prev;
  },
  unwatch: function () { clearInterval(this.timer); this.timer = null; return this.log; },

  /* One row of evidence per step: the script position and the list beside it. */
  row: function (tag) {
    var p = llPanel(), g = llGoals();
    return { tag: tag, step: g && g.cur ? g.cur.id : null, count: p.count,
             list: p.nodes.map(function (n) { return n.name; }),
             missing: p.missing.map(function (c) { return c.name; }),
             scroll: p.menu.scroll[0], money: llPark().money,
             traps: llStats().traps.length };
  },

  /* ---- the lesson, phase by phase --------------------------------------- */

  /* Steps 0..5 (six Gardeners) and step 8 (PATHSCENERY 25 + one of each).  The
   * scenery loop is the long one: it plants every path-adjacent free cell and
   * took ~7 minutes and 247 flower beds here. */
  hire: function () { return M19.bg('hire', function () { return P4L3.hire(6); }); },
  scenery: function () {
    return M19.bg('scenery', async function () {
      await P4.dismiss(); await P4.openTab('legoland');
      var before = M19.row('step 8 before');
      var L = await P4L3.scenery();
      return { before: before, after: M19.row('step 8 after'), n: L.length };
    });
  },

  /* Arm a class by name and put it on the first square the GAME's own cursor
   * calls legal (CursorIsValid -> llSel().editCursor.valid).  P4.buildNamed
   * sweeps the VIEWPORT on a 14 px grid, which cannot find an anchor for a ride
   * whose footprint is 16x18 cells once the park is carpeted with flower beds;
   * this searches the MAP for an anchor whose whole footprint is free and
   * centres on it, which is how the Spider Ride goes down. */
  buildBig: async function (name, fp) {
    if (P4.armed() !== name) { var a = await P4.armByName(name); if (!a.ok) return { ok: false, why: 'not in panel', a: a }; }
    var L = fp[0], T = fp[1], R = fp[2], B = fp[3], anchors = [];
    for (var y = -T; y < 58 + T; y++) for (var x = -L; x < 58 - R; x++) {
      var ok = true;
      for (var dy = T; dy <= B && ok; dy++)
        for (var dx = L; dx <= R && ok; dx++) if (!P4.free(x + dx, y + dy)) ok = false;
      if (ok) anchors.push([x, y]);
    }
    for (var i = 0; i < anchors.length; i += Math.max(1, anchors.length >> 3)) {
      var t = anchors[i], c = await P4.centerOn(t[0], t[1]);
      if (!c.fitOk) continue;
      if (P4.armed() !== name) await P4.armByName(name);
      var q = P4.cellTo(t[0], t[1]);
      await llMove(q[0], q[1], 200);
      var ec = llSel().editCursor;
      if (!ec.valid) continue;
      var m0 = llPark().money;
      await llClick(q[0], q[1]); await P4.W(1500);
      if (llPark().money !== m0)
        return { ok: true, cell: t, at: q, money: [m0, llPark().money] };
    }
    return { ok: false, why: 'no legal anchor', anchors: anchors.length };
  },

  /* Steps 9..15 — every build, with the list read after each.  The Spider's
   * footprint is (-7,-8)..(8,9); everything else fits P4.buildNamed. */
  rides: function () {
    return M19.bg('rides', async function () {
      var out = [];
      var plan = [['Space Tower', null], ['Power Station', null],
                  ['Toy Shop', null], ['Clothes Shop', null],
                  ['Spider', [-7, -8, 8, 9]], ['Power Station', null]];
      for (var i = 0; i < plan.length; i++) {
        await P4.dismiss();
        var id0 = llGoals() && llGoals().cur ? llGoals().cur.id : null;
        var b;
        if (plan[i][1]) b = await M19.buildBig('Spider Ride', plan[i][1]);
        else {
          var spot = P4.clearArea(4); if (spot) await P4.centerOn(spot[0], spot[1]);
          b = await P4.buildNamed(plan[i][0]);
        }
        var d = await P4.waitStep(id0, 30000);
        out.push({ want: plan[i][0], build: b, drained: d, row: M19.row(plan[i][0]) });
        if (!llGoals() || !llGoals().cur) break;    /* the script list emptied */
      }
      return out;
    });
  },

  cold: async function () {
    this.watch();
    this.hire();
    return 'hire started -- poll M19.R.hire.done, then M19.scenery(), then M19.rides()';
  },

  /* ---- the two comparisons the verdict rests on -------------------------- */

  /* A: the list, read.  B: PORT-P4's own arming sweep, verbatim.  Run twice:
   * the SECOND run inherits the scroll the first left and returns a different
   * set from the same list, which is the whole of P4-2's reading. */
  ab: async function () {
    var list = llPanel().nodes.map(function (n) { return n.name; });
    await P4.dismiss();
    var first = await P4L3.panelNames();
    var scroll1 = llPanel().menu.scroll[0];
    var second = await P4L3.panelNames();
    return { A_list: list, A_count: list.length,
             B_first: first, B_second: second,
             scrollAfterFirst: scroll1, scrollAfterSecond: llPanel().menu.scroll[0],
             lostBetweenSweeps: first.filter(function (n) { return second.indexOf(n) < 0; }) };
  },

  /* M19-1: read the wreck after the loop has died. */
  postMortem: function () {
    var a = llAddrs(), I = HEAP32, U = HEAPU8, F = new Float32Array(HEAP32.buffer);
    var xv = a.g_xverts, lo = I[(xv + 8) >> 2], hi = lo, i;
    for (i = 0; i < 64; i++) { var t = I[(xv + i * 12 + 8) >> 2]; if (t < lo) lo = t; if (t > hi) hi = t; }
    var blokes = [], p = I[a.g_people_head >> 2], n = 0;
    while (p && n++ < 256) {
      var per = I[(p + 4) >> 2];
      if (per) {
        var m = [0, 1, 2, 3, 4, 5, 6, 7, 8].map(function (k) { return I[(per + 0x58 + k * 4) >> 2]; });
        blokes.push({ bloke: p, person: per, matrix: m,
                      zeroMatrix: m.every(function (v) { return v === 0; }),
                      blokeFlags: '0x' + U[p + 0x62].toString(16),
                      onRide: !!(U[p + 0x62] & 0x80),
                      rot: [F[(per + 0x40) >> 2], F[(per + 0x44) >> 2], F[(per + 0x48) >> 2]],
                      ydepthBits: '0x' + (I[(per + 0x38) >> 2] >>> 0).toString(16),
                      frame: I[(per + 0x4c) >> 2] });
      }
      p = I[p >> 2];
    }
    return { dead: llStats().dead,
             zExtentOverFirst64: { lo: lo, hi: hi, d: hi - lo, divisor: (hi - lo) >> 5 },
             blokes: blokes.length,
             zeroMatrix: blokes.filter(function (b) { return b.zeroMatrix; }) };
  }
};
'M19 installed';
