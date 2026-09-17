// LEGOLAND portable -- getting the game's files into the browser.
//
// Two ways in, one result (store.js's records in discplan.js's layout):
//
//   download   the site's data pack (tools/web_datapack.py): core.tar,
//              speech.tar and the three volumes, streamed into IndexedDB with
//              the manifest's sizes and hashes checked on the way
//   import     whatever the player hands the page -- a disc image (.iso/.bin),
//              a zip of one, the files of a copied CD, or an installed LEGOLAND
//              folder -- read where it lies; main.z is expanded here
//
// Nothing the player picks leaves the computer: File objects are read with
// Blob.slice, in place.

import { openIso } from './iso9660.js';
import { readZipDirectory, zipEntryBlob } from './zip.js';
import { listInstallShieldZ, extractMember } from './installshield.js';
import { planSources, mergeInstall } from './discplan.js';
import { parseTar } from './tar.js';
import { CHUNK } from './store.js';
import { canonicalSaveName } from './saves.js';

export const FORMAT = 1;

const IMAGE_RE = /\.(iso|bin|img|mdf|nrg)$/i;

class BlobSource {
  constructor(blob, path) {
    this.src = blob;
    this.path = path;
    this.size = blob.size;
  }
  async read(offset = 0, length = this.size - offset) {
    return new Uint8Array(await this.src.slice(offset, offset + length).arrayBuffer());
  }
}

// A zip member, expanded on first use (a stored member is a slice, so free).
class ZipSource {
  constructor(zip, entry) {
    this.zip = zip;
    this.entry = entry;
    this.path = entry.name;
    this.size = entry.size;
    this.blobPromise = null;
  }
  async read(offset = 0, length = this.size - offset) {
    if (!this.blobPromise) this.blobPromise = zipEntryBlob(this.zip, this.entry);
    const blob = await this.blobPromise;
    return new Uint8Array(await blob.slice(offset, offset + length).arrayBuffer());
  }
}

function throwIfAborted(signal) {
  if (signal && signal.aborted) {
    const e = new Error('Cancelled');
    e.name = 'AbortError';
    throw e;
  }
}

// ---- what the player picked -> [{ file, path }] ---------------------------

export function filesFromInput(fileList) {
  return Array.from(fileList).map((file) => ({ file, path: file.webkitRelativePath || file.name }));
}

// A drop can carry folders; walk them.
export async function filesFromDrop(dataTransfer) {
  const items = Array.from(dataTransfer.items || []);
  const entries = items.map((it) => (it.webkitGetAsEntry ? it.webkitGetAsEntry() : null)).filter(Boolean);
  if (!entries.length) return filesFromInput(dataTransfer.files || []);
  const out = [];
  const readAllEntries = (dir) => new Promise((resolve, reject) => {
    const reader = dir.createReader();
    const all = [];
    const next = () => reader.readEntries((batch) => {
      if (!batch.length) resolve(all);
      else { all.push(...batch); next(); }
    }, reject);
    next();
  });
  const walk = async (entry, prefix) => {
    if (entry.isFile) {
      const file = await new Promise((resolve, reject) => entry.file(resolve, reject));
      out.push({ file, path: prefix + entry.name });
    } else if (entry.isDirectory) {
      for (const child of await readAllEntries(entry)) await walk(child, prefix + entry.name + '/');
    }
  };
  for (const e of entries) await walk(e, '');
  return out;
}

// ---- [{ file, path }] -> planner entries ------------------------------------

