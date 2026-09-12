/* PORT-P2 replay 01 — a cold load to the end of TUTORIAL LESSON 1.
 *
 * Lessons 2..5 are GATED on the progress screen: "Select tutorial level" draws
 * Lesson 1 in black with a tick and Lessons 2-5 greyed, and a click on a grey
 * line does nothing (measured: the frame hash changes with the cursor only and
 * the tick stays on Lesson 1).  So every P2 walk starts by finishing Lesson 1,
 * which flips Lesson 1 to a red cross and Lesson 2 to the tick.
 *
 * Paste into the page console of
 *   legoland.html?args=-nointro+WINDEBUG&beat=1000
 * on a VIRGIN profile (no /gamedata/profiles in IDBFS).  Runtime ~3 min.
 *
 * The aiming helpers are the load-bearing part: the map is isometric and every
 * objective in Lessons 2 and 3 is "do something to THIS cell", so a replay that
 * hard-codes pixels is only valid for one scroll position.  P2.aim(cx,cy)
 * closes the loop against g_input.mapRef -- the cell the GAME says the cursor
 * is on -- and is exact in one iteration once the basis is known:
 *     screen +32 px in x  ->  cell (+1, -1)
 *     screen +16 px in y  ->  cell (+1, +1)
 * so  dpx = 16*(dcx-dcy),  dpy = 8*(dcx+dcy).
 */

window.P2 = window.P2 || {};

P2.sleep = ms => new Promise(r => setTimeout(r, ms));

P2.pt     = () => { const a = llAddrs();
                    return [HEAP32[(a.g_input + 4) >> 2], HEAP32[(a.g_input + 8) >> 2]]; };
P2.mapRef = () => { const a = llAddrs();
                    return [HEAP32[(a.g_input + 0x24) >> 2], HEAP32[(a.g_input + 0x28) >> 2]]; };
P2.scroll = () => { const a = llAddrs();
                    return [HEAP32[a.g_scroll_x >> 2], HEAP32[a.g_scroll_y >> 2]]; };
/* the script events, which is where "is this objective met" actually lives:
   flags 1 = live and unmet, 0x82 = met this step (PORT-M13 §2). */
P2.objs   = () => llLink().events.map(e => ({ kind: e.kind, flags: e.flags,
                                              elem: e.elemName, strid: e.strid }));

P2.probe = async function (x, y) {
  await llMove(x, y); await P2.sleep(90);
  return { px: P2.pt(), cell: P2.mapRef() };
};
P2.centre = async () => (await P2.probe(320, 200)).cell;

/* Put the cursor on map cell (cx,cy).  x0 excludes the LEGOLAND side panel,
   which covers game x < 130 while it is open. */
P2.aim = async function (cx, cy, opts) {
  opts = opts || {};
  const X0 = opts.x0 || 10, X1 = opts.x1 || 630, Y0 = opts.y0 || 10, Y1 = opts.y1 || 378;
  let px = P2.pt()[0], py = P2.pt()[1];
  for (let i = 0; i < 8; i++) {
    await llMove(px, py); await P2.sleep(80);
    const c = P2.mapRef(), p = P2.pt();
    if (c[0] === cx && c[1] === cy) return { ok: true, px: p, cell: c, iters: i };
    px = Math.round(p[0] + 16 * ((cx - c[0]) - (cy - c[1])));
    py = Math.round(p[1] +  8 * ((cx - c[0]) + (cy - c[1])));
    if (px < X0 || px > X1 || py < Y0 || py > Y1)
      return { ok: false, why: 'offscreen', want: [px, py], cell: c };
  }
  return { ok: false, why: 'noconverge', cell: P2.mapRef() };
};

/* Edge-of-screen autoscroll.  MouseScrollMap (pathtile2.c:408) reads the map
   header's own margins: screen 640x480, edge 8, so the four live strips are
   x < 8, x > 632, y < 8 and y > 472.  y > 472 is UNDER the interface panel and
   is still the scroll strip -- a hold at y = 470 does nothing at all. */
P2.pan = async function (cx, cy, budgetMs) {
  const t0 = Date.now(); budgetMs = budgetMs || 25000;
  let last = null;
  while (Date.now() - t0 < budgetMs) {
    const c = await P2.centre();
    const ox = 16 * ((cx - c[0]) - (cy - c[1])), oy = 8 * ((cx - c[0]) + (cy - c[1]));
    if (Math.abs(ox) < 260 && Math.abs(oy) < 110) return { ok: true, centre: c };
    let ex = 320, ey = 240;
    if (ox >  260) ex = 638; else if (ox < -260) ex = 2;
    if (oy >  110) ey = 477; else if (oy < -110) ey = 2;
    await llMove(ex, ey); await P2.sleep(150);
    await llMove(320, 240); await P2.sleep(50);
    if (last && last[0] === c[0] && last[1] === c[1]) return { ok: false, why: 'stuck', centre: c };
    last = c;
  }
  return { ok: false, why: 'budget', centre: await P2.centre() };
};

