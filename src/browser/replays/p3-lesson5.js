/* PORT-P3 replay: TUTORIAL LESSON 5 — "Big rides and the Wild West".
 *
 * Needs p3-lesson4.js loaded first (p3UnlockLessons / p3KeepAwake) and a
 * profile in slot 1.  Then `await p3Lesson5()`.
 *
 * NOTE FOR WHOEVER BRIEFED THIS LANE: lesson 5's "really big ride" is the
 * BOATING SCHOOL — a composite ride whose water pieces are looped back on
 * themselves (LOOPCOMPOSITE) — plus the Western attractions (Spinning
 * Barrels, Sheriff, Saloon, cacti).  There is NO coaster and NO log flume in
 * ANY tutorial lesson: `ObjList1..5.txt` never name one.  The coaster and the
 * flume first appear in game level 2 (`ObjList7.txt`, map GLTWO) and again in
 * ObjList9/10/11/12.
 *
 * SCRIPT (Legoland.res `ObjList5.txt`, map FIVE.MAP 60x60, CURRENCY 1000,
 * WORKERS 1,1 — both gardeners and mechanics; FEATURE ENERGY/RIDEWEAR/
 * AUTOREPAIR all 1; MAXBLOKES 5).  INIT PLACEs an incomplete Boating School
 * at (45,41) with eleven water pieces and a mermaid, and GLUEs four regions.
 *
 *   obj A  [REMINDER] FIXRIDES 0, 25
 *   obj 1  NEEDIN "FLOWERS" 5 / 10 / 15 in (49,26)..(51,18)   <-- BLOCKED, P3-2
 *   obj 2  NEEDGARDENERS 0        (put the gardeners back)
 *   obj 3  NEED "SMALL POWER STATION", 1   (+ a PERMANENT re-check)
 *   obj 4  NEEDMECHANICS 1 / 3 / 4  (+ a PERMANENT re-check)  <-- P3-1 and P3-2
 *   obj 6  LINK "BOATING SCHOOL"   -> GIVE the water pieces and the mermaid
 *   obj 7  LOOPCOMPOSITE "BOATING SCHOOL", 5 -> INTERVAL "boating school.txt"
 *          SAVE 400 -> FLASHBUTTON WESTERN, THEMEICON 1,1, FMV "Western.avi"
 *   obj 8  SELECTTHEME WESTERN -> GIVE the six Western classes
 *   obj 9  [PERMANENT] NEEDIN SPINNING BARRELS / SHERIFF / SALOON in
 *          (0,28)..(26,0) ; ENDLEVEL
 */

