// PORT-B12 replay 01 — P1-5, the doubled typed character, is NOT the keyboard.
//
// ONE LINE: `UpdateControllerFromKeyboardData` (input.c:368) shifts the cheat
// ring with `memcpy(g_type_buf, g_type_buf + 1, 19)` — a memcpy whose source
// and destination OVERLAP, which is undefined behaviour. VC6's x86 memcpy
// copies forward and happens to produce the intended shift; LLVM inlines the
// constant-size copy as wide load/stores that assume no overlap and DUPLICATES
// three bytes of every shift. The keyboard, the press latch and the game's own
// rising-edge detector are all correct.
//
// Owner: PORT-M (LEGOLAND/input.c is read-only for a PORT-B lane).
// Fix: `memmove` in a `#ifdef LEGOLAND_PORTABLE` arm — see §2 of
// docs/lanes/scope-port-b12.md. Proved on a throwaway build: the ring then
// reads back EXACTLY and `:PRAISEME` opens RunAppraisalScreen.
//
// Paste p1-00-prelude.js, run P1.start() and let the park come up, then this.

(async function () {
  const W = (ms) => new Promise((r) => setTimeout(r, ms));
  const U = HEAPU8, I = HEAP32, L = globalThis.LL_DEBUG;
  const show = (a) => Array.from(U.slice(a, a + 20))
    .map((c) => (c > 31 && c < 127) ? String.fromCharCode(c) : '.').join('');

  // ---- 1. locate g_type_buf (x86 0x00668d94, 20 bytes) -------------------
  // Type a marker and search for it. NOTE the off-by-one in P1's replay 04:
  // the ring base is the marker's start minus (20 - marker length), and the
  // marker itself is corrupted by the very defect this replay is about, so
  // search for the first FOUR characters only.
  await llType('QQXXZZQQ'); await W(600);
  let base = -1;
  for (let p = 0; p < U.length - 8; p++)
    if (U[p] === 81 && U[p + 1] === 81 && U[p + 2] === 88 && U[p + 3] === 88) {
      base = p; break;
    }
  if (base < 0) return 'g_type_buf not found — is the park running?';
  const ring = base - 12;
  const log = [{ g_type_buf: ring, after_QQXXZZQQ: show(ring) }];
  //  broken build -> ".......ADDAMQQXXZZZQ"   D doubled, one Q lost
  //  fixed  build -> "........ADAMQQXXZZQQ"   exactly the 12 characters typed

  // ---- 2. the browser layer is innocent: 16 events for 16 characters -----
  // LL_DEBUG is ll_canvas.js's LL, exported for exactly this. Every DOM key
  // event the page's listeners accept is pushed here and nowhere else.
  const rec = [];
  if (!L.__b12) {
    L.__b12 = L.push;
    L.push = function (t, a, b, c) {
      if (t === 1 || t === 2) rec.push((t === 1 ? 'D' : 'U') + a.toString(16));
      return L.__b12.call(L, t, a, b, c);
    };
  }
  rec.length = 0;
  await llType('ABCDEFGHIJKLMNOP'); await W(700);
  log.push({
    typed: 'ABCDEFGHIJKLMNOP',
    ring: show(ring),               // broken: ".QQABCDEFFGHIJKLMMNO"  (F, M doubled, P lost)
                                    // fixed : "ZZQQABCDEFGHIJKLMNOP"
    keydowns: rec.filter((s) => s[0] === 'D').length,   // 16
    keyups:   rec.filter((s) => s[0] === 'U').length,   // 16
    dikOrder: rec.filter((s) => s[0] === 'D').join(' ')
    //  D1e D30 D2e D20 D12 D21 D22 D23 D17 D24 D25 D26 D32 D31 D18 D19
    //  = A B C D E F G H I J K L M N O P, one event each, all correct
  });

  // ---- 3. and so is the shim: ONE rising edge per key in g_key_state -----
  // Sample the GAME's own 256-byte DirectInput array at 1 ms while typing and
  // log every transition. g_key_state is what GetTypedChar reads; a doubled
  // character would need 0x80 -> 0 -> 0x80 within one press.
  const KS = llAddrs().g_key_state;
  const diks = [0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23,
                0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19];
  const last = {}, trans = []; let stop = false;
  diks.forEach((d) => last[d] = U[KS + d]);
  (async function () {
    while (!stop) {
      for (const d of diks) {
        const v = U[KS + d];
        if (v !== last[d]) { trans.push(d.toString(16) + (v ? '+' : '-')); last[d] = v; }
      }
      await W(1);
    }
  })();
  await llType('ABCDEFGHIJKLMNOP'); await W(700); stop = true;
  log.push({
    keyStateTransitions: trans.length,          // 32 = 16 rises + 16 falls
    rises: trans.filter((s) => s.endsWith('+')).length,   // 16, never 17
    sequence: trans.join(' '),                  // strictly alternating +/-
    ring: show(ring)                            // and the ring STILL doubles
  });

  // ---- 4. the same 59-entry prev array the game edges on -----------------
  // g_typed_key_prev (x86 0x00668de4) sits 0x60 bytes past g_type_buf in the
  // portable layout. One byte per KEY MAP INDEX, not per DIK: F is index 9,
  // M is 16, A is 4 (g_key_map, 2 bytes per entry, 59 entries).
  const TP = ring + 0x60;
  U[0] = U[0];                                   // (no-op; keeps the linter quiet)
  window.dispatchEvent(new KeyboardEvent('keydown', { code: 'KeyM', key: 'M', bubbles: true }));
  await W(220);
  const held = Array.from(U.slice(TP, TP + 59)).map((v, i) => [i, v]).filter(([, v]) => v);
  window.dispatchEvent(new KeyboardEvent('keyup', { code: 'KeyM', key: 'M', bubbles: true }));
  log.push({ g_typed_key_prev: TP, whileMheld: held });   // [[16, 128]] — one entry, correct

  // ---- 5. THE PROOF: drive the ring shift with a known pattern -----------
  // Write 20 distinct bytes over g_type_buf, type ONE character, read back.
  // This takes the keyboard out of the picture entirely: all that runs between
  // the two reads is `memcpy(g_type_buf, g_type_buf + 1, 19); g_type_buf[19] = ch`.
  const pat = '0123456789abcdefghij';
  for (let i = 0; i < 20; i++) U[ring + i] = pat.charCodeAt(i);
  const before = show(ring);
  await llKey('KeyZ', 'Z'); await W(600);
  log.push({
    shiftProbe_before: before,     // "0123456789abcdefghij"
    shiftProbe_after:  show(ring), // broken: "123456789abcdefghijZ" with 3 bytes duplicated
    shiftProbe_expected: '123456789abcdefghijZ'
  });

  // ---- 6. and the cheat it kills / unlocks -------------------------------
  // ':' is not a key: GetTypedChar (uimisc.c:527) maps DIK 0x2a/0x36/0x3a
  // (LeftShift / RightShift / CapsLock) to -10 and turns -10 into ':'.
  await llKey('ShiftLeft', ''); await llType('PRAISEME'); await W(1400);
  const p1 = llPark(); await W(1200); const p2 = llPark();
  log.push({
    typed: ':PRAISEME',
    ring: show(ring),
    //  broken: "...:PRAISSEME"  — the S doubles, the word is off the offset
    //          `strnicmp(":PRAISEME", &g_type_buf[11], 9)` compares at, and
    //          NO cheat in the game can ever fire.
    //  fixed : "FGHIJKLMNOP:PRAISEME" — ":PRAISEME" lands exactly at [11..19]
    simFrozen: p1.simFrame === p2.simFrame
    //  TRUE on the fixed build: AppraisalDueTick (goalstate.c:243) saw
    //  g_instant_appraisal, called PauseGameTimer() and entered
    //  RunAppraisalScreen — the REPORT notepad with the inspector minifigure.
    //  Close it with the GoBack icon at game (525, 370) (appraisal.c:517
    //  puts it at 0x1fb,0x161) and the sim resumes.
  });
  return log;
})();

