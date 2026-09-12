/* PORT-M20 replay: M19-1 — "the game loop dies once the Spider Ride carries a
 * rider".
 *
 * VERDICT: the rider's BNV path name was "manbox00". `sprintf_w` (0x0049e573,
 * which IS `sprintf`) is declared `(char*, const char*, int)` in mechrides.c
 * and variadic everywhere else in the tree; on wasm32 a variadic callee takes
 * a POINTER to the varargs buffer as its third parameter, so the SEAT NUMBER
 * was handed over as a va_list and every "%02d" name the four BNV rides build
 * came out "...00". Fixed in a LEGOLAND_PORTABLE arm.
 *
 * Serve `portable/build-wasm` and open
 *   legoland.html?args=-nointro+WINDEBUG&awake=1
 * with the pane at >= ~820x760 (P4-4). `replays/` is not created by the build:
 *   ln -sfn <repo>/portable/src/browser/replays portable/build-wasm/replays
 *
 *   await fetch('replays/p4-00-prelude.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/p4-02-lesson3.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/m19-01-the-lesson-3-panel-has-every-class.js')
 *         .then(r=>r.text()).then(eval);
 *   await fetch('replays/m20-01-the-spider-ride-asked-for-manbox00.js')
 *         .then(r=>r.text()).then(eval);
 *   P4.fast();
 *   await P4.toLesson(3);        // reload once if it answers reload:true
 *   await M20.setup();           // hire 6, arm + build the Spider, add visitors
 *   M20.board();                 // put ONE bloke on the ride
 *   M20.rider();                 // read the rider back
 *
 * ---------------------------------------------------------------------------
 * WHY THE LESSON IS NOT PLAYED OUT
 *
 * PORT-M19 reached the rider by finishing all eight of lesson 3's objectives
 * (~20 minutes, 247 flower beds). None of that is load-bearing for M19-1: the
 * only preconditions are (a) a Spider Ride standing with a SpiderRec, and
 * (b) one bloke on the class's rider list at ObjDef+0xcc with state 0 and
 * action 0. `M20.setup()` gets (a) by poking the class's LLIDB element flags
 * to 0x17 (`(flags & 0x13) == 0x13` is fpui2.c:602's own predicate, so the
 * panel builds the icon the game's way) and building from the panel, and
 * `M20.board()` gets (b) by writing the same RiderNode that rides.c's
 * PutWorkerOnRide writes. Both are page-side pokes of values the game itself
 * writes; nothing in the game is patched.
 *
 * Visitors: lesson 3 starts with `g_visitor_limit` 0 and it is recomputed from
 * the park every tick, so a single write is undone within a second. A 20 ms
 * interval holding it at 19 lets SpawnVisitor fill the park to `maxBlokes`.
 *
 * ---------------------------------------------------------------------------
 * THE A/B, one build each way, same park, same cell (11,13), same poke
 *
 *   build     path name   path->person_height   ydepth bits   p->matrix    loop
 *   --------  ----------  --------------------  ------------  -----------  ----
 *   pre-fix   "manbox00"  NaN                   0x7fc00000    ALL ZERO     DEAD
 *   fixed     "manbox09"  -0.22060275           0xbee1e5b0    16.16 rot    alive
 *
 * -0.2206027 is exactly what `GetZSkew` gives for manbox09's first vertex read
 * straight out of `gamedata/main/spideron.bnv`, and 0xbee1e5b0 is 2x it. The
 * pre-fix rider's flags62 is 0xab and the fixed rider's is 0xab/0xa9 — the same
 * bloke in the same state, so the only difference is the name.
 *
 * Pre-fix the page dies within ~200 frames of the rider mounting. PORT-M19 saw
 * `RuntimeError: divide by zero` (Draw3DPersonModel's
 * `zscale = 0x40000000 / ((hi - lo) >> 5)`, person3d.c:1351, on an all-zero
 * matrix that maps every vertex onto the origin). This lane saw the OTHER
 * face of the same NULL object first — emscripten's zero-address sentinel:
 *   Aborted(Runtime error: The application has corrupted its heap memory area
 *   (address zero)!)
 * which is `object->orientation[0..8] *= inverse_length` (bnvpath.c:328-336)
 * writing nine NaNs to addresses 0x10..0x33 because `object` is 0. Which one
 * fires first is a race inside one frame; both are the missing "manbox<seat>".
 *
 * Fixed, the rider completes the whole cycle — mount along the BNV path, seat,
 * alight — and the loop runs on at 35.7 fps with 0 traps.
 *
 * ---------------------------------------------------------------------------
 * THE CHAIN, READ OFF THE LIVE HEAP
 *
 * `M20.bnv()` walks the loaded `spideron.bnv` the way the game does
 * (GetBinVFrame's backwards list walk, GetObjectFromName's NameCompare loop)
 * and evaluates GetZSkew on the result:
 *
 *   "manbox01" -> object 0x19ef9f9, vertex 0x19efa2d, GetZSkew -0.22060272
 *   "manbox00" -> object 0,         vertex 0,         GetZSkew 0/0 = NaN
 *                 and `object->orientation[0..2]` reads address 0x10 = (0,0,0),
 *                 so 1/sqrt(0) = +inf and all nine products are NaN.
 *
 * NOTE the BNV image is loaded at an ODD address (its frame lists and vertices
 * land on 1 mod 4), so read it through a DataView, not HEAP32 — HEAP32 rounds
 * the index down and returns garbage. That is not a defect: wasm allows
 * unaligned loads, and x86 did too.
 */

