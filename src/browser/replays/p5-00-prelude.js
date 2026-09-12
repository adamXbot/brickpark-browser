/* PORT-P5 prelude — PORT-P4's park driver, plus what this lane needed on top.
 *
 *   await fetch('replays/p4-00-prelude.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/p5-00-prelude.js').then(r=>r.text()).then(eval);
 *   P5.guard();                 // call this FIRST in every measurement
 *   P4.fast();
 *
 * Everything in PORT-P4's `P4` object is used unchanged; this file adds the
 * four things this lane could not do without.
 *
 * 1. `P5.guard()` — THE SHARED BROWSER PANE.  PORT-M17 §7.2 warned that another
 *    lane can navigate your tab out from under a walk.  It happened to this
 *    lane too, mid-measurement, and the only symptom was `P4 is not defined`
 *    (the honest one) — a reading taken one call earlier would have been
 *    silently from ANOTHER LANE'S BUILD.  `P5.guard()` throws unless the page
 *    is still the origin the lane pinned, so a stolen tab can never be read as
 *    a result.  Pin with `P5.pin()` right after the page loads.
 *
 * 2. `P5.personSweep(cells, step, box)` — the P4-1 measurement, the other way
 *    round.  PORT-P4 swept the hedge pen and counted ZERO person hits; the same
 *    sweep on the PORT-M17 tree is how this lane closed it.  Returns the hit
 *    type histogram, the distinct Bloke addresses seen, and every person pixel.
 *
 * 3. `P5.carry()` / `P5.drop(cx, cy)` — pick a worker up and put it down.
 *    `WorkerPopUp` (fpui4.c:470) IS the pick-up: it parks the bloke in
 *    `g_wp_obj` (= `llWorkers().carrying`) and sets `g_drag_lock`.  The drop is
 *    `CheckWorkerOnMouseStatus` (workers2.c:1201) on the mouse-button RELEASE.
 *
 * 4. `P5.panel()` — enumerate the object panel by ARMING every slot on every
 *    page and reading the display name back, the way PORT-P4 §3.4 did, but
 *    also reading the four icon POINTERS out of the panel's own slot array so
 *    a short list can be told apart from a list whose icons are parked.  That
 *    distinction is the whole of P4-2 and PORT-P4 could not make it.
 */

