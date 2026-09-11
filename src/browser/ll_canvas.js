// LEGOLAND portable build -- the browser side of the host shim
// (scopes PORT-B, PORT-B2).
//
// Linked with --js-library. Four jobs:
//   1. open a canvas at the mode ddraw.c's SetDisplayMode settled on;
//   2. convert the game's 16-bpp RGB565 primary surface to RGBA ImageData and
//      put it on that canvas;
//   3. translate DOM keyboard/mouse events into the records user32.c's
//      ll_host_drain_events expects, WITHOUT calling into wasm;
//   4. surface what a tester needs to see: the frame count, the last
//      MessageBoxA and its auto-answer, and any TRAP line.
//
// (3) is load-bearing. The main loop is ASYNCIFY (docs/lanes/scope-port-b.md
// §1): while a yield is in flight the whole C stack is unwound into a side
// buffer, and calling a wasm export then would re-enter a half-unwound program.
// So every handler here does nothing but push onto a plain JS array; the wasm
// side drains it at its own safe points.
//
// HEADLESS / NODE SAFETY (PORT-B2). legoland_hostwin is linked into
// legoland_tests and legoland_headless, which run under node where there is no
// `document`, no `window` and no canvas. Every function below therefore has to
// work with neither: LL.isHeadless() is decided once, the display becomes a plain
// counter plus a sampled checksum (enough for a headless test to prove that
// pixels are changing), the event queue stays a JS array that simply never
// receives anything, and NOTHING throws. A ReferenceError in here would abort
// the wasm call that triggered it, which in a test harness reads as a mysterious
// failure deep inside the game rather than as "there is no DOM".
//   Proof: `node portable/build-wasm/shimtest.js --frames 300`.

