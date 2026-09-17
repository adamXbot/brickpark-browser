#!/usr/bin/env node
// Asset-free tests of the player page's data libraries (src/web/js).
//
//   node tests/web/test_web_libs.mjs
//
// Every input is synthesised here, so CI runs it with no gamedata. The same
// libraries against the REAL discs are tools/web_disc_check.mjs.

import assert from 'node:assert/strict';
import { deflateRawSync } from 'node:zlib';
import { explode, listInstallShieldZ, extractMember } from '../../src/web/js/installshield.js';
import { parseTar } from '../../src/web/js/tar.js';
import { makeZip, readZipDirectory, zipEntryBlob, crc32 } from '../../src/web/js/zip.js';
import { openIso } from '../../src/web/js/iso9660.js';
import { installPath, planSources, mergeInstall, COASTER_DIR } from '../../src/web/js/discplan.js';
import { canonicalSaveName, parseProfileRecord } from '../../src/web/js/saves.js';

const tests = [];
const test = (name, fn) => tests.push({ name, fn });
const latin1 = (u8) => Buffer.from(u8).toString('latin1');
const bytes = (s) => new Uint8Array(Buffer.from(s, 'latin1'));

function le32(u8, o, v) { u8[o] = v; u8[o + 1] = v >>> 8; u8[o + 2] = v >>> 16; u8[o + 3] = v >>> 24; }
function be32(u8, o, v) { u8[o] = v >>> 24; u8[o + 1] = v >>> 16; u8[o + 2] = v >>> 8; u8[o + 3] = v; }

// ---- DCL explode ----------------------------------------------------------

// zlib's contrib/blast reference stream.
const BLAST_VECTOR = new Uint8Array([0x00, 0x04, 0x82, 0x24, 0x25, 0x8f, 0x80, 0x7f]);

test('explode: the blast reference vector', () => {
  assert.equal(latin1(explode(BLAST_VECTOR, 0, 64)), 'AIAIAIAIAIAIA');
});

test('explode: stops at the recorded size', () => {
  assert.equal(latin1(explode(BLAST_VECTOR, 0, 5)), 'AIAIA');
});

test('explode: rejects a stream that is not DCL', () => {
  assert.throws(() => explode(new Uint8Array([7, 7, 0, 0]), 0, 4), /not a DCL stream/);
});

// ---- InstallShield Z ------------------------------------------------------

function fakeArchive(names) {
  // [sig][pad][one DCL stream per member][meta][len][name]...[meta][0]
  const streamsAt = 16;
  const dirAt = streamsAt + BLAST_VECTOR.length * names.length;
  let size = dirAt;
  for (const n of names) size += 42 + 1 + n.length;
  size += 42 + 1 + 64;
  const u8 = new Uint8Array(size);
  le32(u8, 0, 0x8c655d13);
  let o = dirAt;
  names.forEach((n, i) => {
    u8.set(BLAST_VECTOR, streamsAt + i * BLAST_VECTOR.length);
    le32(u8, o + 16, 13);                              // expanded
    le32(u8, o + 20, BLAST_VECTOR.length);             // stored
    le32(u8, o + 24, streamsAt + i * BLAST_VECTOR.length);
    o += 42;
    u8[o] = n.length;
    u8.set(bytes(n), o + 1);
    o += 1 + n.length;
  });
  return u8;
}

test('InstallShield Z: each name is described by the PRECEDING block', () => {
  const z = fakeArchive(['Lego.TTF', 'segtheme1.sgt', 'octo.bnv']);
  const members = listInstallShieldZ(z);
  assert.deepEqual(members.map((m) => m.name), ['Lego.TTF', 'segtheme1.sgt', 'octo.bnv']);
  assert.deepEqual(members.map((m) => m.offset), [16, 24, 32]);
  for (const m of members) assert.equal(latin1(extractMember(z, m)), 'AIAIAIAIAIAIA');
});

test('InstallShield Z: a wrong signature is refused', () => {
  const z = fakeArchive(['Lego.TTF', 'segtheme1.sgt']);
  z[0] ^= 0xff;
  assert.throws(() => listInstallShieldZ(z), /not an InstallShield Z archive/);
});

// ---- tar ------------------------------------------------------------------

