/* PORT-P5 replay: TUTORIAL LESSON 2 — "Money and planting things", PLAYED TO
 * `ENDLEVEL` on the PORT-M17 tree.
 *
 *   await fetch('replays/p4-00-prelude.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/p4-01-lesson2.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/p5-00-prelude.js').then(r=>r.text()).then(eval);
 *   P5.pin(); P4.fast();
 *   await P4.toLesson(2);        // reload once if it answers reload:true
 *   await P5L2.all();
 *
 * PORT-P4 reached objective 1 of 5 and filed P4-1: `Render3DPerson` (rin.c:534)
 * is the game's only hit path for people and publishes `g_hit_info` 0x306/7/8
 * only when `g_raster_hit` comes back set from `Draw3DPersonModel`, which never
 * painted.  PORT-M17 fixed the three `FMUL`/`FMULA`/`FMULP` macros, and the
 * whole objective chain opens:
 *
 *   step 2      SELECTTHEME LEGOLAND           the LEGOLAND button
 *   steps 3..7  NEED "FLOWERS" 1..5            flower ORDERS, serviced by a
 *                                              Gardener that has left the pen
 *   steps 8..12 NEED "TREE 1" 2,4,6,8,10
 *   step 15     SELECTMODE ERASE + REMOVE the two shops
 *   step 16     NEEDIN "FOUNTAIN 1" 1 in (36,31)..(43,41) ; ENDLEVEL
 *
 * MEASURED, in order: step 2 -> 3 on the LEGOLAND button (money 1510);
 * three flower orders outside the pen at (53,5)/(53,7)/(53,9) are taken
 * (`assigned: 1`, where PORT-P4 measured `assigned: 0` for ever) and BUILT
 * (cell flags 0x8080), draining 3 -> 5; two more drain 5 -> 7 -> 8; twelve
 * Pine Trees drain 8 -> 12 -> 15; both shops erase (money +25, +30) and 15 ->
 * 16; one Small Fountain at (38,33) drains the last objective and the script
 * list EMPTIES (`llGoals().stepCount == 0`, `curAddr == 0`) — ENDLEVEL.  The
 * tutorial screen then lights lesson 3.  0 traps, 35.4-35.8 fps throughout.
 *
 * THREE THINGS THAT WILL STOP A DRIVER HERE, ALL OF THEM RECORDED IN
 * docs/lanes/scope-port-p5.md:
 *
 *  1. P5-1.  A picked-up Gardener CANNOT be put down on plain ground:
 *     `CheckWorkerOnMouseStatus` (workers2.c:1235) reads the cursor Y from
 *     0x00813a4c (`mouse_a.mask`, a constant 1) where the original reads
 *     0x00813a48, so the `0x20 <= y < 0x174` test always fails and the worker
 *     stays on the cursor.  The route that DOES work is the one this replay
 *     uses: drop the Gardener on a cell carrying a WORK ORDER (hit 0x103),
 *     which `CheckWorkerOnMouseStatus` handles before that test.
 *  2. P5-4.  A full-canvas tutorial HELP NOTEPAD appears after the tree
 *     objectives and eats every click while `screenMode` still reads 7.
 *     `P5.clearModals()`.
 *  3. The ERASER is at game (204,442) — bigscreens.c's `g_if_icon_pos[4..8]`
 *     are PATH 54, QUERY 128, ERASER 204, MAP 282, OPTIONS 358, all at y 442.
 *     PORT-P2's (200,443) is the same button; what stopped this lane was the
 *     notepad, not the coordinate.
 */

