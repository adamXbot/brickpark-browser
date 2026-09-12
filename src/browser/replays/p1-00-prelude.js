// PORT-P1 replay 00 — the prelude every other p1-* replay begins with.
//
// Paste this first, then the replay you want. It installs the three helpers
// the replays use and defines P1.start(), which takes a COLD LOAD of
//   legoland.html?args=-nointro+WINDEBUG&beat=1000
// all the way to a running FREE PLAY park.
//
// Wipe IDBFS first or the walk diverges at the title screen:
//     for (const d of await indexedDB.databases())
//       indexedDB.deleteDatabase(d.name);
//     location.reload();
//
// THE THREE POKES, AND WHY THEY ARE NOT CHEATS. Free play is locked behind
// progress a fresh profile does not have, and this lane could not play twelve
// levels to earn it. Each poke writes exactly the byte the game itself writes
// when the player earns it; no code is changed and nothing else is touched.
// All three live in g_cur_profile (x86 0x0080ffa0), whose base the page
// exports as Module._ll_dbg_addr(4):
//
//   +0x39  level_done[5]        P1-1  the title screen's Free Play button is
//                                     flag-0x400'd until this is non-zero.
//                                     HaveCurrentProfile() (frontend2.c:507)
//                                     is 0x0048fc30 = `mov cl,[0x80ffd9]`,
//                                     which is this byte, not "is a profile
//                                     selected". bigscreens.c:303 writes it.
//   +0x30..33  the four theme unlock bytes   P1-3  UpdateThemeIconsFromProfile
//                                     (screens3.c:741) hides the Wild West /
//                                     Castle / Adventurers tabs in the park
//                                     while these are 0. SetThemeIconEnabled
//                                     (eventgoalprim.c:262) writes them.
//   +0x46..+0x10d  g_profile_unlocked[200]   P1-2  InitFreePlayLists
//                                     (fpui2.c:818) skips every id whose byte
//                                     is 0, so all four columns of the picker
//                                     are empty. UnlockFreePlayEntry
//                                     (frontend2.c:278) writes them.
//
// g_cur_screen = -1 is how InitScreens (mapscreen.c:407) is made to rebuild a
// screen it believes is already current, so the icons are built with the byte
// already set. The pokes must land AFTER the profile is committed: the profile
// load clears them.

window.P1 = {
  W: (ms) => new Promise((r) => setTimeout(r, ms)),
  A: () => window.llAddrs(),
  cur: () => Module._ll_dbg_addr(4),          // g_cur_profile base

  // The 20 free-play class ids this lane unlocked: three garden pieces, three
  // food carts, two shops, a restaurant, a power station, four rides, a saloon,
  // a castle BBQ, a shark cafe, a temple slide and the driving school with its
  // roads. Ids and free-play costs come from g_fp_table (x86 0x004bdeb8, 0x86
  // rows of {u8 id, char* name, int cost, int}); the whole table is listed in
  // docs/lanes/scope-port-p1.md §2. Unlock all 200 instead for the full picker.
  IDS: [0, 3, 5, 52, 54, 56, 57, 63, 64, 65, 77, 78, 81, 84, 60, 62, 73, 88, 89, 90],

  // The four theme tabs and the five tool buttons, in GAME pixels.
  TAB: { legoland: [53, 394], west: [140, 394], castle: [228, 394], adv: [315, 394] },
  TOOL: { path: [30, 443], query: [120, 443], eraser: [205, 443],
          map: [285, 443], options: [360, 443] },

  // Is the object-list side panel up? The sample point is grass when it is not.
  panelOpen() {
    const d = Module.canvas.getContext('2d').getImageData(5, 200, 1, 1).data;
    return !(d[0] < 60 && d[1] > 140 && d[2] < 130);
  },

  // The free-play budget gauge (Bar_Rides at 0xd1,0x156), as a percentage.
  // It EASES six pixels a frame, so read it only after ~1.5 s of settling.
  barFill() {
    const d = Module.canvas.getContext('2d').getImageData(205, 345, 240, 8).data;
    let g = 0, n = 0;
    for (let i = 0; i < d.length; i += 4) { n++; if (d[i + 1] > d[i + 2] + 30) g++; }
    return +(100 * g / n).toFixed(1);
  },

  // Cold load -> a running free-play park. Returns the step/hash table.
  async start(ids) {
    const log = [], W = this.W;
    const push = async (s, h) => log.push({ step: s, hash: h || llFrameHash(),
                                            park: llPark() });
    await push('cold load (PLAYER DETAILS)', await llMove(320, 240));
    await push('slot 1 -> the name editor', await llClick(260, 188));
    await push("type 'adam'",               await llType('adam'));
    await push('Accept -> TITLE screen',    await llClick(505, 345));

    const c = this.cur();
    HEAPU8[c + 0x39] = 1;                                  // P1-1
    for (let i = 0; i < 4; i++) HEAPU8[c + 0x30 + i] = 1;  // P1-3
    for (const i of (ids || this.IDS)) HEAPU8[c + 0x46 + i] = 1;   // P1-2
    HEAP32[this.A().g_cur_screen >> 2] = -1;
    await W(1600);
    await push('title screen, Free Play lit');

    await push('Free Play -> the picker', await llClick(215, 70));
    await W(2500);

    // Tick everything in every column. Three rows are visible at a time; the
    // down arrow is the triangle under each column.
    for (const x of [100, 245, 390, 535]) {
      for (let page = 0; page < 5; page++) {
        for (const y of [120, 185, 255]) await llClick(x, y);
        for (let i = 0; i < 3; i++) await llClick(x, 277);
      }
    }
    await W(1500);
    await push('picker ticked, gauge ' + this.barFill() + '%');

    await llClick(535, 395);                 // Accept -> StartFreePlayPark
    await W(9000);                           // the level load takes ~7-9 s
    await push('THE FREE PLAY PARK');
    return log;
  },

  // Open a theme's object list (the button toggles, so this checks first).
  async openTab(which) {
    const [x, y] = this.TAB[which];
    if (!this.panelOpen()) { await llClick(x, y); await this.W(900); }
    else { await llClick(x, y); await this.W(900);
           if (!this.panelOpen()) { await llClick(x, y); await this.W(900); } }
    return this.panelOpen();
  },

  // Hover a grid of map points and click the first one the edit cursor calls
  // legal. llSel().editCursor.status is POSITIVE when CursorIsValid says yes.
  async place(spots) {
    if (!spots) { spots = [];
      for (let x = 160; x <= 610; x += 45) for (let y = 60; y <= 350; y += 45)
        spots.push([x, y]); }
    for (const [x, y] of spots) {
      await llMove(x, y, 70);
      if (llSel().editCursor.valid) {
        await llClick(x, y); await this.W(1800);
        return { at: [x, y], map: llSel().input.mapRef, ok: true };
      }
    }
    return { ok: false };
  }
};
'P1 prelude installed';
