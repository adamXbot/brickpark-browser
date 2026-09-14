// LEGOLAND portable -- the player page.
//
// Screens: setup (get the game's files in), review and progress (while they
// arrive), the menu, saved games, game files, options, and the screen a player
// lands on when the park closes. game.js runs the game itself.

import { h, bytes, duration, when, throttle, rateMeter, saveBlob, loadOptions, saveOptions, takeExit, rememberExit } from './util.js';
import { openDataStore, storageEstimate, requestPersistence } from './store.js';
import { prepareImport, runImport, readFoundSaves, fetchManifest, downloadSize, runDownload, filesFromInput, filesFromDrop } from './importer.js';
import { listSaves, readSaveFiles, writeSaveFiles, deletePlayer, deleteSave, deleteOther, exportEntries, canonicalSaveName } from './saves.js';
import { makeZip, readZipDirectory, zipEntryBlob } from './zip.js';
import { GameSession, otherTabPlaying, reloadToLauncher } from './game.js';

const app = {
  store: null,
  install: null,
  manifest: undefined,          // undefined: not fetched yet; null: this site has none
  options: loadOptions(),
  dataBase: dataBase(),
  busy: null,                   // AbortController while files are being installed
  onDrop: null                  // what a file dropped on the page does right now
};

// ?data=<path> lets a site keep the data pack elsewhere -- on this origin only.
function dataBase() {
  const param = new URLSearchParams(location.search).get('data');
  if (param) {
    try {
      const u = new URL(param.endsWith('/') ? param : param + '/', location.href);
      if (u.origin === location.origin) return u.href;
    } catch (e) { /* fall through */ }
  }
  return new URL('data/', location.href).href;
}

// ---- icons ------------------------------------------------------------------

const ICONS = {
  play: '<path d="M8 5.5v13l10.5-6.5z" fill="currentColor" stroke="none"/>',
  download: '<path d="M12 4v11m0 0-4.5-4.5M12 15l4.5-4.5M5 19.5h14"/>',
  disc: '<circle cx="12" cy="12" r="8.5"/><circle cx="12" cy="12" r="2.5"/><path d="M12 3.5a8.5 8.5 0 0 1 8.5 8.5"/>',
  folder: '<path d="M3.5 7.5a2 2 0 0 1 2-2h4l2 2h7a2 2 0 0 1 2 2v7.5a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z"/>',
  saves: '<path d="M6 3.5h9l3.5 3.5v13.5H6z"/><path d="M9 3.5v5h6v-5M9 20.5v-6h6v6"/>',
  box: '<path d="M4 7.5 12 3.5l8 4v9l-8 4-8-4z"/><path d="M4 7.5 12 11.5l8-4M12 11.5v9"/>',
  gear: '<circle cx="12" cy="12" r="3"/><path d="M12 2.5v3M12 18.5v3M2.5 12h3M18.5 12h3M5.3 5.3l2.1 2.1M16.6 16.6l2.1 2.1M5.3 18.7l2.1-2.1M16.6 7.4l2.1-2.1"/>',
  check: '<path d="M5 12.5 10 17.5 19 7"/>',
  warn: '<path d="M12 4 2.8 19.5h18.4z"/><path d="M12 10v4.5M12 17.2v.3"/>',
  cross: '<path d="M6 6l12 12M18 6 6 18"/>',
  info: '<circle cx="12" cy="12" r="8.5"/><path d="M12 11v5.5M12 7.8v.3"/>',
  trash: '<path d="M4.5 6.5h15M9.5 6.5V4h5v2.5M6.5 6.5l1 13.5h9l1-13.5"/>',
  export: '<path d="M12 15V3.5m0 0L7.5 8M12 3.5 16.5 8M5 13.5v6h14v-6"/>',
  import: '<path d="M12 3.5V15m0 0-4.5-4.5M12 15l4.5-4.5M5 13.5v6h14v-6"/>',
  lock: '<rect x="5" y="10.5" width="14" height="10" rx="2"/><path d="M8 10.5V8a4 4 0 0 1 8 0v2.5"/>',
  back: '<path d="M14.5 5.5 8 12l6.5 6.5"/>',
  gate: '<path d="M3.5 20.5V9l8.5-5 8.5 5v11.5"/><path d="M8 20.5v-7h8v7M12 13.5v7"/>'
};

function icon(name, cls = 'icon') {
  const span = h('span', { class: cls, 'aria-hidden': 'true' });
  span.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">' + ICONS[name] + '</svg>';
  return span;
}

function badge(name, tone) {
  return h('div', { class: 'badge ' + tone }, icon(name));
}

const plural = (n, one, many = one + 's') => n + ' ' + (n === 1 ? one : many);

// ---- chrome: screens, dialog, toast -------------------------------------------

const host = () => document.getElementById('screen');

function mount(node, { nav = true, active = '' } = {}) {
  host().replaceChildren(node);
  const navEl = document.getElementById('nav');
  navEl.hidden = !(nav && app.install);
  for (const b of navEl.querySelectorAll('button')) {
    const on = b.dataset.go === active;
    b.classList.toggle('active', on);
    if (on) b.setAttribute('aria-current', 'page');
    else b.removeAttribute('aria-current');
  }
  app.onDrop = null;
  const heading = node.querySelector('h1');
  if (heading) {
    heading.setAttribute('tabindex', '-1');
    heading.focus({ preventScroll: true });
  }
  window.scrollTo(0, 0);
}

