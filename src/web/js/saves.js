// LEGOLAND portable -- the player's profiles and saved parks, managed from the page.
//
// The game keeps them where the original did, in its profiles\ folder
// (profiles.c): Profile1.txt .. Profile8.txt, one 0x110-byte record per player,
// and for each player up to eight saved parks, "<p>save<s>.sav" (the park) plus
// "<p>save<s>.sh" (a 0x110-byte header: the name typed in the save dialog). In the
// browser that folder is /gamedata/profiles, an Emscripten IDBFS mount (main.c),
// so every file lives in an IndexedDB database the game syncs with:
//
//   database "/gamedata/profiles", version 21, store "FILE_DATA", index "timestamp"
//   key "/gamedata/profiles/Profile1.txt" -> { timestamp: Date, mode, contents }
//
// (libidbfs.js, Emscripten 6.0.9). This module reads and writes that database
// directly, so saves can be listed, exported, imported and deleted without the
// game running -- the one rule is that the game must NOT be running in this tab
// or another one while it does, because IDBFS reconciles by timestamp and a
// running game's next sync would put back what the page deleted. app.js holds a
// Web Lock for the game's lifetime and every writer here checks it.

const DB_NAME = '/gamedata/profiles';
const DB_VERSION = 21;
const STORE = 'FILE_DATA';
const PREFIX = '/gamedata/profiles/';
const FILE_MODE = 0o100644;   // what a Profile1.txt the game wrote carries (measured)

export const SAVE_FILE_RE = /^(profile([1-8])\.txt|([1-8])save([1-8])\.(sav|sh))$/i;

