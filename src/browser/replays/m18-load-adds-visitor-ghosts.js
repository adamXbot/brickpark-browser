/* PORT-M18 replay — P1-7, the visitor chain that grows on every load.
 *
 * Paste p1-00-prelude.js first, then this.  Served from build-wasm on
 * a port of your own, legoland.html?args=-nointro+WINDEBUG&awake=1, own tab,
 * virgin IDBFS:
 *     for (const d of await indexedDB.databases()) indexedDB.deleteDatabase(d.name);
 *     location.reload();
 *
 * WHAT IT SHOWS, AND WHAT THE ANSWER IS
 *
 * PORT-P1 filed this as "one save/load DOUBLES the visitor chain": 30 before
 * the save, 60 after.  Both halves of that sentence are wrong.
 *
 *   * It is not a doubling.  Every load adds exactly `g_visitor_limit` more
 *     blokes to whatever the save held — 30 -> 60 -> 90 — up to the pool
 *     ceiling `g_map->max_blokes`.  Measured here on two cycles of the same
 *     park, and on tutorial lesson 1, where max_blokes is 5 and the park
 *     therefore jams at 5 for the rest of the session.
 *   * It is not a port defect.  `LoadGame` (savegame.c:1033, x86 0x0047e980)
 *     restores the Bloke chain and never touches `g_visitor_count`
 *     (goalstate.c:178, x86 0x006661bc) — the counter the spawner gates on.
 *     A raw scan of LoadGame's 3,552 bytes in original/legoland.exe finds four
 *     references to g_people_head (0x0066b574) and one to g_bloke_base
 *     (0x0066b57c) and NOT ONE to 0x006661bc or to g_visitor_spawn_clock
 *     (0x006661c8).  `BeginParkLoad` (gameframe.c:419, 0x00458b20) runs
 *     `ClearBlokeList` (pathobj2.c:451, 0x00483090) first, whose last
 *     instruction is literally `mov dword ptr [0x6661bc], 0`, so the count is
 *     ZERO on entry to every load.  `g_visitor_limit` (0x0083291c) does come
 *     back, because it lives at offset 0x11c inside the 0x3f0-byte g_mapai
 *     blob (0x00832800) that savegame.c:1138 reads.  So the limit is restored
 *     and the count is not, and `SpawnVisitor` (simcore2.c:122, 0x0044ea50)
 *     duly tops the park up from zero on top of everything the save restored.
 *
 * THE ARITHMETIC, which every measurement below fits exactly:
 *
 *     chain after a load  =  min( blokes in the save + g_visitor_limit,
 *                                 g_map->max_blokes )
 *     g_visitor_count     =  chain - blokes in the save
 *
 * The steady state is stable, not drifting: when a ghost leaves, goalstate.c's
 * leave path decrements the count, the count drops below the limit, and
 * SpawnVisitor immediately replaces it.  So the surplus is permanent.
 *
 * MEASURED on be436d48+M18, wasm32 Release ILP32, 35.7 fps, 0 traps, dead null
 * in every run.  ~20 minutes end to end.
 */

