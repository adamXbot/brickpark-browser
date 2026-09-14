// LEGOLAND portable -- the player page's `main.z` reader.
//
// The retail disc packs the install (music, ride z-buffers, coaster data, the
// string table, the typeface, the advisor clips) into an InstallShield 5.x "Z"
// archive, signature 0x8C655D13, every member compressed with PKWARE DCL
// implode. This is a line-for-line port of tools/iscab.py and tools/blast.py,
// whose format notes are docs/INSTALLSHIELD_Z.md; read those for the WHY. The
// two must stay in step: portable/tools/web_disc_check.mjs extracts every
// member with this file and compares it with the Python extraction in
// gamedata/main.

const SIG = 0x8c655d13;

function u32(u8, o) {
  return (u8[o] | (u8[o + 1] << 8) | (u8[o + 2] << 16) | (u8[o + 3] << 24)) >>> 0;
}

function printable(bytes) {
  for (let i = 0; i < bytes.length; i++) if (bytes[i] < 32 || bytes[i] >= 127) return false;
  return true;
}

function latin1(bytes) {
  let s = '';
  for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return s;
}

export function isInstallShieldZ(u8) {
  return u8.length >= 4 && u32(u8, 0) === SIG;
}

// iscab.py _find_dir_start: the first [len][name][42-byte meta] record whose
// NEXT record also parses, scanning the last 200 KB.
function findDirectory(u8) {
  const n = u8.length;
  for (let pos = Math.max(0, n - 200000); pos < n - 60; pos++) {
    const L = u8[pos];
    if (L < 5 || L > 40) continue;
    const meta = pos + 1 + L;
    if (meta + 42 > n) continue;
    const name = u8.subarray(pos + 1, meta);
    const c = name[0];
    const leadOk = (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c === 95 || (c >= 48 && c <= 57);
    if (!printable(name) || name.indexOf(0x2e) < 0 || !leadOk) continue;
    const csize = u32(u8, meta + 20), off = u32(u8, meta + 24);
    if (off > 0 && off < n && csize > 0 && off + csize <= n) {
      const next = meta + 42;
      if (next < n && u8[next] >= 1 && u8[next] <= 40) return pos;
    }
  }
  throw new Error('could not find the InstallShield directory in main.z');
}

// iscab.py parse_directory. Each name is described by the 42-byte block that
// PRECEDES it (docs/INSTALLSHIELD_Z.md, "the off-by-one tell").
export function listInstallShieldZ(u8) {
  if (!isInstallShieldZ(u8)) throw new Error('not an InstallShield Z archive');
  const n = u8.length;
  const members = [];
  let pos = findDirectory(u8);
  while (pos < n - 1) {
    const L = u8[pos];
    if (L < 1 || L > 60 || pos + 1 + L > n) break;
    const name = u8.subarray(pos + 1, pos + 1 + L);
    if (!printable(name)) break;
    const metaAt = pos - 42;
    if (metaAt >= 0) {
      const size = u32(u8, metaAt + 16), stored = u32(u8, metaAt + 20);
      const offset = u32(u8, metaAt + 24), dosTime = u32(u8, metaAt + 28);
      if (offset > 0 && offset < n && stored > 0 && offset + stored <= n)
        members.push({ name: latin1(name), size, stored, offset, dosTime });
    }
    pos += 1 + L + 42;
  }
  return members;
}

// ---- blast.py: PKWARE DCL explode ------------------------------------------

const LEN_BASE = [3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264];
const LEN_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8];
const LIT_REP = [
  11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8,
  9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5,
  7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12,
  8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27,
  44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45,
  44, 173];
const LEN_REP = [2, 35, 36, 53, 38, 23];
const DIST_REP = [2, 20, 53, 230, 247, 151, 248];

function huffman(rep) {
  const lengths = [];
  for (const b of rep) {
    const run = (b >> 4) + 1, len = b & 15;
    for (let i = 0; i < run; i++) lengths.push(len);
  }
  const count = new Array(Math.max(...lengths) + 1).fill(0);
  for (const l of lengths) if (l) count[l]++;
  const symbol = lengths
    .map((l, s) => [l, s])
    .filter(([l]) => l)
    .sort((a, b) => a[0] - b[0] || a[1] - b[1])
    .map(([, s]) => s);
  return { count, symbol };
}

const LIT = huffman(LIT_REP);
const LEN = huffman(LEN_REP);
const DIST = huffman(DIST_REP);

// Decompress the DCL stream at u8[pos] into exactly `size` bytes (the
// directory records the expanded size), or fewer if the stream ends first --
// the caller compares. `size` may be an over-estimate for a bare stream.
export function explode(u8, pos, size) {
  const litMode = u8[pos], dictBits = u8[pos + 1];
  if ((litMode !== 0 && litMode !== 1) || dictBits < 4 || dictBits > 6)
    throw new Error('not a DCL stream (mode ' + litMode + ', dictionary ' + dictBits + ')');
  const end = u8.length;
  let inPos = pos + 2, bitbuf = 0, bitcnt = 0;

  function bits(need) {
    let val = bitbuf, cnt = bitcnt;
    while (cnt < need) {
      if (inPos >= end) throw new Error('DCL stream ends early');
      val |= u8[inPos++] << cnt;
      cnt += 8;
    }
    bitbuf = val >>> need;
    bitcnt = cnt - need;
    return val & ((1 << need) - 1);
  }

  function decode(h) {
    // DCL reads each code bit inverted relative to canonical order.
    let code = 0, first = 0, index = 0;
    const count = h.count;
    for (let len = 1; len < count.length; len++) {
      code |= bits(1) ^ 1;
      const c = count[len];
      if (code - c < first) return h.symbol[index + (code - first)];
      index += c;
      first = (first + c) << 1;
      code <<= 1;
    }
    throw new Error('bad DCL code');
  }

  const out = new Uint8Array(size);
  let o = 0;
  while (o < size) {
    if (bits(1)) {
      const sym = decode(LEN);
      const extra = LEN_EXTRA[sym];
      const length = LEN_BASE[sym] + (extra ? bits(extra) : 0);
      if (length === 519) break;               // end-of-stream marker
      const dsym = decode(DIST);
      const dist = (length === 2 ? (dsym << 2) + bits(2) : (dsym << dictBits) + bits(dictBits)) + 1;
      let from = o - dist;
      if (from < 0) throw new Error('DCL distance reaches before the start');
      const stop = Math.min(o + length, size);
      while (o < stop) out[o++] = out[from++];  // may overlap: byte by byte
    } else {
      out[o++] = litMode === 0 ? bits(8) : decode(LIT);
    }
  }
  return o === size ? out : out.subarray(0, o);
}

// One member, expanded and length-checked.
export function extractMember(u8, member) {
  const data = explode(u8, member.offset, member.size);
  if (data.length !== member.size)
    throw new Error(member.name + ': expanded to ' + data.length + ' bytes, expected ' + member.size);
  return data;
}
