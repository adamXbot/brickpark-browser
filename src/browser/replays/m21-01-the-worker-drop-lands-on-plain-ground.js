/* PORT-M21 replay: P5-1 is CLOSED in the C, and this is the A/B that proves it.
 *
 *   await fetch('replays/p4-00-prelude.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/p4-01-lesson2.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/p5-00-prelude.js').then(r=>r.text()).then(eval);
 *   await fetch('replays/m21-01-the-worker-drop-lands-on-plain-ground.js')
 *         .then(r=>r.text()).then(eval);
 *   P5.pin(); P4.fast();
 *   await P4.toLesson(2, 'm21');        // first run returns {reload:true}
 *   await M21.run();                    // the whole measurement
 *
 * ------------------------------------------------------------------------
 * WHAT CHANGED
 * ------------------------------------------------------------------------
 * `CheckWorkerOnMouseStatus` (workers2.c, 0x00470620) tested a field this file
 * called `y2` at `CursorState +0x0c` = 0x00813a4c.  The original reads
 * 0x00813a48:
 *
 *     0x004706dd: mov eax, dword ptr [0x813a48]    ; g_cursor.point.y
 *     0x004706e2: cmp eax, 0x20
 *     0x004706e5: jl  0x470895                     ; fail
 *     0x004706eb: cmp eax, 0x174
 *     0x004706f0: jge 0x470895                     ; fail
 *
 * 0x00813a4c is bighelp.c's `g_input.mouse_a.MASK`, a constant 1, so the test
 * failed on every pixel.  The C now reads `g_cursor.point.y` (+0x08), which is
 * what the original reads.  The struct field is kept, under its real name, to
 * hold the layout out to `buttons` at +0x10.
 *
 * PORT-P5's `p5-04` A/B faked the fix from the outside by writing 0x21 into the
 * mask.  This one is the real thing: the same build, one word of C, and arm B
 * is the tree with that word put back (`ninja -C build-wasm
 * legoland_browser` after one `sed`).
 *
 * ------------------------------------------------------------------------
 * MEASURED -- `M21.run()`, twice, the only difference between the runs being
 * that one word of C.  Tutorial lesson 2, the hedge pen on screen, a Gardener
 * in hand, dropped on plain ground at cell (46,13) = screen (166,246), cursor Y
 * 246 (inside the viewport band 0x20..0x174), hit 0x109, and the word at
 * CursorState +0x0c reading 1 in BOTH arms:
 *
 *   probe                      arm A (fixed)         arm B (the word put back)
 *   hover (46,13), in hand     (46,13) lock 1        (46,13) lock 1
 *   CLICK -- the drop          lock 1 -> 0           lock stays 1
 *   hover (46,13)              (46,13)               (46,13)
 *   hover (48,13)              (46,13)  STAYS        (48,13)  FOLLOWS
 *   hover (44,12)              (46,15)  WALKING      (44,12)  FOLLOWS
 *   cursor to (200,100)        (46,23)               (38,3)   FOLLOWS
 *   cursor to (350,200)        (43,26)               (49,4)   FOLLOWS
 *   cursor to (500,300)        (41,22)               (59,6)   FOLLOWS
 *
 * (The first hover after the drop is the drop cell itself, so `follows` reads
 * true in both arms for that one probe and for that one only.)
 *
 * In arm A the worker is on the ground: it ignores the cursor from the second
 * probe on and then walks under its own AI, right out of the hedge pen.  In arm
 * B its 24.8 world position at +0x68/+0x6c equals the hovered cell EVERY time,
 * including 150 px jumps across the park -- `SetWorkersPositionAtMouse` from
 * the tail on every tick, i.e. glued to the cursor, for ever.
 * `llWorkers().gardeners` is 4 throughout both arms, 0 traps in both, 35.2-35.7
 * fps in both.
 *
 * ONE TRAP FOR THE NEXT LANE.  `llWorkers().carrying` (`g_worker_on_mouse`,
 * 0x007fdff0) is NOT cleared by a successful drop -- PORT-P5 recorded this and
 * it is why `P5.drop()` returns `ok:false` on a drop that worked.  The oracle
 * is `g_drag_lock` (0 = nothing in hand) plus the worker's own world position.
 * `M21.carriedCell()` reads that position through the stale pointer, which is
 * exactly what makes the table above decisive: the same pointer, the same
 * worker, and in one arm it tracks the cursor while in the other it does not.
 */

