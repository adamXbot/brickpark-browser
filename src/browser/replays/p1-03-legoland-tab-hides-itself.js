// PORT-P1 replay 03 — P1-4, the headline defect.
//
// ONE LINE: in a free-play park, almost any toolbar or menu click sets flag
// 0x400 on g_theme_icon[0] — the LEGOLAND theme tab — so it stops being drawn
// (a blank blue plate where the word LEGOLAND was) and can no longer be
// clicked. The other three tabs are never touched. The index is always 0,
// whichever theme is open, so this is a write to the wrong icon and not
// "hide the active tab".
//
// Consequence for a player: the LEGOLAND build menu — the one with the shops,
// the food carts, the restaurants, the power stations, the Space Tower, the
// Copters and the Miniland models in it — becomes unreachable a few seconds
// into the park, permanently, with no way back short of a save and reload.
//
// Paste p1-00-prelude.js, then p1-02's build section or just P1.start(), then
// this file. It finds the four theme icons in the heap by their layout,
// clears the bit, and shows what puts it back.

(async function () {
  const P = window.P1, W = P.W, I32 = HEAP32, I16 = HEAP16;

  // ---- find the four theme icons ---------------------------------------
  // Icon (screens2.c:39): +0x0c short x, +0x0e short y, +0x14 ushort group,
  // +0x2c input, +0x34 flags, +0x3c help_id. The four theme tabs are
  // fpui4.c:240's "four theme tabs on row 379", group 0x9a, and
  // bigscreens.c:994-1018 gives them help ids 0x5e..0x61 in the order
  // legoland, western, castle, adventurers.
  const found = {};
  for (let p = 0; p < HEAPU8.length - 0x40; p += 4) {
    if (I16[(p + 0x0e) >> 1] !== 379) continue;
    if ((I16[(p + 0x14) >> 1] & 0xffff) !== 0x9a) continue;
    const help = I32[(p + 0x3c) >> 2];
    if (help === 0x5e) found.legoland = p;
    else if (help === 0x5f) found.west = p;
    else if (help === 0x60) found.castle = p;
    else if (help === 0x61) found.adv = p;
  }
  const fl = () => { const o = {}; for (const k in found)
    o[k] = '0x' + (I32[(found[k] + 0x34) >> 2] >>> 0).toString(16); return o; };
  const clear = () => { for (const k in found) I32[(found[k] + 0x34) >> 2] &= ~0x400; };
  window.P1_THEME_ICONS = found;
  P.unhide = clear;                       // p1-02 uses this

  const log = [{ icons: found, atStart: fl() }];
  //   e.g. {legoland: 13709560, west: 13708384, castle: 13708200, adv: 13709064}
  //   A healthy tab is 0x6012 (0x6002 from InitGameInterface | 0x10). The
  //   broken one is 0x6412.

  // ---- the trigger, one action at a time -------------------------------
  const probe = async (what, fn) => {
    clear(); await W(300);
    await fn(); await W(800);
    log.push({ action: what, legoland: fl().legoland, others: fl() });
  };

  await probe('open the ADVENTURERS tab',      () => llClick(...P.TAB.adv));
  await probe('click a class row in it',       () => llClick(60, 90));
  await probe('the PATH tool',                 () => llClick(...P.TOOL.path));
  await probe('the QUERY tool',                () => llClick(...P.TOOL.query));
  await probe('the ERASER tool',               () => llClick(...P.TOOL.eraser));
  await probe('open the WILD WEST tab',        () => llClick(...P.TAB.west));

  // Every one of those comes back legoland: "0x6412", others all "0x6012".

  // ---- it is not a timer -----------------------------------------------
  clear();
  const idle = [];
  for (let i = 0; i < 10; i++) { await W(3000);
    idle.push([llPark().simFrame, fl().legoland]); }
  log.push({ idle30s: idle });
  // Thirty seconds of park time with the mouse parked: the bit stays CLEAR.
  // Only a UI click puts it back. The level script cannot be the cause either
  // — FreePlayTest.txt (Scripts\, 728 bytes) is [INIT] only and contains no
  // THEMEICON/SetThemeIcon primitive; and the three profile theme bytes at
  // g_cur_profile+0x30 read [1,1,1,1] the whole time, so
  // UpdateThemeIconsFromProfile (screens3.c:741) would CLEAR the bit, not set it.

  // ---- and the tab really is dead, not just unpainted -------------------
  await llClick(...P.TOOL.query); await W(800);       // re-set the bit
  const before = llFrameHash(), opened = [];
  for (let i = 0; i < 6; i++) { await llClick(...P.TAB.legoland);
                                opened.push(P.panelOpen()); }
  log.push({ sixClicksOnTheDeadTab: opened, hashUnchanged: llFrameHash() === before });
  // Six clicks, panelOpen() false every time. 0x400 stops the icon being
  // drawn, and front-end focus is claimed by the SPRITE that painted the pixel
  // under the cursor (the BlitCtx owner, uimisc3.c:759's RenderIndicatorIcon
  // shape), so an unpainted icon can never be focussed and never gets input.
  // Clearing the bit by hand makes it work again immediately.

  return log;
})();

// ---------------------------------------------------------------------------
// WHAT THIS LANE COULD NOT SETTLE, and where the next one should look.
//
// The only two functions in the recovered sources that set 0x400 on a theme
// icon are UpdateThemeIconsFromProfile (screens3.c:741) and
// UpdateThemeIconsFromFlags (screens3.c:726), and both run only from the tail
// of InitGameInterface (bigscreens.c:1075-1077), which runs once per park.
// SetThemeIconEnabled (eventgoalprim.c:262) is the third writer and is reached
// only from the SetThemeIcon script primitive, which this level never uses.
// So the write that fires here is not one the sources say should fire, which
// puts it in this wave's known classes:
//
//   * PORT-A — a split record or an object whose extent is wrong, so a store
//     that belongs to some other global lands on g_theme_icon[0]->flags.
//     g_theme_icon is x86 0x007fdd70 (bigscreens.c:833 frames the same address
//     as `ThemeIcons g_theme_icons`, a struct of four Icon*; screens3.c:219
//     frames it as `Icon* g_theme_icon[4]` — two framings of one object).
//     Worth checking gen/extents.md and gen/rawwords.md for 0x007fdd70, and
//     note that bigscreens.c:1001 also keeps `g_western_icon` (0x00668e3c) as
//     a SECOND name for the same icon pointer.
//   * PORT-M — a by-value or K&R declaration on one of the icon-flag helpers:
//     run `$PY tools/bvstruct_sweep.py` and
//     `$PY tools/port_m10_bvstruct_sweep.py` over bigscreens.c, screens3.c,
//     eventgoalprim.c, fpui4.c and popupmisc.c.
//
// A JS write watch is the fastest way in: this file's `found.legoland` plus a
// 60 ms poll on (addr + 0x34) names the click that does it, which is how the
// finding was narrowed to a single action here.
