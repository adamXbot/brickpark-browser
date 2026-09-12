/* PORT-P5 replay: TUTORIAL LESSON 3 — "Power and special rides", PLAYED TO
 * `ENDLEVEL` TWICE, on two COLD runs, and the refutation of P4-2.
 *
 *   await fetch('replays/p4-00-prelude.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/p4-02-lesson3.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/p5-00-prelude.js').then(r=>r.text()).then(eval);
 *   P5.pin(); P4.fast();
 *   await P4.toLesson(3);        // reload once if it answers reload:true
 *   await P5L3.all();
 *
 * ------------------------------------------------------------------------
 * P4-2 DOES NOT REPRODUCE, AND HERE IS WHY IT LOOKED LIKE A DEFECT
 * ------------------------------------------------------------------------
 * PORT-P4 §3.4 reported that after the `GIVE "SPIDER RIDE"` the LEGOLAND panel
 * offers exactly `Flowers | Pine Tree | Small Fountain | LEGO Clothes Shop |
 * LEGO Toy Shop` and has also lost the Space Tower Ride and the Small Power
 * Station, so lesson 3 cannot end.  Measured on two cold runs of this lane,
 * reading `g_object_list` ITSELF rather than arming icons:
 *
 *   step 14, cold run 2, the eight nodes in order, all `keep = 1`:
 *     Flowers, Pine Tree, Small Fountain, LEGO Clothes Shop, LEGO Toy Shop,
 *     Small Power Station, Space Tower Ride, SPIDER RIDE
 *   and `P5.panelTop()` arms all eight from the panel.
 *
 * PORT-P4's five names are entries 0..4 of that list.  The panel is a FOUR-slot
 * window (game y 89/153/217/281) over the list, the arrows are at y 44 and
 * y 344, and `MakeUpObjectList` (fpui2.c:1196) RESTORES
 * `g_list_scroll[g_list_menu]` on every rebuild — so the scroll offset survives
 * a panel close/open AND a MAP round trip, which is exactly what PORT-P4
 * re-checked with.  `P4L3.panelNames()` never clicks the UP arrow, so it can
 * only ever see a four-entry window from wherever the offset happens to be.
 * `P5.panelTop()` scrolls to the top first.
 *
 * The second half of it is P5-2: `P4.panelOpen()` samples canvas pixel (5,200)
 * and answered TRUE in lesson 5 with `g_menu_index == 5` (the panel SHUT and
 * `g_object_list` EMPTY), so the slot clicks land on the map, nothing arms and
 * `P4.armed()` keeps returning the previously-armed name.  Ask `g_menu_index`.
 *
 * WHAT THE PANEL'S GATE ACTUALLY IS, for whoever does find a real defect here.
 * `ObjectLinkedList` (fpui2.c:572, 0x00475720) rebuilds the list from the LLIDB
 * on every `TestMenu`, and needs, per class:
 *     (elem->type_flags & 0x13) == 0x13      loaded | AVAILABLE | type bit
 *     d->parent (+0x58) == "BUILD MENU"      else it is a CHILD (keep = 0)
 *     d->theme  (+0x5c) == this theme, or "COMMON THEME" with g_menu_index 0
 *     d->submenu(+0x60) == one of the four g_submenus
 * `EventTick_Give` (eventtick.c:354) -> `MarkElemAvailable` (0x00469900) is what
 * sets the 0x2 and raises `g_menu_dirty`; fpui3.c:694 consumes that with
 * `UpdateMenu`, which does NOTHING when `g_menu_index == 5` and clears the flag
 * anyway.  Measured here at step 0: every one of the eleven classes has
 * `parent = "BUILD MENU"`, and `type_flags` is 0x15 (not given) or 0x17 (given)
 * exactly in step with what the panel offers.  `P5.classGate()` prints it.
 *
 * ------------------------------------------------------------------------
 * WHAT ACTUALLY STOPPED LESSON 3 ON THE FIRST ATTEMPT (P5-6)
 * ------------------------------------------------------------------------
 * The Spider Ride's footprint is `left -7, top -8, right 8, bottom 9` — 16x18
 * CELLS.  `P4.buildNamed` sweeps the viewport and answers "no legal square in
 * view"; `P4.clearArea(4)` finds it nowhere.  THREE.MAP has 57 anchors that fit
 * it.  Find one, centre on it, then place.
 */

