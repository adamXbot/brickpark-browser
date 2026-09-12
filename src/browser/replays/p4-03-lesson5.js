/* PORT-P4 replay: TUTORIAL LESSON 5 — "Big rides and the Wild West".
 *
 *   await fetch('replays/p4-00-prelude.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/p4-03-lesson5.js').then(r=>r.text()).then(eval);
 *   P4.fast();
 *   await P4.toLesson(5);      // reload once if it answers reload:true
 *   await P4L5.workers();      // the two buildings PORT-P3 could never reach
 *   await P4L5.flowers();      // steps 3..5  NEEDIN FLOWERS 5/10/15 -- DRAIN
 *   await P4L5.canWorkersBeClicked();   // step 6  NEEDGARDENERS 0 -- BLOCKED
 *
 * PORT-P3 reached this lesson's park and stopped at objective 1 of 9, blocked
 * twice over: the MECHANICS HUT at (52,5) was 183 px beyond the scroll clamp
 * (P3-1) and no worker could be hired at all (P3-2).  BOTH ARE CLOSED.
 *
 * MEASURED on this tree:
 *   POTTING SHED (40,5) and MECHANICS HUT (52,5) both centre on screen;
 *   four clicks on the shed  -> 4 Gardeners, 1000 -> 880 (30 each);
 *   four clicks on the hut   -> 4 Mechanics, 880 -> 760 (30 each);
 *   hit type 0x103 on every one of the eight clicks;
 *   18 flower beds inside (49,26)..(51,18) drain steps 3 -> 6 in one go --
 *   all three NEEDIN goals (5, 10 and 15) at once.
 *
 * AND WHERE IT STOPS.  Step 6 is `NEEDGARDENERS 0`: "put the Gardeners back".
 * `EventTick_Needgardeners` (eventtick2.c:398) with f1c <= 0 passes only when
 * `GetGardenerCount()` falls to zero, and the only way to remove a Gardener is
 * the worker pop-up -- `PopUpInfoSetUp` case 0x307/0x308 (fpui2.c:1583) ->
 * `WorkerPopUp`.  Hit type 0x307 has exactly one producer, `Render3DPerson`
 * (rin.c:534), which publishes `g_hit_info` only if `g_raster_hit` came back
 * set from `Draw3DPersonModel`.  So the objective is gated on the person
 * rasteriser, and `canWorkersBeClicked()` is the measurement: 968 probes in
 * +-40 px boxes around all eight workers, ZERO person hits.  Same result as
 * lesson 2's hedge pen on a different map.  See P4-1.
 *
 * SCRIPT (`ObjList5.txt`, map FIVE.MAP, CURRENCY 1000, WORKERS 1,1, ENERGY /
 * RIDEWEAR / AUTOREPAIR all 1, MAXBLOKES 5; INIT PLACEs an incomplete Boating
 * School at (45,41) with eleven water pieces and a mermaid and GLUEs four
 * regions):
 *   step 3..5  NEEDIN "FLOWERS" 5 / 10 / 15 in (49,26),(51,18)
 *   step 6     NEEDGARDENERS 0
 *   step 7/8   NEED "SMALL POWER STATION" 1 (+ a PERMANENT re-check)
 *   step 9..11 NEEDMECHANICS 1 / 3 / 4 (+ a PERMANENT re-check)
 *   then       LINK "BOATING SCHOOL", LOOPCOMPOSITE 5, SAVE 400,
 *              SELECTTHEME WESTERN, three NEEDINs in (0,28),(26,0), ENDLEVEL
 */

