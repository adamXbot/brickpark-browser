#!/usr/bin/env python3
"""Extract one file from a Microsoft cabinet (the music's instruments from the CD).

WHY (the DirectMusic lane). The game's music ships no instruments: its bands
name General MIDI / GS programs, and DirectMusic plays them from the collection
the registry's GMFilePath names. The LEGOLAND CD carries the DirectX 7
redistributable, whose directx.cab holds that collection as gm16.dls -- the
Roland GS Sound Set, 235 instruments over 495 16-bit waves -- and whose
directx.inf copies it to %windir%\\system\\gm16.dls and points GMFilePath at it
on Windows 95 (Windows 98 ships its own drivers\\gm.dls). So the port plays the
music with the file from the user's own disc.

libarchive -- bsdtar and `cmake -E tar` -- stops on that cabinet with "Invalid
CFDATA" and leaves a file of zeros, though all 476 of its data blocks carry
good checksums (measured 2026-09-17), hence this reader: the stdlib's zlib does
MSZIP.

This script runs at BUILD time (cmake/browser.cmake) over the user's
own disc files. Nothing it writes is committed.

FORMAT (MS-CAB). CFHEADER, then one CFFOLDER per folder and one CFFILE per
file; a file is a byte range of its folder's uncompressed stream, which is the
folder's CFDATA blocks decoded in order. An MSZIP block is "CK" and one raw
deflate stream that may refer back into the previous block's 32 KB. Every block
is checked against its CFDATA checksum, so a damaged disc image fails the build
instead of playing noise.

    mscab.py --member gm16.dls --out build/dls/gm.dls gamedata/disc/directx.cab
"""
import argparse
import os
import struct
import sys
import zlib

COMP_NONE, COMP_MSZIP = 0, 1
PREV_CABINET, NEXT_CABINET, RESERVE_PRESENT = 0x0001, 0x0002, 0x0004


def checksum(data, seed):
    """The CFDATA checksum: XOR of little-endian 32-bit words, the tail packed high."""
    n = len(data) // 4
    csum = seed
    for word in struct.unpack_from("<%dI" % n, data):
        csum ^= word
    tail = 0
    for byte in data[4 * n:]:
        tail = tail << 8 | byte
    return csum ^ tail


def extract(cab, member):
    if cab[:4] != b"MSCF":
        raise ValueError("not a cabinet")
    coff_files, = struct.unpack_from("<I", cab, 16)
    n_folders, n_files, flags = struct.unpack_from("<HHH", cab, 26)
    if flags & (PREV_CABINET | NEXT_CABINET):
        raise ValueError("cabinets spanning several files are not read")
    folder_off, folder_reserve, data_reserve = 36, 0, 0
    if flags & RESERVE_PRESENT:
        header_reserve, folder_reserve, data_reserve = struct.unpack_from("<HBB", cab, 36)
        folder_off = 40 + header_reserve
    folders = [struct.unpack_from("<IHH", cab, folder_off + (8 + folder_reserve) * i)
               for i in range(n_folders)]

    off, entry = coff_files, None
    for _ in range(n_files):
        size, start, ifolder = struct.unpack_from("<IIH", cab, off)
        end = cab.index(b"\0", off + 16)
        if cab[off + 16:end].decode("latin-1").lower() == member.lower():
            entry = (size, start, ifolder)
        off = end + 1
    if entry is None:
        raise ValueError(member + " is not in the cabinet")
    size, start, ifolder = entry
    if ifolder >= n_folders:
        raise ValueError(member + " continues in another cabinet")

    data_off, n_blocks, comp = folders[ifolder]
    method = comp & 0x000F
    if method not in (COMP_NONE, COMP_MSZIP):
        raise ValueError("%s is compressed with method %d; only stored and MSZIP are read"
                         % (member, method))
    out = bytearray()
    off = data_off
    for block in range(n_blocks):
        if len(out) >= start + size:
            break
        csum, cb, ucb = struct.unpack_from("<IHH", cab, off)
        payload = cab[off + 8 + data_reserve:off + 8 + data_reserve + cb]
        if csum and checksum(cab[off + 4:off + 8 + data_reserve],
                             checksum(payload, 0)) != csum:
            raise ValueError("data block %d of folder %d fails its checksum" % (block, ifolder))
        off += 8 + data_reserve + cb
        if method == COMP_NONE:
            chunk = payload
        else:
            if payload[:2] != b"CK":
                raise ValueError("data block %d of folder %d is not MSZIP" % (block, ifolder))
            history = bytes(out[-32768:])
            inflater = (zlib.decompressobj(-15, zdict=history) if history
                        else zlib.decompressobj(-15))
            chunk = inflater.decompress(payload[2:]) + inflater.flush()
        if len(chunk) != ucb:
            raise ValueError("data block %d of folder %d decodes to %d bytes, not %d"
                             % (block, ifolder, len(chunk), ucb))
        out += chunk
    if len(out) < start + size:
        raise ValueError(member + " runs past its folder's data")
    return bytes(out[start:start + size])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--member", required=True, help="file to extract (case-insensitive)")
    ap.add_argument("--out", required=True, help="where to write it")
    ap.add_argument("cabinet")
    args = ap.parse_args()
    with open(args.cabinet, "rb") as f:
        cab = f.read()
    try:
        body = extract(cab, args.member)
    except (ValueError, struct.error, zlib.error) as e:
        sys.exit("mscab.py: %s: %s" % (args.cabinet, e))
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    tmp = args.out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(body)
    os.replace(tmp, args.out)
    print("mscab.py: %s, %d bytes, from %s" % (args.member, len(body), args.cabinet))


if __name__ == "__main__":
    main()