function go(name) {
  if (app.busy) return;
  const screens = { menu: showMenu, saves: showSaves, data: showData, options: showOptions, setup: showSetup };
  (screens[name] || showMenu)();
}

function ask({ title, body, confirm = 'OK', cancel = 'Cancel', danger = false }) {
  const dlg = document.getElementById('dialog');
  dlg.returnValue = '';
  dlg.replaceChildren(h('form', { method: 'dialog', class: 'dialog-body' },
    h('h2', null, title),
    typeof body === 'string' ? h('p', null, body) : body,
    h('div', { class: 'dialog-actions' },
      cancel ? h('button', { value: 'cancel', class: 'btn ghost' }, cancel) : null,
      h('button', { value: 'ok', class: 'btn ' + (danger ? 'danger' : 'primary'), autofocus: true }, confirm))));
  return new Promise((resolve) => {
    dlg.addEventListener('close', () => resolve(dlg.returnValue === 'ok'), { once: true });
    dlg.showModal();
  });
}

function toast(text, tone = '') {
  const t = document.getElementById('toast');
  t.textContent = text;
  t.className = 'toast show ' + tone;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { t.className = 'toast ' + tone; }, 4200);
}

function friendly(e) {
  const name = e && e.name, msg = (e && e.message) || String(e);
  if (name === 'QuotaExceededError' || /quota/i.test(msg))
    return 'The browser ran out of storage space for the game files (they need about 240 MB). Free some space, or allow this site to store more, then try again.';
  if (name === 'TypeError' && /fetch|network/i.test(msg))
    return 'The download was interrupted. Check your connection and try again.';
  return msg;
}

// ---- setup --------------------------------------------------------------------

async function showSetup({ replacing = false } = {}) {
  if (app.manifest === undefined) {
    mount(h('section', { class: 'screen narrow center' }, h('div', { class: 'spinner', 'aria-hidden': 'true' })), { nav: false });
    app.manifest = await fetchManifest(app.dataBase);
  }
  const m = app.manifest;
  const cards = [];
  if (m) cards.push(downloadCard(m, replacing));
  cards.push(discCard());
  const view = h('section', { class: 'screen setup' },
    h('div', { class: 'intro' },
      replacing ? null : h('p', { class: 'eyebrow' }, 'One-time setup'),
      h('h1', null, replacing ? 'Replace the game files' : 'Let’s get the park ready'),
      h('p', { class: 'lede' }, replacing
        ? 'The files you install now replace the current ones. Your saved games stay where they are.'
        : 'LEGOLAND needs its game files before the gates can open. You only do this once — the files stay in this browser for next time.')),
    h('div', { class: 'cards' + (cards.length === 1 ? ' single' : '') }, cards),
    m ? null : h('p', { class: 'fineprint' }, 'This site doesn’t offer the game files for download, so you’ll need your own LEGOLAND CD, a disc image of it, or an installed copy.'),
    replacing ? h('div', { class: 'row-actions' }, h('button', { class: 'btn ghost', onclick: () => go('data') }, icon('back'), 'Keep the current files')) : null);
  mount(view, { nav: replacing, active: 'data' });
  app.onDrop = (inputs) => reviewImport(inputs, replacing);
}

function downloadCard(m, replacing) {
  const speech = m.packs.find((p) => p.id === 'speech');
  let includeSpeech = !!speech;
  const size = h('strong', null, bytes(downloadSize(m, includeSpeech)));
  return h('article', { class: 'card' },
    badge('download', 'blue'),
    h('h2', null, 'Download the game files'),
    h('p', null, (m.edition && m.edition.label) || 'LEGOLAND', ' · ', size),
    speech ? h('label', { class: 'check' },
      h('input', { type: 'checkbox', checked: true, onchange: (e) => {
        includeSpeech = e.target.checked;
        size.textContent = bytes(downloadSize(m, includeSpeech));
      } }),
      h('span', null, 'Include voice narration ', h('span', { class: 'muted' }, '(' + bytes(speech.bytes) + ')'))) : null,
    h('div', { class: 'card-actions' },
      h('button', { class: 'btn primary', onclick: () => startDownload(includeSpeech, replacing) }, icon('download'), 'Download')),
    h('p', { class: 'note' }, 'Downloads once, then plays without downloading again.'));
}