function tarOf(files) {
  const parts = [];
  for (const [name, data] of files) {
    const h = new Uint8Array(512);
    h.set(bytes(name), 0);
    h.set(bytes('0000644\0'), 100);
    h.set(bytes('0000000\0'), 108);
    h.set(bytes('0000000\0'), 116);
    h.set(bytes(data.length.toString(8).padStart(11, '0') + '\0'), 124);
    h.set(bytes('00000000000\0'), 136);
    h[156] = 0x30;
    h.set(bytes('ustar\0' + '00'), 257);
    h.fill(32, 148, 156);
    let sum = 0;
    for (const b of h) sum += b;
    h.set(bytes(sum.toString(8).padStart(6, '0') + '\0 '), 148);
    parts.push(h, data, new Uint8Array((512 - (data.length % 512)) % 512));
  }
  parts.push(new Uint8Array(1024));
  return new Uint8Array(Buffer.concat(parts.map((p) => Buffer.from(p))));
}

test('tar: regular files, offsets and sizes', () => {
  const t = tarOf([['strings/stab.str', bytes('hello')], ['IMusic/Band19.bnd', new Uint8Array(1000).fill(7)]]);
  const files = parseTar(t);
  assert.deepEqual(files.map((f) => [f.name, f.size]), [['strings/stab.str', 5], ['IMusic/Band19.bnd', 1000]]);
  assert.equal(latin1(t.subarray(files[0].offset, files[0].offset + 5)), 'hello');
  assert.equal(t[files[1].offset + 999], 7);
});

test('tar: a damaged header is refused', () => {
  const t = tarOf([['a.txt', bytes('x')]]);
  t[0] ^= 1;
  assert.throws(() => parseTar(t), /checksum/);
});

// ---- zip ------------------------------------------------------------------

test('zip: crc32 check value', () => {
  assert.equal(crc32(bytes('123456789')), 0xcbf43926);
});

test('zip: makeZip round-trips through the reader', async () => {
  const files = [
    { name: 'profiles/Profile1.txt', data: new Uint8Array(0x110).fill(65) },
    { name: 'profiles/1save1.sav', data: bytes('park') }
  ];
  const blob = makeZip(files);
  const entries = await readZipDirectory(blob);
  assert.deepEqual(entries.map((e) => [e.name, e.size, e.method]), [
    ['profiles/Profile1.txt', 0x110, 0], ['profiles/1save1.sav', 4, 0]]);
  for (const [i, e] of entries.entries()) {
    const got = new Uint8Array(await (await zipEntryBlob(blob, e)).arrayBuffer());
    assert.deepEqual(got, files[i].data);
    assert.equal(e.crc, crc32(files[i].data));
  }
});

test('zip: a deflated member (what an OS "compress" makes)', async () => {
  const data = bytes('Graphics1.res '.repeat(500));
  const comp = deflateRawSync(data);
  const name = bytes('LEGOLAND/Graphics1.txt');
  const local = new Uint8Array(30 + name.length);
  le32(local, 0, 0x04034b50);
  local[4] = 20; local[8] = 8;
  le32(local, 14, crc32(data));
  le32(local, 18, comp.length);
  le32(local, 22, data.length);
  local[26] = name.length;
  local.set(name, 30);
  const cen = new Uint8Array(46 + name.length);
  le32(cen, 0, 0x02014b50);
  cen[4] = 20; cen[6] = 20; cen[10] = 8;
  le32(cen, 16, crc32(data));
  le32(cen, 20, comp.length);
  le32(cen, 24, data.length);
  cen[28] = name.length;
  cen.set(name, 46);
  const end = new Uint8Array(22);
  le32(end, 0, 0x06054b50);
  end[8] = 1; end[10] = 1;
  le32(end, 12, cen.length);
  le32(end, 16, local.length + comp.length);
  const blob = new Blob([local, comp, cen, end]);
  const [entry] = await readZipDirectory(blob);
  assert.equal(entry.method, 8);
  const got = new Uint8Array(await (await zipEntryBlob(blob, entry)).arrayBuffer());
  assert.deepEqual(got, data);
});

// ---- ISO 9660 -------------------------------------------------------------

function dirRecord(name, lba, size, isDir) {
  const nameLen = name.length;
  const len = 33 + nameLen + (nameLen % 2 === 0 ? 1 : 0);
  const r = new Uint8Array(len);
  r[0] = len;
  le32(r, 2, lba); be32(r, 6, lba);
  le32(r, 10, size); be32(r, 14, size);
  r.set([99, 12, 31, 12, 0, 0, 0], 18);
  r[25] = isDir ? 2 : 0;
  r[32] = nameLen;
  r.set(name, 33);
  return r;
}

const ucs2 = (s) => { const u = new Uint8Array(s.length * 2); for (let i = 0; i < s.length; i++) { u[2 * i] = s.charCodeAt(i) >> 8; u[2 * i + 1] = s.charCodeAt(i) & 255; } return u; };
const SELF = new Uint8Array([0]), PARENT = new Uint8Array([1]);