window.M18 = {
  W: (ms) => new Promise(r => setTimeout(r, ms)),
  samples: [],

  /* The live Bloke chain, its Person3D kinds, and the two counters beside it.
     Bloke.next is +0x00 and Bloke.person is +0x04; Person3D.kind is +0x08
     (1 = visitor).  The cycle guard is the same one llPark() uses. */
  chain() {
    const a = llAddrs(), I = HEAP32;
    let p = I[a.g_people_head >> 2], n = 0, kinds = {}, ptrs = [], seen = {};
    while (p && n < 4096 && !seen[p]) {
      seen[p] = 1;
      const per = I[(p + 4) >> 2];
      const k = per ? I[(per + 8) >> 2] : 'noperson';
      kinds[k] = 1 + (kinds[k] || 0);
      ptrs.push(p); p = I[p >> 2]; n++;
    }
    /* pool slot indices: g_bloke_base + 172*i (blokeai.c:12).  llAddrs()
       carries g_bloke_base from PORT-M18 on; before that, min(ptr) is slot 0
       whenever slot 0 is live, which it always is in a fresh park. */
    const a2 = llAddrs();
    const base = a2.g_bloke_base ? HEAP32[a2.g_bloke_base >> 2]
                                 : (ptrs.length ? Math.min.apply(null, ptrs) : 0);
    const g = HEAP32[a.g_map >> 2];
    return { n: n, kinds: kinds,
             count: HEAP32[a.g_visitor_count >> 2],
             limit: HEAP32[a.g_visitor_limit >> 2],
             maxBlokes: g ? HEAPU16[(g + 0x1a) >> 1] : 0,
             slots: ptrs.map(v => (v - base) / 172).sort((x, y) => x - y) };
  },
  sample(tag) {
    const c = this.chain();
    this.samples.push({ tag: tag, simFrame: llPark().simFrame, n: c.n,
                        count: c.count, limit: c.limit });
  },
  startSampler(ms) { this.timer = setInterval(() => this.sample('auto'), ms || 500); },
  stopSampler() { clearInterval(this.timer); },

  /* Cold load -> a running free-play park.  P1.start() ticks all four picker
     columns, which is 120 clicks and two minutes; ONE tick is all Accept
     needs (fpui2.c:918 sets flag 0x400 on it, sysstubs.c:480 clears it on the
     first charge), and this lane builds nothing. */
  async toPark() {
    const W = this.W, P = window.P1;
    await llMove(320, 240);
    await llClick(260, 188);            /* profile slot 1 -> the name editor */
    await llType('adam');
    await llClick(505, 345);            /* Accept -> the TITLE screen        */
    const c = P.cur();
    HEAPU8[c + 0x39] = 1;                                  /* P1-1 free play */
    for (let i = 0; i < 4; i++) HEAPU8[c + 0x30 + i] = 1;  /* P1-3 themes    */
    for (const i of P.IDS) HEAPU8[c + 0x46 + i] = 1;       /* P1-2 picker    */
    HEAP32[P.A().g_cur_screen >> 2] = -1;
    await W(1600);
    await llClick(215, 70);  await W(2500);   /* Free Play -> the picker */
    await llClick(100, 120); await W(1500);   /* one tick, column 1 row 1 */
    await llClick(535, 395); await W(10000);  /* Accept -> StartFreePlayPark */
    return this.chain();                      /* {n:30 count:30 limit:30} eventually */
  },

  /* The green tick that commits a save-slot name.  The slot's own tick moves
     with the row, so it is found by colour rather than hard-coded: it is the
     only saturated-green blob on the left two thirds of the canvas. */
  greenTick() {
    const d = Module.canvas.getContext('2d').getImageData(0, 100, 640, 280).data;
    const cl = [];
    for (let y = 0; y < 280; y++) for (let x = 0; x < 640; x++) {
      const i = (y * 640 + x) * 4;
      if (d[i + 1] > 140 && d[i] < 110 && d[i + 2] < 110) {
        let hit = null;
        for (const c of cl)
          if (Math.abs(c.cx - x) < 25 && Math.abs(c.cy - (y + 100)) < 25) { hit = c; break; }
        if (hit) { hit.n++; hit.sx += x; hit.sy += y + 100;
                   hit.cx = hit.sx / hit.n; hit.cy = hit.sy / hit.n; }
        else cl.push({ cx: x, cy: y + 100, sx: x, sy: y + 100, n: 1 });
      }
    }
    return cl.filter(c => c.n > 40 && c.n < 400 && c.cx < 500)
             .map(c => ({ x: Math.round(c.cx), y: Math.round(c.cy), n: c.n }));
  },

  /* Save through the game's own UI, from the park.  The saved-game screen
     puts one panel per slot at (0x51, 0x6f + 38*slot) (bigscreens.c:534), so
     slot 1 is y=149, slot 2 y=187, slot 3 y=225 -- click a few pixels into
     the row, NOT at the row's own top edge (y=231 misses slot 3 entirely). */
  async saveToSlot(slot, name) {
    const W = this.W, P = window.P1;
    await llClick(...P.TOOL.options); await W(2500);   /* -> screen 5        */
    await llClick(516, 222);          await W(2400);   /* Save  (0x1df,0xbd) */
    await llClick(221, 0x6f + 38 * slot + 6); await W(1500);
    await llType(name);               await W(900);
    const t = this.greenTick();
    if (!t.length) return { ok: false, why: 'no name editor opened' };
    await llClick(t[0].x, t[0].y);    await W(4000);
    await W(8000);                                      /* the IDBFS flush   */
    return { ok: true, files: FS.readdir('/gamedata/profiles') };
  },

  /* Load slot `slot` through the IN-GAME options screen (no page reload).
     The options screen's Load icon is at (0x168,0x13a) (screens2.c:21). */
  async loadInSession(slot) {
    const W = this.W, P = window.P1;
    await llClick(...P.TOOL.options); await W(2500);
    await llClick(395, 330);          await W(2500);   /* Load -> screen 4 */
    await llClick(221, 0x6f + 38 * slot + 6); await W(1400);
    await llClick(564, 362);                           /* the LOAD Accept  */
  },

  /* Load slot `slot` from the TITLE screen after a full page reload.
     Load_on_Title is at (0x19,0x118) (screens2.c:14). */
  async loadFromTitle(slot) {
    const W = this.W;
    await llMove(320, 240);
    await llClick(260, 188); await W(900);    /* profile adam            */
    await llClick(505, 345); await W(2200);   /* Accept -> TITLE         */
    await llClick(95, 350);  await W(2400);   /* Load_on_Title -> screen 4 */
    await llClick(221, 0x6f + 38 * slot + 6); await W(1500);
    await llClick(564, 362);
  }
};

