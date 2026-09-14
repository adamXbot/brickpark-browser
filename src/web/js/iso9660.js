// LEGOLAND portable -- the player page's disc reader: read-only ISO 9660.
//
// It reads the CD image a player picked straight out of the File object. Nothing
// is uploaded anywhere, and nothing is read into memory except the sectors a
// caller asks for, so a 600 MB image costs a few directory sectors to open.
//
// The images people actually have of this game come in two geometries:
//
//   .iso / .img   2048-byte ("cooked") sectors -- all four LEGOLAND images
//                 checked while writing this (docs: portable/README.md, "The
//                 player page") are this, with a Joliet supplementary volume
//                 descriptor next to the primary one
//   .bin (+.cue)  2352-byte raw sectors. The CUE sheets that travel with the
//                 LEGOLAND BINs say MODE2/2352: CD-ROM XA, user data at +24.
//                 Mode 1 (+16) is accepted too.
//
// Names come from the Joliet tree whenever there is one. The primary tree's
// names are upper case and, on a level-1 image, cannot spell "Graphics1.res" at
// all; everything that consumes these names matches case-insensitively anyway.
//
// Platform-neutral on purpose: it needs Blob.slice().arrayBuffer() and nothing
// else, so node (fs.openAsBlob) runs exactly this code in
// portable/tools/web_disc_check.mjs.

export const SECTOR = 2048;

const MAX_DIR_BYTES = 16 * 1024 * 1024;
const MAX_ENTRIES = 200000;

function u32(u8, o) {
  return (u8[o] | (u8[o + 1] << 8) | (u8[o + 2] << 16) | (u8[o + 3] << 24)) >>> 0;
}

async function readBlob(blob, start, length) {
  return new Uint8Array(await blob.slice(start, start + length).arrayBuffer());
}

function isVolumeDescriptor(b) {
  // "\x??CD001\x01": type byte, standard identifier, version 1.
  return b.length >= 7 && b[1] === 0x43 && b[2] === 0x44 && b[3] === 0x30 &&
         b[4] === 0x30 && b[5] === 0x31 && b[6] === 1;
}

// The user-data stream of an image, whatever its sector geometry: byte `pos`
// of the stream is byte `pos % 2048` of logical block `pos / 2048`.
class SectorStream {
  constructor(blob, rawSize, dataOffset) {
    this.blob = blob;
    this.raw = rawSize;
    this.dataOffset = dataOffset;
  }

  get cooked() { return this.raw === SECTOR && this.dataOffset === 0; }

  async read(pos, length) {
    if (length <= 0) return new Uint8Array(0);
    if (this.cooked) {
      const u8 = await readBlob(this.blob, pos, length);
      if (u8.length !== length) throw new Error('the disc image is truncated');
      return u8;
    }
    const out = new Uint8Array(length);
    let done = 0;
    while (done < length) {
      const at = pos + done;
      const lba = Math.floor(at / SECTOR);
      const within = at - lba * SECTOR;
      // A run of up to 512 raw sectors per read: ~1.2 MB, bounded memory.
      const sectors = Math.min(Math.ceil((within + length - done) / SECTOR), 512);
      const raw = await readBlob(this.blob, lba * this.raw, sectors * this.raw);
      for (let s = 0; s < sectors && done < length; s++) {
        const from = s === 0 ? within : 0;
        const take = Math.min(SECTOR - from, length - done);
        const base = s * this.raw + this.dataOffset + from;
        if (base + take > raw.length) throw new Error('the disc image is truncated');
        out.set(raw.subarray(base, base + take), done);
        done += take;
      }
    }
    return out;
  }

  // A Blob for [pos, pos + length) of the stream, without reading it -- only
  // possible when the stream IS the file (cooked sectors).
  slice(pos, length) {
    return this.cooked ? this.blob.slice(pos, pos + length) : null;
  }
}

// Which geometry, if any. Checks the primary volume descriptor's position
// under each layout rather than trusting a file extension: people rename.
export async function detectGeometry(blob) {
  const layouts = [[SECTOR, 0], [2352, 24], [2352, 16], [2336, 8]];
  for (const [raw, off] of layouts) {
    const at = 16 * raw + off;
    if (blob.size < at + SECTOR) continue;
    if (isVolumeDescriptor(await readBlob(blob, at, 8))) return { raw, dataOffset: off };
  }
  return null;
}

function decodeName(bytes, joliet) {
  let s = '';
  if (joliet) {
    for (let i = 0; i + 1 < bytes.length; i += 2)
      s += String.fromCharCode((bytes[i] << 8) | bytes[i + 1]);
  } else {
    for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  }
  const semi = s.indexOf(';');                 // "FILE.EXT;1" -> "FILE.EXT"
  if (semi >= 0) s = s.slice(0, semi);
  if (s.endsWith('.')) s = s.slice(0, -1);     // "README." -> "README"
  return s;
}

function recordDate(u8, o) {
  const y = u8[o] + 1900, mo = u8[o + 1], d = u8[o + 2];
  const h = u8[o + 3], mi = u8[o + 4], s = u8[o + 5];
  const tz = (u8[o + 6] << 24 >> 24) * 15;     // signed, quarter hours from GMT
  if (!mo || !d) return null;
  return new Date(Date.UTC(y, mo - 1, d, h, mi, s) - tz * 60000);
}

