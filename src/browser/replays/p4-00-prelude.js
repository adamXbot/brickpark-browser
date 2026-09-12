/* PORT-P4 prelude — the helpers every p4-* replay begins with.
 *
 *   await fetch('replays/p4-00-prelude.js').then(r=>r.text()).then(eval);
 *   await P4.toLesson(2);        // cold load -> the lesson-2 park
 *
 * Serve `portable/build-wasm` and open
 *   legoland.html?args=-nointro+WINDEBUG&awake=1
 * `awake=1` is not optional: a hidden pane throttles the chained setTimeout
 * the port yields through and the whole game runs at 0.3-0.5 fps, which is how
 * PORT-P2 came to file a three-click bug as a race (PORT-M16 §1).
 *
 * WHAT THIS LANE MEASURES.  P1..P3 read progress off the canvas and off
 * `money`.  Both move for reasons that are not progress.  This lane reads the
 * script: `llGoals()` (added to the page by this lane) walks eventmake.c's
 * ScriptStep list, so `llGoals().cur.id` IS the objective number on the
 * notepad and `llGoals().cur.goals[]` is what is still unmet inside it.  An
 * objective has drained when `cur.id` advances; the level is over when the
 * ENDLEVEL step is reached.
 *
 * THE ONE POKE.  screens3.c's InitTutorialScreen only draws a marker for a
 * lesson the profile has reached, so lesson 2/3/5 cannot be selected from a
 * fresh profile.  PORT-P3 found the profile RECORD on disk is the raw 0x110
 * Profile struct and that the fifteen bytes at +0x34 ARE g_level_done[0..14];
 * writing 1 into five of them is exactly what the game writes when a lesson is
 * finished.  Nothing else is touched and no code is changed.
 */

