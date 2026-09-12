/* PORT-P4 replay: TUTORIAL LESSON 3 — "Power and special rides".
 *
 *   await fetch('replays/p4-00-prelude.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/p4-02-lesson3.js').then(r=>r.text()).then(eval);
 *   P4.fast();
 *   await P4.toLesson(3);        // reload once if it answers reload:true
 *   await P4L3.hire(6);          // steps 0..5   NEEDGARDENERS 1..6   -- DRAINS
 *   await P4L3.scenery();        // step  8      PATHSCENERY 25 + 3 NEEDs -- DRAINS
 *   await P4L3.rides();          // steps 9..13  tower, power, shops   -- DRAINS
 *                                // step  14     SPIDER RIDE          -- BLOCKED
 *
 * PORT-P3 could not start this lesson at all (lesson 2 gates it) and PORT-P2
 * filed the hire path as dead: `fpui2.c` gave `g_popup` the request block at
 * 0x007fdec0 while `bighelp.c`/`popup.c` gave the same name the 376-byte
 * PopUpUI at 0x007fdea4, so every store `PopUpInfoSetUp` made landed 0x1c low
 * and the "this is a hut, hire someone" arm compared against zeros.  PORT-M15
 * split the two (`g_popup_info`).  Measured here: six clicks on the
 * Greenhouse, six Gardeners, 30 bricks each (1200 -> 1020), steps 0..5 drained
 * one per click.  P3-2 is closed.
 *
 * SCRIPT (`ObjList3.txt`, map THREE.MAP, CURRENCY 1200, WORKERS 1,0,
 * FEATURE ENERGY 0 until the Space Tower reward turns it on):
 *   step 0..5   NEEDGARDENERS 1..6             click the Greenhouse (40,5)
 *   step 8      PATHSCENERY 25 + NEED FOUNTAIN 1 / TREE 1 / FLOWERS
 *   step 9      NEED "SPACE TOWER RIDE" 1      -> GIVE power station, ENERGY 1
 *   step 10     SELECTTHEME LEGOLAND
 *   step 11     NEED "SMALL POWER STATION" 1
 *   step 12     [PERMANENT] the same            -> GIVE both shops
 *   step 13     NEED "LEGO SHOP 1" + "LEGO SHOP 2" -> FMV Spider.avi, GIVE spider
 *   step 14     NEED "SPIDER RIDE" 1            -> FLASHBUTTON MAP
 *   step 15     NEED "SMALL POWER STATION" 2 ; ENDLEVEL
 *
 * TWO THINGS THAT COST THIS LANE TIME AND ARE WORTH KNOWING:
 *
 *  1. THE "You have a new object" POP-UP SWALLOWS EVERY CLICK.  While one is
 *     up, `llSel().hit.typeHex` reads 0x1 over a building that is plainly
 *     there and `g_edit_object` never changes, so a driver reads a blocked
 *     pop-up as "the panel is empty" or "the hut is not clickable".  ALWAYS
 *     `P4.dismiss()` before measuring anything.  Its close box is at game
 *     (545,235).
 *  2. `llGoals().cur.goals` is EMPTY for a step in progress -- the step's goal
 *     list is handed to the tick engine on entry.  `P4.unmet()` reads
 *     `g_script_event` (fpui3.c:603), which is the list actually being
 *     checked; `llGoals().cur.id` is the drain signal.
 */