window.M21 = {
  /* The cell the worker that WAS picked up is standing on, read through
   * g_worker_on_mouse -- stale after a drop, which is the point. */
  carriedCell: function () {
    var I = HEAP32, b = llWorkers().carrying;
    return b ? [I[(b + 0x68) >> 2] >> 8, I[(b + 0x6c) >> 2] >> 8] : null;
  },

  run: async function () {
    P5.guard();
    var L = [];

    /* The park as PORT-P5 left it: dismiss the "new object" pop-up, bring the
     * hedge pen on screen. */
    await llClick(543, 234); await P4.W(1200);
    await P4.scroll('e'); await P4.fit();
    L.push({ pen: P4.cellTo(48, 5), workers: llWorkers(), cursor: P5.cursor() });

    var c = await P5.carry('0x307', [280, 370, 165, 250]);
    L.push({ pickUp: { ok: c.ok, at: c.at, bloke: c.bloke, dragLock: c.dragLock } });
    if (!c.ok) { L.push({ abort: 'no Gardener in hand' }); return L; }

    /* Plain, unowned, on-screen ground outside the pen. */
    var tgt = null, cand = [[46, 13], [48, 13], [44, 12], [50, 13], [44, 8]];
    for (var i = 0; i < cand.length; i++)
      if (P4.free(cand[i][0], cand[i][1]) && P4.onScreen(cand[i][0], cand[i][1])) {
        tgt = cand[i]; break;
      }
    L.push({ target: tgt });
    if (!tgt) return L;
    var q = P4.cellTo(tgt[0], tgt[1]);

    await llMove(q[0], q[1], 250);
    L.push({ beforeDrop: { hovered: tgt, carriedAt: this.carriedCell(),
                           dragLock: P5.cursor().dragLock,
                           cursorY: P5.cursor().pointY,
                           maskAt0x0c: P5.cursor().y2_at_0x0c,
                           hit: llSel().hit.typeHex } });

    var r = await P5.drop(tgt[0], tgt[1]);
    /* dragLock 1 -> 0 IS the drop.  `r.ok` is false either way: see the note
     * about g_worker_on_mouse above. */
    L.push({ DROP: { dragLock: r.dragLock, placed: r.dragLock === 0,
                     hitAtDrop: r.hitAtDrop, staleCarrying: r.carryingAfter } });

    /* Does it still follow the cursor?  This is the whole defect. */
    for (var k = 0; k < cand.length && k < 3; k++) {
      var p = P4.cellTo(cand[k][0], cand[k][1]);
      await llMove(p[0], p[1], 300); await P4.W(700);
      L.push({ hovered: cand[k], workerAt: this.carriedCell(),
               follows: JSON.stringify(this.carriedCell()) ===
                        JSON.stringify(cand[k]),
               dragLock: P5.cursor().dragLock });
    }

    /* On the fixed tree it walks away on its own. */
    for (var w = 0; w < 3; w++) {
      await llMove(200 + w * 150, 100 + w * 100, 400); await P4.W(1800);
      L.push({ walk: w, gardeners: P5.workerCells().gardeners,
               workerAt: this.carriedCell(), dragLock: P5.cursor().dragLock });
    }

    L.push({ gardeners: llWorkers().gardeners, traps: llStats().traps.length,
             fps: llStats().fps });
    return L;
  },

  /* How arm B was built, for anyone repeating this.  One word of C, the browser
   * target only, and the page reloaded:
   *
   *   sed -i '' 's/g_cursor\.point\.y < 0x20 || g_cursor\.point\.y >= 0x174/\
   *      g_cursor.mouse_a_mask < 0x20 || g_cursor.mouse_a_mask >= 0x174/' \
   *      LEGOLAND/workers2.c
   *   ninja -C build-wasm legoland_browser
   *
   * and `git checkout LEGOLAND/workers2.c` to get back.  The VC6 gate does not
   * move between the two arms -- that is the other half of this lane's point,
   * and why tools/relocs.py grew a WIPRELOC pass. */
  armB: 'workers2.c: g_cursor.point.y -> g_cursor.mouse_a_mask, rebuild browser'
};
'M21 installed';
