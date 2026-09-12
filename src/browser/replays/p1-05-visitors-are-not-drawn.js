// PORT-P1 replay 05 — P1-6: the park is full of visitors and none of them is
// drawn.
//
// ONE LINE: sixty live Bloke records, every one of them a Person3D of kind 1
// with a sane scale and a screen position, eleven of those positions inside
// the map viewport — and the frame shows bare path at all eleven.
//
// This is recorded as OPEN, not as a proven defect. PORT-M12's lesson applies:
// the last "obvious" rendering complaint in this wave turned out to be the
// ride's own sprite. What is offered here is the measurement and the overlay
// that makes it checkable in one look; a render-side lane should confirm it
// against the tutorial (where PORT-M10 reported "blokes walk the paths") on
// the SAME build before anyone changes code.
//
// Paste p1-00-prelude.js, run P1.start() and let the park fill (numVisitors
// climbs to g_visitor_limit = 30 on its own in about a minute), then this.

(async function () {
  const W = (ms) => new Promise((r) => setTimeout(r, ms));
  const a = llAddrs(), I = HEAP32, F = new Float32Array(I.buffer);

  // ---- walk the live Bloke chain ---------------------------------------
  // Bloke (blokeai.c:125): next +0x00, Person3D* person +0x04, lt_action +0x0c,
  // stage +0x60, flags62 +0x62.
  // Person3D (blokeai.c:96): prev +0x00, next +0x04, kind +0x08 (1 = visitor),
  // bloke +0x0c, scale x/y/z +0x10/+0x14/+0x18, screen sx/sy +0x1c/+0x20
  // (anim2.c:49), rot +0x40.
  await llMove(600, 50, 300);              // park the cursor out of the way
  let p = I[a.g_people_head >> 2], all = 0, onScreen = [], rows = [];
  while (p && all < 400) {
    const per = I[(p + 4) >> 2];
    if (per) {
      const sx = I[(per + 0x1c) >> 2], sy = I[(per + 0x20) >> 2];
      if (rows.length < 6) rows.push({ bloke: p, person: per,
        kind: I[(per + 8) >> 2], back: I[(per + 0x0c) >> 2],
        prev: I[per >> 2], next: I[(per + 4) >> 2],
        scale: [F[(per + 0x10) >> 2], F[(per + 0x14) >> 2], F[(per + 0x18) >> 2]],
        rotY: F[(per + 0x44) >> 2], sx: sx, sy: sy,
        ltAction: HEAPU8[p + 0x0c], stage: HEAPU8[p + 0x60] });
      if (sx > 140 && sx < 635 && sy > 45 && sy < 370) onScreen.push([sx, sy]);
    }
    p = I[p >> 2]; all++;
  }

  // ---- ring each of them on a copy of the frame -------------------------
  // Save this PNG and look at it: every ring is empty path or grass.
  const c = document.createElement('canvas'); c.width = 640; c.height = 480;
  const g = c.getContext('2d');
  g.drawImage(Module.canvas, 0, 0);
  g.strokeStyle = '#ff00ff'; g.lineWidth = 1;
  for (const [x, y] of onScreen) { g.beginPath(); g.arc(x, y, 9, 0, 7); g.stroke(); }
  window.P1_OVERLAY = c.toDataURL('image/png');

  // ---- and nothing person-shaped moves ----------------------------------
  const grab = () => Module.canvas.getContext('2d')
                     .getImageData(0, 0, 640, 480).data.slice();
  const A1 = grab(); await W(2500); const A2 = grab();
  let moved = 0;
  for (let i = 0; i < A1.length; i += 4)
    if (A1[i] !== A2[i] || A1[i+1] !== A2[i+1] || A1[i+2] !== A2[i+2]) moved++;

  return {
    chainLength: all,                  // 60
    visitorLimit: llPark().visitorLimit,   // 30
    numVisitors: llPark().numVisitors,     // 30
    onScreen: onScreen.length,         // 11
    positions: onScreen,
    sample: rows,
    movedPixelsIn2500ms: moved,        // ~3,400
    overlay: 'window.P1_OVERLAY (a data: URL — open it)',
    note: 'Every moving pixel in the 2.5 s diff belongs to scenery: the Earth '
        + "Slide's rocking top ornament, the hot-dog cart's steam plume, the "
        + 'LEGOLAND entrance banner. No walking figures anywhere.'
  };
})();

// ---------------------------------------------------------------------------
// WHAT IS ALREADY RULED OUT
//
//   * The blokes are not corrupt. kind is 1, the back pointer to the Bloke is
//     right, scale is (1,1,1), rot.y takes real headings (pi/2 was seen), the
//     prev/next chain is well formed in both directions, sx/sy change every
//     frame and stay inside the viewport.
//   * They are not all off-camera: eleven of sixty are inside the map area at
//     any instant, spread over the whole park.
//   * It is not the camera or the terrain pass: the same frame draws the ride,
//     the shops, the carts, the paths and the entrance correctly, and the MAP
//     button draws the whole park.
//   * It is not the 3D person model being missing: no trap fires all session
//     (llStats().dead null, traps [] over ~60,000 frames).
//
// WHERE TO LOOK
//
// The depth-sorted print list is the join between "a Person3D exists" and
// "a Person3D is drawn": blokeai.c:140's PrintItem carries type 0x2000 for a
// 3D person and the arena is bump-allocated at 0x007cb600 through the byte
// offset at 0x0066b5a8, with printlist.c's DrawAndClearPrintList (the ObjDef
// +0xb0 slot PORT-M8 closed) consuming it. If person items never reach the
// list, nothing downstream can draw them. Adding the print-list head and the
// arena offset to main.c's LL_DBG_TABLE would answer this in one read.
//
// Also worth one minute: PORT-B10's tutorial walk on this build, with the same
// probe. PORT-M10's merge note says "blokes walk the paths" there. If they do
// and free play's do not, the difference is in what StartFreePlayPark
// (uimisc2.c:404) sets up and the level-script path does not — it calls
// AllocBlokeCounters(g_game->max_blokes) and EnterParkPlayMode by itself.