window.P5 = {
  origin: null,
  log: [],

  /* Pin the page this lane is measuring. */
  pin: function () {
    this.origin = location.origin + location.pathname;
    return this.origin;
  },
  /* Refuse to return a reading from a page that is no longer ours.  A shared
   * pane makes this the cheapest assertion in the file. */
  guard: function () {
    if (!this.origin) return this.pin();
    if (location.origin + location.pathname !== this.origin)
      throw new Error('TAB STOLEN: expected ' + this.origin + ' got ' +
                      location.origin + location.pathname);
    if (typeof window.P4 === 'undefined')
      throw new Error('P4 prelude gone — the page reloaded');
    return this.origin;
  },

  /* ---- the P4-1 measurement ---------------------------------------------
   * `Render3DPerson` (rin.c:534) publishes g_hit_info.type 0x306 (visitor) /
   * 0x307 (gardener) / 0x308 (mechanic) ONLY when `g_raster_hit` comes back
   * set from `Draw3DPersonModel`, so this histogram is simultaneously "can the
   * model be clicked" and "did the model paint".  Sweep fine: one minifigure
   * is about 20x30 game pixels. */
  personSweep: async function (cells, step, rx, ry) {
    this.guard();
    step = step || 4; rx = rx === undefined ? 28 : rx; ry = ry === undefined ? 40 : ry;
    var out = [], blokes = {}, total = 0, persons = 0, dist = {};
    for (var i = 0; i < cells.length; i++) {
      var q = P4.cellTo(cells[i][0], cells[i][1]), hits = [];
      for (var x = q[0] - rx; x <= q[0] + rx; x += step)
        for (var y = q[1] - ry; y <= q[1] + ry / 2; y += step) {
          if (x < 10 || x > 630 || y < 10 || y > 378) continue;
          await llMove(x, y, 35); total++;
          var h = llHit();
          dist[h.hitType] = (dist[h.hitType] || 0) + 1;
          if (h.hitType === '0x306' || h.hitType === '0x307' || h.hitType === '0x308') {
            persons++; blokes[h.hitBloke] = h.hitType;
            hits.push({ at: [x, y], t: h.hitType, bloke: h.hitBloke });
          }
        }
      out.push({ cell: cells[i], at: q, hits: hits.length, sample: hits.slice(0, 3) });
    }
    return { probes: total, personHits: persons, dist: dist,
             distinctBlokes: Object.keys(blokes).length, blokes: blokes, per: out };
  },

  /* Find a pixel that is currently answering with a person hit.
   *
   * THE TRAP THIS LANE FELL INTO, WORTH THE PARAGRAPH.  `g_hit_info` is STICKY:
   * `Render3DPerson` writes it when a person's model paints the mouse pixel and
   * nothing clears it when the next hover hits nothing, so the FIRST pixel of a
   * fresh sweep reads back whatever the PREVIOUS sweep's last person hit was.
   * A driver that trusts one read picks a pixel 150 px away from any bloke,
   * clicks it, picks nobody up, and reads that as "the pick-up does not work".
   * So: park the cursor on a pixel known to be person-free, require the type to
   * fall away there, and only then believe a 0x306/7/8 — and re-read it at the
   * candidate pixel a second time. */
  findPerson: async function (type, x0, x1, y0, y1, step) {
    this.guard();
    type = type || '0x307';
    step = step || 4;
    for (var x = (x0 || 170); x <= (x1 || 610); x += step)
      for (var y = (y0 || 40); y <= (y1 || 350); y += step) {
        await llMove(x, y, 40);
        var h = llHit();
        if (h.hitType !== type) continue;
        var bloke = h.hitBloke;
        /* clear the sticky record on a pixel 200 px away, then come back */
        await llMove(x > 320 ? 20 : 620, 370, 120);
        if (llHit().hitType === type && llHit().hitBloke === bloke) {
          /* still the same record with the cursor nowhere near: it is STALE */
          await llMove(x, y, 40);
          continue;
        }
        await llMove(x, y, 120);
        var h2 = llHit();
        if (h2.hitType === type)
          return { at: [x, y], bloke: h2.hitBloke, type: h2.hitType, confirmed: true };
      }
    return null;
  },

  /* Pick a worker up.  One click on a person pixel is the whole gesture:
   * fpui2.c:1583 routes 0x307/0x308 to `WorkerPopUp`, which parks the bloke in
   * g_wp_obj and raises g_drag_lock.  Confirmed from g_worker_on_mouse. */
  carry: async function (type, box) {
    this.guard();
    box = box || [];
    var p = await this.findPerson(type, box[0], box[1], box[2], box[3]);
    if (!p) return { ok: false, why: 'no ' + (type || '0x307') + ' pixel' };
    await llClick(p.at[0], p.at[1]); await P4.W(900);
    var w = llWorkers();
    return { ok: w.carrying === p.bloke, at: p.at, bloke: p.bloke,
             carrying: w.carrying, kind: llHit().carryKind,
             dragLock: llHit().dragLock };
  },

  /* Put the carried worker down on one cell.  See P5-1 in the notes: the plain
   * ground route is refused on this tree because `CheckWorkerOnMouseStatus`
   * reads `g_cursor.y2` (struct +0x0c = 0x813a4c, which is mouse_a.MASK = 1)
   * where the original reads 0x813a48, the cursor Y — so the 0x20..0x174 test
   * always fails.  The WORK-ORDER route (hit 0x103, a cell carrying an order)
   * is taken before that test and does work. */
  drop: async function (cx, cy) {
    this.guard();
    var q = P4.cellTo(cx, cy);
    if (q[0] < 10 || q[0] > 630 || q[1] < 10 || q[1] > 378)
      return { ok: false, why: 'offscreen', at: q };
    await llMove(q[0], q[1], 250);
    var hit = llSel().hit.typeHex, was = llWorkers().carrying;
    await llClick(q[0], q[1]); await P4.W(1000);
    var w = llWorkers();
    return { ok: w.carrying === 0, cell: [cx, cy], at: q, hitAtDrop: hit,
             carryingBefore: was, carryingAfter: w.carrying,
             dragLock: llHit().dragLock, cells: this.workerCells() };
  },

  /* Both worker lists' cells, from their 24.8 world position at +0x68/+0x6c.
   * Workers are NOT on g_people_head (NewBlokeWOList allocates them off-list),
   * which is why llPark().people never counts them. */
  workerCells: function () {
    var a = llAddrs(), I = HEAP32, out = { gardeners: [], mechanics: [] };
    function walk(head) {
      var p = I[head >> 2], o = [];
      while (p && o.length < 24) {
        o.push([I[(p + 0x68) >> 2] >> 8, I[(p + 0x6c) >> 2] >> 8]);
        p = I[p >> 2];
      }
      return o;
    }
    out.gardeners = walk(a.g_gardener_list);
    out.mechanics = walk(a.g_mechanic_list);
    return out;
  },

  /* The cursor-state block, as the three files that share 0x00813a40 see it.
   * `y2` is what workers2.c's struct calls +0x0c; it is bighelp.c's
   * mouse_a.MASK and it is a constant.  This one read is P5-1's evidence. */
  cursor: function () {
    var a = llAddrs(), I = HEAP32, b = a.g_input;
    return { flags: I[b >> 2], pointX: I[(b + 4) >> 2], pointY: I[(b + 8) >> 2],
             y2_at_0x0c: I[(b + 0x0c) >> 2],       /* mouse_a.mask — the bug */
             mouseA: I[(b + 0x10) >> 2], mouseB: I[(b + 0x18) >> 2],
             mouseC: I[(b + 0x20) >> 2],
             dragLock: I[a.g_drag_lock >> 2] };
  },

  /* ---- the panel, both ways ----------------------------------------------
   * P4-2 says lesson 3's LEGOLAND panel is missing three classes.  PORT-P4
   * could only enumerate what ARMS, which cannot tell a short list from a list
   * whose icon pointers are parked.  `armed` is the P4 reading; `slots` reads
   * the panel's own four icon pointers so the two can be compared. */
  panelSlots: [89, 153, 217, 281],
  panel: async function (pages) {
    this.guard();
    if (!P4.panelOpen()) { await llClick(53, 394); await P4.W(1200); }
    var seen = [], pageRows = [];
    for (var p = 0; p < (pages || 4); p++) {
      var row = [];
      for (var s = 0; s < 4; s++) {
        await llClick(63, this.panelSlots[s]); await P4.W(400);
        var n = P4.armed();
        row.push(n);
        if (n && seen.indexOf(n) < 0) seen.push(n);
      }
      pageRows.push({ page: p, names: row, hash: llFrameHash() });
      await llClick(63, 344); await P4.W(600);          /* the DOWN arrow */
    }
    return { reachable: seen, pages: pageRows };
  },

  /* Every class the level holds, with the three discriminators a panel builder
   * could plausibly be using.  `llClasses()` is the ObjDef census. */
  classTable: function () {
    this.guard();
    var cs = llClasses() || [];
    return cs.map(function (c) {
      return { name: c.name, elem: c.elem, type: c.type,
               flags: '0x' + (c.flags >>> 0).toString(16),
               addr: c.addr, placed: c.chainInsts.length };
    });
  },

  /* ---- THE LIST ITSELF, which is the answer to P4-2 ---------------------
   * `g_object_list` is what the panel draws; `ObjectLinkedList` (fpui2.c:572)
   * rebuilds it from the LLIDB on every `TestMenu`.  Reading the CHAIN instead
   * of arming icons separates the three things an icon walk cannot:
   *   - the class is not in the list at all (the GIVE did not take, or it
   *     failed ObjectLinkedList's `(type_flags & 0x13) == 0x13` /
   *     theme / submenu / parent gate);
   *   - it is in the list as a CHILD (`keep == 0`) of a collapsed parent, so
   *     MakeUpObjectList draws a bar instead of an icon;
   *   - it is in the list, with an icon, OUTSIDE the four-slot scroll window.
   * The third is what PORT-P4 measured and read as the first. */
  str: function (p) {
    var s = '', c; if (!p) return null;
    while ((c = HEAPU8[p++]) && s.length < 64) s += String.fromCharCode(c);
    return s;
  },
  objectList: function () {
    this.guard();
    var a = llAddrs(), I = HEAP32, out = [], n = 0;
    var p = I[a.g_object_list >> 2];
    while (p && n++ < 128) {
      var d = I[(p + 4) >> 2];
      var el = d ? I[(d + 0xc4) >> 2] : 0;
      out.push({ node: p, keep: I[(p + 8) >> 2],
                 name: d ? this.str(I[(d + 0x78) >> 2]) : null,
                 elem: el ? this.str(I[el >> 2]) : null,
                 typeFlags: el ? '0x' + (I[(el + 8) >> 2] >>> 0).toString(16) : null,
                 parent: I[(d + 0x58) >> 2] ? this.str(I[I[(d + 0x58) >> 2] >> 2]) : null,
                 theme: I[(d + 0x5c) >> 2] ? this.str(I[I[(d + 0x5c) >> 2] >> 2]) : null,
                 submenu: I[(d + 0x60) >> 2] ? this.str(I[I[(d + 0x60) >> 2] >> 2]) : null });
      p = I[p >> 2];
    }
    return { count: out.length, menuIndex: I[a.g_menu_index >> 2],
             listMenu: HEAPU8[a.g_list_menu], mode: I[a.g_object_list_mode >> 2],
             dirty: I[a.g_menu_dirty >> 2],
             menus: [0, 1, 2, 3].map(function (i) {
               return P5.str(a.g_menus + i * 20); }),
             submenus: [0, 1, 2, 3].map(function (i) {
               return P5.str(a.g_submenus + i * 20); }),
             nodes: out };
  },

  /* The same three gate fields for EVERY class the level loaded, whether or
   * not it reached the list — so "why is this one missing" is one read. */
  classGate: function () {
    this.guard();
    var I = HEAP32, cs = llClasses() || [], self = this;
    return cs.map(function (c) {
      var d = c.addr, el = I[(d + 0xc4) >> 2];
      return { name: c.name, elem: c.elem,
               typeFlags: el ? '0x' + (I[(el + 8) >> 2] >>> 0).toString(16) : null,
               gatePasses: el ? ((I[(el + 8) >> 2] & 0x13) === 0x13) : false,
               parent: I[(d + 0x58) >> 2] ? self.str(I[I[(d + 0x58) >> 2] >> 2]) : null,
               theme: I[(d + 0x5c) >> 2] ? self.str(I[I[(d + 0x5c) >> 2] >> 2]) : null,
               submenu: I[(d + 0x60) >> 2] ? self.str(I[I[(d + 0x60) >> 2] >> 2]) : null };
    });
  },

  /* Enumerate the panel HONESTLY: scroll to the TOP first.  PORT-P4's
   * `panelNames()` starts from wherever the scroll happens to be and only ever
   * clicks the DOWN arrow, and `MakeUpObjectList` restores
   * `g_list_scroll[g_list_menu]` on every rebuild — so the offset survives a
   * panel close/open AND a MAP round trip, and a four-slot window gets
   * reported as the whole list.  That is P4-2. */
  panelTop: async function (pages) {
    this.guard();
    if (llPark().editState !== 1 && !P4.panelOpen()) {
      await llClick(53, 394); await P4.W(1300);
    }
    for (var i = 0; i < 14; i++) { await llClick(63, 44); await P4.W(260); }
    return await this.panel(pages || 8);
  },

  /* A black-fill reading for P4-3: the share of pure-black pixels in a game
   * rectangle, sampled n times so a transient can be told from a persistent. */
  blackPct: function (x, y, w, h) {
    var c = Module.canvas, sx = c.width / 640, sy = c.height / 480;
    var d = c.getContext('2d').getImageData(Math.round(x * sx), Math.round(y * sy),
                                           Math.round(w * sx), Math.round(h * sy)).data;
    var n = 0, tot = d.length / 4;
    for (var i = 0; i < d.length; i += 4)
      if (d[i] < 8 && d[i + 1] < 8 && d[i + 2] < 8) n++;
    return Math.round(1000 * n / tot) / 10;
  },
  blackWatch: async function (boxes, samples, gapMs) {
    this.guard();
    var rows = [];
    for (var k = 0; k < (samples || 6); k++) {
      var r = { t: k };
      for (var b in boxes) r[b] = this.blackPct.apply(this, boxes[b]);
      rows.push(r);
      await P4.W(gapMs || 1000);
    }
    return rows;
  },

  /* ---- the THIRD modal, which PORT-P4 never met -------------------------
   * PORT-P4 §3.1 (P4-8) warns that a "You have a new object" pop-up swallows
   * every click and reads back as hit 0x1.  There is a second one, and it cost
   * this lane four measurements: the TUTORIAL HELP NOTEPAD — a full-canvas
   * page of text ("Earning and spending money") that a script MESSAGE step
   * raises.  While it is up EVERY click is eaten, `llPark().screenMode` stays
   * 7, `llStats().modal` reads "—", and hovering anything answers 0x100 — so
   * a driver reads it as "the ERASER button does nothing" and "the shop
   * cannot be selected", which is two defects that are not there.  It is
   * dismissed by TURNING THE PAGE (the flashing arrow, game (458,447)) until
   * the last page, and then the Thumbs Up at (577,419).
   *
   * The oracle is that a hover over two different map pixels answers with two
   * different hit records; under any of the three modals it does not. */
  modalUp: async function () {
    await llMove(300, 120, 130); var a = llSel().hit;
    await llMove(520, 300, 130); var b = llSel().hit;
    return !(a.type !== b.type || a.obj !== b.obj || a.cell !== b.cell);
  },
  clearModals: async function () {
    this.guard();
    var tried = [];
    for (var i = 0; i < 8; i++) {
      if (!(await this.modalUp())) return { clear: true, tried: tried };
      var pt = [[458, 447], [577, 419], [545, 235]][i % 3];
      await llClick(pt[0], pt[1]); await P4.W(1300);
      tried.push(pt);
    }
    return { clear: !(await this.modalUp()), tried: tried };
  },

  note: function (tag, extra) {
    this.guard();
    var e = P4.note(tag);
    if (extra) for (var k in extra) e[k] = extra[k];
    this.log.push(e);
    return e;
  }
};
'P5 prelude installed';