window.P5L3 = {
  /* Steps 0..5 — six Gardeners out of the Greenhouse at (40,5).  Unchanged
   * from PORT-P4: hit 0x103 each, 30 bricks each, one objective per click,
   * 1200 -> 1020, and step 5 -> 8 because 6 and 7 are reward-only. */
  hire: async function () { return await P4L3.hire(6); },

  /* The panel, before anything is GIVEn and after every GIVE.  This is the
   * P4-2 measurement, taken two ways at once. */
  panelNow: async function (tag) {
    var ol = P5.objectList();
    var p = await P5.panelTop(8);
    return { tag: tag, step: llGoals().cur ? llGoals().cur.id : null,
             menuIndex: ol.menuIndex, listMenu: ol.listMenu, dirty: ol.dirty,
             list: ol.nodes.map(function (n) {
               return n.name + ' keep=' + n.keep + ' tf=' + n.typeFlags +
                      ' parent=' + n.parent + ' submenu=' + n.submenu; }),
             armsFromPanel: p.reachable,
             moneyBarBlack: P5.blackPct(228, 3, 200, 19) };
  },

  /* Step 8 — PATHSCENERY 25 plus one fountain, one tree and one flower bed.
   * `P4L3.tally()` reimplements TallyBuildFootprints (mapbuild2.c:75), which
   * recomputes at most once every ten seconds; stop planting at 30 %. */
  scenery: async function (L, want) {
    want = want || 30;
    var path = [], seen = {}, T = [], x, y, j, k;
    for (y = 0; y < 60; y++) for (x = 0; x < 60; x++) {
      var c = llCellAt(x, y); if (c && c.isPathTile) path.push([x, y]);
    }
    for (j = 0; j < path.length; j++) {
      var d = [[1, 0], [-1, 0], [0, 1], [0, -1]];
      for (k = 0; k < 4; k++) {
        var cx = path[j][0] + d[k][0], cy = path[j][1] + d[k][1];
        var key = cx + ',' + cy; if (seen[key]) continue; seen[key] = 1;
        if (P4.free(cx, cy)) T.push([cx, cy]);
      }
    }
    L.push({ tag: 'scenery targets', nPath: path.length, nFree: T.length });
    var i = 0, planted = 0, guard = 0;
    while (i < T.length && P4L3.tally().percent < want && guard++ < 25) {
      var c0 = await P4.centerOn(T[i][0], T[i][1]);
      if (!c0.fitOk) { i += 8; continue; }
      await P5.arm('flower');
      var here = 0;
      for (j = 0; j < T.length; j++) {
        if (!P4.onScreen(T[j][0], T[j][1]) || !P4.free(T[j][0], T[j][1])) continue;
        var r = await P4.plant(T[j][0], T[j][1]);
        if (r.ok) { planted++; here++; if (llPark().editState !== 1) await P5.arm('flower'); }
        if (P4L3.tally().percent >= want) break;
      }
      L.push({ view: i, here: here, planted: planted, tally: P4L3.tally().percent });
      if (here === 0) i += 10;
      else { while (i < T.length && !P4.free(T[i][0], T[i][1])) i++; i += 1; }
    }
    for (k = 0; k < 2; k++) {
      var what = ['fountain', 'tree'][k];
      var spot = P4.clearArea(what === 'fountain' ? 4 : 1);
      if (spot) await P4.centerOn(spot[0], spot[1]);
      await P5.arm(what); await P4.fit();
      var placed = null;
      for (var sx = 170; sx <= 600 && !placed; sx += 16)
        for (var sy = 36; sy <= 344 && !placed; sy += 16) {
          await llMove(sx, sy, 50);
          if (!llSel().editCursor.valid) continue;
          var ref = llSel().input.mapRef;
          await llClick(sx, sy); await P4.W(900);
          var cc = llCellAt(ref[0], ref[1]);
          if (cc && cc.owner) placed = { cell: ref, owner: cc.owner };
        }
      L.push({ what: what, placed: placed });
    }
    L.push({ drain8: await P4.waitStep(8, 60000), tally: P4L3.tally() });
    return planted;
  },

  /* Steps 9..13 — Space Tower, Small Power Station, both shops.  The panel is
   * re-read from the top after every one; that table is §4 of the notes. */
  rides: async function (L) {
    var want = ['Space Tower', 'Power Station', 'Toy Shop', 'Clothes Shop'];
    for (var i = 0; i < want.length; i++) {
      await P5.clearModals(); await P4.dismiss();
      var spot = P4.clearArea(4); if (spot) await P4.centerOn(spot[0], spot[1]);
      var id0 = llGoals().cur ? llGoals().cur.id : null;
      var b = await P4.buildNamed(want[i]);
      var d = await P4.waitStep(id0, 30000);
      await P4.dismiss();
      L.push({ want: want[i], built: b.ok, cell: b.cell, money: b.money,
               from: id0, to: llGoals().cur ? llGoals().cur.id : null,
               drained: d.drained, panel: await this.panelNow('after ' + want[i]) });
      if (!llGoals().cur) break;
    }
  },

  /* Step 14 — the SPIDER RIDE, which P4-2 said could not be armed.  It arms;
   * what it needs is ROOM (P5-6): 16x18 cells. */
  spider: async function (L) {
    await P5.clearModals(); await P4.dismiss();
    var fits = [], cx, cy, dx, dy;
    for (cy = 9; cy < 50 && fits.length < 60; cy++)
      for (cx = 8; cx < 50 && fits.length < 60; cx++) {
        var good = true;
        for (dy = -8; dy <= 9 && good; dy++)
          for (dx = -7; dx <= 8 && good; dx++)
            if (!P4.free(cx + dx, cy + dy)) good = false;
        if (good) fits.push([cx, cy]);
      }
    L.push({ spiderFits: fits.length, footprint: '16x18 cells (-7,-8)..(8,9)' });
    if (!fits.length) return false;
    var tgt = fits[Math.floor(fits.length / 2)];
    await P4.centerOn(tgt[0], tgt[1]); await P4.fit();
    var a = await P4.armByName('Spider'); L.push({ arm: a });
    var placed = null;
    for (dx = -4; dx <= 4 && !placed; dx++) for (dy = -4; dy <= 4 && !placed; dy++) {
      var x = tgt[0] + dx, y = tgt[1] + dy;
      if (!P4.onScreen(x, y)) continue;
      var q = P4.cellTo(x, y);
      await llMove(q[0], q[1], 110);
      if (!llSel().editCursor.valid) continue;
      var m0 = llPark().money;
      await llClick(q[0], q[1]); await P4.W(1800);
      var c = llCellAt(x, y);
      if (c && c.owner) placed = { cell: [x, y], owner: c.owner, money: [m0, llPark().money] };
    }
    L.push({ spider: placed, drain14: await P4.waitStep(14, 30000),
             step: llGoals().cur ? llGoals().cur.id : null });
    return !!placed;
  },

  /* Step 15 — a SECOND Small Power Station, then ENDLEVEL. */
  finish: async function (L) {
    await P5.clearModals(); await P4.dismiss();
    var spot = P4.clearArea(4); if (spot) await P4.centerOn(spot[0], spot[1]);
    var b = await P4.buildNamed('Power Station');
    var d = await P4.waitStep(15, 30000);
    L.push({ build: b, drained: d, endLevel: { stepCount: llGoals().stepCount,
             curAddr: llGoals().curAddr, traps: llStats().traps.length,
             hash: llFrameHash(), money: llPark().money } });
  },

  all: async function () {
    var L = [];
    await P5.clearModals(); await P4.dismiss();
    L.push(await this.panelNow('step 0, before any GIVE'));
    L.push({ classGate: P5.classGate() });
    L.push({ hire: await this.hire() });
    L.push(await this.panelNow('after 6 hires'));
    await this.scenery(L, 30);
    await this.rides(L);
    await this.spider(L);
    await this.finish(L);
    return L;
  }
};
'P5L3 installed';