async function gatherEntries(inputs, status) {
  const entries = [];
  const images = [];
  const addImage = async (blob, name) => {
    status('Opening ' + name + '…');
    const iso = await openIso(blob, name);
    if (!iso) return false;
    images.push({ name, format: iso.format, volume: iso.volumeId });
    for (const f of iso.files()) entries.push({ path: f.path, size: f.size, file: f });
    return true;
  };
  for (const { file, path } of inputs) {
    const lower = path.toLowerCase();
    if (IMAGE_RE.test(lower)) {
      if (!(await addImage(file, file.name)))
        throw new Error(file.name + ' is not a CD image this page can read. Disc images must be ISO 9660 (.iso) or raw (.bin).');
      continue;
    }
    if (lower.endsWith('.zip')) {
      status('Reading ' + file.name + '…');
      const members = (await readZipDirectory(file)).filter((m) => !m.dir);
      const inner = members.filter((m) => IMAGE_RE.test(m.name));
      if (inner.length) {
        for (const m of inner) {
          status('Unpacking ' + m.name + ' from ' + file.name + '…');
          const blob = await zipEntryBlob(file, m);
          if (!(await addImage(blob, m.name)))
            throw new Error(m.name + ' (inside ' + file.name + ') is not a CD image this page can read.');
        }
      } else {
        for (const m of members) entries.push({ path: m.name, size: m.size, file: new ZipSource(file, m) });
      }
      continue;
    }
    if (lower.endsWith('.cue')) continue;       // the .bin holds the data
    entries.push({ path, size: file.size, file: new BlobSource(file, path) });
  }
  return { entries, images };
}

// Look at the player's files and say what an install from them would be. Reads
// directories and main.z; copies nothing.
export async function prepareImport(inputs, status = () => {}) {
  if (!inputs.length) throw new Error('Nothing was chosen.');
  const { entries, images } = await gatherEntries(inputs, status);
  status('Looking for the game files…');
  const plan = planSources(entries);
  let archive = null, merged = null;
  if (!plan.problems.length) {
    let members = [];
    if (plan.archive) {
      status('Reading main.z…');
      archive = await plan.archive.file.read();
      members = listInstallShieldZ(archive);
    }
    merged = mergeInstall(plan, members);
    if (merged.lacking.length)
      plan.problems.push('Some of the game\'s install files could not be found (' + merged.lacking.join(', ') + ').');
    else if (merged.short.length)
      plan.warnings.push('The install files look incomplete (' + merged.short.join(', ') + '), so parts of the game may not work.');
  }
  const label = images.length === 1 ? images[0].name
    : inputs.length === 1 ? inputs[0].path
    : (inputs[0].path.split('/')[0] || 'your files');
  return {
    plan, merged, archive, images, label,
    installBytes: merged ? merged.items.reduce((n, it) => n + (it.member ? it.member.size : it.entry.size), 0) : 0,
    volumeBytes: Object.values(plan.volumes).reduce((n, v) => n + v.entry.size, 0),
    speechBytes: plan.speech.reduce((n, s) => n + s.entry.size, 0)
  };
}

// Copy an import into the store. onProgress(done, total, label).
export async function runImport(prep, store, { includeSpeech = true, onProgress, signal } = {}) {
  const { plan, merged, archive } = prep;
  const speech = includeSpeech ? plan.speech : [];
  const total = prep.installBytes + prep.volumeBytes + (includeSpeech ? prep.speechBytes : 0);
  let done = 0;
  const tick = (n, label) => { done += n; if (onProgress) onProgress(done, total, label); };

  await store.clear();

  // Small files in batches; each batch is read, then written in one transaction.
  const smallFiles = async (items, label) => {
    let batch = [], batchBytes = 0;
    const flush = async () => {
      if (!batch.length) return;
      await store.putSmallFiles(batch);
      tick(batchBytes, label);
      batch = [];
      batchBytes = 0;
    };
    for (const it of items) {
      throwIfAborted(signal);
      const data = it.member ? extractMember(archive, it.member) : await it.entry.file.read();
      if (data.length > CHUNK) {
        await flush();
        const w = store.writer(it.dest, data.length);
        await w.write(data);
        await w.close();
        tick(data.length, label);
        continue;
      }
      batch.push({ path: it.dest, data });
      batchBytes += data.length;
      if (batchBytes >= 16 * 1024 * 1024) await flush();
    }
    await flush();
  };
  await smallFiles(merged.items, 'Install files');
  await smallFiles(speech, 'Voice narration');

  for (const v of Object.values(plan.volumes)) {
    const size = v.entry.size;
    const w = store.writer(v.dest, size);
    for (let off = 0; off < size; off += CHUNK) {
      throwIfAborted(signal);
      const piece = await v.entry.file.read(off, Math.min(CHUNK, size - off));
      await w.write(piece);
      tick(piece.length, v.dest.split('/').pop());
    }
    await w.close();
  }

  const info = {
    format: FORMAT,
    edition: { id: plan.edition.id, label: plan.edition.label, verified: plan.edition.verified },
    source: { kind: 'disc', label: prep.label, images: prep.images },
    files: merged.items.length + speech.length + 3,
    bytes: total,
    speech: speech.length,
    advisorFrames: 0,
    installedAt: new Date()
  };
  await store.setInstall(info);
  return info;
}