function discCard() {
  const pick = (e) => {
    const inputs = filesFromInput(e.target.files);
    e.target.value = '';
    if (inputs.length) reviewImport(inputs);
  };
  const fileInput = h('input', { type: 'file', accept: '.iso,.bin,.img,.cue,.zip,.mdf,.nrg', multiple: true, hidden: true, onchange: pick });
  const dirInput = h('input', { type: 'file', webkitdirectory: true, multiple: true, hidden: true, onchange: pick });
  const zone = h('div', {
    class: 'dropzone', tabindex: 0, role: 'button', 'aria-label': 'Choose a disc image',
    onclick: () => fileInput.click(),
    onkeydown: (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); fileInput.click(); } }
  },
  icon('disc', 'icon big'),
  h('span', null, h('strong', null, 'Drop a disc image or your LEGOLAND folder here'), h('br'),
    h('span', { class: 'muted' }, '.iso or .bin, a .zip of one, or an installed copy')));
  return h('article', { class: 'card' },
    badge('disc', 'red'),
    h('h2', null, 'Use your own LEGOLAND CD'),
    h('p', null, 'Read the game files from a disc image, a copy of the CD, or an installed game.'),
    zone,
    h('div', { class: 'card-actions' },
      h('button', { class: 'btn', onclick: () => fileInput.click() }, icon('disc'), 'Choose disc image…'),
      h('button', { class: 'btn', onclick: () => dirInput.click() }, icon('folder'), 'Choose folder…')),
    h('p', { class: 'note' }, icon('lock'), 'Nothing is uploaded. Your files are read right here, on this computer.'),
    fileInput, dirInput);
}

async function reviewImport(inputs, replacing = false) {
  const status = h('p', { class: 'muted', role: 'status' }, 'Opening your files…');
  mount(h('section', { class: 'screen narrow center' },
    h('h1', null, 'Checking your files'),
    h('div', { class: 'spinner', 'aria-hidden': 'true' }), status), { nav: false });
  try {
    const prep = await prepareImport(inputs, (s) => { status.textContent = s; });
    showReview(prep, replacing);
  } catch (e) {
    showProblem('Those files couldn’t be read', friendly(e), () => showSetup({ replacing }));
  }
}

function reviewItem(tone, title, text, extra) {
  const iconName = { ok: 'check', warn: 'warn', bad: 'cross', info: 'info' }[tone];
  return h('li', { class: 'review-item ' + tone },
    icon(iconName, 'icon status'),
    h('div', null, h('strong', null, title), h('p', null, text), extra || null));
}

function showReview(prep, replacing) {
  const { plan, merged } = prep;
  const ok = plan.problems.length === 0;
  let includeSpeech = plan.speech.length > 0;
  let importSaves = plan.saves.length > 0;
  const total = h('span');
  const refresh = () => { total.textContent = bytes(prep.installBytes + prep.volumeBytes + (includeSpeech ? prep.speechBytes : 0)); };
  refresh();

  const haveVolumes = Object.keys(plan.volumes).length === 3;
  const items = [
    reviewItem(haveVolumes ? 'ok' : 'bad', 'Game volumes',
      haveVolumes ? 'Legoland.res, Graphics1.res and Graphics2.res · ' + bytes(prep.volumeBytes)
                  : 'Legoland.res, Graphics1.res and Graphics2.res are all needed; ' + Object.keys(plan.volumes).length + ' of 3 found.'),
    merged
      ? reviewItem(merged.lacking.length ? 'bad' : merged.short.length ? 'warn' : 'ok', 'Install files',
          plural(merged.items.length, 'file') + (plan.archive ? ' from main.z' : '') + ' · ' + bytes(prep.installBytes))
      : reviewItem('bad', 'Install files', 'Not found.'),
    plan.speech.length
      ? reviewItem('ok', 'Voice narration', plural(plan.speech.length, 'clip') + ' · ' + bytes(prep.speechBytes),
          h('label', { class: 'check small' }, h('input', { type: 'checkbox', checked: true, onchange: (e) => { includeSpeech = e.target.checked; refresh(); } }), h('span', null, 'Install the narration')))
      : reviewItem('warn', 'Voice narration', 'No Speech folder was found, so the advisor and the lessons will be silent.'),
    reviewItem('info', 'Advisor animations', 'Not included from a disc: the advisor’s corner window shows a plain panel.' +
      (app.manifest ? ' The download from this site includes them.' : ''))
  ];
  if (plan.saves.length)
    items.push(reviewItem('ok', 'Saved games', plural(plan.saves.length, 'save file') + ' found in a profiles folder',
      h('label', { class: 'check small' }, h('input', { type: 'checkbox', checked: true, onchange: (e) => { importSaves = e.target.checked; } }), h('span', null, 'Copy them into this browser too'))));

  const edition = plan.edition;
  mount(h('section', { class: 'screen narrow review' },
    h('p', { class: 'eyebrow' }, prep.images.length ? prep.images.map((i) => i.format).join(', ') : 'Files'),
    h('h1', null, ok ? 'Found LEGOLAND' : 'That’s not quite everything'),
    h('p', { class: 'lede' }, h('span', { class: 'source' }, prep.label),
      edition ? [' · ', edition.label, ' ', h('span', { class: 'tag ' + (edition.verified ? 'good' : 'warn') }, edition.verified ? 'tested' : 'untested')] : null),
    h('ul', { class: 'review-list' }, items),
    plan.problems.length ? h('div', { class: 'callout bad' }, plan.problems.map((p) => h('p', null, p))) : null,
    plan.warnings.filter((w) => !/Speech folder/.test(w)).length
      ? h('div', { class: 'callout warn' }, plan.warnings.filter((w) => !/Speech folder/.test(w)).map((w) => h('p', null, w))) : null,
    h('div', { class: 'row-actions' },
      ok ? h('button', { class: 'btn primary', onclick: () => startImport(prep, includeSpeech, importSaves, replacing) }, icon('download'), 'Install (', total, ')') : null,
      h('button', { class: 'btn ghost', onclick: () => showSetup({ replacing }) }, ok ? 'Choose something else' : 'Try other files'))),
  { nav: false });
}

