/* PORT-P5 replay: P5-1 — a carried worker can never be put down on plain
 * ground, because `CheckWorkerOnMouseStatus` reads the cursor Y from the wrong
 * address.  This is the A/B, on ONE build, with ONE word changed.
 *
 *   await fetch('replays/p4-00-prelude.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/p4-01-lesson2.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/p5-00-prelude.js').then(r=>r.text()).then(eval);
 *   P5.pin(); P4.fast();
 *   await P4.toLesson(2);
 *   await P5Drop.run();
 *
 * ------------------------------------------------------------------------
 * THE DEFECT
 * ------------------------------------------------------------------------
 * workers2.c frames the input block at 0x00813a40 as
 *
 *     typedef struct CursorState {
 *         int           flags;     // +0x00  0x813a40
 *         Pos           point;     // +0x04  0x813a44   (Pos is 8 bytes)
 *         int           y2;        // +0x0c  0x813a4c
 *         unsigned char buttons;   // +0x10  0x813a50
 *         ...
 *
 * and `CheckWorkerOnMouseStatus` (workers2.c:1235, 0x00470620) tests
 *
 *     if (g_cursor.y2 < 0x20 || g_cursor.y2 >= 0x174)
 *         goto fail;
 *
 * The original reads 0x00813a48, not 0x00813a4c:
 *
 *     0x004706dd: mov eax, dword ptr [0x813a48]     ; g_input.point.y
 *     0x004706e2: cmp eax, 0x20
 *     0x004706e5: jl  0x470895                      ; fail
 *     0x004706eb: cmp eax, 0x174
 *     0x004706f0: jge 0x470895                      ; fail
 *     ...
 *     0x00470701: mov eax, dword ptr [0x813a44]     ; g_input.point.x -- and
 *     0x00470709: cmp eax, edx                      ; THIS one we get right
 *
 * 0x00813a48 is `point.y`, the cursor's screen Y, and the test is the map
 * viewport's vertical band (32..371).  0x00813a4c is bighelp.c's
 * `g_input.mouse_a.MASK` — the Controller bit the left button is bound to,
 * written ONCE by SetupControllers and never again.  It reads **1** for the
 * whole session, `1 < 0x20` is always true, the body always takes `goto fail`,
 * which sets `g_drag_lock = 1`, and the tail then calls
 * `SetWorkersPositionAtMouse()` — so the worker stays stuck to the cursor.
 *
 * Two consequences worth stating plainly:
 *   - the VC6 build has it too.  This is a transcription error in a WIP body
 *     (`CheckWorkerOnMouseStatus` is 55.4 %, 82/184 strict), not a wasm
 *     artefact, and fixing it should MOVE the match, not just the behaviour.
 *   - `relocs.py` cannot catch it.  The relocation gate only checks functions
 *     that are MATCHED; this one is WIP and is skipped outright.
 *
 * ------------------------------------------------------------------------
 * THE A/B
 * ------------------------------------------------------------------------
 * One build, one park, the same click twice.  Arm B writes **0x21** into
 * `mouse_a.mask` and nothing else.  0x21 still contains bit 0 — the left
 * button — so the events the game receives are identical and the click is
 * still detected (`mouse_a.state` reaches 2, "released this tick", in both
 * arms); the only thing that changes is that the value the broken test reads
 * is now inside [0x20, 0x174).
 *
 * MEASURED, on tutorial lesson 2, dropping a Gardener on plain ground at cell
 * (46,13) with the cursor Y at 246 (well inside the real band):
 *
 *   arm A, mask = 1      dragLock stays 1,  the Gardener is NOT put down
 *   arm B, mask = 0x21   dragLock 1 -> 0,   the Gardener lands on (46,13)
 *                        and then WALKS AWAY on its own: (46,13) -> (46,18)
 *                        -> (47,24) over the next six seconds, out of the
 *                        hedge pen, and starts servicing flower orders.
 *
 * The mask is put back to 1 at the end.  Nothing else is written, and no game
 * code is changed.
 */

