/* PORT-P5 replay: TUTORIAL LESSON 5 — "Big rides and the Wild West", played to
 * objective 18 of 19, THROUGH `NEEDGARDENERS 0` — the objective PORT-P4 filed
 * as a BLOCKER.
 *
 *   await fetch('replays/p4-00-prelude.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/p4-03-lesson5.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/p5-00-prelude.js').then(r=>r.text()).then(eval);
 *   P5.pin(); P4.fast();
 *   await P4.toLesson(5);        // reload once if it answers reload:true
 *   await P5L5.all();
 *
 * THE SCRIPT (`ObjList5.txt`, FIVE.MAP, CURRENCY 1000, MAXBLOKES 5), read from
 * the live ScriptStep list rather than the text file:
 *   3..5  NEEDIN "FLOWERS" 5 / 10 / 15 in (49,18)..(51,26)
 *   6     NEEDGARDENERS 0
 *   7,8   NEED "SMALL POWER STATION" 1 (+ a PERMANENT re-check)
 *   9..12 NEEDMECHANICS 1 / 3 / 4 / 1
 *   13    LINK "BOATING SCHOOL"   -> GIVE the Mermaid and the Water Way
 *   14    LOOPCOMPOSITE "BOATING SCHOOL" 5
 *   15,16 (reward) ; SAVE 400
 *   17    FMV + THEMEICON 1 + FLASHBUTTON
 *   18    SELECTTHEME WESTERN -> GIVE the cacti, Saloon, Sheriff, Barrels
 *   19    NEEDIN Saloon / Sheriff / Spinning Barrels in (0,0)..(26,28); ENDLEVEL
 *
 * MEASURED: 4 Gardeners and 4 Mechanics hire (1000 -> 760, hit 0x103 on all
 * eight, PORT-P4's reading reproduced); 18 flower beds drain 3 -> 6 in one
 * move; the four Gardeners are FIRED one at a time, 4 -> 3 -> 2 -> 1 -> 0, and
 * step 6 -> 7; one Small Power Station drains 7 -> 13 in one move (the four
 * NEEDMECHANICS steps are already satisfied by the four Mechanics); a five-cell
 * path from (53,40) to (49,40) makes `llLink('BOATING SCHOOL').verdict` read
 * `SATISFIED` and 13 -> 14; four water drags close the loop and 14 -> 18 in one
 * move (LOOPCOMPOSITE, SAVE 400 and the FMV step all drain); the WESTERN tab
 * drains 18 -> 19; Saloon and Sheriff's office place.  0 traps throughout.
 *
 * ------------------------------------------------------------------------
 * HOW A WORKER IS FIRED — the answer to `NEEDGARDENERS 0`
 * ------------------------------------------------------------------------
 * `EventTick_Needgardeners` (eventtick2.c:398) passes only when
 * `GetGardenerCount()` reaches 0, and a Gardener is only removed by
 * `ControlGardeners` (workers3.c:270) when its `slot` (+0x36) reads 100.
 * `WorkerHitOnRide` (0x00470270) is what puts it there — it converts the cursor
 * to a cell, compares that cell's class element against
 * `g_popup_info.elem_shed` (0x007fdfb0) for a 0x307 on the mouse (elem_hut,
 * 0x007fdfb4, for a 0x308), calls `PutWorkerOnRide` and writes 100 into the
 * bloke.  So: PICK THE GARDENER UP AND DROP IT ON THE GREENHOUSE.
 *
 * That route also side-steps P5-1: `CheckWorkerOnMouseStatus` (workers2.c:1214)
 * tries `WorkerHitOnRide()` FIRST, on hit type 0x103, and only reaches the
 * broken `g_cursor.y2` test — which reads 0x00813a4c, `mouse_a.mask`, a
 * constant 1, where the original reads 0x00813a48, the cursor Y — when the drop
 * lands on ordinary ground.  A drop on plain ground therefore never completes;
 * a drop on the Greenhouse, the Mechanic's Hut or a work order does.
 *
 * TWO HARNESS TRAPS THIS LANE PAID FOR:
 *   - `g_hit_info` is STICKY (P5-5): the first probe of a fresh sweep reads the
 *     previous sweep's last person hit.  `P5.findPerson` parks the cursor on a
 *     person-free pixel and requires the record to fall away first.
 *   - Sweeping the whole viewport for a 0x307 pixel costs ~150 s a pass.  Read
 *     the worker's own cell out of `g_gardener_list` (+0x68/+0x6c, 24.8),
 *     centre on it, and sweep a +-40 px box.
 */