function progressView(title, subtitle, onCancel) {
  const fill = h('div', { class: 'meter-fill' });
  const meterEl = h('div', { class: 'meter', role: 'progressbar', 'aria-valuemin': '0', 'aria-valuemax': '100', 'aria-valuenow': '0' }, fill);
  const pct = h('span', { class: 'progress-pct' }, '0%');
  const detail = h('span', { class: 'progress-detail' }, 'Starting…');
  const what = h('p', { class: 'progress-what muted' }, ' ');
  const rate = rateMeter();
  const el = h('section', { class: 'screen narrow progress' },
    h('h1', null, title),
    h('p', { class: 'lede' }, subtitle),
    meterEl,
    h('div', { class: 'progress-row' }, detail, pct),
    what,
    h('p', { class: 'note' }, 'Keep this tab open until it finishes.'),
    onCancel ? h('div', { class: 'row-actions' }, h('button', { class: 'btn ghost', onclick: async () => {
      if (await ask({ title: 'Stop installing?', body: 'What has been copied so far is thrown away.', confirm: 'Stop', cancel: 'Keep going', danger: true }))
        onCancel();
    } }, 'Cancel')) : null);
  const update = throttle((done, totalBytes, label) => {
    const p = totalBytes ? (100 * done) / totalBytes : 0;
    fill.style.width = p.toFixed(2) + '%';
    meterEl.setAttribute('aria-valuenow', String(Math.floor(p)));
    pct.textContent = Math.floor(p) + '%';
    const r = rate(done, totalBytes);
    detail.textContent = bytes(done) + ' of ' + bytes(totalBytes) +
      (r.rate > 0 ? ' · ' + bytes(r.rate) + '/s' : '') +
      (done > 0 && isFinite(r.left) ? ' · about ' + duration(r.left) + ' left' : '');
    if (label) what.textContent = label;
  });
  return { el, update };
}

async function runInstall(title, subtitle, work) {
  const ctl = new AbortController();
  app.busy = ctl;
  const view = progressView(title, subtitle, () => ctl.abort());
  mount(view.el, { nav: false });
  requestPersistence();              // a 240 MB install is worth asking to keep
  try {
    app.install = await work(ctl.signal, view.update);
    app.busy = null;
    toast('LEGOLAND is ready to play.', 'good');
    showMenu({ fresh: true });
  } catch (e) {
    app.busy = null;
    try { await app.store.clear(); } catch (err) { /* already empty */ }
    app.install = null;
    if (e && e.name === 'AbortError') {
      toast('Stopped. Nothing was kept.');
      showSetup();
    } else {
      console.error(e);
      showProblem('The game files couldn’t be installed', friendly(e), () => showSetup());
    }
  }
}

async function startDownload(includeSpeech, replacing) {
  if (replacing && !(await ask({ title: 'Replace the game files?', body: 'The current files are removed before the download starts. Saved games are kept.', confirm: 'Download' })))
    return;
  const m = app.manifest;
  runInstall('Downloading LEGOLAND', ((m.edition && m.edition.label) || 'LEGOLAND') + ' · ' + bytes(downloadSize(m, includeSpeech)),
    (signal, onProgress) => runDownload(m, app.dataBase, app.store, { includeSpeech, signal, onProgress }));
}

async function startImport(prep, includeSpeech, importSaves, replacing) {
  if (replacing && !(await ask({ title: 'Replace the game files?', body: 'The current files are removed before copying starts. Saved games are kept.', confirm: 'Replace' })))
    return;
  runInstall('Copying LEGOLAND into your browser', 'From ' + prep.label, async (signal, onProgress) => {
    const info = await runImport(prep, app.store, { includeSpeech, signal, onProgress });
    if (importSaves && prep.plan.saves.length) {
      try {
        const r = await writeSaveFiles(await readFoundSaves(prep));
        if (r.written.length) toast('Copied ' + plural(r.written.length, 'save file') + ' too.');
      } catch (e) {
        console.warn('saves were not copied:', e);
      }
    }
    return info;
  });
}

function showProblem(title, message, retry) {
  mount(h('section', { class: 'screen narrow' },
    badge('warn', 'amber'),
    h('h1', null, title),
    h('p', { class: 'lede' }, message),
    h('div', { class: 'row-actions' },
      retry ? h('button', { class: 'btn primary', onclick: retry }, 'Try again') : null,
      app.install ? h('button', { class: 'btn ghost', onclick: () => go('menu') }, 'Back to the menu') : null)),
  { nav: false });
}

// ---- menu ---------------------------------------------------------------------