// 28 sectors: descriptors at 16-18, primary tree 19-20, Joliet tree 21-22,
// file data 23-27. Graphics1.res spans two sectors.
function fakeIso() {
  const S = 2048, img = new Uint8Array(28 * S);
  const g1 = new Uint8Array(3000).map((_, i) => (i * 7) & 255);
  const wav = bytes('RIFF....WAVE');
  function descriptor(sector, type, rootLba, joliet) {
    const d = img.subarray(sector * S, (sector + 1) * S);
    d[0] = type; d.set(bytes('CD001'), 1); d[6] = 1;
    d.set(joliet ? ucs2('TESTDISC') : bytes('TESTDISC'.padEnd(32)), 40);
    if (joliet) d.set(bytes('%/E'), 88);
    d.set(dirRecord(SELF, rootLba, S, true), 156);
  }
  descriptor(16, 1, 19, false);
  descriptor(17, 2, 21, true);
  img.set([255, 0x43, 0x44, 0x30, 0x30, 0x31, 1], 18 * S);
  function dir(sector, records) {
    let o = sector * S;
    for (const r of records) { img.set(r, o); o += r.length; }
  }
  dir(19, [dirRecord(SELF, 19, S, true), dirRecord(PARENT, 19, S, true),
           dirRecord(bytes('GRAPHIC1.RES;1'), 23, g1.length, false),
           dirRecord(bytes('SPEECH'), 20, S, true)]);
  dir(20, [dirRecord(SELF, 20, S, true), dirRecord(PARENT, 19, S, true),
           dirRecord(bytes('HELLO.WAV;1'), 26, wav.length, false)]);
  dir(21, [dirRecord(SELF, 21, S, true), dirRecord(PARENT, 21, S, true),
           dirRecord(ucs2('Graphics1.res;1'), 23, g1.length, false),
           dirRecord(ucs2('Speech'), 22, S, true)]);
  dir(22, [dirRecord(SELF, 22, S, true), dirRecord(PARENT, 21, S, true),
           dirRecord(ucs2('hello.wav;1'), 26, wav.length, false)]);
  img.set(g1, 23 * S);
  img.set(wav, 26 * S);
  return { img, g1, wav };
}

// The same image as 2352-byte Mode 2 Form 1 sectors (what a .bin is).
function rawOf(img) {
  const n = img.length / 2048, raw = new Uint8Array(n * 2352);
  for (let s = 0; s < n; s++) {
    const o = s * 2352;
    raw.set([0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00], o);
    raw[o + 15] = 2;
    raw.set(img.subarray(s * 2048, (s + 1) * 2048), o + 24);
  }
  return raw;
}

for (const [label, make] of [['cooked .iso', (i) => i], ['raw Mode 2 .bin', rawOf]]) {
  test('ISO 9660 (' + label + '): Joliet names, subfolders, file bytes', async () => {
    const { img, g1, wav } = fakeIso();
    const iso = await openIso(new Blob([make(img)]), 'test');
    assert.ok(iso, 'recognised as a disc image');
    assert.equal(iso.joliet, true);
    assert.equal(iso.volumeId, 'TESTDISC');
    const files = iso.files();
    assert.deepEqual(files.map((f) => f.path).sort(), ['Graphics1.res', 'Speech/hello.wav']);
    const g = files.find((f) => f.name === 'Graphics1.res');
    assert.deepEqual(await g.read(), g1);
    assert.deepEqual(await g.read(2040, 20), g1.subarray(2040, 2060));  // across a sector edge
    assert.deepEqual(await files.find((f) => f.name === 'hello.wav').read(), wav);
    assert.equal(!!g.blob(), label === 'cooked .iso');
  });
}

test('ISO 9660: a file that is not a disc image resolves to null', async () => {
  assert.equal(await openIso(new Blob([new Uint8Array(40000)]), 'x'), null);
});

// ---- where files go -------------------------------------------------------

test('installPath: the shipped install layout', () => {
  assert.equal(installPath('SegTheme1.sgt'), 'IMusic/SegTheme1.sgt');
  assert.equal(installPath('INSTALL/IMusic/Band19.bnd'), 'IMusic/Band19.bnd');
  assert.equal(installPath('joustride.bnv'), 'zbuffers/joustride.bnv');
  assert.equal(installPath('ROLLERCOASTER0003.ltx'), COASTER_DIR + 'ROLLERCOASTER0003.ltx');
  assert.equal(installPath('ROLLERCOASTER.txt'), COASTER_DIR + 'ROLLERCOASTER.txt');
  assert.equal(installPath('stab.str'), 'strings/stab.str');
  assert.equal(installPath('Lego.TTF'), 'Lego.TTF');
  assert.equal(installPath('AD_Blink.avi'), 'AD_Blink.avi');
  assert.equal(installPath('Intro.avi'), null);
  assert.equal(installPath('legoland.exe'), null);
  assert.equal(installPath('autorun.bmp'), null);
  assert.equal(installPath('EGC.BMP'), 'EGC.BMP');
  assert.equal(installPath('readme.txt'), null);
});

