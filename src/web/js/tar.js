// LEGOLAND portable -- reading the data packs tools/web_datapack.py
// writes. They are plain POSIX ustar files, so `tar -tvf` can inspect them and
// Python's tarfile can write them; this reader only needs regular files.

function field(h, start, len) {
  let s = '';
  for (let i = start; i < start + len && h[i]; i++) s += String.fromCharCode(h[i]);
  return s;
}

function octal(h, start, len) {
  const s = field(h, start, len).trim();
  return s ? parseInt(s, 8) : 0;
}

// -> [{ name, offset, size }] for every regular file, offsets into `u8`.
export function parseTar(u8) {
  const files = [];
  let o = 0;
  while (o + 512 <= u8.length) {
    const h = u8.subarray(o, o + 512);
    let zero = true, sum = 0;
    for (let i = 0; i < 512; i++) {
      if (h[i]) zero = false;
      sum += i >= 148 && i < 156 ? 32 : h[i];
    }
    if (zero) break;                            // end-of-archive blocks
    if (sum !== octal(h, 148, 8)) throw new Error('data pack is damaged (bad tar header checksum)');
    const size = octal(h, 124, 12);
    const type = h[156];
    let name = field(h, 0, 100);
    if (field(h, 257, 5) === 'ustar') {
      const prefix = field(h, 345, 155);
      if (prefix) name = prefix + '/' + name;
    }
    const offset = o + 512;
    if (offset + size > u8.length) throw new Error('data pack is truncated');
    if (type === 0 || type === 0x30) files.push({ name, offset, size });
    o = offset + Math.ceil(size / 512) * 512;
  }
  return files;
}