function tile(iconName, title, text, onclick) {
  const textEl = h('span', { class: 'tile-text' }, text);
  const el = h('button', { class: 'tile', type: 'button', onclick },
    icon(iconName, 'icon tile-icon'), h('span', { class: 'tile-title' }, title), textEl);
  return { el, setText: (t) => { textEl.textContent = t; } };
}

async function showMenu({ fresh = false } = {}) {
  if (!app.install) return showSetup();
  const inst = app.install;
  const busy = h('p', { class: 'callout warn', hidden: true }, 'LEGOLAND is already open in another tab. Close it there to play here.');
  const saves = tile('saves', 'Saved games', ' ', () => go('saves'));
  const files = tile('box', 'Game files', inst.edition.label + ' · ' + bytes(inst.bytes), () => go('data'));
  const opts = tile('gear', 'Options', optionsSummary(), () => go('options'));
  mount(h('section', { class: 'screen menu' },
    h('div', { class: 'hero' },
      h('p', { class: 'eyebrow' }, fresh ? 'All set — the gates are open' : 'Welcome back'),
      h('h1', { class: 'wordmark' }, 'LEGOLAND'),
      h('p', { class: 'lede' }, 'Build rides, hire the staff, and keep your visitors smiling.'),
      h('button', { class: 'brick play', type: 'button', onclick: play },
        h('span', { class: 'studs', 'aria-hidden': 'true' }), icon('play'), h('span', null, 'Play')),
      busy),
    h('div', { class: 'tiles' }, saves.el, files.el, opts.el),
    h('details', { class: 'about' },
      h('summary', null, 'About this version'),
      h('p', null, 'This is LEGO Media’s LEGOLAND (1999) running from its decompiled source code, rebuilt for the web. The game itself is unchanged; only the parts that talked to Windows were rewritten.'),
      h('p', null, 'Not in the browser yet: the background music and the full-motion movies. Everything is saved in this browser — use Saved games to keep a backup.'))),
  { active: 'menu' });
  listSaves().then((s) => {
    const parks = s.players.reduce((n, p) => n + p.saves.length, 0);
    saves.setText(s.players.length ? plural(s.players.length, 'player') + ' · ' + plural(parks, 'saved park') : 'No saved games yet');
  }).catch(() => saves.setText('Players and parks'));
  if (await otherTabPlaying()) busy.hidden = false;
}

function optionsSummary() {
  const o = app.options;
  return [o.scale === 'integer' ? 'Sharpest size' : 'Fit to window', o.muted ? 'sound off' : 'sound on'].join(' · ');
}

async function play() {
  if (app.busy) return;
  if (!app.install) return showSetup();
  const session = new GameSession({
    store: app.store,
    install: app.install,
    options: app.options,
    confirmLeave: () => ask({
      title: 'Leave the park?',
      body: 'The game closes and you go back to the menu. Anything you haven’t saved in the game is lost.',
      confirm: 'Leave', cancel: 'Keep playing', danger: true
    })
  });
  session.onOptionsChanged = (o) => saveOptions(o);
  try {
    await session.start();
  } catch (e) {
    if (!document.body.classList.contains('playing')) {
      ask({ title: 'The park can’t open', body: friendly(e), cancel: null });
      return;
    }
    rememberExit({ kind: 'crash', detail: String((e && e.stack) || e) });
    reloadToLauncher();
  }
}

// ---- after the game -------------------------------------------------------------

function showExit(info) {
  const crash = info.kind === 'crash';
  const title = crash ? 'The game stopped unexpectedly'
    : info.kind === 'left' ? 'You left the park'
    : 'Thanks for visiting LEGOLAND!';
  const text = crash ? 'Sorry about that. Your players and saved parks are safe.'
    : info.kind === 'left' ? 'The park is closed. What you saved in the game is kept; anything unsaved is gone.'
    : 'The park is closed for the day. Your players and saved parks are kept in this browser for next time.';
  const details = crash ? [info.detail || '', ...(info.log || [])].join('\n') : '';
  mount(h('section', { class: 'screen narrow exit' + (crash ? ' crashed' : '') },
    h('div', { class: 'gate-art', 'aria-hidden': 'true' }, icon(crash ? 'warn' : 'gate', 'icon huge')),
    h('h1', null, title),
    h('p', { class: 'lede' }, text),
    h('div', { class: 'row-actions' },
      h('button', { class: 'btn primary', onclick: play }, icon('play'), 'Play again'),
      h('button', { class: 'btn', onclick: () => go('saves') }, icon('saves'), 'Saved games'),
      h('button', { class: 'btn ghost', onclick: () => go('menu') }, 'Main menu')),
    crash ? h('details', { class: 'about' },
      h('summary', null, 'What happened'),
      h('pre', { class: 'log' }, details),
      h('button', { class: 'btn small', onclick: () => navigator.clipboard.writeText(details).then(() => toast('Copied.'), () => toast('Couldn’t copy.')) }, 'Copy the details')) : null),
  { active: '' });
}

// ---- saved games ----------------------------------------------------------------