/* =========================================================================
 * 1. THE CONTROL ARM — free play, three loads, two save/load cycles
 * =========================================================================
 *
 *   await M18.toPark();                       // ~45 s, then wait for 30/30/30
 *   M18.chain()
 *     -> { n: 30, kinds: {1:30}, count: 30, limit: 30, maxBlokes: 200,
 *          slots: [0..29] }                   // ONE pool slot per bloke
 *   await M18.saveToSlot(1, 'fp30');
 *     -> 1save1.sav  755,359 bytes,  1save1.sh 272
 *
 *   // full page reload, re-paste the prelude and this file, then:
 *   M18.startSampler(500); await M18.loadFromTitle(1);
 *
 *   simFrame   n  count  limit
 *     1148     0    0      0      the park is torn down (BeginParkLoad)
 *     1165    30    0     30      <-- LoadGame has finished.  THIRTY BLOKES
 *     1183    30    0     30          RESTORED AND A COUNT OF ZERO.
 *     1201    31    1     30      SpawnVisitor starts topping up from zero,
 *     1236    32    2     30      one per 30 sim frames (simcore2.c:124)
 *      ...
 *     2557    60   30     30      <-- it stops exactly when count == limit
 *     2646    60   30     30          and stays there.  kinds {1:60}.
 *
 *   // an IN-SESSION load, no reload, from that same 60-bloke park:
 *   M18.samples = []; M18.startSampler(500); await M18.loadInSession(1);
 *
 *     4632    60   30     30      before
 *     4649    31    1     30      <-- same story: BeginParkLoad zeroed the
 *      ...                            count, LoadGame restored 30
 *     5277    52   22     30      climbing back to 60
 *
 *   // a THIRD load, unpatched: 30 -> 60 again.  Then save the 60-bloke park:
 *   await M18.saveToSlot(2, 'fp60');
 *     -> 1save2.sav  764,305 bytes.  That is 8,946 bytes MORE than the
 *        30-bloke save.  BLK4 writes 4 bytes of pool index plus the
 *        0x124-byte g_bs record per bloke (savegame.c:751), so 30 more blokes
 *        is 30 * 296 = 8,880 of that; the remaining 66 bytes are the rider
 *        entries and block bookkeeping the extra visitors drag with them.
 *        The file proves the ghosts are written back as real visitors, which
 *        is why the next load stacks another thirty on top of them.
 *
 *   // reload, then load slot 2 — the second cycle:
 *      737     0    0      0
 *      819    60    0     30      <-- SIXTY restored, count still zero
 *      885    62    2     30
 *      ...
 *     2428    90   30     30      <-- NINETY.  kinds {1:90}.  Not a doubling:
 *     2478    90   30     30          +30 per round trip, i.e. +visitorLimit.
 *
 * =========================================================================
 * 2. THE TREATED ARM — the same build, the same save, one word written
 * =========================================================================
 *
 * The fix is to give g_visitor_count the value the save never stored.  Do it
 * by hand, at the moment LoadGame returns, and the surplus never appears:
 *
 *   (async () => {
 *     const a = llAddrs(); M18.samples = [];
 *     await M18.loadInSession(1);
 *     const t0 = performance.now();
 *     while (performance.now() - t0 < 40000) {         // arm on the load
 *       const c = M18.chain();
 *       if (c.n >= 20 && c.n - c.count >= 10) {
 *         HEAP32[a.g_visitor_count >> 2] = c.n;        // <-- THE ONE WORD
 *         break;
 *       }
 *       await M18.W(16);
 *     }
 *     M18.startSampler(1000);
 *   })();
 *
 *   armed: { simFrame: 11546, chain: 31, countWas: 1, wrote: 31 }
 *
 *   simFrame   n  count  limit
 *    11582    31   31     30
 *    11618    31   31     30
 *      ...    (twenty consecutive 1 s samples, ~700 sim frames)
 *    12260    31   31     30      <-- FLAT.  No growth, no drift, 0 traps.
 *
 * (31 rather than 30 because one spawn slipped through in the 16 ms before the
 * arm fired; the point is the chain stops dead where the count is put.)
 *
 * Write a count ABOVE the limit and the park drains instead: setting it to 60
 * on a 60-bloke chain took the park from 60 down through 50 to 39 over ~4,000
 * sim frames, chain and count falling together, because no spawn can happen
 * while count >= limit and every departure decrements both.  That is the third
 * confirmation that g_visitor_count, and nothing else, is the governor.
 *
 * =========================================================================
 * 3. THE TUTORIAL — where the same bug jams the park for good
 * =========================================================================
 *
 * Tutorial lesson 1 (title -> "Start LEGOLAND Game" (275,375) -> Lesson 1
 * (200,145) -> briefing (577,419) / (455,445) / (577,419)):
 *
 *   M18.chain()  -> { n: 3, count: 3, limit: 3, maxBlokes: 5 }
 *   await M18.saveToSlot(3, 'tut1');      // 1save3.sav 154,375 bytes
 *   // reload, then:
 *   M18.startSampler(500); await M18.loadFromTitle(3);
 *
 *    simFrame   n  count  limit
 *      737      0    0      0
 *      754      3    0      3     <-- three restored, count zero
 *      790      4    1      3
 *      826      5    2      3     <-- and it STOPS HERE, not at 6.
 *     1415      5    2      3         Six hundred sim frames later: still 5/2.
 *
 * `NewBloke` (blokeai.c:363, 0x00482ef0) scans `i < g_map->max_blokes` for a
 * slot with flags62 bit 0 clear, and lesson 1's map header says max_blokes 5.
 * All five are taken, so MakeBloke returns 0 for every spawn from here on.
 * The park is left permanently BELOW its own visitor limit (count 2 < limit 3)
 * with a full pool, and no visitor can ever enter lesson 1 again in that
 * session.  On free play (MAXCAPACITY 200 in Scripts\FreePlayTest.txt) the
 * ceiling is far away, which is why the free-play symptom is a crowd and the
 * tutorial symptom is a freeze.
 *
 * =========================================================================
 * 4. WHAT IS NOT WRONG
 * =========================================================================
 *
 *   * The chain is not corrupt and holds no duplicates: 30 blokes in pool
 *     slots 0..29, 60 in 0..59, every entry a Person3D of kind 1 with a live
 *     back pointer.  llPark().peopleTruncated is false throughout.
 *   * `StartFreePlayPark` (uimisc2.c:404) is NOT on the load path, so P1's
 *     suspicion that it and the save both append is not it: GameFrame
 *     (gameframe.c:497) calls LoadGame *instead of* StartPark when
 *     g_game_load_pending is set.
 *   * The save writes the chain correctly and once (savegame.c:741-751 counts
 *     it, then walks it), and LoadGame clears the chain before restoring it
 *     (savegame.c:1169).  Neither end duplicates anything.
 *   * `g_visitor_limit` is restored correctly — it is inside the g_mapai blob.
 *     It is only `g_visitor_count` (and `g_visitor_spawn_clock`, harmlessly)
 *     that the save format has no field for.
 */
