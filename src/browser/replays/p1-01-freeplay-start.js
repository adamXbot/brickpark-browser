// PORT-P1 replay 01 — the FREE PLAY route, cold load to a running park.
//
// Serve build-wasm and open
//   legoland.html?args=-nointro+WINDEBUG&beat=1000
// then paste this whole file into the page console. It is one async IIFE and
// returns the table of (step, hash, llPark()) it walked.
//
// START FROM A VIRGIN IDBFS, or step 3 lands on a title screen with "Saved
// games" lit and every hash below changes with it:
//     for (const d of await indexedDB.databases())
//       indexedDB.deleteDatabase(d.name);
//     location.reload();
//
// SUPERSEDED IN PART BY p1-00-prelude.js: this file keeps the narrative walk
// and its hashes, but P1.start() there is what later replays call, and it adds
// a THIRD poke this file does not make — g_cur_profile+0x30..0x33, the four
// theme unlock bytes (P1-3). Without it the park's Wild West, Castle and
// Adventurers tabs are blank plates and only the LEGOLAND menu can be opened.
//
// TWO POKES ARE REQUIRED AND BOTH ARE THE GAME'S OWN RULE, NOT A CHEAT.
// Free play is locked behind progress the front end never grants to a fresh
// profile, and this lane could not play twelve levels to unlock it:
//
//   P1-1  the Free Play button on the title screen is inert until
//         g_cur_profile.level_done[5] (CurProfile+0x39, x86 0x0080ffd9) is
//         non-zero. InitTitleScreen (screens2.c:1117) sets flag 0x400 on the
//         icon when HaveCurrentProfile() (frontend2.c:507) is false, and that
//         function is 0x0048fc30 = `mov cl, byte [0x80ffd9]` — level six's
//         done byte, NOT "is a profile selected". bigscreens.c:303 is what
//         writes it when a level is finished. The poke sets that one byte.
//
//   P1-2  all four columns of the free-play picker are EMPTY until the
//         profile has unlocked classes. InitFreePlayLists (fpui2.c:818) walks
//         g_profile_unlocked[200] = CurProfile+0x46 (x86 0x0080ffe6) and adds
//         nothing for an id whose byte is 0. UnlockFreePlayEntry
//         (frontend2.c:278) is what writes them. The poke sets all 200, i.e.
//         "every level has been played".
//
// Both pokes write bytes the game itself writes; no code is changed.
// g_cur_screen = -1 is how InitScreens (mapscreen.c:407) is made to rebuild a
// screen it thinks is already current, so the icons are built with the byte set.
//
// Frame hashes below are from build cc6e926d+P1 on 2026-09-12. The still
// screens repeat; anything with a help bubble or a blinking icon on it does
// not (see docs/lanes/scope-port-p1.md §1).

(async function () {
  const log = [];
  const A = () => window.llAddrs();
  const cur = () => Module._ll_dbg_addr(4);            // g_cur_profile base
  const wait = (ms) => new Promise((r) => setTimeout(r, ms));
  const step = async (what, h) => {
    log.push({ step: what, hash: h || llFrameHash(), park: llPark() });
  };

  // ---- the front end ----------------------------------------------------
  await step('cold load (PLAYER DETAILS)', await llMove(320, 240));  // 0x093021ac
  await step('slot 1 -> name editor',      await llClick(260, 188)); // 0x19edb1f8
  await step("type 'adam'",                await llType('adam'));    // 0x319a6a6c
  await step('Accept -> TITLE screen',     await llClick(505, 345)); // 0x46e23314
  // llPark().screenMode is 1 here: ProfileAcceptInput (screens3.c:960) sets
  // g_screen_mode = 1 and InitScreens case 1 is InitTitleScreen.

  // ---- P1-1: unlock the Free Play button --------------------------------
  HEAPU8[cur() + 0x39] = 1;                       // level_done[5] = 1
  HEAP32[A().g_cur_screen >> 2] = -1;             // rebuild the title screen
  await wait(1500);
  await step('title screen, Free Play now lit');
  // Hover (215,70) and the help bubble reads "Start new Free Play game".

  // ---- the free-play picker (front-end screen 3) ------------------------
  await step('Free Play -> the picker', await llClick(215, 70));     // 0xfede103c
  // As shipped for this profile all four lists are empty: see P1-2.

  // ---- P1-2: unlock every class -----------------------------------------
  for (let i = 0; i < 200; i++) HEAPU8[cur() + 0x46 + i] = 1;
  HEAP32[A().g_cur_screen >> 2] = -1;             // rebuild the picker
  await wait(3000);
  await step('the picker, populated');            // 0x82a5cf1c
  // Four columns now: LEGOLAND / Wild West / Castle / Adventurers, from
  // FreePlayObjectList(200, 42,65) / (500, 187,65) / (400, 332,65) /
  // (300, 477,65), each 0xec tall, plus the Bar_Rides budget gauge at
  // (0xd1,0x156) and Accept_On_FreePlay at (0x1e4,0x14b) under FP_Cover.

  // ---- tick nine items (the budget bar is the 0x4e20 = 20000 gauge) -----
  for (const [x, y] of [[100, 120], [100, 185], [100, 255],
                        [245, 120], [390, 120], [535, 120]])
    await llClick(x, y);
  for (let i = 0; i < 4; i++) await llClick(100, 277);   // scroll column 1
  for (const y of [120, 185, 255]) await llClick(100, y);
  await step('nine items ticked, gauge ~67%');
  // The Accept bubble is covered by FP_Cover.lls while g_freeplay_progress is
  // 0 (fpui2.c:918 sets flag 0x400; sysstubs.c:480 clears it on the first
  // charge). The "smeared" bottom-right button before the first tick is that
  // cover, not a defect.

  // ---- ACCEPT -> StartFreePlayPark --------------------------------------
  await llClick(535, 395);
  await wait(6000);                                // the level load takes ~6.8 s
  await step('THE FREE PLAY PARK');
  // gameMode 3, money 10000, visitorLimit 30, visitors climb to ~28 on their
  // own. uimisc2.c:404 loads FreePlayTest.txt and calls sub_457870(0), which
  // sets g_brick_lock, so BricksAreLimited() is false and RenderMoneyBar
  // (money.c:100) returns on its first line: THE MONEY BAR IS BLANK IN FREE
  // PLAY BY DESIGN. Do not report it.

  return log;
})();