async function showSaves() {
  const locked = await otherTabPlaying();
  const list = h('div', { class: 'players' }, h('div', { class: 'spinner', 'aria-hidden': 'true' }));
  const importInput = h('input', { type: 'file', multiple: true, accept: '.zip,.txt,.sav,.sh', hidden: true,
    onchange: (e) => { const files = Array.from(e.target.files); e.target.value = ''; if (files.length) importSaves(files); } });
  mount(h('section', { class: 'screen' },
    h('div', { class: 'section-head' },
      h('div', null,
        h('h1', null, 'Saved games'),
        h('p', { class: 'lede' }, 'Players and their saved parks are kept in this browser. Export a backup to keep them safe, or to move them to another computer.')),
      h('div', { class: 'head-actions' },
        h('button', { class: 'btn', onclick: () => importInput.click(), disabled: locked }, icon('import'), 'Import…'),
        h('button', { class: 'btn', onclick: exportAll }, icon('export'), 'Export all'))),
    locked ? h('p', { class: 'callout warn' }, 'LEGOLAND is open in another tab, so saves can’t be changed here right now.') : null,
    list, importInput),
  { active: 'saves' });
  app.onDrop = locked ? null : (inputs) => importSaves(inputs.map((i) => i.file));

  let s;
  try {
    s = await listSaves();
  } catch (e) {
    list.replaceChildren(h('p', { class: 'callout bad' }, 'The saved games couldn’t be read: ' + friendly(e)));
    return;
  }
  if (!s.players.length && !s.other.length) {
    list.replaceChildren(h('div', { class: 'empty' },
      icon('saves', 'icon huge'),
      h('h2', null, 'No saved games yet'),
      h('p', null, 'Start the game and create a player. Their progress and the parks they save show up here.'),
      h('button', { class: 'btn primary', onclick: play }, icon('play'), 'Play')));
    return;
  }
  const cards = s.players.map((p) => h('article', { class: 'player' },
    h('div', { class: 'player-head' },
      h('div', { class: 'avatar', 'aria-hidden': 'true' }, (p.name.trim()[0] || '?').toUpperCase()),
      h('div', { class: 'player-id' },
        h('h2', null, p.name),
        h('p', { class: 'muted' }, 'Player ' + p.slot +
          (p.exists ? ' · ' + p.lessons + '/5 lessons · ' + p.levels + '/10 levels unlocked' : '') +
          (p.modified ? ' · ' + when(p.modified) : ''))),
      h('div', { class: 'player-actions' },
        h('button', { class: 'btn small', onclick: () => exportPlayer(p), title: 'Export ' + p.name }, icon('export'), h('span', { class: 'label' }, 'Export')),
        h('button', { class: 'btn small danger-ghost', onclick: () => removePlayer(p), disabled: locked, title: 'Delete ' + p.name }, icon('trash'), h('span', { class: 'label' }, 'Delete')))),
    p.saves.length
      ? h('ul', { class: 'saves' }, p.saves.map((sv) => h('li', null,
          h('span', { class: 'slot' }, sv.slot),
          h('span', { class: 'save-name' }, sv.name, sv.complete ? null : h('span', { class: 'tag warn' }, 'incomplete')),
          h('span', { class: 'muted save-when' }, when(sv.modified)),
          h('span', { class: 'muted save-size' }, bytes(sv.bytes)),
          h('button', { class: 'icon-btn', onclick: () => removeSave(p, sv), disabled: locked, title: 'Delete “' + sv.name + '”', 'aria-label': 'Delete saved park ' + sv.name }, icon('trash')))))
      : h('p', { class: 'muted no-saves' }, 'No saved parks.')));
  if (s.other.length) {
    cards.push(h('article', { class: 'player other' },
      h('div', { class: 'player-head' },
        h('div', { class: 'player-id' }, h('h2', null, 'Other files'),
          h('p', { class: 'muted' }, s.other.join(', '))),
        h('div', { class: 'player-actions' },
          h('button', { class: 'btn small danger-ghost', disabled: locked, onclick: async () => {
            if (await ask({ title: 'Delete these files?', body: s.other.join(', '), confirm: 'Delete', danger: true })) {
              await deleteOther(s.other);
              showSaves();
            }
          } }, icon('trash'), h('span', { class: 'label' }, 'Delete'))))));
  }
  list.replaceChildren(...cards,
    h('p', { class: 'fineprint' }, plural(s.fileCount, 'file') + ' · ' + bytes(s.bytes) + '. Saves use the game’s own profiles folder format (Profile1.txt, 1save1.sav, 1save1.sh).'));
}

const RECORD = 0x110;

