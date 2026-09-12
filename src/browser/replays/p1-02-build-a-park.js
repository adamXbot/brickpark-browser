// PORT-P1 replay 02 — build one of every kind the free-play menus offer, then
// open every panel. Paste p1-00-prelude.js first, then this.
//
// What it proves, end to end, from a cold load:
//   * the four theme object lists fill from the picker's selection and price
//     every class (money.c prints the ODF price, not the picker's budget cost);
//   * the edit cursor answers legal/illegal per square (llSel().editCursor
//     .status is positive when CursorIsValid says yes; error 3 = blocked,
//     error 10 = no room for the footprint + clearance ring);
//   * placing charges nothing in free play — uimisc2.c:404's sub_457870(0)
//     sets g_brick_lock, so GetBrickCount never moves and RenderMoneyBar
//     (money.c:100) returns before it draws. The blank money bar is CORRECT;
//   * every ride, shop, cart and scenery piece renders (the Earth Slide's
//     globe, the Shark Cafe, the hot-dog cart with its steam);
//   * the MAP button draws the whole park with its viewport rectangle;
//   * OPTIONS -> SAVE writes a 759,397-byte .sav + a 272-byte .sh into IDBFS,
//     and the park comes back byte-for-byte after a full page reload.
//
// Frame hashes are from build cc6e926d+P1, 2026-09-12. Only the still screens
// repeat: the park animates (ride ornaments, cart steam, the entrance banner)
// and any frame with a help bubble on it depends on where the cursor stopped.

(async function () {
  const P = window.P1, W = P.W, log = [];
  const note = (s) => log.push({ step: s, hash: llFrameHash(), park: llPark(),
                                 dead: llStats().dead, traps: llStats().traps.length });

  log.push(...await P.start());            // cold load -> the park

  // ---- build ------------------------------------------------------------
  // Row y for the object list: 90, 157, 222, 288, 330; the list's own down
  // arrow is at (63, 350). The theme button TOGGLES, so P.openTab checks.
  const build = async (tab, row, tag) => {
    P.unhide && P.unhide();                // see P1-4: the LEGOLAND tab hides
    await P.openTab(tab);
    await llClick(60, row); await W(700);
    const r = await P.place();
    note('build ' + tag + ' -> ' + JSON.stringify(r));
    return r;
  };

  await build('adv', 90,  'EARTH SLIDE RIDE (40)');
  await build('adv', 157, 'BALLOONZ (50)');
  await build('legoland', 288, 'FOODCART (20)');
  await build('legoland', 330, 'FOODCART (20)');
  await build('west', 90,  'SALOON (30)');
  await build('castle', 90, 'CASTLE BBQ');

  // Deeper in the LEGOLAND list: the shops, the restaurant, the power station.
  P.unhide && P.unhide();
  await P.openTab('legoland');
  for (let i = 0; i < 4; i++) await llClick(63, 350);      // page the list down
  await llClick(60, 157); await W(700); note('pick LEGO MEDIA SHOP (25)');
  note('placed ' + JSON.stringify(await P.place()));
  P.unhide && P.unhide();
  await P.openTab('legoland');
  await llClick(60, 222); await W(700);
  note('placed LEGO SHOP ' + JSON.stringify(await P.place()));

  // ---- let the park run -------------------------------------------------
  // FreePlayTest.txt's [INIT] gives MAXCAPACITY 200 / MINCAPACITY 30,
  // WORKERS 1,1, Feature AutoRepair 1 and EntranceFee 0, so visitors arrive on
  // their own up to g_visitor_limit = 30 and nothing has to be paid for.
  await W(20000);
  note('20 s of park time');               // llPark().numVisitors climbs 5 -> 30

  // ---- every panel ------------------------------------------------------
  P.unhide && P.unhide();
  await llClick(...P.TOOL.query);  await W(700);  note('QUERY mode');
  await llClick(...P.TOOL.path);   await W(700);  note('PATH mode');
  await llClick(...P.TOOL.eraser); await W(700);  note('ERASE mode');
  await llClick(...P.TOOL.map);    await W(2000); note('MAP mode');
  //   the whole park, its path network, the hedge border and a white viewport
  //   rectangle. gameMode goes 3 -> 1 while the map is up.
  await llClick(...P.TOOL.map);    await W(1500); note('back from MAP');
  await llClick(...P.TOOL.options); await W(2000); note('OPTIONS (screen 5)');
  //   three volume rows, Accept bottom-left, Exit top-right, Load and Save.

  // ---- save through the game's own UI -----------------------------------
  await llClick(516, 222); await W(2000);  note('SAVE GAME (screen 4)');
  await llClick(221, 155); await W(1200);  // slot 1 -> the name editor
  await llType('freeplay1');
  await llClick(99, 155);  await W(1000);  // the slot's own tick
  await llClick(287, 127); await W(2500);  // the popup's green OK
  note('slot 1 = freeplay1');
  log.push({ step: 'files', files: FS.readdir('/gamedata/profiles'),
             sav: FS.stat('/gamedata/profiles/1save1.sav').size,
             sh:  FS.stat('/gamedata/profiles/1save1.sh').size });
  // 1save1.sav 759,397 bytes and 1save1.sh 272 bytes, both in IDBFS.

  return log;
})();

// ---------------------------------------------------------------------------
// The round trip. Reload the page completely, paste p1-00-prelude.js, then:
//
//   await llMove(320, 240);
//   await llClick(260, 188);           // profile adam
//   await llClick(505, 345);           // Accept -> the TITLE screen
//   await llClick( 95, 350);           // Load_on_Title (0x19,0x118) -> screen 4
//   await llClick(221, 155);           // select slot 1 (SaveSlotInput only
//                                      // lights it; there is no double-click)
//   await llClick(564, 362);           // the LOAD screen's own Accept
//   await P1.W(12000);
//
// The park comes back complete: every ride, shop, cart and path, all four
// theme tabs labelled, llStats().dead null and traps []. 32.9 fps on arrival.
//
// ONE THING IS WRONG ACROSS THE LOAD — finding P1-7. Before the save the live
// Bloke chain (blokeai.c:125, `next` at +0x00) is 30 long with
// llPark().numVisitors 30 and visitorLimit 30. After the load it settles at
// SIXTY, every one of them Person3D kind 1 (a visitor), while numVisitors and
// visitorLimit both still read 30. Count it with:
//
//   const a = llAddrs(), I = HEAP32;
//   let p = I[a.g_people_head >> 2], n = 0, kinds = {};
//   while (p && n < 400) { const per = I[(p + 4) >> 2];
//     kinds[per ? I[(per + 8) >> 2] : 'none'] = 1 + (kinds[...] || 0);
//     p = I[p >> 2]; n++; }
//   // n === 60, kinds === {1: 60}
