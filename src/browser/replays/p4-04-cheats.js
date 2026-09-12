/* PORT-P4 replay: THE CHEAT RING, on the first tree that carries the memmove.
 *
 *   await fetch('replays/p4-00-prelude.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/p4-04-cheats.js').then(r=>r.text()).then(eval);
 *   P4.fast();
 *   await P4.toLesson(2);          // any running park will do (g_game_mode 3)
 *   await P4C.all();
 *
 * PORT-B12 proved P1-5 was `memcpy(g_type_buf, g_type_buf + 1, 19)` at
 * `LEGOLAND/input.c:368` -- a memcpy on OVERLAPPING memory that VC6's forward
 * byte copy forgives and LLVM's inlined constant-size copy does not,
 * duplicating 3 of the 19 bytes on every shift.  Every cheat is a `strnicmp`
 * of the ring's TAIL at a FIXED offset, so one duplicated byte kills all of
 * them, silently.  B12 could only demonstrate the fix on a throwaway build;
 * the integrator has since put the `memmove` in a `LEGOLAND_PORTABLE` arm.
 * This is the readback on the honest tree.
 *
 * MEASURED, 40+ characters typed, no doubling anywhere:
 *
 *   ring after ":PRAISEME"      "...PRAISEME:PRAISEME"   tail[11] = ":PRAISEME"
 *     -> g_instant_appraisal 0 -> 1
 *     -> the APPRAISAL SCREEN opens: the REPORT notepad with the inspector
 *        minifigure and "Objectives: Congratulations you have built a thriving
 *        Park!", the sim frozen (g_sim_frame stuck at 16629 while the page
 *        kept presenting 35.7 fps of identical frames), and one click at
 *        (577,419) closes it with the sim resuming (16629 -> 16688).
 *   ring after ":EGYPT"          "ISEME:PRAISEME:EGYPT"  tail[14] = ":EGYPT"
 *     -> SetTheme(1); g_imt_state is 1, so sysmisc2.c:134 takes the
 *        SetThemeInTransition arm and the mailbox argument goes 0 -> 1
 *   ring after ":CASTLE"         "RAISEME:EGYPT:CASTLE"  tail[13] = ":CASTLE"
 *     -> SetTheme(3); mailbox argument 1 -> 3
 *   ring after ":COLDHARDCASH"   ":CASTLE:COLDHARDCASH"  tail[7] = ":COLDHARDCASH"
 *     -> AddBricks(5000): money 2420 -> 7460 (the extra 40 is park income
 *        over the interval)
 *
 * THE COLON IS NOT A KEY.  `GetTypedChar` (uimisc.c:527) maps DIK 0x2a / 0x36 /
 * 0x3a -- LeftShift, RightShift, CapsLock -- to -10 and turns -10 into ':'.
 * `llType(':PRAISEME')` therefore pushes only PRAISEME and the cheat never
 * fires, which looks exactly like the doubling bug and is not: the ring reads
 * back "............PRAISEME" with byte[11] still 0xF2.  Press Shift, then
 * type the word.  (This lane lost a measurement to it; PORT-B12 got it right.)
 */

window.P4C = {
  /* Press Shift (which the game types as ':'), then the word. */
  cheat: async function (word) {
    var before = llRing();
    await llKey('ShiftLeft', ''); await llType(word); await P4.W(1500);
    var after = llRing();
    return { word: ':' + word, ringBefore: before.ring, ringAfter: after.ring,
             tail: after.tail, effectsBefore: before.effects,
             effectsAfter: after.effects,
             doubled: P4C.doubled(after.ring, word) };
  },

  /* A doubling shows up as the typed word appearing with a repeated letter in
   * the ring's tail.  The ring is 20 bytes, so the last |word|+1 characters
   * must be exactly ":" + word. */
  doubled: function (ring, word) {
    return ring.slice(20 - word.length - 1) !== ':' + word;
  },

  all: async function () {
    var L = [];
    L.push({ tag: 'start', ring: llRing().ring, effects: llRing().effects,
             gameMode: llPark().gameMode });

    /* 1. :PRAISEME -> g_instant_appraisal, then the appraisal screen */
    var p = await this.cheat('PRAISEME');
    L.push({ tag: ':PRAISEME', r: p,
             fired: p.effectsAfter.instantAppraisal === 1 &&
                    p.effectsBefore.instantAppraisal === 0 });

    /* The screen takes a few seconds: AppraisalDueTick pauses the sim and
     * RunAppraisalScreen draws over the park, so g_screen_mode does NOT
     * change -- g_sim_frame freezing while llStats().frames keeps rising is
     * the signal, not the screen mode. */
    var f0 = llPark().simFrame, i;
    for (i = 0; i < 40 && llPark().simFrame !== f0; i++) await P4.W(500);
    f0 = llPark().simFrame;
    for (i = 0; i < 60; i++) { await P4.W(500); if (llPark().simFrame === f0) break; f0 = llPark().simFrame; }
    L.push({ tag: 'appraisal', simFrameFrozenAt: llPark().simFrame,
             hash: llFrameHash(), stats: llStats(), park: llPark() });
    await llClick(577, 419); await P4.W(1500);            /* GoBack */
    L.push({ tag: 'appraisal closed', simFrame: llPark().simFrame,
             hash: llFrameHash(), note: P4.note('sim resumed') });

    /* 2. the theme cheats -- with MIDI stubbed, SetTheme's mailbox IS the
     *    effect (sysmisc2.c:134: g_imt_cmd / g_imt_cmd_arg / SetEvent). */
    L.push({ tag: ':EGYPT', r: await this.cheat('EGYPT') });
    L.push({ tag: ':CASTLE', r: await this.cheat('CASTLE') });

    /* 3. :COLDHARDCASH -- AddBricks(5000), the one cheat whose effect is a
     *    number on the HUD. */
    var m0 = llPark().money;
    var c = await this.cheat('COLDHARDCASH');
    L.push({ tag: ':COLDHARDCASH', r: c, money: [m0, llPark().money],
             delta: llPark().money - m0, note: P4.note('coldhardcash') });

    /* 4. the control: llType with a literal colon, which the game cannot
     *    receive.  The ring ends in PRAISEME with no colon and nothing fires. */
    var before = llRing();
    await llType(':PRAISEME'); await P4.W(1200);
    L.push({ tag: 'control: llType(":PRAISEME")', ringBefore: before.ring,
             ringAfter: llRing().ring, tail11: llRing().tail.at11,
             note: 'the colon is Shift, not a key -- uimisc.c:527' });
    return L;
  }
};
'P4C installed';
