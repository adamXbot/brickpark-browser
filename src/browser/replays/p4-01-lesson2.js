/* PORT-P4 replay: TUTORIAL LESSON 2 — "Money and planting things", replayed
 * on the tree that carries PORT-M15 (the dual-address class), PORT-M16 and the
 * memmove fix.  PORT-P2 declared this lesson UNWINNABLE at objective 1 of 5.
 *
 *   await fetch('replays/p4-00-prelude.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/p4-01-lesson2.js').then(r=>r.text()).then(eval);
 *   P4.fast();
 *   await P4.toLesson(2);      // (reload once if it answers reload:true)
 *   await P4L2.open();         // objective 1 -- DRAINS
 *   await P4L2.flowers();      // objective 2 -- BLOCKED, and why
 *
 * SCRIPT (Legoland.res `ObjList2.txt`, map TWO.MAP 54x54, CURRENCY 1000,
 * GARDENER(48,5) 4 spawned INSIDE a closed 8x10 hedge pen, WORKERS 1,0,
 * THEMEICON 0,1 -- LEGOLAND only):
 *
 *   step 2   SELECTTHEME LEGOLAND        -> GIVE FLOWERS, FLASHBUTTON
 *   step 3.. NEED "FLOWERS" 1..5         (five steps, one per flower)
 *   step 8.. NEED "TREE 1" 2,4,6,8,10    (five steps, two trees each)
 *   step 15  SELECTMODE ERASE + REMOVE "LEGO MEDIA SHOP" + REMOVE "LEGO SHOP 1"
 *   step 16+ NEEDIN "FOUNTAIN 1" 1 in (36,31)..(43,41) ; ENDLEVEL
 *
 * RESULT ON THIS TREE: objective 1 DRAINS (step 2 -> 3); objective 2 does not,
 * and the reason is no longer P2-1.
 *
 * WHAT PORT-P2 COULD NOT DO, AND WHY IT CAN BE DONE NOW.  `ClampScrollToMap`
 * read `g_view_left/top/right/bottom` -- four names `coaster3d.c` also used
 * for its 3D span clip -- so the four EDGE clamps ran with 0 slack instead of
 * the shipped 243200 and the view stopped 475 px short of every edge, leaving
 * the pen 160 px off the right of a 54-wide map with no way in.  PORT-M15
 * renamed the scroll set to `g_scroll_slack_*`.  Measured here: the east stop
 * is g_scroll_x = 91904, which is PORT-P2's own closed form for the SHIPPED
 * constants (4096*mapw - 129280 = 91904) TO THE UNIT, against -29696 for the
 * zero-slack clamp; and the pen's centre cell (48,5) lands at screen x = 325,
 * mid-canvas.  P2-1 / P3-1 are closed.
 */