function parseDirectory(u8, joliet) {
  const out = [];
  let o = 0;
  while (o < u8.length) {
    const len = u8[o];
    if (len === 0) {                           // padding to the next sector
      o = (Math.floor(o / SECTOR) + 1) * SECTOR;
      continue;
    }
    if (len < 34 || o + len > u8.length) break;
    const nameLen = u8[o + 32];
    const nameBytes = u8.subarray(o + 33, o + 33 + nameLen);
    if (!(nameLen === 1 && (nameBytes[0] === 0 || nameBytes[0] === 1))) {
      out.push({
        name: decodeName(nameBytes, joliet),
        // The extent starts at the extended attribute record, if any; the
        // data follows it.
        lba: u32(u8, o + 2) + u8[o + 1],
        size: u32(u8, o + 10),
        flags: u8[o + 25],
        mtime: recordDate(u8, o + 18)
      });
    }
    o += len;
  }
  return out;
}

export class IsoFile {
  constructor(image, entry) {
    this.image = image;
    this.path = entry.path;
    this.name = entry.name;
    this.size = entry.size;
    this.extents = entry.extents;
    this.mtime = entry.mtime;
  }

  // `length` bytes from `offset`, across extents.
  async read(offset = 0, length = this.size - offset) {
    length = Math.max(0, Math.min(length, this.size - offset));
    const out = new Uint8Array(length);
    let done = 0, skip = offset;
    for (const ext of this.extents) {
      if (done >= length) break;
      if (skip >= ext.size) { skip -= ext.size; continue; }
      const take = Math.min(ext.size - skip, length - done);
      out.set(await this.image.stream.read(ext.lba * SECTOR + skip, take), done);
      done += take;
      skip = 0;
    }
    return out;
  }

  // The file as a Blob without reading it, when the image allows that.
  blob() {
    if (this.extents.length !== 1) return null;
    return this.image.stream.slice(this.extents[0].lba * SECTOR, this.size);
  }
}

export class IsoImage {
  constructor(blob, name, geometry, stream, volumeId, joliet, entries) {
    this.blob = blob;
    this.name = name;
    this.geometry = geometry;
    this.stream = stream;
    this.volumeId = volumeId;
    this.joliet = joliet;
    this.entries = entries;                    // files only
  }

  get format() {
    if (this.geometry.raw === SECTOR) return 'ISO 9660 image';
    return 'raw CD image (' + this.geometry.raw + '-byte sectors)';
  }

  files() { return this.entries.map((e) => new IsoFile(this, e)); }
}

async function walk(stream, root, joliet) {
  const files = [];
  const seen = new Set();
  const stack = [{ path: '', lba: root.lba, size: root.size }];
  while (stack.length) {
    const dir = stack.pop();
    if (seen.has(dir.lba)) continue;           // a malformed image can loop
    seen.add(dir.lba);
    if (dir.size > MAX_DIR_BYTES) throw new Error('a directory on the disc is implausibly large');
    const records = parseDirectory(await stream.read(dir.lba * SECTOR, dir.size), joliet);
    let pending = null;
    for (const r of records) {
      const path = dir.path ? dir.path + '/' + r.name : r.name;
      if (r.flags & 0x02) {
        stack.push({ path, lba: r.lba, size: r.size });
        continue;
      }
      // Multi-extent files: consecutive records with the same name, all but
      // the last flagged 0x80.
      if (pending && pending.name === r.name) {
        pending.extents.push({ lba: r.lba, size: r.size });
        pending.size += r.size;
        if (!(r.flags & 0x80)) { files.push(pending); pending = null; }
        continue;
      }
      if (pending) { files.push(pending); pending = null; }
      const entry = { path, name: r.name, size: r.size, mtime: r.mtime,
                      extents: [{ lba: r.lba, size: r.size }] };
      if (r.flags & 0x80) pending = entry;
      else files.push(entry);
    }
    if (pending) files.push(pending);
    if (files.length > MAX_ENTRIES) throw new Error('too many files on the disc');
  }
  return files;
}

// Open a disc image. Resolves to null when the blob is not an image at all
// (the caller then tries the next kind of source); throws when it IS one but
// cannot be read.
export async function openIso(blob, name = '') {
  const geometry = await detectGeometry(blob);
  if (!geometry) return null;
  const stream = new SectorStream(blob, geometry.raw, geometry.dataOffset);
  let primary = null, joliet = null;
  for (let s = 16; s < 64; s++) {
    const vd = await stream.read(s * SECTOR, SECTOR);
    if (!isVolumeDescriptor(vd) || vd[0] === 255) break;
    if (vd[0] === 1 && !primary) primary = vd;
    if (vd[0] === 2 && vd[88] === 0x25 && vd[89] === 0x2f &&
        (vd[90] === 0x40 || vd[90] === 0x43 || vd[90] === 0x45)) joliet = vd;
  }
  const vd = joliet || primary;
  if (!vd) throw new Error('the disc image has no readable volume descriptor');
  const root = { lba: u32(vd, 158) + vd[157], size: u32(vd, 166) };
  const volumeId = primary
    ? decodeName(primary.subarray(40, 72), false).trim()
    : decodeName(vd.subarray(40, 72), true).replace(/\0/g, '').trim();
  const entries = await walk(stream, root, !!joliet);
  return new IsoImage(blob, name, geometry, stream, volumeId, !!joliet, entries);
}
