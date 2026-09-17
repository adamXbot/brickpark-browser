// PORT-P1 replay 04 — P1-5: roughly every seventh typed character is
// delivered TWICE into the game's cheat ring, so no cheat code can ever fire.
//
// ONE LINE: type ABCDEFGHIJKLMNOP into a running park and g_type_buf holds
// ABCDEFFGHIJKLMMNOP — F and M doubled, identically on every repeat.
//
// WHY IT MATTERS BEYOND CHEATS. input.c:361's UpdateControllerFromKeyboardData
// pushes each typed char into a 20-byte ring and then matches each cheat with
// a strnicmp against the ring's TAIL AT A FIXED OFFSET
// (`strnicmp(":PRAISEME", &g_type_buf[11], 9)`). One extra character shifts the
// word off that offset, so the comparison can never succeed. Every cheat is
// dead, and with them the only way into two screens:
//
//   :PRAISEME       sets g_instant_appraisal, which is the ONLY route to
//                   RunAppraisalScreen (appraisalscreen.c:381, the 8,085-
//                   instruction WIP body) in a free-play park — FreePlayTest.txt
//                   sets no APPRAISAL deadline, so goalstate.c:243's
//                   AppraisalDueTick never fires on its own.
//   :COLDHARDCASH   AddBricks(5000)
//   :HARDASNAILS    g_ride_wear = 0
//   :WELOVELEGOLAND EndLevel(1), the level-complete sequence
//   :THEME/:EGYPT/:INCA/:CASTLE/:WEST  SetTheme
//
// So this one defect is also why this lane could not exercise the appraisal
// screen. Fix it and that screen is one shift-key away.
//
// The ':' is not a key: GetTypedChar (uimisc.c:527) maps DIK 42 (LeftShift),
// 54 (RightShift) and 58 (CapsLock) to -10 and turns -10 into ':'. So a cheat
// is typed as Shift, then the letters.

(async function () {
  const W = (ms) => new Promise((r) => setTimeout(r, ms));
  const U = HEAPU8;

  // ---- find g_type_buf (x86 0x00668d94, 20 bytes) -----------------------
  // It is not in the LL_DBG_TABLE, so type a marker and search for it.
  await llType('QQXXZZQQ'); await W(600);
  let base = -1;
  for (let p = 0; p < U.length - 8; p++)
    if (U[p] === 81 && U[p + 1] === 81 && U[p + 2] === 88 &&
        U[p + 3] === 88 && U[p + 4] === 90) { base = p; break; }
  if (base < 0) return 'g_type_buf not found — is the park running?';
  // The marker lands at the ring's tail; the ring starts 20 bytes back from
  // the byte after the last char written.
  const ringStart = base - (20 - 9);         // QQXXZZ(Z)QQ is 9 long by then
  const show = (a) => Array.from(U.slice(a, a + 20))
    .map((c) => (c > 31 && c < 127) ? String.fromCharCode(c) : '.').join('');

  const log = [{ g_type_buf: ringStart, after_QQXXZZQQ: show(ringStart) }];

  // ---- the defect, three ways ------------------------------------------
  await llType('ABCDEFGHIJKLMNOP'); await W(600);
  log.push({ typed: 'ABCDEFGHIJKLMNOP', ring: show(ringStart) });
  // -> "MEABCDEFFGHIJKLMMNOP"   F doubled (6th), M doubled (13th)

  await llType('ABCDEFGHIJKLMNOP'); await W(600);
  log.push({ typed: 'ABCDEFGHIJKLMNOP again', ring: show(ringStart) });
  // -> "OPABCDEFFGHIJKLMMNOP"   the SAME two, so it is deterministic

  await llType('ZYXWVUTSRQPONM'); await W(600);
  log.push({ typed: 'ZYXWVUTSRQPONM', ring: show(ringStart) });
  // -> "MNOPZYXWWVUTSRQPPONM"   W (4th) and P (11th): seven apart again

  // ---- and the cheat it breaks -----------------------------------------
  await llKey('ShiftLeft'); await llType('PRAISEME'); await W(800);
  log.push({ typed: ':PRAISEME', ring: show(ringStart),
             fired: llPark().screenMode });
  // -> ring ends ":PRAISSEME" (S doubled) and nothing happens: the appraisal
  //    screen never opens. Poke the ring by hand to prove the game side is
  //    fine — write ":PRAISEME" into bytes [11..19] and the next typed char
  //    makes AppraisalDueTick pick it up.

  // ---- the keyboard state itself is correct ----------------------------
  const a = llAddrs();
  window.dispatchEvent(new KeyboardEvent('keydown',
    { code: 'KeyP', key: 'P', bubbles: true }));
  await W(500);
  const held = Array.from(U.slice(a.g_key_state, a.g_key_state + 256))
    .map((v, i) => [i, v]).filter(([, v]) => v);
  window.dispatchEvent(new KeyboardEvent('keyup',
    { code: 'KeyP', key: 'P', bubbles: true }));
  log.push({ g_key_state_while_P_held: held });
  // -> [[25, 128]] : exactly one DIK set, and to 0x80, which is what
  //    GetTypedChar's `cur & 0x80` wants. The shim's key BYTE is right; what
  //    doubles is the number of times the press is seen as a fresh edge.

  return log;
})();

// ---------------------------------------------------------------------------
// OWNER: PORT-B first (src/hostwin/dinput.c), then a game-side lane.
//
// GetTypedChar (uimisc.c:527, 0x00474130) emits a character only on a RISING
// edge: `cur = g_key_state[dik]; prev = g_typed_key_prev[i] & 0x80;
// g_typed_key_prev[i] = cur; if ((cur & 0x80) && !prev) result = ch;`
// A doubled character therefore means the same physical press produced two
// rising edges — g_key_state went 0x80, 0, 0x80 across three consecutive
// ProcessSystemEvents (input.c:303) calls while the key was never released.
//
// The prime suspect is the PRESS LATCH PORT-B6 added to dinput.c: it holds a
// press for at least one drain so a down+up inside one poll is not collapsed
// into "up". If the latch releases a frame early and the real key is still
// down, the next poll re-latches it and the edge fires a second time. That
// matches everything seen here: it needs a key held across several polls (this
// lane's llKey holds 150 ms, about five frames at the 28.85 ms floor), it is
// deterministic, and it is periodic rather than per-key.
//
// The game-side alternative, worth ruling out with `extern_sweep.py` /
// `gen/extents.md`, is g_typed_key_prev (59 bytes) being emitted with the
// wrong extent so some indices never remember the previous state. It fits the
// determinism less well — a lost index would repeat on EVERY frame the key is
// held, five times, not twice.
//
// Note for whoever fixes it: the same rising-edge helper is what the profile
// and save-game name editors use, and PORT-B7 measured those as correct — a
// doubled letter there is swallowed because the editor compares against what
// is already in the field. So this defect is invisible everywhere except the
// cheat ring, which is the one consumer that counts characters.