P2.lesson1 = async function () {
  const log = [];
  const H = t => log.push([t, llFrameHash(), llPark() ? llPark().money : null]);

  /* --- the front end ---------------------------------------------------- */
  await llMove(320, 240);                       await P2.sleep(200);
  await llClick(260, 188);                      await P2.sleep(400);  H('slot 1');
  await llType('p2');                           await P2.sleep(400);  H('name typed');
  await llClick(505, 345);                      await P2.sleep(1200); H('OK -> title');
  /* On this build the profile's OK lands on the TITLE screen, not straight on
     the tutorial list.  The six bubbles are, by their own bubble help:
       (200, 62) no tooltip, inert   (500, 72) Quit LEGOLAND
       ( 95,175) Select a player     (340,215) Watch LEGOLAND Movies
       ( 95,355) Saved games         (275,375) Start LEGOLAND Game        */
  await llClick(275, 375);                      await P2.sleep(1800); H('progress screen');
  await llClick(200, 145);                      await P2.sleep(500);  H('Lesson 1 selected');
  await llClick(577, 419);                      await P2.sleep(1800); H('briefing p1');
  await llClick(455, 445);                      await P2.sleep(900);  H('briefing p2');
  await llClick(577, 419);                      await P2.sleep(3000); H('THE PARK  money 1000');

  /* --- objective 1: SELECTTHEME LEGOLAND -------------------------------- */
  await llClick(558, 243);                      await P2.sleep(400);  /* close "new object" */
  await llClick( 53, 394);                      await P2.sleep(900);  H('LEGOLAND menu');
  await llClick(558, 243);                      await P2.sleep(400);  /* close Space Tower popup */

  /* --- objective 2: NEED "SPACE TOWER RIDE", 1 -------------------------- */
  await llClick( 27, 156);                      await P2.sleep(500);  /* arm the ride */
  await P2.pan(60, 56);
  const at = await P2.aim(60, 56, { x0: 150 });
  if (!at.ok) throw new Error('could not aim at (60,56): ' + JSON.stringify(at));
  await llClick(at.px[0], at.px[1]);            await P2.sleep(1500); H('built  money -40');
  await llClick(577, 419);                      await P2.sleep(1400); H('"Your First Ride!" read');
  /* (60,56) is chosen so the clearance ring RefreshObjList paves lands on row
     53, one square from the reachable row-52 path: LINK is SATISFIED on the
     frame the ride finishes.  llLink('SPACE TOWER RIDE').verdict proves it. */

  /* --- objective 3: SELECTMODE PATH, then LINK "LEGO SHOP 1" ------------ */
  await llClick( 40, 443);                      await P2.sleep(700);  H('PATH armed');
  /* The shop ships at (74,61); its ring squares are rows 56/66 and columns
     70/77, all flag-1 (unreachable), and the reachable network's nearest limb
     is column 77 from y32..y52.  The gap is exactly (77,53)..(77,55). */
  await P2.pan(77, 54);
  const a1 = await P2.aim(77, 53, { x0: 150 }), a2 = await P2.aim(77, 55, { x0: 150 });
  await llDrag(a1.px[0], a1.px[1], a2.px[0], a2.px[1], 8);
  await P2.sleep(1200);                                               H('shop LINKed');

  /* --- objective 4: SELECTMODE ERASE, then REMOVE "LEGO SHOP 1", 0 ------ */
  await llClick(200, 443);                      await P2.sleep(700);  H('ERASE armed');
  const a3 = await P2.aim(74, 61, { x0: 150 });
  await llClick(a3.px[0], a3.px[1]);            await P2.sleep(1500); H('shop erased  money +30');
  await llClick(577, 419);                      await P2.sleep(1500); H('"end level" read');

  /* --- the End Level button, bottom right ------------------------------- */
  await llClick(600, 435);                      await P2.sleep(2000); H('CONGRATULATIONS');
  await llClick(577, 419);                      await P2.sleep(2500); H('progress: Lesson 2 lit');
  return log;
};

/* await P2.lesson1(); */