window.P4 = {
  /* A hidden Browser pane clamps setTimeout to ~1 Hz.  `?awake=1` fixes that
   * for the GAME (PORT-A10's MessageChannel yield) but NOT for the driver:
   * every `await sleep(150)` in llMove/llClick becomes a second, and a lane
   * that does not notice reads the slowdown as the port being slow (PORT-P2
   * filed a three-click bug as a race for exactly this reason, PORT-M16 §1).
   * A MessageChannel pump is not clamped, so the driver uses one.  Measured:
   * one llClick costs 3.2 s on setTimeout and 0.81 s on this. */
  W: function (ms) {
    return new Promise(function (res) {
      var t0 = performance.now(), ch = new MessageChannel();
      ch.port1.onmessage = function () {
        if (performance.now() - t0 >= ms) { ch.port1.close(); res(); }
        else ch.port2.postMessage(0);
      };
      ch.port2.postMessage(0);
    });
  },
  log: [],

  /* The same mouse events llMove/llClick send, waited on with the pump above.
   * Installing these over the page's own hooks makes every replay ~4x faster
   * in a hidden pane and changes nothing about what the game receives. */
  pt: function (gx, gy) {
    var c = Module.canvas, r = c.getBoundingClientRect();
    return { c: c, x: r.left + gx * r.width / 640, y: r.top + gy * r.height / 480 };
  },
  ev: function (type, p, btn) {
    p.c.dispatchEvent(new MouseEvent(type, { clientX: p.x, clientY: p.y,
      button: btn || 0, bubbles: true, cancelable: true }));
  },
  move: async function (gx, gy, ms) {
    P4.ev('mousemove', P4.pt(gx, gy));
    await P4.W(ms === undefined ? 150 : ms);
    return llFrameHash();
  },
  click: async function (gx, gy, btn) {
    var p = P4.pt(gx, gy);
    P4.ev('mousemove', p); await P4.W(150);
    P4.ev('mousedown', p, btn); await P4.W(150);
    P4.ev('mouseup', p, btn); await P4.W(500);
    return llFrameHash();
  },
  key: async function (code, n) {
    for (var i = 0; i < (n || 1); i++) {
      Module.canvas.dispatchEvent(new KeyboardEvent('keydown',
        { code: code, key: code, bubbles: true }));
      await P4.W(60);
      Module.canvas.dispatchEvent(new KeyboardEvent('keyup',
        { code: code, key: code, bubbles: true }));
      await P4.W(60);
    }
  },
  fast: function () { window.llMove = P4.move; window.llClick = P4.click; return true; },

  /* THE LIVE UNMET-GOAL LIST.  `llGoals().cur.goals` is empty once a step is
   * in progress: the step's goal list is handed to the tick engine, and
   * `g_script_event` (fpui3.c:603, 0x00668784) is where the goals being
   * checked actually live.  This is the list that answers "what is this
   * objective still waiting for". */
  unmet: function () {
    var a = llAddrs(), I = HEAP32, e = I[a.g_script_event >> 2], o = [];
    while (e && o.length < 40) {
      o.push({ kind: I[(e + 0x0c) >> 2], f14: I[(e + 0x14) >> 2],
               f1c: I[(e + 0x1c) >> 2],
               elem: I[(e + 4) >> 2] ? llStrAt(I[I[(e + 4) >> 2] >> 2]) : null });
      e = I[e >> 2];
    }
    return o;
  },

  /* One row of evidence: the frame, the script's own position, the money and
   * the trap state.  Everything this lane claims is a row in this table. */
  note: function (tag) {
    var p = llPark(), s = llStats(), g = llGoals();
    var e = { tag: tag, h: llFrameHash(), money: p.money, people: p.people,
              gm: p.gameMode, sm: p.screenMode, ed: p.editState,
              step: g && g.cur ? g.cur.id : null,
              unmet: g && g.cur ? g.cur.goals.map(function (q) {
                       return q.kindName + (q.elem ? ' ' + q.elem : '') +
                              (q.f1c ? ' ' + q.f1c : ''); }) : null,
              fps: s.fps, dead: s.dead, traps: s.traps.length };
    this.log.push(e); return e;
  },

  /* Wait until the script's current step changes, or time out.  This is the
   * assertion a play lane actually wants: "the objective drained", not "the
   * screen changed".  Returns {drained, from, to, ms}. */
  waitStep: async function (fromId, ms) {
    var t0 = Date.now(), g;
    ms = ms || 20000;
    for (;;) {
      g = llGoals();
      var id = g && g.cur ? g.cur.id : null;
      if (id !== fromId) return { drained: true, from: fromId, to: id,
                                  ms: Date.now() - t0 };
      if (Date.now() - t0 > ms) return { drained: false, from: fromId, to: id,
                                         ms: Date.now() - t0 };
      await this.W(250);
    }
  },

  /* The affine screen<->cell map, three probes, exactly as PORT-P3 fitted it.
   * Re-fit after every scroll: the fit is of the CURRENT view. */
  fit: async function () {
    var p = [], i, xy = [[150, 100], [500, 100], [150, 300]];
    for (i = 0; i < 3; i++) {
      await llMove(xy[i][0], xy[i][1]); await this.W(140);
      var m = llSel().input.mapRef;
      p.push([xy[i][0], xy[i][1], m[0], m[1]]);
    }
    var p0 = p[0], p1 = p[1], p2 = p[2];
    var det = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1]);
    function sol(k) {
      var d1 = p1[k] - p0[k], d2 = p2[k] - p0[k];
      var A = (d1 * (p2[1] - p0[1]) - d2 * (p1[1] - p0[1])) / det;
      var B = (d2 * (p1[0] - p0[0]) - d1 * (p2[0] - p0[0])) / det;
      return [A, B, p0[k] - A * p0[0] - B * p0[1]];
    }
    this.A = sol(2); this.B = sol(3);
    /* A fit taken while the view sits off the map is GARBAGE: ScreenToMapRef
     * happily returns negative or out-of-range cells there, the affine solve
     * still succeeds, and every later cellTo() is wrong -- which is how a
     * driver ends up clicking the same dead pixel two hundred times and
     * reporting it as "the game refuses to build".  Reject it instead. */
    /* Validate by ROUND TRIP, not by the probe cells: ScreenToMapRef is affine
     * everywhere and happily reports negative cells for screen pixels north of
     * the map, which is legitimate.  What is NOT legitimate is a fit whose
     * inverse does not land back on the pixel it came from -- that happens
     * when the view has been scrolled clean off the map, and a driver that
     * does not check it goes on to click one dead pixel two hundred times and
     * report it as "the game refuses to build". */
    this.fitOk = false;
    var back = this.cellTo(p[1][2], p[1][3]);
    this.fitOk = Math.abs(back[0] - p[1][0]) < 4 && Math.abs(back[1] - p[1][1]) < 4;
    return { probes: p, ok: this.fitOk, roundTrip: back };
  },

  /* Bring a CELL on screen.  The four arrow keys scroll the map (measured on
   * THREE.MAP: Left/Right are +-18432 in g_scroll_x, Down +9216 in
   * g_scroll_y, i.e. 72 px and 36 px in the 24.8 scroll units), so the screen
   * delta needed converts straight into a press count.  Two or three passes
   * converge; the fit is retaken each pass and the result says whether the
   * cell really is on screen, which is the only claim worth making. */
  centerOn: async function (cx, cy, tx, ty) {
    tx = tx === undefined ? 320 : tx; ty = ty === undefined ? 190 : ty;
    for (var pass = 0; pass < 4; pass++) {
      var f = await this.fit();
      if (!f.ok) {                       /* lost: walk back onto the map */
        await this.key('ArrowLeft', 12); await this.key('ArrowUp', 12);
        await this.W(300);
        continue;
      }
      var s = this.cellTo(cx, cy);
      var dx = tx - s[0], dy = ty - s[1];
      if (Math.abs(dx) < 48 && Math.abs(dy) < 48) break;
      var nx = Math.round(-dx * 256 / 18432), ny = Math.round(-dy * 256 / 9216);
      if (nx > 0) await this.key('ArrowRight', Math.min(nx, 40));
      else if (nx < 0) await this.key('ArrowLeft', Math.min(-nx, 40));
      if (ny > 0) await this.key('ArrowDown', Math.min(ny, 40));
      else if (ny < 0) await this.key('ArrowUp', Math.min(-ny, 40));
      await this.W(300);
    }
    await this.fit();
    return { cell: [cx, cy], at: this.cellTo(cx, cy),
             on: this.fitOk && this.onScreen(cx, cy), fitOk: this.fitOk };
  },

  /* Is this cell empty ground?  0x40 is GLUE (levelkw3.c:206 -- the lesson-2
   * hedge pen is glued, so nothing can be built inside it), 0x8800 is an
   * ordered-but-unbuilt scenery square, an owner is anything placed. */
  free: function (cx, cy) {
    var c = llCellAt(cx, cy);
    return !!c && !c.owner && !c.isPathTile && !(c.flags & 0x40) &&
           !(c.flags & 0x8800);
  },

  /* Place the armed class on one CELL, and say whether it took.  The click is
   * only sent when the game's own cursor calls the square legal AND the cell
   * the cursor reports is the cell asked for -- a hover that lands elsewhere
   * is a stale fit, not a refusal. */
  plant: async function (cx, cy) {
    if (!this.free(cx, cy)) return { ok: false, why: 'occupied' };
    var q = this.cellTo(cx, cy);
    if (q[0] < 160 || q[0] > 612 || q[1] < 24 || q[1] > 360)
      return { ok: false, why: 'offscreen', at: q };
    await llMove(q[0], q[1], 120);
    var m = llSel().input.mapRef;
    if (m[0] !== cx || m[1] !== cy) return { ok: false, why: 'fit', got: m };
    if (!llSel().editCursor.valid)
      return { ok: false, why: 'cursor', err: llSel().editCursor.error };
    var m0 = llPark().money, o0 = llWorkers().gardenerOrderCount;
    await llClick(q[0], q[1]); await this.W(500);
    var c = llCellAt(cx, cy);
    return { ok: !!(c && (c.owner || (c.flags & 0x8800))),
             cell: [cx, cy], owner: c && c.owner, flags: c && c.flagsHex,
             money: [m0, llPark().money],
             orders: [o0, llWorkers().gardenerOrderCount] };
  },
  cellTo: function (cx, cy) {
    var a1 = this.A[0], b1 = this.A[1], c1 = this.A[2];
    var a2 = this.B[0], b2 = this.B[1], c2 = this.B[2];
    var det = a1 * b2 - b1 * a2, u = cx - c1, v = cy - c2;
    return [Math.round((u * b2 - v * b1) / det), Math.round((a1 * v - a2 * u) / det)];
  },
  /* Is that cell actually inside the 640x340 map viewport right now? */
  onScreen: function (cx, cy) {
    var q = this.cellTo(cx, cy);
    return q[0] > 8 && q[0] < 632 && q[1] > 6 && q[1] < 380;
  },

  /* Hold the cursor in one of MouseScrollMap's four edge strips until the view
   * stops moving.  The bottom strip is y > 472, UNDER the interface panel
   * (PORT-P2 P2-4), so only three of the four are usable from here. */
  scroll: async function (dir, ticks) {
    var a = llAddrs(), pt = { e: [634, 200], w: [4, 200], n: [320, 4] }[dir];
    var last = null, same = 0;
    for (var i = 0; i < (ticks || 90); i++) {
      await llMove(pt[0] + (i % 2), pt[1] + (i % 3)); await this.W(28);
      var now = HEAP32[a.g_scroll_x >> 2] + ',' + HEAP32[a.g_scroll_y >> 2];
      if (now === last) { if (++same > 8) break; } else same = 0;
      last = now;
    }
    await llMove(320, 240); await this.W(200);
    return { scroll: [HEAP32[a.g_scroll_x >> 2], HEAP32[a.g_scroll_y >> 2]] };
  },

  /* Drag the view by grabbing nothing: the game has no drag-scroll, so the
   * only two ways in are the edge strips and the MAP screen's jump.  This is
   * the MAP jump: open MAP, click the cell in the overview, close MAP. */
  mapJumpTo: async function (cx, cy, mapw) {
    await llClick(285, 443); await this.W(1200);         /* MAP button */
    /* mapscreen.c draws the whole map as a diamond centred at (320,190) with
     * one cell = 4 px on the long axis: sx = 320 + 2*(cx-cy),
     * sy = 90 + (cx+cy) - is fitted, not assumed, by clicking and reading. */
    var sx = Math.round(320 + 2 * (cx - cy));
    var sy = Math.round(190 + (cx + cy) - (mapw || 54));
    await llClick(sx, sy); await this.W(900);
    await llClick(285, 443); await this.W(1400);         /* back to the park */
    await this.fit();
    return { clicked: [sx, sy], onScreen: this.onScreen(cx, cy) };
  },

  /* Open a theme's object list.  The row is at game y 394; LEGOLAND is the
   * leftmost pill.  The button TOGGLES, so this checks the panel first. */
  TAB: { legoland: [53, 394], west: [140, 394], castle: [228, 394], adv: [315, 394] },
  panelOpen: function () {
    var d = Module.canvas.getContext('2d').getImageData(5, 200, 1, 1).data;
    return !(d[0] < 60 && d[1] > 140 && d[2] < 130);
  },
  openTab: async function (which) {
    var t = this.TAB[which || 'legoland'];
    if (!this.panelOpen()) { await llClick(t[0], t[1]); await this.W(1100); }
    return this.panelOpen();
  },

  /* Place the armed class on the first legal square of a search.  The game's
   * own CursorIsValid (0x0045f4b0) is the oracle -- llSel().editCursor.valid
   * -- so this never "clicks and hopes". */
  place: async function (spots) {
    if (!spots) {
      spots = [];
      for (var x = 140; x <= 600; x += 32)
        for (var y = 50; y <= 360; y += 32) spots.push([x, y]);
    }
    for (var i = 0; i < spots.length; i++) {
      await llMove(spots[i][0], spots[i][1], 60);
      if (llSel().editCursor.valid) {
        var m0 = llPark().money;
        await llClick(spots[i][0], spots[i][1]); await this.W(1400);
        return { at: spots[i], cell: llSel().input.mapRef, ok: true,
                 money: [m0, llPark().money] };
      }
    }
    return { ok: false, tried: spots.length };
  },
  /* Place on, or as near as possible to, one CELL. */
  placeAtCell: async function (cx, cy, r) {
    var spots = [], q = this.cellTo(cx, cy), d, e;
    r = r === undefined ? 3 : r;
    for (d = 0; d <= r; d++)
      for (e = -d; e <= d; e++) {
        var p = this.cellTo(cx + e, cy + (d - Math.abs(e)));
        var n = this.cellTo(cx + e, cy - (d - Math.abs(e)));
        spots.push(p); if (d) spots.push(n);
      }
    spots = spots.filter(function (s) {
      return s[0] > 8 && s[0] < 632 && s[1] > 6 && s[1] < 380; });
    return await this.place(spots);
  },

  /* Cold load -> the tutorial park for `n`.  Creates the profile, writes the
   * five level_done bytes, reloads (the tutorial screen builds its markers on
   * screen init) and walks the front end. */
  toLesson: async function (n, name) {
    var log = [];
    document.querySelector('canvas').scrollIntoView({ block: 'center' });
    await llMove(320, 240);
    /* Is there already a profile in slot 1?  A fresh IDBFS shows EMPTY. */
    var fresh = true;
    try { FS.stat('/gamedata/profiles/Profile1.txt'); fresh = false; } catch (e) {}
    if (fresh) {
      await llClick(260, 188); await this.W(500);
      await llType(name || 'p4');
      await llClick(505, 345); await this.W(1800);
      log.push({ step: 'profile created', h: llFrameHash() });
      var b = FS.readFile('/gamedata/profiles/Profile1.txt');
      for (var i = 0; i < 5; i++) b[0x34 + i] = 1;
      FS.writeFile('/gamedata/profiles/Profile1.txt', b);
      await new Promise(function (res, rej) {
        FS.syncfs(false, function (e) { e ? rej(e) : res(); }); });
      log.push({ step: 'level_done[0..4] = 1, RELOAD REQUIRED',
                 bytes: Array.from(b.slice(0x34, 0x39)) });
      return { log: log, reload: true };
    }
    await llClick(260, 188); await this.W(600);
    log.push({ step: 'profile slot 1', h: llFrameHash() });
    await llClick(505, 345); await this.W(1400);
    log.push({ step: 'OK -> TITLE', h: llFrameHash() });
    await llClick(252, 362); await this.W(1400);
    log.push({ step: 'New -> SELECT TUTORIAL LEVEL', h: llFrameHash() });
    /* screens3.c: five markers at x=30, y = 130, 154, 178, 202, 226. */
    await llClick(41, 130 + 24 * (n - 1) + 7); await this.W(900);
    log.push({ step: 'marker ' + n + ' lit', h: llFrameHash() });
    await llMove(320, 440); await this.W(300);
    await llClick(577, 419); await this.W(4500);
    log.push({ step: 'Accept -> BRIEFING', h: llFrameHash(), money: llPark().money });
    await llClick(577, 419); await this.W(10000);
    log.push({ step: 'Thumbs Up -> THE PARK', h: llFrameHash(),
               park: llPark(), goals: llGoals() && llGoals().stepCount });
    await this.fit();
    return { log: log, reload: false };
  }
};

