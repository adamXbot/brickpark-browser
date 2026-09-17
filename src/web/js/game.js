// LEGOLAND portable -- running the game inside the player page.
//
// One session per page load, on purpose. The module is loaded when the player
// presses Play (not before: the launcher stays light), the game's files go
// from IndexedDB into the module's in-memory file system, and main() starts.
// When the game ends -- the player confirmed Exit, left from the page, or the
// module died -- the saves are flushed and the page RELOADS into the launcher's
// exit screen. A reload is the only way to get a clean module back: the game's
// globals are not re-initialisable, and a second instance in the same page
// would keep the first one's timers, listeners and its periodic save sync,
// which would put back saves the launcher deletes.

import { rememberExit, throttle } from './util.js';

const LOCK = 'legoland-game';

// Is a game running in ANOTHER tab of this site? (Save editing waits for it.)
export async function otherTabPlaying() {
  try {
    if (!navigator.locks || !navigator.locks.query) return false;
    const state = await navigator.locks.query();
    return state.held.some((l) => l.name === LOCK);
  } catch (e) {
    return false;
  }
}

function acquireLock() {
  if (!navigator.locks || !navigator.locks.request) return Promise.resolve(true);
  return new Promise((resolve) => {
    navigator.locks.request(LOCK, { ifAvailable: true }, (lock) => {
      if (!lock) { resolve(false); return undefined; }
      resolve(true);
      return new Promise(() => {});          // held until the page goes away
    }).catch(() => resolve(true));
  });
}

// Reload into the launcher with no auto-launch fragment left on the URL.
export function reloadToLauncher() {
  history.replaceState(null, '', location.pathname + location.search);
  location.reload();
}

