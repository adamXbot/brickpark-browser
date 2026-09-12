/* PORT-P3 replay: TUTORIAL LESSON 4 — "Mechanics and repairing rides".
 *
 * Paste into the page console of
 *   http://localhost:<port>/legoland.html?args=-nointro+WINDEBUG&beat=1000
 * then `await p3Lesson4()`.  Every step returns a row; the `money` column is
 * the assertion (`llFrameHash` only says the frame changed).
 *
 * SCRIPT (Legoland.res `ObjList4.txt`, map FOUR.MAP 54x84, CURRENCY 450):
 *   obj 0 [REWARD only]  DEGRADE the four rides to 24, FLASHBUTTON QUERY
 *   obj 1  SELECTMODE QUERY + SELECTTHEME LEGOLAND     -> GIVE "COPTERS"
 *   obj 2  NEED "COPTERS", 1                           -> LOOKAT 45, 5
 *   obj 3  NEEDMECHANICS 1     (click the Mechanic's Hut)   <-- BLOCKED, P3-1
 *   obj 4  FIXRIDES 0, 20      (drop a mechanic on the Space Tower)
 *   obj 5..8  NEEDMECHANICS 2..5
 *   obj 9  FIXRIDES 0, 100 ; ENDLEVEL
 *
 * PRECONDITION — the tutorial select screen only offers a lesson that the
 * profile has reached (screens3.c InitTutorialScreen: a marker icon exists
 * only for `g_level_map->level - 1` or a level whose `g_level_done[i]` is 1,
 * and bigscreens.c InitProgressScreen sets level = g_last_level + 1).  Rather
 * than play lessons 1..3 again, mark them done in the profile RECORD, which is
 * the raw 0x110-byte `Profile` struct (profiles.c) on disk: the 15 bytes at
 * +0x34 ARE g_level_done[0..14].
 *
 *   // once, with a profile already created in slot 1:
 *   await p3UnlockLessons(5);   // then reload the page
 */

window.p3UnlockLessons = async function (n) {
  var p = '/gamedata/profiles/Profile1.txt';
  var b = FS.readFile(p);                    // 272 bytes == 0x110
  for (var i = 0; i < (n || 5); i++) b[0x34 + i] = 1;
  FS.writeFile(p, b);
  await new Promise(function (res, rej) { FS.syncfs(false, function (e) { e ? rej(e) : res(); }); });
  return Array.from(b.slice(0x34, 0x43));
};

/* A hidden Browser pane throttles setTimeout to 1 Hz and the game (which
 * yields through emscripten_sleep, not rAF) drops to 0.3 fps.  A quiet
 * oscillator makes the tab "audible", which exempts it: 34 fps again. */
window.p3KeepAwake = async function () {
  if (window.__p3osc) return 'already';
  var ctx = new (window.AudioContext || window.webkitAudioContext)();
  if (ctx.state !== 'running') await ctx.resume();
  var o = ctx.createOscillator(), g = ctx.createGain();
  g.gain.value = 0.0008; o.frequency.value = 60;
  o.connect(g); g.connect(ctx.destination); o.start();
  window.__p3osc = o; window.__p3ctx = ctx;
  return ctx.state;
};

window.p3Lesson4 = async function () {
  var log = [];
  var S = function (ms) { return new Promise(function (r) { setTimeout(r, ms); }); };
  function note(tag) {
    var p = llPark(), s = llStats();
    var e = { tag: tag, h: llFrameHash(), money: p.money, people: p.people,
              lim: p.visitorLimit, gm: p.gameMode, sm: p.screenMode,
              fps: s.fps, dead: s.dead, traps: s.traps.length };
    log.push(e); return e;
  }
  document.querySelector('canvas').scrollIntoView({ block: 'center' });

  /* ---- front end ------------------------------------------------------- */
  await llMove(320, 240);
  await llClick(260, 188); await S(400);   note('profile slot 1');
  await llClick(505, 345); await S(900);   note('OK -> TITLE screen');
  /* screens2.c InitTitleScreen: New_on_Title is the icon at (0xca,0x138) and
   * the sprite's CENTRE is ~(252,362) -- the corner does not take the click. */
  await llClick(252, 362); await S(900);   note('New -> SELECT TUTORIAL LEVEL');
  /* screens3.c: the five markers are at x=30, y=130,154,178,202,226; one
   * click selects (level = marker + 1), a second within 500 ms accepts. */
  await llClick(41, 213);  await S(600);   note('marker: Lesson 4 lit');
  await llMove(320, 440);  await S(300);
  await llClick(577, 419); await S(2500);  note('Accept -> BRIEFING  money=450');
  await llClick(577, 419); await S(6000);  note('Thumbs Up -> THE PARK');

  /* ---- objective 1: QUERY mode, then the LEGOLAND menu ----------------- */
  /* bigscreens.c InitGameInterface: g_if_icon_pos[4..8] = PATH, QUERY, ERASER,
   * MAP, OPTIONS -- game x 54, 128, 204, 282, 358 at y 442. */
  await llClick(128, 442); await S(600);   note('QUERY button (g_edit_changed = 0)');
  await llClick(140, 170); await S(900);   note('query a ride -> the readout panel');
  await llClick(53, 394);  await S(1500);  note('LEGOLAND menu -> obj 1 done, GIVE COPTERS');
  await llClick(548, 244); await S(1200);  note('close "You have a new object"');

  /* ---- objective 2: build the Copters Ride (60) ------------------------ */
  await llClick(60, 340);  await S(900);   note('pick COPTERS in the side panel');
  /* Only two squares in the whole viewport take it: llSel().editCursor.status
   * must be POSITIVE (CursorIsValid, 0x0045f4b0). */
  await llMove(200, 60);   await S(300);
  await llClick(200, 60);  await S(2000);  note('BUILD -> money 460->400, LOOKAT 45,5');

  /* ---- objective 3: NEEDMECHANICS 1 ------------------------------------ */
  /* The Mechanic's Hut is at map cell (46,6) (FOUR.MAP).  It CANNOT be
   * brought on screen in this build -- see P3-1 in docs/lanes/scope-port-p3.md.
   * Hold the cursor in the right margin (MouseScrollMap, pathtile2.c) and the
   * scroll converges on g_scroll_x = 2*g_scroll_y - (view_w<<8) exactly, which
   * is ClampScrollToMap with g_view_left == 0.  The east-most reachable
   * column is cx-cy = 31; the hut is at cx-cy = 40. */
  for (var i = 0; i < 60; i++) { await llMove(634 + (i % 2), 180 + (i % 3)); await S(30); }
  await llMove(320, 240); await S(200);
  var a = llAddrs();
  var scroll = [HEAP32[a.g_scroll_x >> 2], HEAP32[a.g_scroll_y >> 2]];
  var vw = HEAPU16[(HEAP32[a.g_map >> 2] + 16) >> 1] << 8;   /* MapHdr +0x10 */
  log.push({ tag: 'east clamp', scroll: scroll, view_w_fixed: vw,
             predicted_x_with_hl0: 2 * scroll[1] - vw,
             matches: (2 * scroll[1] - vw) === scroll[0],
             hutCell: [46, 6], hutOwner: llCellAt(46, 6).owner });
  note('BLOCKED at objective 3');
  return log;
};