/* ---- the side panel, and the pop-up that hides it -------------------------
 *
 * THE ONE THING THAT COST THIS LANE THE MOST TIME.  While a "You have a new
 * object" pop-up is up, the game answers every hover with hit type 0x1 over a
 * building that is plainly there and never changes `g_edit_object`, so a
 * driver reads a blocked pop-up as "the hut is not clickable" or "the panel is
 * empty" and files a defect that is not there.  Dismiss first, then measure.
 *
 * The LEGOLAND object panel has FOUR icon slots, at game y 89/153/217/281,
 * with scroll arrows at y 44 and y 344.  A fifth "slot" at y 345 is the DOWN
 * ARROW -- reading it as an icon is what first made this lane believe the list
 * held five entries and would not scroll.
 */
P4.panelSlots = [89, 153, 217, 281];

/* The class the edit cursor is holding, by its DISPLAY name (ObjDef +0x78 --
 * the name a player or a screenshot would give; +0xc4 -> +0x00 is the LLIDB
 * element name, which is the other half of PORT-P3's P3-4). */
P4.armed = function () {
  var a = llAddrs(), I = HEAP32, eo = I[a.g_edit_object >> 2];
  return eo ? llStrAt(I[(eo + 0x78) >> 2]) : null;
};

/* Close whatever modal pop-up is up, and prove it by hovering a known object
 * and watching the hit type come back. */