window.p3Lesson5 = async function () {
  var log = [];
  var S = function (ms) { return new Promise(function (r) { setTimeout(r, ms); }); };
  function note(t) {
    var p = llPark(), s = llStats();
    var e = { tag: t, h: llFrameHash(), money: p.money, gm: p.gameMode,
              sm: p.screenMode, ed: p.editState, fps: s.fps, dead: s.dead,
              traps: s.traps.length };
    log.push(e); return e;
  }
  document.querySelector('canvas').scrollIntoView({ block: 'center' });

  await llMove(320, 240);
  await llClick(260, 188); await S(400);   note('profile slot 1');
  await llClick(505, 345); await S(900);   note('OK -> TITLE');
  await llClick(252, 362); await S(900);   note('New -> SELECT TUTORIAL LEVEL');
  await llClick(41, 237);  await S(700);   note('marker: Lesson 5 lit');
  await llMove(320, 440);  await S(300);
  await llClick(577, 419); await S(4000);  note('Accept -> BRIEFING  money=1000');
  await llClick(577, 419); await S(8000);  note('Thumbs Up -> THE PARK');
  await llClick(556, 241); await S(1200);  note('close "Boating School Entrance" (90)');

  /* ---- the cell <-> screen map ---------------------------------------- */
  /* The projection is affine, so three probes invert it exactly.  Every cell
   * this returns was checked against llSel().input.mapRef and matched. */
  window.p3Fit = async function () {
    var p = [], i, xy = [[150, 100], [500, 100], [150, 300]];
    for (i = 0; i < 3; i++) {
      await llMove(xy[i][0], xy[i][1]); await S(140);
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
    window.p3A = sol(2); window.p3B = sol(3); return p;
  };
  window.p3CellTo = function (cx, cy) {
    var a1 = p3A[0], b1 = p3A[1], c1 = p3A[2];
    var a2 = p3B[0], b2 = p3B[1], c2 = p3B[2];
    var det = a1 * b2 - b1 * a2, u = cx - c1, v = cy - c2;
    return [Math.round((u * b2 - v * b1) / det), Math.round((a1 * v - a2 * u) / det)];
  };
  await p3Fit();

  /* ---- objective one: gardeners, then fifteen flowers ------------------ */
  /* The Greenhouse (POTTING SHED) is at cell (40,5) and IS reachable.  The
   * click is recognised -- llSel() gives hit 0x103, selDef "Greenhouse",
   * elem "POTTING SHED" -- and hires nobody, at any brick count.  P3-2. */
  var shed = p3CellTo(40, 5);
  var m0 = llPark().money;
  await llMove(shed[0], shed[1]); await S(300);
  await llClick(shed[0], shed[1]); await S(1500);
  log.push({ tag: 'click Greenhouse', screen: shed, money: [m0, llPark().money],
             hit: llSel().hit.typeHex, elem: llSel().selDef && llSel().selDef.elem,
             hired: llPark().money !== m0 });

  /* Flowers can still be ORDERED: the cells take the order (owner "Flowers",
   * flags 0x8800) but stay unbuilt forever because no gardener exists. */
  await llClick(53, 394); await S(1500);   note('LEGOLAND menu');
  await llClick(60, 92);  await S(900);    note('pick FLOWERS');
  await p3Fit();
  var cells = [[49, 19], [50, 20], [49, 21], [50, 22], [49, 23], [50, 24], [49, 25]];
  for (var j = 0; j < cells.length; j++) {
    var q = p3CellTo(cells[j][0], cells[j][1]);
    await llMove(q[0], q[1]); await S(160);
    await llClick(q[0], q[1]); await S(400);
  }
  await S(3000);
  log.push({ tag: 'seven flower orders', money: llPark().money,
             cell: llCellAt(49, 19) && { owner: llCellAt(49, 19).owner,
                                         flags: llCellAt(49, 19).flagsHex,
                                         built: llCellAt(49, 19).isFootprint } });
  note('BLOCKED at objective one');

  /* ---- the other blocker, for the record ------------------------------- */
  /* The Mechanic's Hut is at cell (52,5) -- cx-cy = 47.  Scroll east until
   * the clamp stops and the east-most reachable column is cx-cy = 36. */
  for (var k = 0; k < 60; k++) { await llMove(634 + (k % 2), 200 + (k % 3)); await S(28); }
  await llMove(600, 240); await S(150);
  var e = llSel().input.mapRef;
  var a = llAddrs();
  var sc = [HEAP32[a.g_scroll_x >> 2], HEAP32[a.g_scroll_y >> 2]];
  var vw = HEAPU16[(HEAP32[a.g_map >> 2] + 16) >> 1] << 8;
  log.push({ tag: 'east clamp', scroll: sc, view_w_fixed: vw,
             clampWithZeroSlack: 2 * sc[1] - vw, matches: (2 * sc[1] - vw) === sc[0],
             eastMostColumn: e[0] - e[1], mechanicsHutColumn: 47 });
  return log;
};

/* The g_popup shear, measured rather than argued.  Click any placed object,
 * then run this: the pop-up REQUEST fields land at the object's base + the
 * offsets fpui2.c's PopUpInfo uses, while bighelp.c's PopUpUI (the layout the
 * object was emitted with) puts the same fields 0x1c higher -- and the two
 * class elements InitPopUpInfo stored at +0x10c/+0x110 read as 0 where
 * PopUpInfoSetUp looks for them (+0xf0/+0xf4). */
window.p3PopupShear = function (objAddrFromLlSel) {
  var hits = [], H = HEAP32;
  for (var i = 0; i < H.length; i++) if (H[i] === objAddrFromLlSel) hits.push(i << 2);
  var B = null, x;
  for (x = 0; x < hits.length; x++)            /* the base is the +0x10c hit */
    if (hits.indexOf(hits[x] + 0x108) >= 0) { B = hits[x] - 4; break; }
  if (B === null) return { hits: hits, note: 'base not identified' };
  var rd = function (o) { return H[(B + o) >> 2]; };
  return {
    base: B,
    fpui2_view: { type: rd(0x00), obj: rd(0x04), ref: rd(0x08),
                  pos: [rd(0x0c), rd(0x10)], cls: rd(0xbc), cell: rd(0xc4),
                  kind: rd(0xdc), active: rd(0xe0), resize: rd(0xe8),
                  elem_shed: rd(0xf0), elem_hut: rd(0xf4) },
    shipped_PopUpUI: { icon_mech: rd(0x00), spr_full: rd(0x08), spr_norepair: rd(0x0c),
                  elem_shed: rd(0x10c), elem_hut: rd(0x110),
                  elem_path: rd(0x114), elem_entrance: rd(0x118) }
  };
};