window.P5L5 = {
  SHED: [40, 5],
  HUT: [52, 5],
  RECT: { x0: 49, x1: 51, y0: 18, y1: 26 },

  workers: async function () { return await P4L5.workers(); },

  flowers: async function (L) {
    await P4.centerOn(50, 22); await P4.fit();
    var a = await P5.arm('flower'); L.push({ arm: a });
    var cells = [], x, y, n = 0;
    for (x = this.RECT.x0; x <= this.RECT.x1; x++)
      for (y = this.RECT.y0; y <= this.RECT.y1; y++) cells.push([x, y]);
    for (var pass = 0; pass < 3 && n < 18; pass++) {
      for (var j = 0; j < cells.length && n < 18; j++) {
        var r = await P4.plant(cells[j][0], cells[j][1]);
        if (r.ok) n++;
        if (llPark().editState !== 1) await P5.arm('flower');
      }
      await P4.centerOn(50, 22); await P4.fit();
      if (llPark().editState !== 1) await P5.arm('flower');
    }
    L.push({ planted: n, drain: await P4.waitStep(3, 60000),
             step: llGoals().cur && llGoals().cur.id });
  },

  /* Fire ONE worker of `kind` by dropping it on `building`. */
  fire: async function (L, kind, building) {
    var list = kind === '0x307' ? P5.workerCells().gardeners : P5.workerCells().mechanics;
    if (!list.length) return false;
    var c = list[0];
    await P4.centerOn(c[0], c[1]); await P4.fit();
    var q = P4.cellTo(c[0], c[1]), p = null, x, y;
    for (x = q[0] - 36; x <= q[0] + 36 && !p; x += 4)
      for (y = q[1] - 44; y <= q[1] + 20 && !p; y += 4) {
        if (x < 165 || x > 620 || y < 30 || y > 360) continue;
        await llMove(x, y, 32);
        var h = llHit();
        if (h.hitType !== kind) continue;
        /* P5-5: g_hit_info is sticky.  Prove the record is fresh. */
        await llMove(x > 320 ? 170 : 610, 350, 100);
        if (llHit().hitType === kind && llHit().hitBloke === h.hitBloke) continue;
        await llMove(x, y, 100);
        if (llHit().hitType === kind) p = { at: [x, y], bloke: llHit().hitBloke };
      }
    if (!p) { L.push({ miss: c, kind: kind }); return false; }
    await llClick(p.at[0], p.at[1]); await P4.W(900);
    if (!llWorkers().carrying) { L.push({ pickupFailed: p }); return false; }
    await P4.centerOn(building[0], building[1]); await P4.fit();
    var s = P4.cellTo(building[0], building[1]);
    await llMove(s[0], s[1], 300);
    var n0 = kind === '0x307' ? llWorkers().gardeners : llWorkers().mechanics;
    var hit = llSel().hit.typeHex;
    await llClick(s[0], s[1]); await P4.W(1500); await P4.W(2000);
    var n1 = kind === '0x307' ? llWorkers().gardeners : llWorkers().mechanics;
    L.push({ fired: { kind: kind, from: c, pixel: p.at, hoverHit: hit,
                      count: [n0, n1], dragLock: llHit().dragLock,
                      step: llGoals().cur && llGoals().cur.id } });
    return n1 < n0;
  },

  /* Step 6 — NEEDGARDENERS 0.  Disarm first: while the edit cursor holds a
   * class every click PLACES it and never reaches PopUpInfoSetUp. */
  needGardenersZero: async function (L) {
    if (llPark().editState === 1) { await llClick(53, 394); await P4.W(1300); }
    for (var i = 0; i < 8 && llWorkers().gardeners > 0; i++)
      await this.fire(L, '0x307', this.SHED);
    L.push({ gardeners: llWorkers().gardeners,
             drain6: await P4.waitStep(6, 25000),
             step: llGoals().cur && llGoals().cur.id });
  },

  power: async function (L) {
    await P5.clearModals(); await P4.dismiss();
    var spot = P4.clearArea(4); if (spot) await P4.centerOn(spot[0], spot[1]);
    await P5.arm('Power Station'); await P4.fit();
    var placed = null;
    for (var sx = 180; sx <= 600 && !placed; sx += 14)
      for (var sy = 40; sy <= 340 && !placed; sy += 14) {
        await llMove(sx, sy, 50);
        if (!llSel().editCursor.valid) continue;
        var ref = llSel().input.mapRef, m0 = llPark().money;
        await llClick(sx, sy); await P4.W(1200);
        var c = llCellAt(ref[0], ref[1]);
        if (c && c.owner) placed = { cell: ref, owner: c.owner, money: [m0, llPark().money] };
      }
    L.push({ power: placed, drain7: await P4.waitStep(7, 25000),
             step: llGoals().cur && llGoals().cur.id });
  },

  /* Step 13 — LINK "BOATING SCHOOL".  `llLink()` diagnoses it outright:
   * "MISSING: no reachable path square on the link square".  The link square is
   * (49,43); its PathSquare (column 49, y34..48) exists but is `reachable:
   * false`, and the reachable network's nearest limb is the path at (53,40) /
   * (54,40).  The gap is exactly (50,40)..(52,40).  Draw it with the PATH tool
   * (game (54,442)) and the verdict flips to SATISFIED. */
  link: async function (L) {
    L.push({ before: llLink('BOATING SCHOOL').verdict });
    await llClick(54, 442); await P4.W(1200);
    await P4.centerOn(51, 40); await P4.fit();
    var a = P4.cellTo(53, 40), b = P4.cellTo(49, 40), m0 = llPark().money;
    await llDrag(a[0], a[1], b[0], b[1], 8); await P4.W(2000);
    L.push({ after: llLink('BOATING SCHOOL').verdict, money: [m0, llPark().money],
             drain13: await P4.waitStep(13, 25000),
             step: llGoals().cur && llGoals().cur.id });
  },

  /* Step 14 — LOOPCOMPOSITE 5.
   *
   * The Water Way and the Little Mermaid are NOT top-level panel entries: they
   * are CHILDREN of the Boating School Entrance (`g_object_list` shows them
   * with `keep = 0` and `parent = "BOATING SCHOOL"`), so `MakeUpObjectList`
   * draws a collapsed BAR instead of icons and the class cannot be armed until
   * the bar is expanded — which is exactly what the game's own hint says
   * ("Click the arrow below the Boating School Entrance").  The arrow is at
   * game (63,330) with the list scrolled to the top, and expanding it sets
   * bit 8 in the PARENT element's `type_flags` (0x17 -> 0x1f), which is the
   * flag `MakeUpObjectList` tests.  This is the same mechanism P4-2 would have
   * needed — and lesson 3 has no children at all, every class there carries
   * `parent = "BUILD MENU"`.
   *
   * The water is then built by DRAGGING between the loop's open ends; a single
   * click places nothing and spends nothing.  `llSel().editCursor.valid` marks
   * the open ends (everything else answers cursor error 14). */
  expandBoatingSchool: async function () {
    await P5.openPanel('legoland');
    for (var i = 0; i < 14; i++) { await llClick(63, 44); await P4.W(170); }
    function tf() {
      var n = P5.objectList().nodes.filter(function (x) {
        return x.name === 'Boating School Entrance'; })[0];
      return n ? parseInt(n.tf, 16) : 0;
    }
    for (var k = 0; k < 4; k++) {
      if (tf() & 8) return { expanded: true, tf: '0x' + tf().toString(16), tries: k };
      await llClick(63, 330); await P4.W(700);
    }
    return { expanded: !!(tf() & 8) };
  },
  armChild: async function (want) {
    for (var down = 0; down < 6; down++) {
      for (var s = 0; s < 4; s++) {
        await llClick(63, P5.panelSlots[s]); await P4.W(420);
        var n = P4.armed();
        if (n && n.toLowerCase().indexOf(want.toLowerCase()) >= 0 &&
            llPark().editState === 1) return { ok: true, name: n, down: down, slot: s };
      }
      await llClick(63, 344); await P4.W(500);
    }
    return { ok: false, last: P4.armed() };
  },
  openEnds: async function (views) {
    var out = [];
    for (var i = 0; i < views.length; i++) {
      await P4.centerOn(views[i][0], views[i][1]); await P4.fit();
      for (var sx = 170; sx <= 610; sx += 12) for (var sy = 36; sy <= 350; sy += 12) {
        await llMove(sx, sy, 20);
        if (!llSel().editCursor.valid) continue;
        var m = llSel().input.mapRef;
        if (!out.some(function (q) { return q[0] === m[0] && q[1] === m[1]; })) out.push(m);
      }
    }
    return out;
  },
  waterDrag: async function (from, to) {
    if (P4.armed() !== 'Boating School Water Way') {
      var a = await this.armChild('water'); if (!a.ok) return { ok: false, why: 'arm' };
    }
    await P4.centerOn(Math.round((from[0] + to[0]) / 2), Math.round((from[1] + to[1]) / 2));
    await P4.fit();
    if (!P4.onScreen(from[0], from[1]) || !P4.onScreen(to[0], to[1]))
      return { ok: false, why: 'offscreen' };
    var a2 = P4.cellTo(from[0], from[1]), b2 = P4.cellTo(to[0], to[1]);
    await llMove(a2[0], a2[1], 250);
    var v = llSel().editCursor.valid, err = llSel().editCursor.error, m0 = llPark().money;
    await llDrag(a2[0], a2[1], b2[0], b2[1], 10); await P4.W(2200);
    return { ok: llPark().money !== m0, from: from, to: to, startValid: v,
             startErr: err, money: [m0, llPark().money],
             step: llGoals().cur && llGoals().cur.id };
  },
  loop: async function (L) {
    L.push({ expand: await this.expandBoatingSchool() });
    L.push({ arm: await this.armChild('water') });
    /* The four drags this lane used, in order.  The open ends move after every
     * one, so re-read them with openEnds() rather than assuming these. */
    var plan = [[[39, 31], [49, 31]], [[39, 36], [39, 51]], [[34, 41], [39, 46]]];
    for (var i = 0; i < plan.length; i++) {
      L.push(await this.waterDrag(plan[i][0], plan[i][1]));
      if (!llGoals().cur || llGoals().cur.id !== 14) break;
    }
    L.push({ step: llGoals().cur ? llGoals().cur.id : null,
             ends: llGoals().cur && llGoals().cur.id === 14
                   ? await this.openEnds([[42, 40], [42, 48]]) : null });
  },

  /* Steps 18/19 — the Western theme and its three attractions.
   *
   * STOPS HERE, and not on a defect.  `EventTick_Needin` (eventtick.c:854)
   * counts cells whose `cell->obj == elem` AND `cell->x/y == x/y` — i.e. the
   * object's ANCHOR must be inside (0,0)..(26,28) — and the Spinning Barrels
   * Ride's footprint is `left -4, top -5, right 8, bottom 11` = 13x17 CELLS.
   * Measured: 3,996 cursor probes over four views covering the whole goal
   * rectangle return ZERO legal squares (cursor error 10, "something under
   * it", 3,356 of them), and a geometric scan agrees — a 13x17 block cannot
   * avoid FIVE.MAP's own path network inside that rectangle, with or without
   * the Saloon and the Sheriff's office in the way.  A player would erase
   * paths; this lane erased nothing. */
  west: async function (L) {
    await P5.clearModals(); await P4.dismiss();
    L.push({ west: await P5.openPanel('west'),
             list: P5.objectList().nodes.map(function (n) { return n.name; }),
             step: llGoals().cur && llGoals().cur.id });
    for (var i = 0; i < 3; i++) {
      var want = ['Saloon', 'Sheriff', 'Spinning Barrels'][i];
      var pad = [3, 3, 6][i];
      var a = await P5.armWest(want); L.push({ arm: a });
      if (!a.ok) continue;
      var spot = null, cx, cy, dx, dy;
      for (cy = 2 + pad; cy <= 26 - pad && !spot; cy++)
        for (cx = 2 + pad; cx <= 24 - pad && !spot; cx++) {
          var good = true;
          for (dy = -pad; dy <= pad && good; dy++)
            for (dx = -pad; dx <= pad && good; dx++)
              if (!P4.free(cx + dx, cy + dy)) good = false;
          if (good) spot = [cx, cy];
        }
      if (!spot) { L.push({ noRoom: want }); continue; }
      await P4.centerOn(spot[0], spot[1]); await P4.fit();
      var placed = null;
      for (dx = -6; dx <= 6 && !placed; dx++) for (dy = -6; dy <= 6 && !placed; dy++) {
        var x = spot[0] + dx, y = spot[1] + dy;
        if (x < 0 || y < 0 || x > 26 || y > 28 || !P4.onScreen(x, y)) continue;
        var q = P4.cellTo(x, y);
        await llMove(q[0], q[1], 80);
        if (!llSel().editCursor.valid) continue;
        var m0 = llPark().money;
        await llClick(q[0], q[1]); await P4.W(1500);
        var c = llCellAt(x, y);
        if (c && c.owner) placed = { cell: [x, y], owner: c.owner, money: [m0, llPark().money] };
      }
      L.push({ built: want, placed: placed, step: llGoals().cur ? llGoals().cur.id : null });
    }
  },

  all: async function () {
    var L = [];
    await P5.clearModals(); await P4.dismiss();
    L.push({ hire: await this.workers() });
    await this.flowers(L);
    await this.needGardenersZero(L);
    await this.power(L);
    await this.link(L);
    await this.loop(L);
    await this.west(L);
    L.push({ end: { step: llGoals().cur ? llGoals().cur.id : null,
                    stepCount: llGoals().stepCount,
                    traps: llStats().traps.length, money: llPark().money } });
    return L;
  }
};

/* `P5.arm` for the WESTERN panel (menu 1). */
P5.armWest = async function (want) {
  await P5.openPanel('west');
  for (var up = 0; up < 12; up++) { await llClick(63, 44); await P4.W(180); }
  for (var p = 0; p < 6; p++) {
    for (var s = 0; s < 4; s++) {
      await llClick(63, P5.panelSlots[s]); await P4.W(420);
      var n = P4.armed();
      if (n && n.toLowerCase().indexOf(want.toLowerCase()) >= 0 &&
          llPark().editState === 1) return { ok: true, name: n, page: p, slot: s };
    }
    await llClick(63, 344); await P4.W(500);
  }
  return { ok: false, last: P4.armed() };
};
'P5L5 installed';