P4.dismiss = async function () {
  for (var i = 0; i < 4; i++) {
    await llClick(545, 235); await P4.W(1100);
    await llMove(325, 185, 200);
    if (llSel().hit.typeHex !== '0x1')
      return { closed: true, tries: i + 1, hit: llSel().hit.typeHex };
  }
  return { closed: false, hit: llSel().hit.typeHex };
};

P4.armByName = async function (name) {
  if (!P4.panelOpen()) { await llClick(53, 394); await P4.W(1200); }
  var want = name.toLowerCase(), p, s;
  for (p = 0; p < 6; p++) {
    for (s = 0; s < 4; s++) {
      await llClick(63, P4.panelSlots[s]); await P4.W(400);
      var n = P4.armed();
      if (n && n.toLowerCase().indexOf(want) >= 0)
        return { ok: true, name: n, page: p, slot: s };
    }
    await llClick(63, 344); await P4.W(500);
  }
  return { ok: false, last: P4.armed() };
};

/* Arm a class by name and put one down on the first square the GAME's own
 * cursor calls legal (CursorIsValid, 0x0045f4b0 -- llSel().editCursor.valid).
 * The placement is confirmed from the map, not from the click. */
P4.buildNamed = async function (name) {
  var a = await P4.armByName(name);
  if (!a.ok) return { ok: false, why: 'not in panel', a: a };
  for (var sx = 170; sx <= 600; sx += 14)
    for (var sy = 36; sy <= 344; sy += 14) {
      await llMove(sx, sy, 50);
      if (!llSel().editCursor.valid) continue;
      var cell = llSel().input.mapRef, m0 = llPark().money;
      await llClick(sx, sy); await P4.W(900);
      var c = llCellAt(cell[0], cell[1]);
      if (c && c.owner && c.owner.toLowerCase().indexOf(name.toLowerCase()) >= 0)
        return { ok: true, name: a.name, at: [sx, sy], cell: cell,
                 money: [m0, llPark().money], flags: c.flagsHex };
      if (llPark().money !== m0)
        return { ok: true, name: a.name, at: [sx, sy], cell: cell,
                 money: [m0, llPark().money], note: 'money moved' };
      if (llPark().editState !== 1) {
        var r = await P4.armByName(name);
        if (!r.ok) return { ok: false, why: 'lost arm' };
      }
    }
  return { ok: false, why: 'no legal square in view', armed: a.name };
};

/* A clear (2n+1)^2 block of empty ground -- what a 4x4 ride footprint needs
 * and a 1x1 flower bed does not.  Cursor error 10 (objmap2.c:1837, "something
 * under it") on every square a flower accepts is the signature of asking for
 * a big footprint in a crowded view. */
P4.clearArea = function (n) {
  n = n || 4;
  for (var y = n; y < 58 - n; y++) for (var x = n; x < 58 - n; x++) {
    var ok = true;
    for (var dy = -n; dy <= n && ok; dy++)
      for (var dx = -n; dx <= n && ok; dx++)
        if (!P4.free(x + dx, y + dy)) ok = false;
    if (ok) return [x, y];
  }
  return null;
};
'P4 prelude installed';