window.P4L5 = {
  SHED: [40, 5],
  HUT:  [52, 5],
  /* the NEEDIN rectangle the flowers have to land in */
  RECT: { x0: 49, x1: 51, y0: 18, y1: 26 },

  /* The two buildings, and four workers out of each.  NOTE the dismiss: this
   * lesson opens with a "You have a new object: Boating School Entrance (90)"
   * pop-up, and while it is up every hover over the hut reads hit 0x1 -- which
   * is exactly how a driver mistakes a blocked pop-up for an unclickable
   * building. */
  workers: async function () {
    var L = [], i, b;
    L.push({ tag: 'dismiss', r: await P4.dismiss() });
    for (i = 0; i < 2; i++) {
      b = [this.SHED, this.HUT][i];
      var c = await P4.centerOn(b[0], b[1]);
      var cell = llCellAt(b[0], b[1]);
      L.push({ tag: ['shed', 'hut'][i] + ' reach', center: c,
               owner: cell && cell.owner, elem: cell && cell.ownerElem });
    }
    for (i = 0; i < 2; i++) {
      b = [this.SHED, this.HUT][i];
      await P4.centerOn(b[0], b[1]);
      var q = P4.cellTo(b[0], b[1]);
      for (var k = 0; k < 4; k++) {
        var w0 = llWorkers(), m0 = llPark().money;
        await llMove(q[0], q[1], 200);
        var hit = llSel().hit.typeHex;
        await llClick(q[0], q[1]); await P4.W(1400);
        L.push({ tag: ['shed', 'hut'][i] + ' ' + (k + 1), hit: hit,
                 gardeners: [w0.gardeners, llWorkers().gardeners],
                 mechanics: [w0.mechanics, llWorkers().mechanics],
                 money: [m0, llPark().money], cur: llGoals().cur.id });
        await P4.fit(); q = P4.cellTo(b[0], b[1]);
      }
    }
    L.push({ tag: 'hired', workers: llWorkers(), note: P4.note('workers') });
    return L;
  },

  /* Steps 3..5 — fifteen flower beds inside the hedged rectangle.  The three
   * NEEDIN goals are checked together, so eighteen beds drain all three. */
  flowers: async function () {
    var L = [], x, y, cells = [];
    for (x = this.RECT.x0; x <= this.RECT.x1; x++)
      for (y = this.RECT.y0; y <= this.RECT.y1; y++) cells.push([x, y]);
    await P4.centerOn(50, 22);
    await llClick(63, P4.panelSlots[0]); await P4.W(600);   /* FLOWERS is first */
    L.push({ tag: 'armed', a: P4.armed(), ed: llPark().editState });
    var n = 0, id0 = llGoals().cur.id, pass, j;
    for (pass = 0; pass < 3 && n < 18; pass++) {
      for (j = 0; j < cells.length && n < 18; j++) {
        var r = await P4.plant(cells[j][0], cells[j][1]);
        if (r.ok) n++;
        if (llPark().editState !== 1) { await llClick(63, P4.panelSlots[0]); await P4.W(500); }
      }
      await P4.centerOn(50, 22);
      await llClick(63, P4.panelSlots[0]); await P4.W(500);
    }
    L.push({ tag: 'planted', n: n });
    L.push({ tag: 'steps 3..5', drained: await P4.waitStep(id0, 40000),
             unmet: P4.unmet().map(function (u) { return u.kind + ':' + (u.elem || u.f14); }),
             note: P4.note('l5 flowers') });
    return L;
  },

  /* The step-6 blocker, measured rather than argued.  Both worker lists are
   * heap blokes off g_people_head (NewBlokeWOList, workers.c:40) linked
   * through +0x00 with their 24.8 world position at +0x68/+0x6c; this centres
   * on each in turn and asks the game, at 8 px, whether the pixel under the
   * cursor is a person. */
  canWorkersBeClicked: async function () {
    var a = llAddrs(), I = HEAP32;
    function chain(head) {
      var p = I[head >> 2], o = [];
      while (p && o.length < 20) {
        o.push({ addr: p, cell: [I[(p + 0x68) >> 2] >> 8, I[(p + 0x6c) >> 2] >> 8] });
        p = I[p >> 2];
      }
      return o;
    }
    var gs = chain(a.g_gardener_list), ms = chain(a.g_mechanic_list);
    var all = gs.concat(ms), probes = 0, dist = {}, person = [];
    for (var i = 0; i < all.length; i++) {
      await P4.centerOn(all[i].cell[0], all[i].cell[1]);
      var b = P4.cellTo(all[i].cell[0], all[i].cell[1]);
      for (var dx = -40; dx <= 40; dx += 8) for (var dy = -40; dy <= 40; dy += 8) {
        var x = b[0] + dx, y = b[1] + dy;
        if (x < 16 || x > 620 || y < 12 || y > 372) continue;
        await llMove(x, y, 40); probes++;
        var h = llHit(); dist[h.hitType] = (dist[h.hitType] || 0) + 1;
        if (h.hitType === '0x306' || h.hitType === '0x307' || h.hitType === '0x308')
          person.push({ at: [x, y], type: h.hitType });
      }
    }
    return { gardeners: gs.map(function (g) { return g.cell; }),
             mechanics: ms.map(function (m) { return m.cell; }),
             probes: probes, dist: dist, personHits: person.length,
             carrying: llWorkers().carrying };
  },

  /* The RIDE info pop-up, which PORT-P1 never reached.  Clicking any placed
   * piece of the Boating School opens it: a titled frame carrying the display
   * name ("Boating School Water Way"), two lines -- "Repair cost: 0" and
   * "Scrap value: 5" -- a wrench icon and a red close box, over a body panel
   * that draws SOLID BLACK (P4-3).  It follows the cursor, and while it is up
   * the game reports hit 0x1 everywhere. */
  ridePopUp: async function (cx, cy) {
    await P4.centerOn(cx === undefined ? 45 : cx, cy === undefined ? 41 : cy);
    var q = P4.cellTo(cx === undefined ? 45 : cx, cy === undefined ? 41 : cy);
    await llMove(q[0], q[1], 250);
    var before = { hit: llSel().hit.typeHex, hash: llFrameHash() };
    await llClick(q[0], q[1]); await P4.W(1500);
    return { before: before, after: { hit: llHit(), hash: llFrameHash() },
             cell: llCellAt(cx === undefined ? 45 : cx, cy === undefined ? 41 : cy) };
  }
};
'P4L5 installed';