function isFatal(err) {
  if (!err) return false;
  if (typeof WebAssembly !== 'undefined' && err instanceof WebAssembly.RuntimeError) return true;
  const text = String(err && err.message ? err.message : err) + ' ' + String(err && err.name);
  return /RuntimeError|Aborted\(|unreachable|ExitStatus|memory access out of bounds/.test(text);
}

// While the tab is hidden, deliver the game's 0-1 ms yields as MessageChannel
// messages instead of timers, which background tabs throttle to about once a
// second (the developer page's llAwake, portable/src/browser/index.html). Only
// when the player asked for it: it keeps a hidden tab using a full core.
function installBackgroundRunning() {
  if (typeof MessageChannel !== 'function') return;
  const nativeSetTimeout = window.setTimeout.bind(window);
  const nativeClearTimeout = window.clearTimeout.bind(window);
  const chan = new MessageChannel();
  const queue = [];
  const pending = new Map();
  let next = -1;
  chan.port1.onmessage = () => {
    const id = queue.shift();
    const fn = pending.get(id);
    pending.delete(id);
    if (fn) { try { fn(); } catch (e) { console.error(e); } }
  };
  window.setTimeout = function (fn, delay) {
    if (document.hidden && typeof fn === 'function' && arguments.length <= 2 && !(delay > 1)) {
      const id = next--;
      pending.set(id, fn);
      queue.push(id);
      chan.port2.postMessage(0);
      return id;
    }
    return nativeSetTimeout.apply(null, arguments);
  };
  window.clearTimeout = function (id) {
    if (typeof id === 'number' && id < 0) { pending.delete(id); return; }
    nativeClearTimeout(id);
  };
}

export class GameSession {
  constructor({ store, install, options, confirmLeave }) {
    this.store = store;
    this.install = install;
    this.options = options;
    this.confirmLeave = confirmLeave;
    this.finished = false;
    this.runtimeReady = false;
    this.frames = 0;
    this.log = [];
    this.el = {
      stage: document.getElementById('stage'),
      canvas: document.getElementById('canvas'),
      loading: document.getElementById('stage-loading'),
      loadingTitle: document.getElementById('stage-loading-title'),
      loadingBar: document.getElementById('stage-loading-bar'),
      loadingText: document.getElementById('stage-loading-text'),
      bar: document.getElementById('stage-bar'),
      fps: document.getElementById('stage-fps'),
      fullscreen: document.getElementById('stage-fullscreen'),
      sound: document.getElementById('stage-sound'),
      leave: document.getElementById('stage-leave')
    };
    this.onBeforeUnload = (e) => {
      if (this.finished) return;
      e.preventDefault();
      e.returnValue = '';
    };
  }

  note(line) {
    this.log.push(line);
    if (this.log.length > 200) this.log.shift();
  }

  loading(title, done, total) {
    const el = this.el;
    el.loadingTitle.textContent = title;
    if (total) {
      const pct = Math.min(100, (100 * done) / total);
      el.loadingBar.style.width = pct.toFixed(1) + '%';
      el.loadingBar.parentElement.classList.remove('indeterminate');
      el.loadingText.textContent = Math.round(done / 1048576) + ' of ' + Math.round(total / 1048576) + ' MB';
    } else {
      el.loadingBar.parentElement.classList.add('indeterminate');
      el.loadingText.textContent = '';
    }
  }

  async start() {
    if (!(await acquireLock()))
      throw new Error('LEGOLAND is already running in another tab. Close that tab (or leave the park there) first.');

    document.body.classList.add('playing');
    this.el.stage.hidden = false;
    this.bindStage();
    window.addEventListener('beforeunload', this.onBeforeUnload);
    if (this.options.background) installBackgroundRunning();

    // The build id cache-busts the module: a rebuilt legoland.wasm must never
    // come out of the HTTP cache (a lesson the port's test lanes paid for).
    let build = String(Date.now());
    try {
      const res = await fetch('version.json', { cache: 'no-store' });
      if (res.ok) build = (await res.json()).build || build;
    } catch (e) { /* development server without a stamp */ }

    this.loading('Opening the gates…', 0, 0);
    const files = new Map();
    const progress = throttle((read, total) => this.loading('Unpacking the park', read, total));
    await this.store.readAll((path, data) => {
      if (!this.options.narration && path.startsWith('speech/')) return;
      files.set(path, data);
    }, progress);
    if (!files.has('strings/stab.str'))
      throw new Error('The stored game files are incomplete. Open "Game files" and install them again.');

    this.loading('Starting LEGOLAND…', 0, 0);
    this.startModule(files, build);
  }

  startModule(files, build) {
    const session = this;
    globalThis.__llMuted = !!this.options.muted;

    window.addEventListener('error', (e) => {
      if (isFatal(e.error)) session.crash(e.error);
    });
    window.addEventListener('unhandledrejection', (e) => {
      if (isFatal(e.reason)) session.crash(e.reason);
    });

    window.Module = {
      canvas: this.el.canvas,
      llAbsoluteMouse: true,
      locateFile: (path, prefix) => prefix + path + '?v=' + encodeURIComponent(build),
      print: (text) => { session.note(text); console.log(text); },
      printErr: (text) => { session.note(text); console.warn(text); },
      setStatus: () => {},
      llStatus: () => {},
      llMessageBox: (text) => session.note('MessageBox: ' + text),
      llFrame: (n) => session.frame(n),
      llGameExit: (code) => session.finish({ kind: 'exit', code }),
      onAbort: (what) => session.crash('Aborted: ' + what),
      onRuntimeInitialized: () => {
        try {
          const FS = window.Module.FS;
          const made = new Set(['/gamedata']);
          FS.mkdirTree('/gamedata');
          for (const [path, data] of files) {
            const full = '/gamedata/' + path;
            const dir = full.slice(0, full.lastIndexOf('/'));
            if (!made.has(dir)) { FS.mkdirTree(dir); made.add(dir); }
            FS.writeFile(full, data, { canOwn: true });
          }
          files.clear();
          session.runtimeReady = true;
          window.Module.callMain(session.options.freeplayAll ? ['-ll-freeplay-all'] : []);
        } catch (e) {
          session.crash(e);
        }
      }
    };

    const script = document.createElement('script');
    script.src = 'legoland.js?v=' + encodeURIComponent(build);
    script.onerror = () => this.crash(new Error('The game module (legoland.js) could not be loaded.'));
    document.body.append(script);
  }

  frame(n) {
    if (!this.frames) {
      this.el.loading.hidden = true;
      this.el.canvas.focus({ preventScroll: true });
      this.layout();
    }
    this.frames = n;
    this.fpsFrames = (this.fpsFrames || 0) + 1;
  }

  // ---- the stage ------------------------------------------------------------

  bindStage() {
    const { stage, canvas, bar, fps, fullscreen, sound, leave } = this.el;
    canvas.classList.toggle('smooth', !!this.options.smooth);
    new ResizeObserver(() => this.layout()).observe(stage);
    this.layout();

    // Keep keyboard focus on the game: a toolbar button left focused would
    // turn the game's Space and Enter into clicks on it.
    const refocus = () => canvas.focus({ preventScroll: true });
    stage.addEventListener('mousedown', (e) => { if (e.target === stage) e.preventDefault(); });

    // The toolbar shows when the pointer is near the top of the screen or off
    // the game picture, and fades after a moment.
    let hideTimer = 0;
    const showBar = () => {
      bar.classList.add('visible');
      clearTimeout(hideTimer);
      hideTimer = setTimeout(() => bar.classList.remove('visible'), 2200);
    };
    stage.addEventListener('pointermove', (e) => {
      const r = canvas.getBoundingClientRect();
      const offPicture = e.clientX < r.left || e.clientX > r.right || e.clientY < r.top || e.clientY > r.bottom;
      if (offPicture || e.clientY < 56 || e.pointerType === 'touch') showBar();
    });
    bar.addEventListener('pointerenter', () => { clearTimeout(hideTimer); bar.classList.add('visible'); });
    bar.addEventListener('pointerleave', showBar);
    showBar();

    // Touch: a finger drives the game's mouse. Each touch becomes the mouse
    // events ll_canvas.js understands, and preventDefault on pointerdown stops
    // the browser's own emulated mouse events arriving a second time.
    const relay = (type, e) => canvas.dispatchEvent(new MouseEvent(type, {
      clientX: e.clientX, clientY: e.clientY, button: 0, buttons: type === 'mouseup' ? 0 : 1,
      bubbles: true, cancelable: true
    }));
    canvas.addEventListener('pointerdown', (e) => {
      if (e.pointerType !== 'touch') return;
      e.preventDefault();
      try { canvas.setPointerCapture(e.pointerId); } catch (err) { /* ignore */ }
      relay('mousemove', e);
      relay('mousedown', e);
    });
    canvas.addEventListener('pointermove', (e) => { if (e.pointerType === 'touch') relay('mousemove', e); });
    const up = (e) => { if (e.pointerType === 'touch') relay('mouseup', e); };
    canvas.addEventListener('pointerup', up);
    canvas.addEventListener('pointercancel', up);

    fullscreen.addEventListener('click', async () => {
      refocus();
      try {
        if (document.fullscreenElement) await document.exitFullscreen();
        else {
          await stage.requestFullscreen({ navigationUI: 'hide' });
          // Let Escape reach the game (it opens the in-park options) instead of
          // leaving full screen; the browser then asks for a long press.
          if (navigator.keyboard && navigator.keyboard.lock) navigator.keyboard.lock(['Escape']).catch(() => {});
        }
      } catch (e) { /* the browser refused; nothing to do */ }
    });
    document.addEventListener('fullscreenchange', () => {
      const on = !!document.fullscreenElement;
      fullscreen.setAttribute('aria-pressed', on ? 'true' : 'false');
      fullscreen.title = on ? 'Leave full screen' : 'Full screen';
      this.layout();
    });

    const paintSound = () => {
      const muted = !!this.options.muted;
      sound.setAttribute('aria-pressed', muted ? 'true' : 'false');
      sound.title = muted ? 'Sound off' : 'Sound on';
      sound.classList.toggle('off', muted);
    };
    paintSound();
    sound.addEventListener('click', () => {
      refocus();
      this.options.muted = !this.options.muted;
      if (this.onOptionsChanged) this.onOptionsChanged(this.options);
      if (typeof window.llAudioMute === 'function') window.llAudioMute(this.options.muted);
      else globalThis.__llMuted = this.options.muted;
      if (!this.options.muted && typeof window.llAudioResume === 'function') window.llAudioResume();
      paintSound();
    });

    leave.addEventListener('click', async () => {
      if (await this.confirmLeave()) {
        this.el.loading.hidden = false;
        this.loading('Closing the park…', 0, 0);
        await this.flushSaves(4000);
        this.finish({ kind: 'left' });
      } else {
        refocus();
      }
    });

    if (this.options.fps) {
      fps.hidden = false;
      setInterval(() => {
        fps.textContent = (this.fpsFrames || 0) + ' fps';
        this.fpsFrames = 0;
      }, 1000);
    }

    // A player who closes or hides the tab keeps what the game last wrote.
    document.addEventListener('visibilitychange', () => {
      if (document.hidden) this.flushSaves(0);
    });
  }

  layout() {
    const { stage, canvas } = this.el;
    const w = stage.clientWidth, h = stage.clientHeight;
    if (!w || !h) return;
    let scale = Math.min(w / 640, h / 480);
    if (this.options.scale === 'integer' && scale >= 1) scale = Math.floor(scale);
    canvas.style.width = Math.floor(640 * scale) + 'px';
    canvas.style.height = Math.floor(480 * scale) + 'px';
  }

  // ---- the end of a session ---------------------------------------------------

  flushSaves(timeoutMs) {
    return new Promise((resolve) => {
      const FS = this.runtimeReady && window.Module ? window.Module.FS : null;
      if (!FS) { resolve(false); return; }
      let settled = false;
      const done = (ok) => { if (!settled) { settled = true; resolve(ok); } };
      if (timeoutMs) setTimeout(() => done(false), timeoutMs);
      try {
        FS.syncfs(false, (err) => done(!err));
      } catch (e) {
        done(false);
      }
      if (!timeoutMs) done(true);
    });
  }

  finish(info) {
    if (this.finished) return;
    this.finished = true;
    window.removeEventListener('beforeunload', this.onBeforeUnload);
    rememberExit(Object.assign({ frames: this.frames }, info));
    reloadToLauncher();
  }

  async crash(err) {
    if (this.finished || this.crashing) return;
    this.crashing = true;
    const status = err && err.name === 'ExitStatus' ? err.status : null;
    if (status === 0) { this.finish({ kind: 'exit', code: 0 }); return; }
    const detail = (err && err.stack) ? String(err.stack) : String(err && err.message ? err.message : err);
    console.error('[legoland] the game stopped:', err);
    await this.flushSaves(3000);
    this.finish({ kind: 'crash', detail: detail.slice(0, 4000), log: this.log.slice(-40) });
  }
}