async function importSaves(files) {
  const items = [], rejected = [];
  try {
    for (const f of files) {
      if (/\.zip$/i.test(f.name)) {
        for (const e of await readZipDirectory(f)) {
          if (e.dir || !canonicalSaveName(e.name)) continue;
          items.push({ name: e.name, data: new Uint8Array(await (await zipEntryBlob(f, e)).arrayBuffer()) });
        }
      } else if (canonicalSaveName(f.name)) {
        items.push({ name: f.name, data: new Uint8Array(await f.arrayBuffer()) });
      } else {
        rejected.push(f.name);
      }
    }
  } catch (e) {
    ask({ title: 'Those files couldn’t be read', body: friendly(e), cancel: null });
    return;
  }
  // A profile or header of the wrong size would be read past its end by the game.
  const valid = items.filter((it) => {
    const name = canonicalSaveName(it.name);
    const ok = /\.sav$/.test(name) ? it.data.length > 0 : it.data.length === RECORD;
    if (!ok) rejected.push(it.name.split('/').pop());
    return ok;
  });
  if (!valid.length) {
    ask({ title: 'No saved games found', body: 'Choose Profile1.txt and 1save1.sav/.sh style files from a LEGOLAND profiles folder, or a zip exported from this page.', cancel: null });
    return;
  }
  const existing = new Set((await readSaveFiles()).map((f) => f.name.toLowerCase()));
  const clashes = valid.filter((it) => existing.has(canonicalSaveName(it.name).toLowerCase()));
  if (clashes.length && !(await ask({
    title: 'Replace saved games?',
    body: plural(clashes.length, 'file') + ' with the same name already ' + (clashes.length === 1 ? 'exists' : 'exist') + ' and will be replaced: ' +
          clashes.map((c) => canonicalSaveName(c.name)).join(', ') + '.',
    confirm: 'Replace', danger: true
  }))) return;
  try {
    const r = await writeSaveFiles(valid);
    toast('Imported ' + plural(r.written.length, 'file') + (rejected.length ? ' (skipped ' + rejected.length + ')' : '') + '.', 'good');
  } catch (e) {
    ask({ title: 'The saves couldn’t be imported', body: friendly(e), cancel: null });
  }
  showSaves();
}

function stamp() {
  const d = new Date();
  return d.getFullYear() + '-' + String(d.getMonth() + 1).padStart(2, '0') + '-' + String(d.getDate()).padStart(2, '0');
}

async function exportAll() {
  const entries = await exportEntries(null);
  if (!entries.length) { toast('There’s nothing to export yet.'); return; }
  saveBlob(makeZip(entries), 'legoland-saves-' + stamp() + '.zip');
}

async function exportPlayer(p) {
  const entries = await exportEntries(p.slot);
  const safe = p.name.replace(/[^A-Za-z0-9 _-]+/g, '').trim().replace(/\s+/g, '-') || 'player' + p.slot;
  saveBlob(makeZip(entries), 'legoland-' + safe + '-' + stamp() + '.zip');
}

async function removePlayer(p) {
  const n = p.saves.length;
  if (!(await ask({
    title: 'Delete ' + p.name + '?',
    body: 'This removes the player' + (n ? ' and ' + plural(n, 'saved park') : '') + ' from this browser. It can’t be undone — export first if you might want them back.',
    confirm: 'Delete', danger: true
  }))) return;
  await deletePlayer(p.slot);
  toast(p.name + ' was deleted.');
  showSaves();
}

async function removeSave(p, sv) {
  if (!(await ask({ title: 'Delete “' + sv.name + '”?', body: 'This saved park of ' + p.name + '’s is removed from this browser. It can’t be undone.', confirm: 'Delete', danger: true })))
    return;
  await deleteSave(p.slot, sv.slot);
  toast('Saved park deleted.');
  showSaves();
}

// ---- game files -------------------------------------------------------------------

async function showData() {
  const inst = app.install;
  if (!inst) return showSetup();
  const est = await storageEstimate();
  const row = (k, v) => h('div', { class: 'kv' }, h('dt', null, k), h('dd', null, v));
  const source = inst.source.kind === 'download' ? 'Downloaded from this site' : 'Copied from ' + inst.source.label;
  const persisted = est.persisted
    ? h('span', { class: 'tag good' }, 'protected')
    : h('button', { class: 'btn small', onclick: async () => {
        const ok = await requestPersistence();
        toast(ok ? 'The browser will keep these files.' : 'The browser didn’t agree to keep them; they stay unless storage runs low.');
        showData();
      } }, 'Keep these files');
  mount(h('section', { class: 'screen narrow' },
    h('h1', null, 'Game files'),
    h('p', { class: 'lede' }, 'The files the game runs from, stored in this browser.'),
    h('dl', { class: 'kvs' },
      row('Edition', [inst.edition.label, ' ', h('span', { class: 'tag ' + (inst.edition.verified ? 'good' : 'warn') }, inst.edition.verified ? 'tested' : 'untested')]),
      row('Source', source),
      row('Installed', when(inst.installedAt)),
      row('Size', plural(inst.files, 'file') + ' · ' + bytes(inst.bytes)),
      row('Voice narration', inst.speech ? plural(inst.speech, 'clip') : 'Not installed'),
      row('Advisor animations', inst.advisorFrames ? 'Included' : 'Not included (plain panel)'),
      row('Browser storage', est.usage !== null ? bytes(est.usage) + ' used' + (est.quota ? ' of ' + bytes(est.quota) + ' allowed' : '') : 'Unknown'),
      row('Clean-up', [est.persisted ? 'The browser won’t remove these files on its own ' : 'The browser may remove these files if storage gets low ', persisted])),
    h('div', { class: 'row-actions' },
      h('button', { class: 'btn', onclick: () => showSetup({ replacing: true }) }, icon('disc'), 'Replace…'),
      h('button', { class: 'btn danger-ghost', onclick: removeData }, icon('trash'), 'Remove the game files')),
    h('p', { class: 'fineprint' }, 'Removing the game files doesn’t touch your saved games.')),
  { active: 'data' });
}

