/* PORT-P2 replay 02 — TUTORIAL LESSON 2, and the scroll clamp that stops it.
 *
 * Run p2-01-lesson1-unlock.js first (it defines P2.aim / P2.pan / P2.centre
 * and unlocks Lesson 2), then `await P2.lesson2()`.
 *
 * WHAT THIS SHOWS.  Lesson 2's first task is "the Gardeners are stuck behind
 * those hedges, pick them up and put them outside".  ObjList2.txt spawns them
 * with GARDENER(48,5) 4 inside a closed hedge pen (TWO.MAP ships 113 HEDGE
 * objects; the pen walls are x=46, x=53, y=1 and y=10).  The pen CANNOT BE
 * BROUGHT ON SCREEN: at the furthest right the scroll clamp allows, cell
 * (48,5) is at game x = 800 and the pen's nearest corner (46,10) at x = 688,
 * on a 640-pixel screen.  Neither the edge autoscroll nor the MAP screen's
 * jump gets there -- both go through ClampScrollToMap.
 *
 * P2.clampProbe() is the measurement.  It drives the view into each corner of
 * the clamp and prints g_scroll_x/g_scroll_y, and those three extreme points
 * sit EXACTLY on the clamp's four edge lines evaluated with
 * g_view_left/top/right/bottom = 0.  In the shipped legoland.exe the four
 * dwords at 0x004b95f4..0x004b9600 are 0x0003b600 each; scrolltick.c reads
 * them under those four names, but coaster3d.c / coaster10.c / unref3.c
 * declare the SAME four names at 0x008299ac..0x008299b8 (the 3D span-clip
 * rect), and in a single-namespace build only one definition survives.  See
 * docs/lanes/scope-port-p2.md finding P2-1.
 */

P2.lesson2 = async function () {
  const log = [];
  const H = t => log.push([t, llFrameHash(), llPark().money, P2.objs()]);

  /* from the progress screen with Lesson 2 lit */
  await llClick(200, 170);                      await P2.sleep(600);  H('Lesson 2 selected');
  await llClick(577, 419);                      await P2.sleep(2500); H('briefing p1');
  await llClick(455, 445);                      await P2.sleep(900);  H('briefing p2');
  await llClick(577, 419);                      await P2.sleep(3500); H('THE PARK  money 1000');

  /* "You have 2 new objects": LEGO Media Shop 25, then the green arrow, Hedge 2 */
  await llClick(558,  83);                      await P2.sleep(700);  H('new object 2 of 2');
  await llClick(558, 243);                      await P2.sleep(700);  H('popups closed');

  /* objective 1 is SELECTTHEME LEGOLAND and NOTHING ELSE -- the script never
     checks that the gardeners were moved, so the button alone advances it. */
  await llClick( 53, 394);                      await P2.sleep(900);  H('obj 1 met: NEED FLOWERS 1 is live');

  /* objective 2: NEED "FLOWERS" 1.  Arming FLOWERS and clicking a free cell
     puts a Flowers object there with cell flags 0x8800 -- an ORDER, waiting
     for a gardener.  All four gardeners are inside the unreachable pen, so
     the order is never serviced: measured 65 s of game time with the cell
     still 0x8800 and the goal still flags 1. */
  await llClick(558, 243);                      await P2.sleep(400);
  await llClick( 27,  90);                      await P2.sleep(600);  /* FLOWERS, price 1 */
  const c0 = await P2.centre();
  const at = await P2.aim(c0[0], c0[1] - 1, { x0: 150 });
  await llClick(at.px[0], at.px[1]);            await P2.sleep(1500);
  H('flower ORDERED at ' + [c0[0], c0[1] - 1]);
  await P2.sleep(40000);
  H('40 s later: still ordered, not planted');
  return log;
};

/* Drive the view into each corner of the clamp and report where the gardener
   pen lands.  Returns the three extreme (scroll_x, scroll_y) pairs and, for
   each, the screen position cell (48,5) would have. */
P2.clampProbe = async function () {
  const out = [];
  const corner = async (x, y, label) => {
    for (let i = 0; i < 8; i++) {
      await llMove(x, y); await P2.sleep(600);
      await llMove(320, 240); await P2.sleep(60);
    }
    const c = await P2.centre();
    /* the pen's centre, in game pixels, from the cell the game says is at
       (320,200) -- no model of the scroll origin needed */
    const pen = [320 + 16 * ((48 - c[0]) - (5 - c[1])),
                 200 +  8 * ((48 - c[0]) + (5 - c[1]))];
    out.push({ corner: label, scroll: P2.scroll(), centreCell: c, penAtScreen: pen });
  };
  await corner(638, 240, 'right');
  await corner(320,   2, 'top');
  await corner(  2, 240, 'left');
  await corner(320, 477, 'bottom');
  await corner(638, 240, 'right again');
  return out;
};

/* The MAP screen's jump is the other way in, and it clamps the same way.
   Enter map mode, click the pen in the overview, leave map mode: the centre
   cell comes back unchanged at the clamp's rightmost point. */
P2.mapJumpProbe = async function (mx, my) {
  const before = await P2.centre();
  await llClick(278, 443); await P2.sleep(1200);          /* Map mode */
  await llClick(mx, my);   await P2.sleep(700);           /* target (a green box appears) */
  await llClick(278, 443); await P2.sleep(1500);          /* back to the park */
  return { before, after: await P2.centre(), scroll: P2.scroll() };
};

/* await P2.lesson2(); await P2.clampProbe(); await P2.mapJumpProbe(552, 217); */