// Saves found in an installed folder, ready for saves.js writeSaveFiles.
export async function readFoundSaves(prep) {
  const out = [];
  for (const entry of prep.plan.saves) {
    if (!canonicalSaveName(entry.path)) continue;
    out.push({ name: entry.path.split('/').pop(), data: await entry.file.read() });
  }
  return out;
}

// ---- the site's data pack -----------------------------------------------------

export async function fetchManifest(base) {
  try {
    const res = await fetch(base + 'manifest.json', { cache: 'no-cache' });
    if (!res.ok) return null;
    const m = await res.json();
    if (m.format !== FORMAT || !Array.isArray(m.packs)) return null;
    return m;
  } catch (e) {
    return null;
  }
}

export function downloadSize(manifest, includeSpeech) {
  return manifest.packs
    .filter((p) => p.required || (includeSpeech && p.id === 'speech'))
    .reduce((n, p) => n + p.bytes, 0);
}

function hex(buffer) {
  return Array.from(new Uint8Array(buffer), (b) => b.toString(16).padStart(2, '0')).join('');
}

export async function runDownload(manifest, base, store, { includeSpeech = true, onProgress, signal } = {}) {
  const packs = manifest.packs.filter((p) => p.required || (includeSpeech && p.id === 'speech'));
  const total = packs.reduce((n, p) => n + p.bytes, 0);
  let done = 0, files = 0, speech = 0;
  const tick = (n, label) => { done += n; if (onProgress) onProgress(done, total, label); };
  // The pack's own generation time busts a stale HTTP cache after a rebuild.
  const bust = manifest.generated ? '?v=' + encodeURIComponent(manifest.generated) : '';

  await store.clear();
  for (const p of packs) {
    throwIfAborted(signal);
    const res = await fetch(base + p.url + bust, { signal });
    if (!res.ok || !res.body) throw new Error('Could not download ' + p.url + ' (HTTP ' + res.status + ').');
    const reader = res.body.getReader();
    if (p.kind === 'file') {
      const w = store.writer(p.dest, p.bytes);
      for (;;) {
        const { done: end, value } = await reader.read();
        if (end) break;
        await w.write(value);
        tick(value.length, p.title);
      }
      await w.close();                         // checks the byte count
      files++;
    } else if (p.kind === 'tar') {
      const buf = new Uint8Array(p.bytes);
      let o = 0;
      for (;;) {
        const { done: end, value } = await reader.read();
        if (end) break;
        if (o + value.length > buf.length) throw new Error(p.url + ' is larger than the manifest says.');
        buf.set(value, o);
        o += value.length;
        tick(value.length, p.title);
      }
      if (o !== p.bytes) throw new Error(p.url + ' arrived incomplete (' + o + ' of ' + p.bytes + ' bytes).');
      if (p.sha256 && globalThis.crypto && crypto.subtle) {
        const digest = hex(await crypto.subtle.digest('SHA-256', buf));
        if (digest !== p.sha256) throw new Error(p.url + ' is damaged (checksum mismatch). Try downloading again.');
      }
      const members = parseTar(buf);
      const small = [];
      for (const m of members) {
        const data = buf.subarray(m.offset, m.offset + m.size);
        if (data.length > CHUNK) {
          const w = store.writer(m.name, data.length);
          await w.write(data);
          await w.close();
        } else {
          small.push({ path: m.name, data });
        }
      }
      await store.putSmallFiles(small);
      files += members.length;
      if (p.id === 'speech') speech = members.length;
    }
  }
  const info = {
    format: FORMAT,
    edition: Object.assign({ verified: manifest.edition && manifest.edition.id === 'en' }, manifest.edition),
    source: { kind: 'download', label: 'this site', generated: manifest.generated },
    files,
    bytes: total,
    speech,
    advisorFrames: manifest.advisorFrames || 0,
    installedAt: new Date()
  };
  await store.setInstall(info);
  return info;
}