window.P5Drop = {
  run: async function () {
    var L = [], a = llAddrs(), I = HEAP32;
    var MASK = (a.g_input + 0x0c) >> 2;      /* 0x00813a4c -- mouse_a.mask */

    /* Get the park into the state the test needs: the hedge pen on screen and
     * a Gardener in hand. */
    await llClick(543, 234); await P4.W(1200);
    await P4.scroll('e'); await P4.fit();
    L.push({ pen: P4.cellTo(48, 5), cursor: P5.cursor() });

    var c = await P5.carry('0x307', [280, 370, 165, 250]);
    L.push({ pickUp: c });
    if (!c.ok) { L.push({ abort: 'no Gardener in hand' }); return L; }

    /* A cell of plain, unglued, unowned ground outside the pen (x45..52,
     * y1..10), on screen. */
    var tgt = null, cand = [[46, 13], [48, 13], [44, 12], [50, 13], [44, 8]];
    for (var i = 0; i < cand.length; i++)
      if (P4.free(cand[i][0], cand[i][1]) && P4.onScreen(cand[i][0], cand[i][1])) {
        tgt = cand[i]; break;
      }
    L.push({ target: tgt });
    if (!tgt) return L;

    var q = P4.cellTo(tgt[0], tgt[1]);

    /* ---- ARM A: the tree as it stands --------------------------------- */
    await llMove(q[0], q[1], 250);
    L.push({ armA_hover: { hit: llSel().hit.typeHex,
                           cursorY: P5.cursor().pointY,
                           valueTheTestReads: P5.cursor().y2_at_0x0c } });
    var rA = await P5.drop(tgt[0], tgt[1]);
    L.push({ ARM_A_untouched: { dropped: rA.ok, hitAtDrop: rA.hitAtDrop,
                                dragLock: rA.dragLock,
                                carryingAfter: rA.carryingAfter } });

    /* ---- ARM B: mouse_a.mask = 0x21, and nothing else ----------------- */
    I[MASK] = 0x21;
    await llMove(q[0], q[1], 300);
    L.push({ armB_hover: { mask: I[MASK], hit: llSel().hit.typeHex,
                           mouseAState: P5.cursor().mouseA } });
    var rB = await P5.drop(tgt[0], tgt[1]);
    L.push({ ARM_B_mask_0x21: { dropped: rB.ok, hitAtDrop: rB.hitAtDrop,
                                dragLock: rB.dragLock,
                                gardenerCells: rB.cells.gardeners } });

    /* The dropped Gardener should now be a free agent: watch it walk. */
    for (var k = 0; k < 3; k++) {
      await llMove(200 + k * 150, 100 + k * 100, 400); await P4.W(1800);
      L.push({ walk: k, gardeners: P5.workerCells().gardeners,
               dragLock: P5.cursor().dragLock });
    }

    I[MASK] = 1;                              /* put it back */
    L.push({ restored: I[MASK], traps: llStats().traps.length });
    return L;
  },

  /* The control a fixing lane will want: the byte range of
   * `CheckWorkerOnMouseStatus` in the shipped binary is 0x00470620..0x004708b8,
   * and the two absolute addresses in it are
   *     0x00813a48   at 0x004706dd   (the cursor Y test -- what we get wrong)
   *     0x00813a44   at 0x00470701   (the cursor X test -- what we get right)
   * Neither 0x00813a4c nor any other word of the mouse_a pair appears in the
   * function at all. */
  theFix: '/* workers2.c: `y2` at CursorState +0x0c is a phantom; the two tests\n' +
          ' * at workers2.c:1235 belong on `g_cursor.point.y` (+0x08, 0x813a48).\n' +
          ' * Owner: a matching lane -- the body is WIP and the address is part\n' +
          ' * of why. */'
};
'P5Drop installed';
