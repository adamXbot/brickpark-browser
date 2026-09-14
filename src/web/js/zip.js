// LEGOLAND portable -- zip reading and writing for the player page.
//
// Two jobs, both small:
//   * READ an archive a player hands the page: a disc image someone zipped
//     ("LEGOLAND_Win_ISO_EN.zip"), a zipped install folder, or a saves backup.
//     Members come back as Blobs; a stored member is a slice of the original
//     file (no copy), a deflated one goes through the browser's own
//     DecompressionStream.
//   * WRITE the saves export: stored (uncompressed) members, because the files
//     are small and a store-only writer is thirty lines with no dependency.
//
// ZIP64 is read (a zipped disc image can pass 4 GB); it is never written.

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

export function crc32(u8) {
  let c = 0xffffffff;
  for (let i = 0; i < u8.length; i++) c = CRC_TABLE[(c ^ u8[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

async function readBlob(blob, start, length) {
  return new Uint8Array(await blob.slice(start, start + length).arrayBuffer());
}

function u16(u8, o) { return u8[o] | (u8[o + 1] << 8); }
function u32(u8, o) { return (u8[o] | (u8[o + 1] << 8) | (u8[o + 2] << 16) | (u8[o + 3] << 24)) >>> 0; }
function u64(u8, o) { return u32(u8, o) + u32(u8, o + 4) * 0x100000000; }

function dosDate(date, time) {
  const y = ((date >> 9) & 0x7f) + 1980, m = (date >> 5) & 0x0f, d = date & 0x1f;
  const h = (time >> 11) & 0x1f, mi = (time >> 5) & 0x3f, s = (time & 0x1f) * 2;
  if (!m || !d) return null;
  return new Date(y, m - 1, d, h, mi, s);
}

function decodeName(bytes, utf8) {
  if (utf8) return new TextDecoder('utf-8').decode(bytes);
  let s = '';
  for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return s;
}

// Cheap test: does the blob end in an end-of-central-directory record?
export async function looksLikeZip(blob) {
  if (blob.size < 22) return false;
  const head = await readBlob(blob, 0, 4);
  return head[0] === 0x50 && head[1] === 0x4b && (head[2] === 3 || head[2] === 5);
}

export async function readZipDirectory(blob) {
  const tailLen = Math.min(blob.size, 22 + 65535);
  const tail = await readBlob(blob, blob.size - tailLen, tailLen);
  let eocd = -1;
  for (let i = tail.length - 22; i >= 0; i--) {
    if (tail[i] === 0x50 && tail[i + 1] === 0x4b && tail[i + 2] === 5 && tail[i + 3] === 6) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) throw new Error('not a zip archive (no central directory)');
  let count = u16(tail, eocd + 10);
  let cdSize = u32(tail, eocd + 12);
  let cdOffset = u32(tail, eocd + 16);
  if (count === 0xffff || cdSize === 0xffffffff || cdOffset === 0xffffffff) {
    const loc = eocd - 20;
    if (loc < 0 || u32(tail, loc) !== 0x07064b50) throw new Error('damaged ZIP64 archive');
    const at = u64(tail, loc + 8);
    const rec = await readBlob(blob, at, 56);
    if (u32(rec, 0) !== 0x06064b50) throw new Error('damaged ZIP64 archive');
    count = u64(rec, 32);
    cdSize = u64(rec, 40);
    cdOffset = u64(rec, 48);
  }
  const cd = await readBlob(blob, cdOffset, cdSize);
  const entries = [];
  let o = 0;
  for (let i = 0; i < count && o + 46 <= cd.length; i++) {
    if (u32(cd, o) !== 0x02014b50) throw new Error('damaged zip central directory');
    const flags = u16(cd, o + 8), method = u16(cd, o + 10);
    const time = u16(cd, o + 12), date = u16(cd, o + 14), crc = u32(cd, o + 16);
    let stored = u32(cd, o + 20), size = u32(cd, o + 24);
    const nameLen = u16(cd, o + 28), extraLen = u16(cd, o + 30), commentLen = u16(cd, o + 32);
    let localOffset = u32(cd, o + 42);
    const name = decodeName(cd.subarray(o + 46, o + 46 + nameLen), (flags & 0x800) !== 0);
    // ZIP64 extended information: only the fields that overflowed are present,
    // in this order.
    let e = o + 46 + nameLen;
    const extraEnd = e + extraLen;
    while (e + 4 <= extraEnd) {
      const id = u16(cd, e), len = u16(cd, e + 2);
      if (id === 0x0001) {
        let p = e + 4;
        if (size === 0xffffffff) { size = u64(cd, p); p += 8; }
        if (stored === 0xffffffff) { stored = u64(cd, p); p += 8; }
        if (localOffset === 0xffffffff) { localOffset = u64(cd, p); p += 8; }
      }
      e += 4 + len;
    }
    entries.push({ name, dir: name.endsWith('/'), method, size, stored, crc,
                   localOffset, encrypted: (flags & 1) !== 0, mtime: dosDate(date, time) });
    o += 46 + nameLen + extraLen + commentLen;
  }
  return entries;
}

// One member as a Blob.
export async function zipEntryBlob(blob, entry) {
  if (entry.encrypted) throw new Error(entry.name + ' is encrypted');
  const lh = await readBlob(blob, entry.localOffset, 30);
  if (u32(lh, 0) !== 0x04034b50) throw new Error('damaged zip entry: ' + entry.name);
  const start = entry.localOffset + 30 + u16(lh, 26) + u16(lh, 28);
  const raw = blob.slice(start, start + entry.stored);
  if (entry.method === 0) return raw;
  if (entry.method === 8) {
    if (typeof DecompressionStream !== 'function')
      throw new Error('this browser cannot decompress zip files; unzip it first');
    return new Response(raw.stream().pipeThrough(new DecompressionStream('deflate-raw'))).blob();
  }
  throw new Error(entry.name + ' uses zip compression method ' + entry.method + ', which is not supported');
}

// ---- writing ---------------------------------------------------------------

function dosStamp(d) {
  const date = ((Math.max(d.getFullYear(), 1980) - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate();
  const time = (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1);
  return { date, time };
}

// files: [{ name, data: Uint8Array, mtime?: Date }] -> Blob (application/zip).
export function makeZip(files) {
  const enc = new TextEncoder();
  const parts = [], central = [];
  let offset = 0;
  for (const f of files) {
    const name = enc.encode(f.name);
    const { date, time } = dosStamp(f.mtime || new Date());
    const crc = crc32(f.data);
    const local = new Uint8Array(30 + name.length);
    const lv = new DataView(local.buffer);
    lv.setUint32(0, 0x04034b50, true);
    lv.setUint16(4, 20, true);
    lv.setUint16(6, 0x800, true);              // names are UTF-8
    lv.setUint16(8, 0, true);                  // stored
    lv.setUint16(10, time, true);
    lv.setUint16(12, date, true);
    lv.setUint32(14, crc, true);
    lv.setUint32(18, f.data.length, true);
    lv.setUint32(22, f.data.length, true);
    lv.setUint16(26, name.length, true);
    local.set(name, 30);
    const cen = new Uint8Array(46 + name.length);
    const cv = new DataView(cen.buffer);
    cv.setUint32(0, 0x02014b50, true);
    cv.setUint16(4, 20, true);
    cv.setUint16(6, 20, true);
    cv.setUint16(8, 0x800, true);
    cv.setUint16(10, 0, true);
    cv.setUint16(12, time, true);
    cv.setUint16(14, date, true);
    cv.setUint32(16, crc, true);
    cv.setUint32(20, f.data.length, true);
    cv.setUint32(24, f.data.length, true);
    cv.setUint16(28, name.length, true);
    cv.setUint32(42, offset, true);
    cen.set(name, 46);
    parts.push(local, f.data);
    central.push(cen);
    offset += local.length + f.data.length;
  }
  const cdSize = central.reduce((n, c) => n + c.length, 0);
  const end = new Uint8Array(22);
  const ev = new DataView(end.buffer);
  ev.setUint32(0, 0x06054b50, true);
  ev.setUint16(8, files.length, true);
  ev.setUint16(10, files.length, true);
  ev.setUint32(12, cdSize, true);
  ev.setUint32(16, offset, true);
  return new Blob([...parts, ...central, end], { type: 'application/zip' });
}