const EN = { 'Legoland.res': 16424086, 'Graphics1.res': 19962774, 'Graphics2.res': 120357911 };

test('planSources: a retail disc', () => {
  const entries = [
    ...Object.entries(EN).map(([path, size]) => ({ path, size })),
    { path: 'main.z', size: 8224647 },
    { path: 'Speech/0001.wav', size: 10 }, { path: 'Speech/0002.wav', size: 10 },
    { path: 'autorun.bmp', size: 518454 }, { path: 'SETUP.EXE', size: 44928 }
  ];
  const plan = planSources(entries);
  assert.deepEqual(plan.problems, []);
  assert.equal(plan.edition.id, 'en');
  assert.equal(plan.archive.path, 'main.z');
  assert.equal(plan.speech.length, 2);
  assert.equal(plan.install.length, 0);
  const merged = mergeInstall(plan, [{ name: 'stab.str' }, { name: 'Lego.TTF' }, { name: 'Legoland.icm' }, { name: 'legoland.exe' }]);
  assert.deepEqual(merged.items.map((i) => i.dest), ['strings/stab.str', 'Lego.TTF', 'Legoland.icm']);
  assert.deepEqual(merged.lacking, []);
  assert.ok(merged.short.includes('IMusic'));
});

test('planSources: an installed folder with saves in it', () => {
  const entries = [
    ...Object.entries(EN).map(([path, size]) => ({ path: 'LEGOLAND/Volumes/' + path, size })),
    { path: 'LEGOLAND/strings/stab.str', size: 11352 },
    { path: 'LEGOLAND/zbuffers/octo.bnv', size: 10800 },
    { path: 'LEGOLAND/profiles/Profile1.txt', size: 272 },
    { path: 'LEGOLAND/profiles/1save2.sav', size: 9000 },
    { path: 'LEGOLAND/exceptlog.txt', size: 100 }
  ];
  const plan = planSources(entries);
  assert.deepEqual(plan.problems, []);
  assert.equal(plan.volumes['graphics2.res'].dest, 'volumes/Graphics2.res');
  assert.deepEqual(plan.install.map((i) => i.dest).sort(), ['strings/stab.str', 'zbuffers/octo.bnv']);
  assert.deepEqual(plan.saves.map((e) => e.path.split('/').pop()), ['1save2.sav', 'Profile1.txt']);
});

test('planSources: missing volumes and the packed Czech release are explained', () => {
  const plan = planSources([{ path: 'data/lego.pak', size: 7896770 }, { path: 'Legoland.res', size: 16504510 }]);
  assert.equal(plan.cpack, true);
  assert.equal(plan.problems.length, 2);
  assert.match(plan.problems[0], /Graphics1\.res, Graphics2\.res/);
  assert.match(plan.problems[1], /Czech/);
});

// ---- saves ----------------------------------------------------------------

test('canonicalSaveName: the spelling the game itself creates', () => {
  assert.equal(canonicalSaveName('PROFILE3.TXT'), 'Profile3.txt');
  assert.equal(canonicalSaveName('backup/profiles/2SAVE7.SAV'), '2save7.sav');
  assert.equal(canonicalSaveName('1save1.SH'), '1save1.sh');
  assert.equal(canonicalSaveName('Profile9.txt'), null);
  assert.equal(canonicalSaveName('exceptlog.txt'), null);
});

test('parseProfileRecord: name and the fifteen reachable flags', () => {
  const r = new Uint8Array(0x110);
  r.set(bytes('Adam'), 0);
  r.fill(1, 0x34, 0x34 + 3);        // lessons 1-3
  r[0x34 + 5] = 1;                  // game level 1
  const p = parseProfileRecord(r);
  assert.equal(p.name, 'Adam');
  assert.equal(p.lessons, 3);
  assert.equal(p.levels, 1);
});

let failed = 0;
for (const t of tests) {
  try {
    await t.fn();
    console.log('ok   ' + t.name);
  } catch (e) {
    failed++;
    console.log('FAIL ' + t.name + '\n     ' + String(e && e.stack || e).split('\n').slice(0, 4).join('\n     '));
  }
}
console.log(`\n${tests.length - failed}/${tests.length} passed`);
process.exit(failed ? 1 : 0);