window.P4L2 = {
  /* Objective 1 — the LEGOLAND button.  The script's goal check really is
   * `SELECTTHEME LEGOLAND` and nothing else (PORT-P2 read the script right):
   * it never verifies the Gardeners were moved.  So it drains whatever happens
   * to them -- and objective 2 then cannot, because planting is a WORK ORDER
   * and a penned Gardener can never walk to it. */
  open: async function () {
    var L = [], a = llAddrs();
    await llClick(543, 234); await P4.W(1200);      /* "2 new objects" close */
    L.push(P4.note('popup closed'));

    var s = await P4.scroll('e');
    await P4.fit();
    var vw = HEAPU16[(HEAP32[a.g_map >> 2] + 16) >> 1] << 8;
    L.push({ tag: 'east clamp', scroll: s.scroll,
             clampWithZeroSlack: 2 * s.scroll[1] - vw,      /* the OLD stop */
             predictedShipped: 4096 * 54 - 129280,          /* P2's own form */
             penCentreScreenX: P4.cellTo(48, 5)[0],
             penOnScreen: P4.onScreen(48, 5) });

    /* THE PICK-UP, and why it cannot be done.  `fpui2.c:1583` routes hit type
     * 0x307 to `WorkerPopUp`, and the ONLY producer of 0x307 is
     * `Render3DPerson` (rin.c:534), which publishes `g_hit_info` AFTER
     * `Draw3DPersonModel` and ONLY IF `g_raster_hit` came back set -- that is,
     * only if the person's model actually painted the pixel under the cursor.
     * There is no cell lookup for blokes the way there is for objects.  So
     * `llHit().hitType` over a bloke IS the whole question, and this sweeps the
     * pen at 8 px and reports the distribution.
     * MEASURED: 578 probes, 0 person hits, the pen visibly empty. */
    var box = [P4.cellTo(45, 1), P4.cellTo(53, 10), P4.cellTo(53, 1), P4.cellTo(45, 10)];
    var xs = box.map(function (b) { return b[0]; });
    var ys = box.map(function (b) { return b[1]; });
    var x0 = Math.max(10, Math.min.apply(null, xs)), x1 = Math.min(630, Math.max.apply(null, xs));
    var y0 = Math.max(10, Math.min.apply(null, ys)), y1 = Math.min(378, Math.max.apply(null, ys));
    var dist = {}, person = [], probes = 0, x, y;
    for (x = x0; x <= x1; x += 8) for (y = y0; y <= y1; y += 8) {
      await llMove(x, y, 45); probes++;
      var h = llHit(); dist[h.hitType] = (dist[h.hitType] || 0) + 1;
      if (h.hitType === '0x306' || h.hitType === '0x307' || h.hitType === '0x308')
        person.push({ at: [x, y], type: h.hitType });
    }
    L.push({ tag: 'can a Gardener be clicked?', probes: probes, dist: dist,
             personHits: person.length, carrying: llWorkers().carrying,
             gardenerCells: P4L2.gardenerCells() });

    await llClick(53, 394); await P4.W(1600);
    L.push(P4.note('LEGOLAND button -> SELECTTHEME'));
    L.push({ tag: 'objective 1', drained: await P4.waitStep(2, 12000) });
    return L;
  },

  /* The four Gardeners are NOT on g_people_head: NewBlokeWOList allocates them
   * off-list (workers.c:40) and they hang on g_gardener_list instead, linked
   * through +0x00, with their 24.8 world position at +0x68/+0x6c.  A driver
   * that walks only the people chain concludes the level has no Gardeners. */
  gardenerCells: function () {
    var a = llAddrs(), I = HEAP32, p = I[a.g_gardener_list >> 2], o = [];
    while (p && o.length < 20) {
      o.push([I[(p + 0x68) >> 2] >> 8, I[(p + 0x6c) >> 2] >> 8]);
      p = I[p >> 2];
    }
    return o;
  },

  /* Objective 2 — five flowers.  Two routes, both measured, both refused:
   *
   *  a) INSIDE the pen, where the Gardeners already are.  Every interior cell
   *     carries map flag 0x40 -- GLUE (levelkw3.c:206, EventTick_Glue
   *     eventtick.c:542) -- and objmap2.c:1827 turns that into cursor error 1.
   *     The pen is glued DELIBERATELY so the player cannot build his way out
   *     of the puzzle.  Measured: the hedge wall is x 45..52, y 1..10 and
   *     every interior cell reads 0x40 with no owner.  GAME RULE, not a port
   *     defect.
   *  b) OUTSIDE the pen.  The order IS accepted (cell owner "Flowers", flags
   *     0x8800, gardenerOrderCount rises) and is never serviced: the Gardeners
   *     take the order into their own +0x50 slot and stay behind the hedge, so
   *     the WorkOrder's `assigned` stays 0 and no flower is ever built. */
  flowers: async function () {
    var L = [], j;
    await P4.openTab('legoland');
    await llClick(60, 92); await P4.W(900);
    L.push(P4.note('FLOWERS armed'));
    await P4.fit();
    var inside = [[48, 4], [49, 6], [51, 4], [47, 3]];
    for (j = 0; j < inside.length; j++) {
      var q = P4.cellTo(inside[j][0], inside[j][1]);
      await llMove(q[0], q[1], 200);
      var c = llCellAt(inside[j][0], inside[j][1]);
      L.push({ tag: 'inside ' + inside[j], valid: llSel().editCursor.valid,
               cursorError: llSel().editCursor.error, cellFlags: c.flagsHex,
               glued: !!(c.flags & 0x40) });
    }
    var outside = [[44, 5], [44, 7], [47, 12], [49, 12], [44, 9]];
    for (j = 0; j < outside.length; j++)
      L.push({ tag: 'outside ' + outside[j],
               r: await P4.plant(outside[j][0], outside[j][1]) });
    await P4.W(20000);
    L.push({ tag: 'after 20 s', orders: llWorkers().gardenerOrders,
             gardenerCells: P4L2.gardenerCells(), cur: llGoals().cur.id,
             note: P4.note('objective 2 blocked') });
    return L;
  }
};
'P4L2 installed';
