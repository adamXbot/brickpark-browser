// LEGOLAND portable -- small helpers the player page's screens share.

// h('button', { class: 'brick', onclick: fn }, 'Play') -> an element.
export function h(tag, attrs, ...children) {
  const el = document.createElement(tag);
  if (attrs) {
    for (const [k, v] of Object.entries(attrs)) {
      if (v === undefined || v === null || v === false) continue;
      if (k === 'class') el.className = v;
      else if (k === 'style' && typeof v === 'object') Object.assign(el.style, v);
      else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2), v);
      else if (k === 'html') el.innerHTML = v;
      else if (v === true) el.setAttribute(k, '');
      else el.setAttribute(k, String(v));
    }
  }
  append(el, children);
  return el;
}

function append(el, children) {
  for (const c of children) {
    if (c === undefined || c === null || c === false) continue;
    if (Array.isArray(c)) append(el, c);
    else el.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
}

export function bytes(n) {
  if (n === null || n === undefined || !isFinite(n)) return '—';
  if (n < 1024) return n + ' B';
  if (n < 1048576) return (n / 1024).toFixed(n < 10240 ? 1 : 0) + ' KB';
  if (n < 1073741824) return (n / 1048576).toFixed(n < 10485760 ? 1 : 0) + ' MB';
  return (n / 1073741824).toFixed(2) + ' GB';
}

export function duration(seconds) {
  if (!isFinite(seconds) || seconds < 0) return '';
  if (seconds < 60) return Math.max(1, Math.round(seconds)) + ' s';
  const m = Math.floor(seconds / 60), s = Math.round(seconds % 60);
  return m + ' min' + (s ? ' ' + s + ' s' : '');
}

export function when(date) {
  if (!date) return '—';
  const d = date instanceof Date ? date : new Date(date);
  const now = new Date();
  const sameDay = d.toDateString() === now.toDateString();
  const time = d.toLocaleTimeString(undefined, { hour: 'numeric', minute: '2-digit' });
  if (sameDay) return 'Today, ' + time;
  const yesterday = new Date(now);
  yesterday.setDate(now.getDate() - 1);
  if (d.toDateString() === yesterday.toDateString()) return 'Yesterday, ' + time;
  return d.toLocaleDateString(undefined, { day: 'numeric', month: 'short', year: 'numeric' }) + ', ' + time;
}

// A throttled progress reporter: at most one UI update per animation frame.
export function throttle(fn) {
  let pending = null, scheduled = false;
  return (...args) => {
    pending = args;
    if (scheduled) return;
    scheduled = true;
    requestAnimationFrame(() => {
      scheduled = false;
      fn(...pending);
    });
  };
}

// Rate and time-left from a stream of (done, total) samples.
export function rateMeter() {
  const samples = [];
  return (done, total) => {
    const t = performance.now();
    samples.push([t, done]);
    while (samples.length > 2 && t - samples[0][0] > 4000) samples.shift();
    const [t0, d0] = samples[0];
    const rate = t > t0 ? (done - d0) * 1000 / (t - t0) : 0;
    return { rate, left: rate > 0 ? (total - done) / rate : Infinity };
  };
}

export function saveBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = h('a', { href: url, download: filename, style: { display: 'none' } });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 30000);
}

const OPTIONS_KEY = 'legoland.options';
export const DEFAULT_OPTIONS = {
  scale: 'fit',          // 'fit' | 'integer'
  smooth: false,         // bilinear instead of crisp pixels
  narration: true,       // load speech/ into the game when it is installed
  background: false,     // keep running in a hidden tab
  fps: false,            // show the frame rate while playing
  freeplayAll: false     // -ll-freeplay-all: Free Play open, every item ticked, no budget
};

export function loadOptions() {
  try {
    const raw = localStorage.getItem(OPTIONS_KEY);
    return Object.assign({}, DEFAULT_OPTIONS, raw ? JSON.parse(raw) : {});
  } catch (e) {
    return Object.assign({}, DEFAULT_OPTIONS);
  }
}

export function saveOptions(options) {
  try { localStorage.setItem(OPTIONS_KEY, JSON.stringify(options)); } catch (e) { /* private mode */ }
}

// What happened to the last game, carried across the reload that follows it.
const EXIT_KEY = 'legoland.lastExit';
export function rememberExit(info) {
  try { sessionStorage.setItem(EXIT_KEY, JSON.stringify(Object.assign({ at: Date.now() }, info))); } catch (e) { /* ignore */ }
}
export function takeExit() {
  try {
    const raw = sessionStorage.getItem(EXIT_KEY);
    sessionStorage.removeItem(EXIT_KEY);
    const info = raw ? JSON.parse(raw) : null;
    return info && Date.now() - info.at < 10 * 60 * 1000 ? info : null;
  } catch (e) {
    return null;
  }
}