window.P5L2 = {
  /* Objective 1 — the LEGOLAND button.  The pen's centre cell (48,5) lands at
   * screen x 325 and the east scroll stop is g_scroll_x = 91904, both of which
   * PORT-P4 already closed (P2-1); re-measured here as a precondition. */
  open: async function () {
    var L = [];
    await llClick(543, 234); await P4.W(1200);           /* "2 new objects" */
    var s = await P4.scroll('e');
    await P4.fit();
    L.push({ tag: 'east clamp', scroll: s.scroll, pen: P4.cellTo(48, 5),
             onScreen: P4.onScreen(48, 5) });
    await llClick(53, 394); await P4.W(1600);
    L.push({ tag: 'objective 1', drained: await P4.waitStep(2, 15000),
             money: llPark().money });
    return L;
  },

  /* THE PICK-UP, and the proof that P4-1 is closed.  A fine sweep around every
   * Gardener the list reports: 960 probes, 186 person hits, four distinct
   * Bloke records, where PORT-P4 measured 578 probes and ZERO. */
  provePeopleAreClickable: async function () {
    var cells = P5.workerCells().gardeners;
    return await P5.personSweep(cells, 4, 28, 40);
  },

  /* Objective 2 — five flowers.  The Gardeners are penned; the pen's interior
   * is GLUEd (flag 0x40, PORT-P4's P4-6) so nothing can be planted inside it,
   * and an order placed OUTSIDE is never serviced while every Gardener is
   * behind the hedge.  So: carry one out.  The drop has to land on a cell that
   * already carries an order (hit 0x103) — see P5-1 — so the order goes down
   * first and the Gardener is dropped onto it. */
  freeOneGardener: async function () {
    var L = [];
    await P4.openTab('legoland');
    var a = await P5.arm('flower'); L.push({ arm: a });
    await P4.fit();
    /* the three cells immediately EAST of the pen (the pen is x45..52, y1..10) */
    var made = [];
    var spots = [[53, 5], [53, 7], [53, 9], [53, 4], [53, 8]];
    for (var i = 0; i < spots.length; i++) {
      var r = await P4.plant(spots[i][0], spots[i][1]);
      if (r.ok) made.push(spots[i]);
      if (made.length >= 3) break;
    }
    L.push({ orders: made, count: llWorkers().gardenerOrderCount });
    /* disarm: while the edit cursor holds a class every click PLACES it, and a
     * click on a bloke never reaches PopUpInfoSetUp.  Closing the theme tab is
     * the disarm (editState 1 -> 0). */
    if (llPark().editState === 1) { await llClick(53, 394); await P4.W(1300); }
    var c = await P5.carry('0x307', [280, 370, 165, 250]);
    L.push({ pickUp: c });
    if (!c.ok) return L;
    for (var j = 0; j < made.length; j++) {
      var d = await P5.drop(made[j][0], made[j][1]);
      L.push({ dropOnOrder: d });
      if (d.dragLock === 0) break;
    }
    await P4.W(3000);
    L.push({ after: { gardeners: P5.workerCells().gardeners,
                      orders: llWorkers().gardenerOrderCount,
                      step: llGoals().cur && llGoals().cur.id } });
    return L;
  },

  /* Objectives 2..6 and 7..11 — five flower steps and five tree steps.  Both
   * are gardener WORK ORDERS, so they drain as the freed Gardener builds. */
  plant: async function (what, n) {
    var L = [], placed = 0;
    var a = await P5.arm(what); L.push({ arm: a });
    if (!a.ok) return L;
    await P4.fit();
    for (var cy = 3; cy <= 20 && placed < n; cy++)
      for (var cx = 43; cx <= 56 && placed < n; cx++) {
        if (!P4.free(cx, cy) || !P4.onScreen(cx, cy)) continue;
        var r = await P4.plant(cx, cy);
        if (r.ok) placed++;
        if (llPark().editState !== 1) await P5.arm(what);
      }
    L.push({ placed: placed, step: llGoals().cur && llGoals().cur.id });
    return L;
  },

  /* Objective 15 — SELECTMODE ERASE and the two shops.  The map census is the
   * authority on where they are (`llFind` answers `insts: []` for both);
   * LEGO Media Shop anchors at (36,23), LEGO Toy Shop at (40,36) on TWO.MAP.
   * The eraser's own oracle is `llSel().destroyCursor.status > 0`. */
  erase: async function () {
    var L = [];
    L.push({ modals: await P5.clearModals() });
    await llClick(204, 442); await P4.W(1400);
    L.push({ tag: 'ERASE armed', editState: llPark().editState });
    var jobs = [['LEGO Media Shop', [36, 23]], ['LEGO Toy Shop', [40, 36]]];
    for (var k = 0; k < jobs.length; k++) {
      var name = jobs[k][0], anchor = jobs[k][1], done = false;
      await P4.centerOn(anchor[0], anchor[1]); await P4.fit();
      for (var dy = -4; dy <= 4 && !done; dy++)
        for (var dx = -4; dx <= 4 && !done; dx++) {
          var cl = [anchor[0] + dx, anchor[1] + dy];
          var c = llCellAt(cl[0], cl[1]);
          if (!c || c.owner !== name || !P4.onScreen(cl[0], cl[1])) continue;
          var q = P4.cellTo(cl[0], cl[1]);
          await llMove(q[0], q[1], 150);
          if (llSel().destroyCursor.status <= 0) continue;
          var m0 = llPark().money;
          await llClick(q[0], q[1]); await P4.W(1800);
          L.push({ erased: name, cell: cl, money: [m0, llPark().money],
                   step: llGoals().cur && llGoals().cur.id });
          done = true;
        }
      if (!done) L.push({ failed: name });
    }
    return L;
  },

  /* Objective 16 — one Small Fountain inside (36,31)..(43,41), then ENDLEVEL.
   * The goal's rectangle is read out of the live ScriptEvent (+0x28..+0x34),
   * not assumed. */
  fountain: async function () {
    var L = [], a = llAddrs(), I = HEAP32, e = I[a.g_script_event >> 2];
    var rect = [I[(e + 0x28) >> 2], I[(e + 0x2c) >> 2],
                I[(e + 0x30) >> 2], I[(e + 0x34) >> 2]];
    L.push({ goalArea: rect });
    await P4.centerOn(Math.round((rect[0] + rect[2]) / 2),
                      Math.round((rect[1] + rect[3]) / 2));
    await P4.fit();
    var arm = await P5.arm('fountain'); L.push({ arm: arm });
    var placed = null;
    for (var cy = rect[1]; cy <= rect[3] && !placed; cy++)
      for (var cx = rect[0]; cx <= rect[2] && !placed; cx++) {
        if (!P4.onScreen(cx, cy)) continue;
        var q = P4.cellTo(cx, cy);
        await llMove(q[0], q[1], 90);
        if (!llSel().editCursor.valid) continue;
        var m0 = llPark().money;
        await llClick(q[0], q[1]); await P4.W(1500);
        var c = llCellAt(cx, cy);
        if (c && c.owner) placed = { cell: [cx, cy], owner: c.owner,
                                     money: [m0, llPark().money] };
      }
    L.push({ fountain: placed });
    /* `g_script_cur` is 0 and `g_script_steps` is empty once ENDLEVEL runs. */
    L.push({ endLevel: { stepCount: llGoals().stepCount,
                         curAddr: llGoals().curAddr,
                         traps: llStats().traps.length,
                         hash: llFrameHash() } });
    return L;
  },

  all: async function () {
    var out = {};
    out.open = await this.open();
    out.people = await this.provePeopleAreClickable();
    out.gardener = await this.freeOneGardener();
    out.flowers = await this.plant('flower', 8);
    out.trees = await this.plant('pine', 12);
    out.erase = await this.erase();
    out.fountain = await this.fountain();
    return out;
  }
};
'P5L2 installed';