window.M20 = {
  R: {},
  bg: function (name, fn) {
    this.R[name] = { done: false };
    var R = this.R[name];
    (async function () { try { R.v = await fn(); } catch (e) { R.e = String(e); } R.done = true; })();
    return 'started ' + name;
  },

  DEF: 0,                 /* the Spider Ride's ObjDef, filled in by setup() */
  CELL: [11, 13],

  /* ---- reading ----------------------------------------------------------- */

  /* Every bloke on `g_people_head`, with the four words M19-1 turns on. */
  blokes: function () {
    var a = llAddrs(), I = HEAP32, U = HEAPU8;
    var p = I[a.g_people_head >> 2], out = [], n = 0;
    while (p && n++ < 256) {
      var per = I[(p + 4) >> 2];
      out.push({ b: p, per: per, f62: '0x' + U[p + 0x62].toString(16),
                 action: U[p + 0x60], seat: U[p + 0x36], bnvpath: I[(p + 0x54) >> 2],
                 m: per ? [0,1,2,3,4,5,6,7,8].map(function (k) { return I[(per + 0x58 + k * 4) >> 2]; }) : null,
                 ydepthBits: per ? '0x' + (I[(per + 0x38) >> 2] >>> 0).toString(16) : null });
      p = I[p >> 2];
    }
    return out;
  },

  /* The rider(s): bloke flags 0x80 (riding) or 0x8 (on this ride), with the
   * BNVPath record's own object_name (+0x08) and person_height (+0x3c). */
  rider: function () {
    var I = HEAP32, U = HEAPU8, F = new Float32Array(I.buffer);
    return this.blokes().filter(function (b) { return parseInt(b.f62, 16) & 0x88; })
      .map(function (b) {
        return { b: b.b, f62: b.f62, action: b.action, seat: b.seat, path: b.bnvpath,
                 m: b.m, zeroMatrix: b.m && b.m.every(function (v) { return v === 0; }),
                 ydepthBits: b.ydepthBits, ydepth: F[(b.per + 0x38) >> 2],
                 pathPersonHeight: b.bnvpath ? F[(b.bnvpath + 0x3c) >> 2] : null,
                 pathName: b.bnvpath ? (function () {
                   var t = '';
                   for (var j = 0; j < 12 && U[b.bnvpath + 8 + j]; j++) t += String.fromCharCode(U[b.bnvpath + 8 + j]);
                   return t; })() : null };
      });
  },

  /* Every "manbox??" / "manBox??" / "BlokeBox??" template in the generated
   * globals, as it reads right now. sprintf_w writes into these IN PLACE. */
  names: function () {
    var U = HEAPU8, out = [];
    for (var i = 0; i < 0x400000; i++) {
      if (U[i] !== 0x6d && U[i] !== 0x42) continue;          /* 'm' | 'B' */
      var s = '';
      for (var j = 0; j < 11 && U[i + j] >= 32 && U[i + j] < 127; j++) s += String.fromCharCode(U[i + j]);
      if (/^(manbox|manBox|BlokeBox)/.test(s)) out.push({ addr: i, s: s.slice(0, 11) });
    }
    return out;
  },

  /* ---- the chain, evaluated on the live .bnv image ------------------------ */
  bnv: function (names) {
    var U = HEAPU8, DV = new DataView(HEAPU8.buffer);
    var i32 = function (o) { return DV.getInt32(o, true); };
    var f32 = function (o) { return DV.getFloat32(o, true); };
    var u16 = function (o) { return DV.getUint16(o, true); };
    var str = function (o) { var s = ''; for (var i = 0; i < 32 && U[o + i]; i++) s += String.fromCharCode(U[o + i]); return s; };

    /* find the loaded image by its own first 32 bytes (LoadBinV only rewrites
     * +0x20 and inward, so the header is a safe needle) */
    var f = FS.readFile('/gamedata/spideron.bnv'), bin = -1;
    outer: for (var i = 0; i < U.length - 32; i++) {
      if (U[i] !== f[0] || U[i + 1] !== f[1]) continue;
      for (var k = 2; k < 32; k++) if (U[i + k] !== f[k]) continue outer;
      bin = i; break;
    }
    if (bin < 0) return { error: 'spideron.bnv is not loaded (build a Spider Ride first)' };

    function GetBinVFrame(p, n) {                 /* sweep1.c:203 */
      if (!p) return 0;
      var node = i32(p + 0x20), count = u16(p + 2);
      if (n >= count) return 0;
      var steps = count - 1;
      if (steps > n) { steps -= n; do { node = i32(node + 8); } while (--steps); }
      return node;
    }
    function GetObjectFromName(list, name) {      /* bnvpath.c:135 */
      if (!list) return 0;
      var node = i32(list + 4), count = i32(list);
      for (var i = 0; i < count; i++) {
        if (str(i32(node + 0xc)).toLowerCase() === name.toLowerCase()) break;
        node = i32(node + 4);
      }
      return node;
    }
    function GetVertex(p, n) {                    /* sweep1.c:222 */
      if (!p) return 0;
      if (n >= i32(p)) return 0;
      return i32(p + 8) + n * 20;
    }
    function GetZSkew(a, b) {                     /* math3d.c:195 */
      var f14 = f32((a || 0) + 0x14), f0c = f32((b || 0) + 0x0c), f10 = f32((b || 0) + 0x10);
      var den = Math.fround(Math.fround(Math.fround(Math.fround(f14 + f14) - 1) * f10) - Math.fround(f0c * f14));
      return { f14: f14, f0c: f0c, f10: f10, r: Math.fround(Math.fround(f10 * f10) / den) };
    }

    var L = GetBinVFrame(bin, 0), out = { bin: bin, frame0: L };
    (names || ['manbox01', 'manbox09', 'manbox00']).forEach(function (nm) {
      var o = GetObjectFromName(L, nm), v = GetVertex(o, 0);
      out[nm] = { object: o, vertex: v, zskew: GetZSkew(bin, v),
                  orientationRow0: o ? [0, 1, 2].map(function (k) { return f32(o + 0x10 + 4 * k); })
                                     : [f32(0x10), f32(0x14), f32(0x18)] };
    });
    return out;
  },

  /* ---- getting there ----------------------------------------------------- */

  /* 20 ms is under one game frame, so the recompute never wins for long. */
  visitors: function (n) {
    var a = llAddrs(), I = HEAP32;
    if (this._v) clearInterval(this._v);
    this._v = setInterval(function () { try { I[a.g_visitor_limit >> 2] = n || 19; } catch (e) {} }, 20);
    return 'visitor limit held at ' + (n || 19);
  },
  noVisitors: function () { clearInterval(this._v); this._v = null; return 'released'; },

  /* Put the class in the panel the game's own way: 0x2 is "available", and
   * fpui2.c:602 builds an icon for (flags & 0x13) == 0x13. */
  give: function (elemName) {
    var I = HEAP32, c = llPanel().classes.filter(function (x) { return x.elem === elemName; })[0];
    if (!c) return { ok: false, why: 'no such class' };
    I[(c.elemAddr + 8) >> 2] |= 0x2;
    return { ok: true, elem: c.elem, addr: c.elemAddr, flags: '0x' + (I[(c.elemAddr + 8) >> 2] >>> 0).toString(16) };
  },

  /* Hire six gardeners (so the lesson stops nagging and the park has blokes),
   * give + build the Spider Ride, and open the taps on visitors.
   *
   * THE ONE FIDDLY PART, and it is PORT-P4's warning in a new costume: while
   * lesson 3's "you need more scenery" bubble is up, `DisableInfoPopUPIcons`
   * hides the panel's icon slots every frame, so `P4.armByName` clicks four
   * empty rectangles and reports "not in panel" with the class plainly in
   * `llPanel().nodes`. The bubble comes back on its own every few seconds, so
   * dismiss -> open -> dismiss -> arm, and retry the whole thing. */
  setup: function () {
    var self = this;
    return this.bg('setup', async function () {
      await P4.dismiss();
      var L = await P4L3.hire(6);
      self.give('SPIDER RIDE');
      var panel = null, b = null;
      for (var t = 0; t < 6 && !(b && b.ok); t++) {
        await P4.dismiss();
        /* the panel must be CLOSED and reopened for MakeUpObjectList to run */
        if (!P4.panelOpen()) { await llClick(53, 394); await P4.W(1500); }
        await P4.dismiss(); await P4.W(400);
        panel = llPanel().nodes.map(function (n) { return n.name; });
        var a = await P4.armByName('Spider Ride');
        if (a.ok) b = await M19.buildBig('Spider Ride', [-7, -8, 8, 9]);
        else if (P4.panelOpen()) { await llClick(53, 394); await P4.W(1200); }
      }
      if (b && b.ok) self.CELL = b.cell;
      var c = llCellAt(self.CELL[0], self.CELL[1]);
      self.DEF = c && c.def;
      if (P4.panelOpen()) { await llClick(53, 394); await P4.W(1200); }
      await P4.centerOn(self.CELL[0], self.CELL[1]);
      self.visitors(19);
      await P4.W(20000);
      return { hired: L.length - 1, panel: panel, build: b, def: self.DEF,
               owner: c && c.owner, blokes: self.blokes().length };
    });
  },

  /* 0x14 bytes of heap nobody is using, for the RiderNode. */
  scratch: function () {
    var U = HEAPU8;
    for (var a = U.length - 0x200000; a < U.length - 0x1000; a += 0x1000) {
      var ok = true;
      for (var k = 0; k < 0x400; k++) if (U[a + k] !== 0) { ok = false; break; }
      if (ok) return a;
    }
    return 0;
  },

  /* One bloke onto the ride, exactly as rides.c's PutWorkerOnRide does it:
   * a RiderNode {next, prev, bloke, ride_id, person} on ObjDef+0xcc, flags62
   * |= 0x20, state 0, action 0 (the Spider's JOIN step). */
  board: function () {
    var I = HEAP32, U = HEAPU8, DV = new DataView(I.buffer);
    var def = this.DEF, cx = this.CELL[0], cy = this.CELL[1];
    if (!def) return { ok: false, why: 'run M20.setup() first' };
    var s = this.blokes().filter(function (b) { return b.per; });
    if (!s.length) return { ok: false, why: 'no bloke has a Person3D' };
    var B = s[s.length - 1].b, node = this.scratch();
    if (!node) return { ok: false, why: 'no scratch page' };
    I[node >> 2] = I[(def + 0xcc) >> 2];             /* next   */
    I[(node + 4) >> 2] = 0;                          /* prev   */
    I[(node + 8) >> 2] = B;                          /* bloke  */
    DV.setUint16(node + 0xc, cx | (cy << 8), true);  /* ride_id = packed tile */
    I[(node + 0x10) >> 2] = I[(B + 4) >> 2];         /* person */
    I[(def + 0xcc) >> 2] = node;
    U[B + 0x62] = (U[B + 0x62] & ~0x88) | 0x20;      /* in a ride */
    DV.setUint16(B + 0x0e, 0, true);                 /* low-level state 0 */
    U[B + 0x60] = 0;                                 /* action 0 = JOIN   */
    return { ok: true, bloke: B, person: I[(B + 4) >> 2], node: node,
             f62: '0x' + U[B + 0x62].toString(16) };
  },

  /* The whole measurement, start to finish. */
  ab: async function () {
    var before = this.names().filter(function (n) { return /\?\?$/.test(n.s); }).length;
    var r = this.board();
    await P4.W(2500);
    var rd = this.rider().filter(function (x) { return x.path; });
    return { boarded: r, templatesStillUnwritten: before, rider: rd,
             names: this.names(), traps: llStats().traps, dead: llStats().dead,
             fps: llStats().fps };
  },

  /* Alive-and-well after the ride: call this ~30 s after ab(). */
  survived: function () {
    return { frames: llStats().frames, fps: llStats().fps,
             dead: llStats().dead, traps: llStats().traps,
             riders: this.rider(), park: llPark() };
  }
};
'M20 installed';
