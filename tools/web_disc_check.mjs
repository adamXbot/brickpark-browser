#!/usr/bin/env node
// Run the player page's disc importer on real disc images, under node, and say
// whether what it would install is what the game needs.
//
//   node tools/web_disc_check.mjs [--gamedata DIR] [--hash] IMAGE...
//
// For each image: open it with src/web/js/iso9660.js (the same file the
// page runs), classify it with discplan.js, expand main.z with installshield.js,
// and compare every byte that would be installed with the extraction in
// gamedata/ (gamedata/main is tools/iscab.py's output; gamedata/disc holds the
// volumes and Speech). --hash also compares the three volumes byte for byte
// (reads ~160 MB per image).
//
// Exit status: 0 when every image that is a supported LEGOLAND disc installs
// byte-identical files, 1 otherwise.

import { openAsBlob, existsSync, readdirSync, readFileSync, statSync } from 'node:fs';
import { createHash } from 'node:crypto';
import path from 'node:path';
import { openIso } from '../src/web/js/iso9660.js';
import { listInstallShieldZ, extractMember } from '../src/web/js/installshield.js';
import { planSources, mergeInstall, installGroup } from '../src/web/js/discplan.js';

const args = process.argv.slice(2);
let gamedata = path.resolve(path.dirname(new URL(import.meta.url).pathname), '../../gamedata');
let hash = false;
const images = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--gamedata') gamedata = path.resolve(args[++i]);
  else if (args[i] === '--hash') hash = true;
  else images.push(args[i]);
}
if (!images.length) {
  console.error('usage: web_disc_check.mjs [--gamedata DIR] [--hash] IMAGE...');
  process.exit(2);
}

function indexDir(dir) {
  const map = new Map();
  if (!existsSync(dir)) return map;
  for (const name of readdirSync(dir)) {
    const p = path.join(dir, name);
    if (statSync(p).isFile()) map.set(name.toLowerCase(), p);
  }
  return map;
}
const mainIndex = indexDir(path.join(gamedata, 'main'));
const discIndex = indexDir(path.join(gamedata, 'disc'));
const speechIndex = indexDir(path.join(gamedata, 'disc', 'Speech'));

function same(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

async function sha256Of(file, size) {
  const h = createHash('sha256');
  const step = 16 * 1024 * 1024;
  for (let off = 0; off < size; off += step) h.update(await file.read(off, Math.min(step, size - off)));
  return h.digest('hex');
}

let failed = false;
for (const image of images) {
  const t0 = performance.now();
  console.log('\n== ' + image);
  const blob = await openAsBlob(image);
  let iso;
  try {
    iso = await openIso(blob, path.basename(image));
  } catch (e) {
    console.log('   cannot read: ' + e.message);
    failed = true;
    continue;
  }
  if (!iso) {
    console.log('   not a disc image');
    continue;
  }
  const files = iso.files();
  console.log(`   ${iso.format}, volume "${iso.volumeId}", ${iso.joliet ? 'Joliet' : 'primary'} names, ` +
              `${files.length} files, opened in ${(performance.now() - t0).toFixed(0)} ms`);
  const plan = planSources(files.map((f) => ({ path: f.path, size: f.size, file: f })));
  for (const [key, v] of Object.entries(plan.volumes))
    console.log(`   volume ${key.padEnd(14)} ${String(v.entry.size).padStart(10)}  from ${v.entry.path}`);
  console.log('   edition: ' + (plan.edition ? plan.edition.label + (plan.edition.verified ? ' (verified)' : ' (untested)') : '-'));
  console.log(`   archive: ${plan.archive ? plan.archive.path : '-'}; loose install files: ${plan.install.length}; ` +
              `speech: ${plan.speech.length}; saves: ${plan.saves.length}; cpack: ${plan.cpack}`);
  for (const p of plan.problems) console.log('   PROBLEM: ' + p);
  for (const w of plan.warnings) console.log('   warning: ' + w);
  if (plan.problems.length) continue;

  let members = [];
  let archive = null;
  if (plan.archive) {
    const t1 = performance.now();
    archive = await plan.archive.file.read();
    members = listInstallShieldZ(archive);
    console.log(`   main.z: ${members.length} members listed in ${(performance.now() - t1).toFixed(0)} ms`);
  }
  const merged = mergeInstall(plan, members);
  const groups = Object.entries(merged.counts).map(([g, n]) => g + ' ' + n).join(', ');
  console.log(`   install: ${merged.items.length} files (${groups})` +
              (merged.short.length ? '; SHORT in ' + merged.short.join(', ') : '') +
              (merged.lacking.length ? '; LACKING ' + merged.lacking.join(', ') : ''));

  // Every installed byte against gamedata/.
  const t2 = performance.now();
  let identical = 0, bytes = 0;
  const differ = [], unknown = [];
  for (const item of merged.items) {
    const data = item.member ? extractMember(archive, item.member) : await item.entry.file.read();
    bytes += data.length;
    const ref = mainIndex.get(path.basename(item.dest).toLowerCase());
    if (!ref) { unknown.push(item.dest); continue; }
    if (same(data, readFileSync(ref))) identical++;
    else differ.push(item.dest);
  }
  console.log(`   install bytes: ${identical}/${merged.items.length} identical to gamedata/main, ` +
              `${(bytes / 1048576).toFixed(1)} MB in ${(performance.now() - t2).toFixed(0)} ms`);
  if (differ.length) console.log('   DIFFER: ' + differ.slice(0, 12).join(', ') + (differ.length > 12 ? ' ...' : ''));
  if (unknown.length) console.log('   not in gamedata/main: ' + unknown.slice(0, 12).join(', '));

  // Speech: sizes against gamedata/disc/Speech, a sample byte-compared.
  let speechSame = 0, speechChecked = 0;
  for (const [i, s] of plan.speech.entries()) {
    const ref = speechIndex.get(path.basename(s.dest).toLowerCase());
    if (!ref) continue;
    if (i % 50 === 0) {
      speechChecked++;
      if (same(await s.entry.file.read(), readFileSync(ref))) speechSame++;
    }
  }
  if (plan.speech.length)
    console.log(`   speech: ${plan.speech.length} files; ${speechSame}/${speechChecked} sampled files identical to gamedata/disc/Speech`);

  const verified = plan.edition && plan.edition.verified;
  for (const v of Object.values(plan.volumes)) {
    const ref = discIndex.get(path.basename(v.dest).toLowerCase());
    const sizeOk = ref && statSync(ref).size === v.entry.size;
    let line = `   ${path.basename(v.dest)}: size ${sizeOk ? 'matches' : 'differs from'} gamedata/disc`;
    if (hash && ref && sizeOk) {
      const a = await sha256Of(v.entry.file, v.entry.size);
      const b = createHash('sha256').update(readFileSync(ref)).digest('hex');
      line += a === b ? ', sha256 identical' : ', SHA256 DIFFERS';
      if (a !== b && verified) failed = true;
    }
    console.log(line);
  }
  if (verified && (differ.length || merged.lacking.length || merged.short.length)) failed = true;
  console.log(`   done in ${((performance.now() - t0) / 1000).toFixed(1)} s`);
}
process.exit(failed ? 1 : 0);