// ---------------------------------------------------------------------------
// WHY THIS IS NOT THE PRESS LATCH (PORT-B6, dinput.c)
//
// P1's prime suspect was the latch releasing a frame early and re-latching a
// still-held key. Step 3 rules it out at the only place it could show: the
// game's own g_key_state goes 0 -> 0x80 -> 0 exactly once per press, sampled
// at 1 ms, and the 16 transitions alternate perfectly. `latch_press` is the
// ONLY writer that sets `down`, and it runs only from an EV_KEYDOWN record —
// of which step 2 counts exactly 16.
//
// WHY IT LOOKED PERIODIC ("every ~7th character")
//
// It is not periodic in time or in keystrokes; it is periodic in the RING.
// The miscompiled copy duplicates a fixed set of byte positions of the 20-byte
// window (this build: ring offsets 7, 14 and 15), so a doubled character
// re-appears every time a character walks past one of those offsets — which,
// for a ring that shifts one byte per character, is every seventh character.
// Same reason the doubled pair is always seven apart and the phase differs per
// run. A cheat word is 5-15 characters long, so it is essentially certain to
// straddle one of them: `:PRAISEME` doubles its S on every single attempt.
//
// WHY NOBODY SAW IT ANYWHERE ELSE
//
// The ring is the only 20-byte overlapping shift in the game that a human can
// drive. The profile and save-game NAME editors use GetInputChar and append to
// a buffer; they never shift one. That is why PORT-B7 measured typing as
// correct and P1 found the ring broken on the same build.
