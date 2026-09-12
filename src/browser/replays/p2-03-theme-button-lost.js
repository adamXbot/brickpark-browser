/* PORT-P2 replay 03 — the LEGOLAND theme button disappears, permanently.
 *
 * Run p2-01-lesson1-unlock.js first for the helpers, be in Lesson 2's park
 * with the theme row intact ("LEGOLAND" in the first of the four pills at
 * game y 394), then `await P2.themeButtonRace()`.
 *
 * WHAT IT DOES.  Opens the LEGOLAND side panel, arms an object, enters MAP
 * mode, jumps the view, and comes back -- with ~1300 ms between steps instead
 * of ~1500.  After it, `llCrop(0,380,420,40,1)` shows FOUR EMPTY PILLS: the
 * theme button is gone, it has no bubble help, a click on it does nothing,
 * llPark().editState never leaves 0 again, and no further map round trip,
 * panel toggle or objective brings it back.  Nothing can be built or planted
 * for the rest of the level, so the level can no longer be finished.
 *
 * IT IS A RACE, and that is the finding's shape: the same six clicks with
 * 1500/800/1500 ms gaps leave the button alone (measured twice), and with
 * 1300/700/1300 ms they lose it (measured three times, one of them the first
 * iteration from a freshly entered level).  What is between the two is the
 * side panel's own scroll animation: UpdateSidePanelScroll (fpui3.c:475)
 * walks the object-list icons one step per tick and only at the END of the
 * run sets g_panel_state.f00 = 2 / f0c = 0x86 and calls
 * RemoveObjectListIcons(0xd2); InitGameInterface (bigscreens.c:941, the
 * resume path a screen change takes) writes exactly those two fields itself
 * at :1067-:1074 and calls the same RemoveObjectListIcons, then rebuilds the
 * theme icons with UpdateThemeIconsFromProfile (screens3.c:741).  Entering
 * MAP mode while the animation is still running puts those two writers on the
 * same state in the same frame.
 *
 * NOT the cause, ruled out by measurement: the profile's own theme bytes.
 * UpdateThemeIconsFromProfile hides theme i when CurProfile+0x30+i is 0, and
 * on this profile those four bytes read [1,0,0,0] -- LEGOLAND unlocked -- both
 * before and after the button vanishes.  So the icon is not being hidden by
 * the profile arm; something is losing the Icon itself.
 *
 * NOT reproduced by the pieces on their own (each measured on a fresh level):
 *   - map mode in and straight out, panel closed          button survives
 *   - map mode + a view jump, panel closed                button survives
 *   - panel open, map mode + jump, nothing armed          button survives
 *   - panel open, object armed, map mode + jump, 1500 ms  button survives
 *   - panel opened and MAP entered 300 ms into it         button survives
 */

P2.themeRow = async function (name) {
  await fetch(P2.sink, { method: 'POST',
                         body: (name || 'theme-row') + '.png\n' + llCrop(0, 380, 420, 40, 1) });
  return llFrameHash();
};

P2.themeButtonRace = async function (gap) {
  gap = gap || 1300;                       /* 1500 does NOT reproduce it */
  const log = [];
  log.push(['before', llFrameHash(), llPark().editState]);
  await llClick( 53, 394); await P2.sleep(gap);          /* LEGOLAND: panel starts scrolling */
  log.push(['panel', llPark().editState]);
  await llClick( 27,  90); await P2.sleep(gap / 2);      /* arm the first object in the panel */
  log.push(['armed', llPark().editState]);               /* 1 = an object is on the cursor */
  await llClick(278, 443); await P2.sleep(gap);          /* MAP mode (gameMode 1) */
  await llClick(420, 300); await P2.sleep(600);          /* pick a spot: a green box appears */
  await llClick(420, 300); await P2.sleep(800);          /* confirm it */
  await llClick(278, 443); await P2.sleep(1600);         /* back to the park */
  log.push(['after', llFrameHash(), llPark().editState]);
  return log;
};

/* The proof that it is permanent and that it ends the level: three more
   clicks on the theme button, each given almost two seconds.  editState stays
   0 every time and the panel never comes back. */
P2.themeButtonDead = async function () {
  const r = [];
  for (let i = 0; i < 3; i++) {
    await llClick(53, 394); await P2.sleep(1800);
    r.push([i, llFrameHash(), llPark().editState]);
  }
  await fetch(P2.sink, { method: 'POST',
                         body: 'theme-dead-panel.png\n' + llCrop(0, 30, 200, 400, 0.5) });
  return r;
};

/* The profile bytes UpdateThemeIconsFromProfile reads, so the innocent
   explanation can be ruled out on the spot.  [1,0,0,0] = LEGOLAND unlocked. */
P2.profileThemes = function () {
  const p = Module._ll_dbg_addr(4);                      /* CurProfile, 0x0080ffa0 */
  return [HEAPU8[p + 0x30], HEAPU8[p + 0x31], HEAPU8[p + 0x32], HEAPU8[p + 0x33]];
};

/* await P2.themeRow('before'); await P2.themeButtonRace(); await P2.themeRow('after');
   await P2.themeButtonDead(); P2.profileThemes(); */
