#!/usr/bin/env python3
"""Decode the advisor's Indeo 5 clips into the frame files avifil32.c serves.

WHY (PARK-2). The in-game panel's bottom-right 112x96 window at game (522, 378)
is the advisor video. InterfaceBG.lls leaves it transparent on purpose and the
original repaints it EVERY frame from RenderAdvisorIcon (screens3.c), which
blits the next AVIStreamGetFrame DIB there. The clips are Indeo 5 (IV50), which
no browser decodes, so avifil32.c used to fail every open: nothing repainted
the window, and whatever the game drew over it -- the cursor stamped by
FlipPrimary, a bubble-help box -- stayed on screen for the rest of the session
(docs/lanes/scope-port-b11.md §4). The same dead branch is the one that makes
the window OWN its pixels (the hand-written hit test after BltAdvisor), so the
game also read panel pixels as map squares there.

This script runs at BUILD time (cmake/browser.cmake), next to
file_packager, over the user's own gamedata/. Nothing it writes is committed.

OUTPUT. One <out>/<stem, lower-cased>.llv per input clip:

    +0x00  char[4]  "LLV1"
    +0x04  u16      width                 (112 for every advisor clip)
    +0x06  u16      height                (96)
    +0x08  u32      frames                (decoded count)
    +0x0c  u32      rate                  strh.dwRate  } frames per second is
    +0x10  u32      scale                 strh.dwScale } rate / scale
    +0x14  frames x (width x height x 2) bytes

Each frame is 16-bpp X1R5G5B5, little-endian, rows BOTTOM-UP: exactly the
pixel block of the DIB Video for Windows returns for the format advisor.c's
InitAdvisorBmi asks for (BITMAPINFOHEADER at 0x004b7d70: biBitCount 16,
BI_RGB, 112 x 96), and what BltAdvisor (blitmisc.c) reads upward from +0x28 --
it widens 555 to 565 itself when g_screen_depth == 2.

    advisor_frames.py --ffmpeg /opt/homebrew/bin/ffmpeg --out build/advisor \
        gamedata/main/ad_blink.avi gamedata/main/AD_LR.avi ...
"""
import argparse
import os
import struct
import subprocess
import sys

MAGIC = b"LLV1"
HEADER = struct.Struct("<4sHHIII")          # 0x14 bytes


def riff_chunks(buf, start, end):
    """Yield (fourcc, list_type_or_None, data_start, data_end) at one level."""
    pos = start
    while pos + 8 <= end:
        fcc, size = struct.unpack_from("<4sI", buf, pos)
        data = pos + 8
        stop = min(data + size, end)
        if fcc in (b"RIFF", b"LIST"):
            yield fcc, buf[data:data + 4], data + 4, stop
        else:
            yield fcc, None, data, stop
        pos = data + size + (size & 1)


def video_header(path):
    """(width, height, rate, scale, length) of the first 'vids' stream."""
    with open(path, "rb") as f:
        buf = f.read()
    riff = next(riff_chunks(buf, 0, len(buf)), None)
    if not riff or riff[0] != b"RIFF" or riff[1] != b"AVI ":
        raise ValueError("not a RIFF AVI file")
    for fcc, kind, s, e in riff_chunks(buf, riff[2], riff[3]):
        if fcc != b"LIST" or kind != b"hdrl":
            continue
        for fcc2, kind2, s2, e2 in riff_chunks(buf, s, e):
            if fcc2 != b"LIST" or kind2 != b"strl":
                continue
            strh = strf = None
            for fcc3, _, s3, e3 in riff_chunks(buf, s2, e2):
                if fcc3 == b"strh":
                    strh = (s3, e3)
                elif fcc3 == b"strf":
                    strf = (s3, e3)
            if not strh or buf[strh[0]:strh[0] + 4] != b"vids" or not strf:
                continue
            scale, rate, _start, length = struct.unpack_from("<IIII", buf, strh[0] + 20)
            width, height = struct.unpack_from("<ii", buf, strf[0] + 4)
            return width, abs(height), rate, scale, length
    raise ValueError("no video stream header")


def decode(ffmpeg, path, width, height):
    """All frames as bottom-up X1R5G5B5 little-endian, concatenated."""
    cmd = [ffmpeg, "-v", "error", "-nostdin", "-i", path, "-map", "0:v:0",
           "-sws_flags", "bicubic+accurate_rnd+full_chroma_int",
           "-vf", "vflip,format=rgb555le", "-f", "rawvideo", "-"]
    raw = subprocess.run(cmd, check=True, stdout=subprocess.PIPE).stdout
    frame = width * height * 2
    if not raw or len(raw) % frame:
        raise ValueError(f"decoded {len(raw)} bytes, not a whole number of "
                         f"{width}x{height} 16-bpp frames")
    return raw, len(raw) // frame


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ffmpeg", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("clips", nargs="+")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    for path in args.clips:
        width, height, rate, scale, length = video_header(path)
        if not (0 < width <= 0xffff and 0 < height <= 0xffff) or not rate or not scale:
            raise SystemExit(f"{path}: implausible header {width}x{height} "
                             f"rate {rate} scale {scale}")
        raw, frames = decode(args.ffmpeg, path, width, height)
        if frames != length:
            print(f"advisor_frames: {os.path.basename(path)}: header says "
                  f"{length} frames, decoded {frames}; serving {frames}",
                  file=sys.stderr)
        stem = os.path.splitext(os.path.basename(path))[0].lower()
        out = os.path.join(args.out, stem + ".llv")
        tmp = out + ".tmp"
        with open(tmp, "wb") as f:
            f.write(HEADER.pack(MAGIC, width, height, frames, rate, scale))
            f.write(raw)
        os.replace(tmp, out)
        print(f"advisor_frames: {stem}.llv {width}x{height} {frames} frames "
              f"@ {rate}/{scale}")


if __name__ == "__main__":
    main()