window.P4L3 = {
  SHED: [40, 5],
  ICON: { flowers: [63, 89], tree: [63, 153], fountain: [63, 217] },

  /* Steps 0..5 — six Gardeners out of one Greenhouse.  A click on the shed is
   * a click on an OBJECT (hit 0x103), not on a bloke, so this path never needs
   * the person rasteriser; it needs PopUpInfoSetUp to recognise the class,
   * which is precisely what the g_popup shear broke. */
  hire: async function (n) {
    var L = [], i;
    await P4.dismiss();
    await P4.centerOn(this.SHED[0], this.SHED[1]);
    var shed = P4.cellTo(this.SHED[0], this.SHED[1]);
    L.push({ tag: 'greenhouse', cell: this.SHED, at: shed,
             mapCell: llCellAt(this.SHED[0], this.SHED[1]) });
    for (i = 0; i < (n || 6); i++) {
      var id = llGoals().cur ? llGoals().cur.id : null;
      var g0 = llWorkers().gardeners, m0 = llPark().money;
      await llMove(shed[0], shed[1], 250);
      var hit = llSel().hit.typeHex;
      await llClick(shed[0], shed[1]); await P4.W(1600);
      var d = await P4.waitStep(id, 15000);
      L.push({ tag: 'click ' + (i + 1), hit: hit,
               gardeners: [g0, llWorkers().gardeners],
               money: [m0, llPark().money], drained: d,
               note: P4.note('hire ' + (i + 1)) });
      if (!d.drained) break;
      await P4.fit(); shed = P4.cellTo(this.SHED[0], this.SHED[1]);
    }
    return L;
  },

  /* Step 8 — PATHSCENERY 25 plus one fountain, one tree, one flower bed.
   *
   * PATHSCENERY is `TallyBuildFootprints` (mapbuild2.c:75): the denominator is
   * the PERIMETER of every PathSquare rectangle and the numerator the
   * perimeter cells holding a type 2/3 object, recomputed at most once every
   * ten seconds.  `P4.tally()` replicates it so the percentage is readable
   * without a rebuild; measured 32% here with 119 flower beds down.
   *
   * The FOUNTAIN is the awkward one: a 4x4 footprint, so it needs a clear
   * block and gives cursor error 10 ("something under it", objmap2.c:1837) on
   * every square that a 1x1 flower accepts.  `P4.clearArea()` finds it room.
   * All three classes become gardener WORK ORDERS, not instant builds; the
   * built state is cell flags 0x8080, the ordered state 0x8800. */
  tally: function () {
    var a = llAddrs(), I = HEAP32, sq = I[a.g_path_squares >> 2];
    var blocked = 0, special = 0, n = 0;
    var SCEN = { 'Flowers': 1, 'Pine Tree': 1, 'Small Fountain': 1 };
    function cell(x, y) {
      var c = llCellAt(x, y);
      if (!c) { blocked--; return; }
      if (c.flags & 0x10) { blocked--; return; }
      if (c.flags & 0x80) {
        if (!c.owner) { blocked--; return; }
        if (SCEN[c.owner]) special++;
      }
    }
    while (sq && n++ < 4096) {
      var x0 = I[(sq + 0x08) >> 2], y0 = I[(sq + 0x0c) >> 2];
      var x1 = I[(sq + 0x10) >> 2], y1 = I[(sq + 0x14) >> 2];
      blocked += 2 * (x1 - x0 + y1 - y0) + 4;
      for (var x = x0; x <= x1; x++) { cell(x, y0 - 1); cell(x, y1 + 1); }
      for (var y = y0; y <= y1; y++) { cell(x0 - 1, y); cell(x1 + 1, y); }
      sq = I[sq >> 2];
    }
    return { squares: n, blocked: blocked, special: special,
             percent: blocked ? Math.floor(special * 100 / blocked) : 0 };
  },

  arm: async function (which) {
    if (!P4.panelOpen()) { await llClick(53, 394); await P4.W(1200); }
    var t = this.ICON[which];
    await llClick(t[0], t[1]); await P4.W(600);
    return { editState: llPark().editState, obj: P4.armed() };
  },

  scenery: async function () {
    var L = [], j;
    /* every free cell orthogonally adjacent to a path square */
    var path = [], seen = {}, T = [], x, y;
    for (y = 0; y < 60; y++) for (x = 0; x < 60; x++) {
      var c = llCellAt(x, y); if (c && c.isPathTile) path.push([x, y]);
    }
    for (j = 0; j < path.length; j++) {
      var p = path[j], d = [[1, 0], [-1, 0], [0, 1], [0, -1]];
      for (var k = 0; k < 4; k++) {
        var cx = p[0] + d[k][0], cy = p[1] + d[k][1], key = cx + ',' + cy;
        if (seen[key]) continue; seen[key] = 1;
        if (P4.free(cx, cy)) T.push([cx, cy]);
      }
    }
    L.push({ tag: 'targets', nPath: path.length, nFree: T.length });

    var id0 = llGoals().cur.id, n = 0, i = 0, guard = 0;
    while (i < T.length && llGoals().cur.id === id0 && guard++ < 60) {
      var c0 = await P4.centerOn(T[i][0], T[i][1]);
      if (!c0.fitOk) { i += 6; continue; }
      await this.arm('flowers');
      var here = 0;
      for (j = 0; j < T.length && llGoals().cur.id === id0; j++) {
        var r = await P4.plant(T[j][0], T[j][1]);
        if (r.ok) { n++; here++;
          if (llPark().editState !== 1) await this.arm('flowers'); }
      }
      L.push({ tag: 'view', placedHere: here, total: n, tally: this.tally() });
      if (here === 0) i += 8;
      else { while (i < T.length && !P4.free(T[i][0], T[i][1])) i++; i += 1; }
    }
    /* the FOUNTAIN, which needs room, and one TREE */
    for (j = 0; j < 2; j++) {
      var what = ['fountain', 'tree'][j];
      await this.arm(what);
      var spot = P4.clearArea(what === 'fountain' ? 4 : 1);
      if (spot) await P4.centerOn(spot[0], spot[1]);
      var placed = null;
      for (var sx = 170; sx <= 600 && !placed; sx += 16)
        for (var sy = 36; sy <= 344 && !placed; sy += 16) {
          await llMove(sx, sy, 55);
          if (!llSel().editCursor.valid) continue;
          var cellRef = llSel().input.mapRef;
          await llClick(sx, sy); await P4.W(800);
          var cc = llCellAt(cellRef[0], cellRef[1]);
          if (cc && cc.owner) placed = { at: [sx, sy], cell: cellRef, owner: cc.owner };
        }
      L.push({ tag: what, placed: placed });
    }
    L.push({ tag: 'step 8', drained: await P4.waitStep(id0, 45000),
             tally: this.tally(), note: P4.note('scenery done') });
    return L;
  },

  /* Steps 9..15 — the ride/shop chain.  Each is an ordinary build: arm the
   * class by NAME out of the side panel (4 slots at game y 89/153/217/281, the
   * scroll arrows at y 44 and y 344 -- a fifth "slot" at y 345 IS the down
   * arrow, which is how this lane first mis-read the panel), then sweep the
   * viewport for a square the game's own cursor calls legal.
   *
   * MEASURED: Space Tower 892 -> 852 and steps 9 -> 11; Small Power Station
   * 852 -> 817 and 11 -> 13; LEGO Toy Shop + LEGO Clothes Shop and 13 -> 14
   * with the FMV reward (AVI is a documented non-trapping stub, so nothing
   * plays and nothing hangs).  Step 14 is where it stops -- see P4-2. */
  rides: async function () {
    var L = [], i;
    var want = ['Space Tower', 'Power Station', 'Toy Shop', 'Clothes Shop',
                'Spider', 'Power Station'];
    for (i = 0; i < want.length; i++) {
      await P4.dismiss();
      var spot = P4.clearArea(4); if (spot) await P4.centerOn(spot[0], spot[1]);
      var id0 = llGoals().cur.id;
      var b = await P4.buildNamed(want[i]);
      var d = await P4.waitStep(id0, 25000);
      L.push({ want: want[i], build: b, drained: d, cur: llGoals().cur.id,
               money: llPark().money,
               panel: await this.panelNames(),
               unmet: P4.unmet().map(function (u) { return u.kind + ':' + (u.elem || u.f14); }) });
    }
    return L;
  },

  /* Every class the LEGOLAND side panel will offer, scrolled to the end.  The
   * answer at step 14 is the finding: Flowers, Pine Tree, Small Fountain, LEGO
   * Clothes Shop, LEGO Toy Shop -- and NOT Space Tower Ride, Small Power
   * Station or Spider Ride, all three of which llClasses() shows present with
   * the same class flags as the shops. */
  panelNames: async function () {
    if (!P4.panelOpen()) { await llClick(53, 394); await P4.W(1200); }
    var seen = [], p, s;
    for (p = 0; p < 6; p++) {
      for (s = 0; s < 4; s++) {
        await llClick(63, P4.panelSlots[s]); await P4.W(300);
        var n = P4.armed(); if (n && seen.indexOf(n) < 0) seen.push(n);
      }
      await llClick(63, 344); await P4.W(450);
    }
    return seen;
  }
};
'P4L3 installed';