var LibraryLLCanvas = {
  $LL: {
    // Decided ONCE, LAZILY, and through `globalThis` property lookups.
    //
    // All three of those matter. Lazily and through globalThis because emcc's
    // -O2 JS optimizer constant-folds `typeof document === 'undefined'` at BUILD
    // time -- it minifies the library with no DOM in scope, decides the answer
    // is `true`, and bakes `headless: true` into the output, so the browser page
    // then runs headless and paints nothing. (That is not hypothetical; it is
    // what the first version of this file did, and the symptom was a black
    // canvas with the frame counter happily climbing.) A property access on
    // globalThis is opaque to the optimizer. Once, because the answer cannot
    // change during a run and every present would otherwise re-test it.
    _headless: undefined,
    isHeadless: function () {
      if (LL._headless === undefined) {
        var d = globalThis['document'], w = globalThis['window'];
        LL._headless = !(d && w && typeof d.getElementById === 'function');
      }
      return LL._headless;
    },

    canvas: null,
    ctx: null,
    image: null,       // ImageData, w*h RGBA
    w: 0,
    h: 0,
    events: [],        // [type, a, b, c] quadruples, flattened
    lastX: null,
    lastY: null,
    bound: false,
    frames: 0,
    checksum: 0,       // headless: a sampled hash of the last frame presented
    lastBox: '',

    // Event type tags -- must match the LLEV_* defines in user32.c.
    EV_KEYDOWN: 1, EV_KEYUP: 2, EV_MOUSEMOVE: 3, EV_MOUSEDOWN: 4,
    EV_MOUSEUP: 5, EV_WHEEL: 6, EV_FOCUS: 7, EV_BLUR: 8, EV_CLOSE: 9,

    // KeyboardEvent.code -> DIK_* scan code. DIK codes ARE the PS/2 set-1 scan
    // codes, which is why this table is mechanical rather than invented. The
    // game reads g_key_state[256] by DIK index (input.c's ScanKeyboard) and
    // input2.c's 59-entry DIK->character map at 0x004bad58 turns them into
    // typed characters, so the letters/digits/arrows must be right for the
    // cheat codes and the name-entry field to work at all. The entries the
    // game's own controller fold reads are 0xcb/0xcd/0xc8/0xd0 (arrows), 0x39
    // (space), 0x0f (tab), 0x01 (escape) and 0x1c (return) -- see
    // docs/lanes/scope-port-b2.md §4.
    dik: {
      Escape: 0x01,
      Digit1: 0x02, Digit2: 0x03, Digit3: 0x04, Digit4: 0x05, Digit5: 0x06,
      Digit6: 0x07, Digit7: 0x08, Digit8: 0x09, Digit9: 0x0a, Digit0: 0x0b,
      Minus: 0x0c, Equal: 0x0d, Backspace: 0x0e, Tab: 0x0f,
      KeyQ: 0x10, KeyW: 0x11, KeyE: 0x12, KeyR: 0x13, KeyT: 0x14, KeyY: 0x15,
      KeyU: 0x16, KeyI: 0x17, KeyO: 0x18, KeyP: 0x19,
      BracketLeft: 0x1a, BracketRight: 0x1b, Enter: 0x1c, ControlLeft: 0x1d,
      KeyA: 0x1e, KeyS: 0x1f, KeyD: 0x20, KeyF: 0x21, KeyG: 0x22, KeyH: 0x23,
      KeyJ: 0x24, KeyK: 0x25, KeyL: 0x26, Semicolon: 0x27, Quote: 0x28,
      Backquote: 0x29, ShiftLeft: 0x2a, Backslash: 0x2b,
      KeyZ: 0x2c, KeyX: 0x2d, KeyC: 0x2e, KeyV: 0x2f, KeyB: 0x30, KeyN: 0x31,
      KeyM: 0x32, Comma: 0x33, Period: 0x34, Slash: 0x35, ShiftRight: 0x36,
      NumpadMultiply: 0x37, AltLeft: 0x38, Space: 0x39, CapsLock: 0x3a,
      F1: 0x3b, F2: 0x3c, F3: 0x3d, F4: 0x3e, F5: 0x3f, F6: 0x40, F7: 0x41,
      F8: 0x42, F9: 0x43, F10: 0x44, NumLock: 0x45, ScrollLock: 0x46,
      Numpad7: 0x47, Numpad8: 0x48, Numpad9: 0x49, NumpadSubtract: 0x4a,
      Numpad4: 0x4b, Numpad5: 0x4c, Numpad6: 0x4d, NumpadAdd: 0x4e,
      Numpad1: 0x4f, Numpad2: 0x50, Numpad3: 0x51, Numpad0: 0x52,
      NumpadDecimal: 0x53, F11: 0x57, F12: 0x58,
      NumpadEnter: 0x9c, ControlRight: 0x9d, NumpadDivide: 0xb5,
      AltRight: 0xb8, Home: 0xc7, ArrowUp: 0xc8, PageUp: 0xc9,
      ArrowLeft: 0xcb, ArrowRight: 0xcd, End: 0xcf, ArrowDown: 0xd0,
      PageDown: 0xd1, Insert: 0xd2, Delete: 0xd3
    },

    // PORT-B6: KeyboardEvent.key -> DIK, for every source that does not fill in
    // `code`.
    //
    // `code` is the PHYSICAL key and is the right thing to read, but it is
    // optional in practice and a real browser hands out keydown events without
    // it: an on-screen or virtual keyboard, an IME commit, a remote-desktop or
    // accessibility client, a `dispatchEvent` from a page script, and -- the one
    // that made this visible -- the Chrome DevTools Protocol's
    // Input.dispatchKeyEvent, which is how this page gets driven from a test
    // runner or an agent. MEASURED in a real tab: `{code: "", key: "A",
    // keyCode: 0, isTrusted: true}`. `LL.dik[""]` is undefined, so `dik` came out
    // 0 and EVERY such keystroke was silently dropped -- name entry on the
    // PLAYER DETAILS screen typed nothing, and there was no way to tell that
    // from "the game ignores the keyboard".
    //
    // So: `code` first (authoritative), then `key`, then `keyCode`. The `key`
    // table only needs the names that differ from a `code` name -- Escape,
    // Enter, Tab, Backspace, Arrow*, F1..F12, Home/End/Page*/Insert/Delete and
    // CapsLock all spell themselves the same way in both, so LL.dik already
    // covers them; what is left is the printable characters, which name the
    // CHARACTER and not the key.
    //
    // Shifted characters map to their unshifted physical key, because that is
    // what a scan code IS: input2.c's 59-entry DIK->character table at
    // 0x004bad58 does the shifting itself from the DIK plus the shift state.
    // Keyed by CHARACTER CODE, not by a character literal: this file is
    // re-parsed by emcc's JS optimizer during the link, and a table containing
    // a quote or a backslash literal made acorn report "Unterminated string
    // constant" and failed the build. Codes cost nothing and cannot do that.
    charDik: {
      32: 0x39,                                          /* space */
      45: 0x0c, 95: 0x0c,                                /* minus  underscore */
      61: 0x0d, 43: 0x0d,                                /* equal  plus */
      91: 0x1a, 123: 0x1a,                               /* [ { */
      93: 0x1b, 125: 0x1b,                               /* ] } */
      59: 0x27, 58: 0x27,                                /* semicolon colon */
      39: 0x28, 34: 0x28,                                /* quote  doublequote */
      96: 0x29, 126: 0x29,                               /* backquote tilde */
      92: 0x2b, 124: 0x2b,                               /* backslash pipe */
      44: 0x33, 60: 0x33,                                /* comma  less */
      46: 0x34, 62: 0x34,                                /* period greater */
      47: 0x35, 63: 0x35,                                /* slash  question */
      33: 0x02, 64: 0x03, 35: 0x04, 36: 0x05, 37: 0x06,  /* shifted digits 1-5 */
      94: 0x07, 38: 0x08, 42: 0x09, 40: 0x0a, 41: 0x0b   /* shifted digits 6-0 */
    },
    // Win32 VK -> DIK, the last resort. Only the keys the game reads: it is a
    // fallback for a source that fills in neither `code` nor a usable `key`.
    vkDik: {
      0x1b: 0x01, 0x08: 0x0e, 0x09: 0x0f, 0x0d: 0x1c, 0x20: 0x39,
      0x25: 0xcb, 0x26: 0xc8, 0x27: 0xcd, 0x28: 0xd0,
      0x10: 0x2a, 0x11: 0x1d, 0x12: 0x38, 0x14: 0x3a, 0x2e: 0xd3
    },

    dikOf: function (e) {
      var d = LL.dik[e.code];
      if (d) return d;
      var k = e.key;
      if (k) {
        d = LL.dik[k];                       /* Escape, Enter, Tab, ArrowUp ... */
        if (d) return d;
        if (k.length === 1) {
          var u = k.toUpperCase();
          if (u >= 'A' && u <= 'Z') return LL.dik['Key' + u];
          if (u >= '0' && u <= '9') return LL.dik['Digit' + u];
          d = LL.charDik[k.charCodeAt(0)];
          if (d) return d;
        }
      }
      var vk = e.keyCode || e.which || 0;
      if (LL.vkDik[vk]) return LL.vkDik[vk];
      if (vk >= 65 && vk <= 90) return LL.dik['Key' + String.fromCharCode(vk)];
      if (vk >= 48 && vk <= 57) return LL.dik['Digit' + String.fromCharCode(vk)];
      return 0;
    },

    // KeyboardEvent.code -> Win32 virtual key. The game asks GetKeyState about
    // exactly one (VK_CAPITAL 0x14, input2.c:325 inside GetInputChar) and reads
    // WM_CHAR for exactly one character (backspace, input2.c's
    // LegoLandWindowProc), so this table only has to be right for those plus
    // whatever a WM_KEYDOWN watcher might want.
    vk: {
      Escape: 0x1b, Enter: 0x0d, Space: 0x20, Tab: 0x09, Backspace: 0x08,
      ShiftLeft: 0x10, ShiftRight: 0x10, ControlLeft: 0x11, ControlRight: 0x11,
      AltLeft: 0x12, AltRight: 0x12, CapsLock: 0x14,
      ArrowLeft: 0x25, ArrowUp: 0x26, ArrowRight: 0x27, ArrowDown: 0x28
    },

    push: function (t, a, b, c) {
      // Cap the queue: a wedged wasm side must not grow the heap without limit.
      if (LL.events.length > 4096) return;
      LL.events.push(t, a | 0, b | 0, c | 0);
    },

    vkOf: function (e) {
      if (LL.vk[e.code] !== undefined) return LL.vk[e.code];
      /* PORT-B6: the same `code`-less sources dikOf handles. LL.vk's names are
       * spelled the same in `key` for every entry it has (Escape, Enter, Tab,
       * Backspace, Arrow*, CapsLock), so one extra lookup covers them. */
      if (e.key && LL.vk[e.key] !== undefined) return LL.vk[e.key];
      if (e.key && e.key.length === 1) {
        var ch = e.key.toUpperCase().charCodeAt(0);
        if ((ch >= 48 && ch <= 57) || (ch >= 65 && ch <= 90)) return ch;
      }
      if (e.keyCode) return e.keyCode;
      return 0;
    },

    // Tell the page something, if there is a page. Every call into the host
    // app's hooks is optional: under node Module exists but has none of them,
    // and in a worker or a thumbnailer the DOM may be half there.
    tell: function (hook, a, b) {
      if (typeof Module === 'undefined' || typeof Module[hook] !== 'function')
        return;
      try { Module[hook](a, b); } catch (e) { /* a page bug is not a game bug */ }
    },

    bind: function () {
      if (LL.bound || LL.isHeadless()) return;
      LL.bound = true;
      var c = LL.canvas;

      // The canvas must be focusable for keydown to reach it; listening on the
      // window instead means the page's own chrome steals nothing.
      window.addEventListener('keydown', function (e) {
        // PORT-B6: dikOf, not LL.dik[e.code] -- a source that fills in no
        // `code` (a virtual keyboard, an IME, CDP) had every key dropped.
        var dik = LL.dikOf(e) || 0;
        if (dik) {
          // Backspace is the one character the game handles as WM_CHAR
          // (input2.c's LegoLandWindowProc), so it travels in slot c.
          LL.push(LL.EV_KEYDOWN, dik, LL.vkOf(e), dik === 0x0e ? 8 : 0);
          // Tab, the arrows and F-keys are game controls here.
          e.preventDefault();
        }
      }, false);

      window.addEventListener('keyup', function (e) {
        var dik = LL.dikOf(e) || 0;
        if (dik) {
          LL.push(LL.EV_KEYUP, dik, LL.vkOf(e), 0);
          e.preventDefault();
        }
      }, false);

      // The game's mouse is RELATIVE (DirectInput DIMOUSESTATE lX/lY), so the
      // absolute pointer position is differenced. The game then applies its own
      // acceleration from whatever SPI_GETMOUSE reported -- user32.c reports
      // acceleration OFF, so these deltas reach the game cursor 1:1 and the
      // drift PORT-B recorded in its §7 is gone.
      //
      // PORT-M13: lastX/lastY track the position the GAME's cursor has been
      // moved to, NOT the position of the real pointer.  The delta is an
      // integer (DIMOUSESTATE is a LONG count of mouse units) while `x` is a
      // fraction whenever the canvas is not displayed at exactly 640x480 --
      // `max-width: 100%` shrinks it on any narrow window, and a page zoom or
      // a device pixel ratio does the same.  Remembering the real position and
      // rounding each delta INDEPENDENTLY throws the remainder away on every
      // event, and the two failures that follows are both real:
      //
      //   * the error is a RANDOM WALK.  Each event contributes up to half a
      //     game pixel and nothing ever corrects it, so the game's cursor
      //     drifts away from the pointer without bound -- measured here at
      //     ~2 px after the tutorial's opening clicks and ~6 px a few hundred
      //     mouse moves later, which on a 32x16 isometric tile is most of the
      //     way to the NEXT SQUARE.  A player who aims at the square in front
      //     of the ride's entrance arrow lays the path one square off, and the
      //     LINK goal -- which tests exactly one square (eventtick.c:939) --
      //     never satisfies however much path is laid.
      //   * a move SMALLER than half a game pixel is lost outright, because
      //     `lastX = x` swallows it.  At a canvas scaled to 3x, one client
      //     pixel is a third of a game pixel and the cursor never moves at all.
      //
      // Accumulating into lastX instead fixes both: the remainder survives to
      // the next event, so a slow drag is delivered a pixel at a time and the
      // error against the real pointer never exceeds half a pixel.  When the
      // game clamps its own cursor at a screen edge this model over-runs it
      // exactly as a physical relative mouse does, which is the behaviour the
      // game was written against.
      c.addEventListener('mousemove', function (e) {
        var r = c.getBoundingClientRect();
        var sx = LL.w / r.width, sy = LL.h / r.height;
        var x = (e.clientX - r.left) * sx, y = (e.clientY - r.top) * sy;
        if (LL.lastX !== null) {
          var dx = Math.round(x - LL.lastX), dy = Math.round(y - LL.lastY);
          if (dx || dy) LL.push(LL.EV_MOUSEMOVE, dx, dy, 0);
          LL.lastX += dx; LL.lastY += dy;
        } else {
          LL.lastX = x; LL.lastY = y;
        }
      }, false);

      // DIMOUSESTATE.rgbButtons[0] is left, [1] is right, [2] is middle
      // (input.c:279-285 reads exactly those three), and a DOM
      // MouseEvent.button is 0 left / 1 middle / 2 right -- hence the swap.
      c.addEventListener('mousedown', function (e) {
        LL.push(LL.EV_MOUSEDOWN, e.button === 1 ? 2 : (e.button === 2 ? 1 : 0), 0, 0);
        e.preventDefault();
      }, false);
      c.addEventListener('mouseup', function (e) {
        LL.push(LL.EV_MOUSEUP, e.button === 1 ? 2 : (e.button === 2 ? 1 : 0), 0, 0);
        e.preventDefault();
      }, false);
      c.addEventListener('contextmenu', function (e) { e.preventDefault(); }, false);

      // One notch is +/-120, the granularity dinput.c reports for DIMOFS_Z.
      // ScanMouse (input.c:133) compares lZ against +/- g_wheel_granularity,
      // whose value comes from that GetProperty -- and whose static initialiser
      // in the image is 60, so 120 crosses the threshold either way.
      c.addEventListener('wheel', function (e) {
        LL.push(LL.EV_WHEEL, e.deltaY > 0 ? 120 : -120, 0, 0);
        e.preventDefault();
      }, { passive: false });

      // Focus drives the game's suspend flag: WM_KILLFOCUS sets g_game->flags
      // bit 0 and the pump then idles in WaitMessage until WM_SETFOCUS.
      window.addEventListener('focus', function () { LL.push(LL.EV_FOCUS, 0, 0, 0); }, false);
      window.addEventListener('blur', function () { LL.push(LL.EV_BLUR, 0, 0, 0); }, false);
    }
  },

  // ---- C entry points ------------------------------------------------------

  ll_js_display_open: function (w, h) {
    LL.w = w; LL.h = h;
    if (LL.isHeadless()) {
      // No DOM: the display is a counter. ll_js_present16 still runs, so a
      // headless harness sees frames and a checksum and the game's control flow
      // is identical to the browser's.
      LL.tell('llStatus', 'headless display ' + w + 'x' + h);
      return;
    }
    var c = document.getElementById('canvas');
    if (!c) {
      c = document.createElement('canvas');
      c.id = 'canvas';
      document.body.appendChild(c);
    }
    c.width = w;
    c.height = h;
    LL.canvas = c;
    LL.ctx = c.getContext('2d', { alpha: false });
    LL.image = LL.ctx.createImageData(w, h);
    // Opaque alpha once; the 565 conversion then only writes RGB.
    var d = LL.image.data;
    for (var i = 3; i < d.length; i += 4) d[i] = 255;
    LL.bind();
    // PORT-B7: hand the page the shim's own state. `LL` is a closure variable
    // inside the linked JS library, so a tool driving this page could see that
    // a keystroke reached the DOM and that the game never received it, and had
    // nothing in between to look at. One assignment makes the queue, the DIK
    // tables and the pointer origin readable from the console.
    try { globalThis.LL_DEBUG = LL; } catch (e) { /* nothing to expose to */ }
    LL.tell('llStatus', 'display ' + w + 'x' + h);
  },

  // RGB565 -> RGBA. The 5/6/5 channels are expanded by replicating their top
  // bits into the low ones ((v << 3) | (v >> 2)), which is what keeps white
  // white instead of 0xf8.
  ll_js_present16: function (ptr, w, h, pitch) {
    var src = HEAPU16;
    var strideWords = pitch >> 1;
    var base = ptr >> 1;
    var x, y;

    LL.frames++;

    if (LL.isHeadless() || !LL.ctx) {
      // A sampled FNV-1a over every 17th pixel of every 7th row: cheap enough
      // to run every frame for hundreds of frames under node, and specific
      // enough that "the checksum changed" means the game really drew
      // something different. Both strides are coprime with the row length so
      // the samples walk across the surface instead of down one column.
      var hash = 0x811c9dc5;
      for (y = 0; y < h; y += 7) {
        var r0 = base + y * strideWords;
        for (x = 0; x < w; x += 17)
          hash = ((hash ^ src[r0 + x]) * 0x01000193) >>> 0;
      }
      LL.checksum = hash;
      LL.tell('llFrame', LL.frames);
      return;
    }

    if (w !== LL.w || h !== LL.h) {
      // A present before/after a mode change: re-open at the reported size.
      LL.frames--;                       // _ll_js_display_open does not count
      _ll_js_display_open(w, h);
      LL.frames++;
      if (!LL.ctx) return;
    }
    var dst = LL.image.data;
    var o = 0;
    for (y = 0; y < h; y++) {
      var row = base + y * strideWords;
      for (x = 0; x < w; x++) {
        var v = src[row + x];
        var r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f;
        dst[o] = (r << 3) | (r >> 2);
        dst[o + 1] = (g << 2) | (g >> 4);
        dst[o + 2] = (b << 3) | (b >> 2);
        o += 4;
      }
    }
    LL.ctx.putImageData(LL.image, 0, 0);
    LL.tell('llFrame', LL.frames);
  },

  // One event per call, written as four ints. Returns 1 while the queue has
  // more. Called only from wasm, never from a DOM handler. Headless-safe by
  // construction: the queue is a plain array nothing ever pushes to.
  ll_js_event_next: function (out) {
    if (LL.events.length === 0) return 0;
    var t = LL.events.shift(), a = LL.events.shift();
    var b = LL.events.shift(), c = LL.events.shift();
    var i = out >> 2;
    HEAP32[i] = t; HEAP32[i + 1] = a; HEAP32[i + 2] = b; HEAP32[i + 3] = c;
    return 1;
  },

  ll_js_set_cursor: function (visible) {
    if (LL.canvas) LL.canvas.style.cursor = visible ? 'default' : 'none';
  },

  // A modal the shim auto-answered (user32.c's MessageBoxA). Shown on the page
  // because a modal that the host answers in 1.2 s is otherwise invisible, and
  // on the front-end path it is how the game reports every fatal startup
  // failure (InitSession, startup.c:136/150/157).
  // UTF8ToString is a library function, not a free-standing global, so it has
  // to be declared as a dependency or a --closure/DCE build can drop it.
  ll_js_messagebox__deps: ['$UTF8ToString'],
  ll_js_messagebox: function (textPtr, captionPtr, answer) {
    var text = UTF8ToString(textPtr);
    var caption = UTF8ToString(captionPtr);
    LL.lastBox = caption + ': ' + text + '  [answered ' + answer + ']';
    LL.tell('llMessageBox', LL.lastBox, answer);
    if (LL.isHeadless() && typeof console !== 'undefined')
      console.log('[MessageBox] ' + LL.lastBox);
  },

  // Frames presented and the last sampled checksum, for a headless harness.
  ll_js_frames: function () { return LL.frames; },
  ll_js_checksum: function () { return LL.checksum | 0; }
};

autoAddDeps(LibraryLLCanvas, '$LL');
addToLibrary(LibraryLLCanvas);