async function removeData() {
  if (await otherTabPlaying()) {
    ask({ title: 'LEGOLAND is still running', body: 'Close the game in the other tab first.', cancel: null });
    return;
  }
  if (!(await ask({ title: 'Remove the game files?', body: 'You’ll need to download them or read them from your disc again before playing. Saved games are kept.', confirm: 'Remove', danger: true })))
    return;
  await app.store.clear();
  app.install = null;
  toast('The game files were removed.');
  showSetup();
}

// ---- options ------------------------------------------------------------------------

function showOptions() {
  const o = app.options;
  const set = (k, v) => { o[k] = v; saveOptions(o); };
  const toggle = (key, title, text, disabled = false) => h('label', { class: 'option' + (disabled ? ' disabled' : '') },
    h('span', { class: 'option-text' }, h('strong', null, title), h('span', { class: 'muted' }, text)),
    h('input', { type: 'checkbox', role: 'switch', class: 'switch', checked: !!o[key], disabled, onchange: (e) => set(key, e.target.checked) }));
  const radio = (key, value, title, text) => h('label', { class: 'option' },
    h('span', { class: 'option-text' }, h('strong', null, title), h('span', { class: 'muted' }, text)),
    h('input', { type: 'radio', name: key, value, checked: o[key] === value, onchange: () => set(key, value) }));
  const narrationInstalled = !!(app.install && app.install.speech);
  mount(h('section', { class: 'screen narrow' },
    h('h1', null, 'Options'),
    h('p', { class: 'lede' }, 'These take effect the next time you press Play.'),
    h('fieldset', { class: 'options' }, h('legend', null, 'Picture'),
      radio('scale', 'fit', 'Fit the window', 'As large as the window allows.'),
      radio('scale', 'integer', 'Sharpest', 'Whole-number scaling, so every game pixel stays square.'),
      toggle('smooth', 'Smooth the picture', 'Blend pixels when scaling instead of keeping them crisp.')),
    h('fieldset', { class: 'options' }, h('legend', null, 'Sound'),
      h('label', { class: 'option' },
        h('span', { class: 'option-text' }, h('strong', null, 'Sound'), h('span', { class: 'muted' }, 'You can also switch it during the game.')),
        h('input', { type: 'checkbox', role: 'switch', class: 'switch', checked: !o.muted, onchange: (e) => set('muted', !e.target.checked) })),
      toggle('narration', 'Voice narration', narrationInstalled ? 'The advisor and the lessons speak. Off saves about 60 MB of memory.' : 'The voice clips aren’t installed.', !narrationInstalled)),
    h('fieldset', { class: 'options' }, h('legend', null, 'Playing'),
      toggle('background', 'Keep running in a background tab', 'The park keeps going when you switch tabs. Uses more battery.'),
      toggle('fps', 'Show the frame rate', 'A small counter in the corner while you play.'))),
  { active: 'options' });
}

// ---- start ----------------------------------------------------------------------------

function showUnsupported(reason) {
  mount(h('section', { class: 'screen narrow' },
    badge('warn', 'amber'),
    h('h1', null, 'This browser can’t run LEGOLAND'),
    h('p', { class: 'lede' }, reason),
    h('p', null, 'Try a current version of Chrome, Edge, Firefox or Safari, outside private browsing.')),
  { nav: false });
}

function wireChrome() {
  document.getElementById('brand').addEventListener('click', () => go(app.install ? 'menu' : 'setup'));
  for (const b of document.querySelectorAll('#nav button')) b.addEventListener('click', () => go(b.dataset.go));
  // Files dropped anywhere on the page go to whatever the current screen does
  // with files, if anything.
  document.addEventListener('dragover', (e) => {
    if (!app.onDrop || !e.dataTransfer || !Array.from(e.dataTransfer.types || []).includes('Files')) return;
    e.preventDefault();
    document.body.classList.add('dragging');
  });
  document.addEventListener('dragleave', (e) => {
    if (!e.relatedTarget) document.body.classList.remove('dragging');
  });
  document.addEventListener('drop', async (e) => {
    document.body.classList.remove('dragging');
    if (!app.onDrop || !e.dataTransfer) return;
    e.preventDefault();
    const handler = app.onDrop;
    const inputs = await filesFromDrop(e.dataTransfer);
    if (inputs.length) handler(inputs);
  });
}

async function boot() {
  wireChrome();
  if (typeof WebAssembly !== 'object') return showUnsupported('It has no WebAssembly.');
  if (!('indexedDB' in window) || !indexedDB) return showUnsupported('It can’t store files for this site (IndexedDB is unavailable — private browsing often does this).');
  try {
    app.store = await openDataStore();
    app.install = await app.store.getInstall();
  } catch (e) {
    return showUnsupported(friendly(e));
  }
  const last = takeExit();
  if (last) return showExit(last);
  if (!app.install) return showSetup();
  showMenu();
}

boot();