function request(r) {
  return new Promise((resolve, reject) => {
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
}

function finished(tx) {
  return new Promise((resolve, reject) => {
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error || new Error('saving to browser storage failed'));
    tx.onabort = () => reject(tx.error || new Error('saving to browser storage was aborted'));
  });
}

// Created exactly as IDBFS.getDB creates it, so a database the page makes first
// is one the game adopts without an upgrade.
function openDb() {
  return new Promise((resolve, reject) => {
    const r = indexedDB.open(DB_NAME, DB_VERSION);
    r.onupgradeneeded = () => {
      const db = r.result;
      const store = db.objectStoreNames.contains(STORE)
        ? r.transaction.objectStore(STORE)
        : db.createObjectStore(STORE);
      if (!store.indexNames.contains('timestamp'))
        store.createIndex('timestamp', 'timestamp', { unique: false });
    };
    r.onsuccess = () => {
      const db = r.result;
      db.onversionchange = () => db.close();
      resolve(db);
    };
    r.onerror = () => reject(r.error || new Error('could not open the saves database'));
    r.onblocked = () => reject(new Error('The saves are in use in another tab.'));
  });
}

// The name the game itself would give a file, whatever case it arrived in: the
// game resolves READS case-insensitively but CREATES the exact spelling of its
// sprintf formats, so a "PROFILE1.TXT" copied off a Windows disk would otherwise
// sit next to the "Profile1.txt" the game writes.
export function canonicalSaveName(name) {
  const base = name.split(/[\\/]/).pop();
  const m = SAVE_FILE_RE.exec(base);
  if (!m) return null;
  if (m[2]) return 'Profile' + m[2] + '.txt';
  return m[3] + 'save' + m[4] + '.' + m[5].toLowerCase();
}

function cString(u8, start, max) {
  let s = '';
  for (let i = start; i < start + max && i < u8.length && u8[i]; i++) {
    const c = u8[i];
    // Windows-1252 covers what the game's name editor can type; keep it simple.
    s += String.fromCharCode(c);
  }
  return s;
}

// A 0x110-byte profile record (profiles.c / saveprof.c `Profile`).
export function parseProfileRecord(u8) {
  if (!u8 || u8.length < 0x43) return null;
  const flags = Array.from(u8.subarray(0x34, 0x43));
  // screens3.c NormaliseLevelsDone: fifteen "reachable" bytes, the five lessons
  // first (mapscreen3.c's low-progress markers) and then the ten game levels;
  // lesson 1 is always reachable.
  return {
    name: cString(u8, 0, 0x1e),
    lessons: Math.max(1, flags.slice(0, 5).filter(Boolean).length),
    levels: flags.slice(5, 15).filter(Boolean).length,
    volumes: u8.length >= 0x34
      ? { speech: new DataView(u8.buffer, u8.byteOffset).getInt32(0x28, true),
          music: new DataView(u8.buffer, u8.byteOffset).getInt32(0x2c, true),
          effects: new DataView(u8.buffer, u8.byteOffset).getInt32(0x30, true) }
      : null
  };
}

// Every file in the profiles folder: [{ name, contents, timestamp }].
export async function readSaveFiles() {
  const db = await openDb();
  try {
    const tx = db.transaction(STORE, 'readonly');
    const store = tx.objectStore(STORE);
    const keys = await request(store.getAllKeys());
    const values = await request(store.getAll());
    const out = [];
    keys.forEach((key, i) => {
      const v = values[i];
      if (typeof key !== 'string' || !key.startsWith(PREFIX) || !v || !v.contents) return;
      const name = key.slice(PREFIX.length);
      if (name.includes('/')) return;
      out.push({ name, contents: new Uint8Array(v.contents), timestamp: v.timestamp instanceof Date ? v.timestamp : new Date(v.timestamp) });
    });
    return out;
  } finally {
    db.close();
  }
}

// The folder, understood: players with their saved parks, plus anything that
// does not belong to a player (a save whose Profile file is gone).
export async function listSaves() {
  const files = await readSaveFiles();
  const byName = new Map(files.map((f) => [f.name.toLowerCase(), f]));
  const players = [];
  const claimed = new Set();
  for (let slot = 1; slot <= 8; slot++) {
    const pf = byName.get('profile' + slot + '.txt');
    const saves = [];
    for (let s = 1; s <= 8; s++) {
      const sav = byName.get(slot + 'save' + s + '.sav');
      const sh = byName.get(slot + 'save' + s + '.sh');
      if (!sav && !sh) continue;
      if (sav) claimed.add(sav.name.toLowerCase());
      if (sh) claimed.add(sh.name.toLowerCase());
      const header = sh ? parseProfileRecord(sh.contents) : null;
      saves.push({
        slot: s,
        name: header && header.name ? header.name : '(no name)',
        complete: !!(sav && sh),
        modified: (sav || sh).timestamp,
        bytes: (sav ? sav.contents.length : 0) + (sh ? sh.contents.length : 0)
      });
    }
    if (!pf && !saves.length) continue;
    if (pf) claimed.add(pf.name.toLowerCase());
    const rec = pf ? parseProfileRecord(pf.contents) : null;
    players.push({
      slot,
      exists: !!pf,
      name: rec && rec.name ? rec.name : pf ? '(no name)' : '(player file missing)',
      lessons: rec ? rec.lessons : 0,
      levels: rec ? rec.levels : 0,
      modified: pf ? pf.timestamp : null,
      saves
    });
  }
  const other = files.filter((f) => !claimed.has(f.name.toLowerCase())).map((f) => f.name);
  return { players, other, fileCount: files.length, bytes: files.reduce((n, f) => n + f.contents.length, 0) };
}

// Write save files. items: [{ name, data: Uint8Array, mtime?: Date }]; names are
// canonicalised and anything that is not a save file is skipped and reported.
export async function writeSaveFiles(items) {
  const accepted = [], skipped = [];
  for (const it of items) {
    const name = canonicalSaveName(it.name);
    if (!name) { skipped.push(it.name); continue; }
    accepted.push({ name, data: it.data, mtime: it.mtime });
  }
  if (!accepted.length) return { written: [], skipped };
  const db = await openDb();
  try {
    const tx = db.transaction(STORE, 'readwrite');
    const store = tx.objectStore(STORE);
    // A save arriving in a different case would otherwise leave the old
    // spelling behind as a second copy.
    const keys = await request(store.getAllKeys());
    const now = new Date();
    for (const it of accepted) {
      for (const k of keys) {
        if (typeof k === 'string' && k.toLowerCase() === (PREFIX + it.name).toLowerCase() && k !== PREFIX + it.name)
          store.delete(k);
      }
      // A FRESH timestamp, never the file's own: IDBFS copies an entry into the
      // game's file system only when its timestamp differs from the local one.
      store.put({ timestamp: now, mode: FILE_MODE, contents: it.data }, PREFIX + it.name);
    }
    await finished(tx);
    return { written: accepted.map((a) => a.name), skipped };
  } finally {
    db.close();
  }
}

async function deleteNames(names) {
  const db = await openDb();
  try {
    const tx = db.transaction(STORE, 'readwrite');
    const store = tx.objectStore(STORE);
    const keys = await request(store.getAllKeys());
    const wanted = new Set(names.map((n) => (PREFIX + n).toLowerCase()));
    let n = 0;
    for (const k of keys) {
      if (typeof k === 'string' && wanted.has(k.toLowerCase())) { store.delete(k); n++; }
    }
    await finished(tx);
    return n;
  } finally {
    db.close();
  }
}

export function deletePlayer(slot) {
  const names = ['Profile' + slot + '.txt'];
  for (let s = 1; s <= 8; s++) names.push(slot + 'save' + s + '.sav', slot + 'save' + s + '.sh');
  return deleteNames(names);
}

export function deleteSave(player, slot) {
  return deleteNames([player + 'save' + slot + '.sav', player + 'save' + slot + '.sh']);
}

export function deleteOther(names) {
  return deleteNames(names);
}

// The files to export for one player (or every file when slot is null), as zip
// members laid out like the original game's own folder.
export async function exportEntries(slot = null) {
  const files = await readSaveFiles();
  return files
    .filter((f) => {
      if (slot === null) return true;
      const m = SAVE_FILE_RE.exec(f.name);
      return m && (m[2] === String(slot) || m[3] === String(slot));
    })
    .map((f) => ({ name: 'profiles/' + f.name, data: f.contents, mtime: f.timestamp }));
}
