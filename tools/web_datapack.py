#!/usr/bin/env python3
"""Build the player page's static directory (cmake/browser.cmake,
target legoland_web). Three subcommands:

  static --src src/web --out build-wasm/web
      Copy the page's HTML, CSS and scripts, rewriting only what changed.

  data --gamedata gamedata --advisor build-wasm/advisor --out build-wasm/web/data
      The downloadable data pack: core.tar (the install files in the shipped
      install's layout, plus the host-decoded advisor frames), speech.tar (the
      narration) and volumes/*.res, described by manifest.json. The layout rule
      is src/web/js/discplan.js's installPath, mirrored below; the disc
      import and the download must install the SAME tree, and
      `node tools/web_disc_check.mjs --pack build-wasm/web/data IMAGE`
      compares the two.

  stamp --out build-wasm/web
      version.json: a build id derived from legoland.wasm, which the page adds
      to the module's URL so a rebuilt module is never served from cache.
"""
import argparse
import datetime
import hashlib
import io
import json
import os
import shutil
import sys
import tarfile

COASTER_DIR = "RollerCoaster/RollerCoaster/CreatedData/"
SKIP = {"legoland.exe", "uninst.dll", "thumbs.db", "desktop.ini", ".ds_store"}
VOLUMES = ["Legoland.res", "Graphics1.res", "Graphics2.res"]
EDITION = {"id": "en", "label": "LEGOLAND (English)"}
EN_SIZES = {"legoland.res": 16424086, "graphics1.res": 19962774, "graphics2.res": 120357911}


def install_path(name):
    """discplan.js installPath, line for line."""
    base = name.replace("\\", "/").split("/")[-1]
    lower = base.lower()
    if not lower or lower in SKIP:
        return None
    ext = lower.rsplit(".", 1)[1] if "." in lower else ""
    if lower in ("rollercoaster.txt", "rollercoaster.obj"):
        return COASTER_DIR + base
    if ext in ("sty", "sgt", "bnd"):
        return "IMusic/" + base
    if ext == "bnv":
        return "zbuffers/" + base
    if ext in ("ltx", "lms", "lfm", "lpt"):
        return COASTER_DIR + base
    if ext == "str":
        return "strings/" + base if lower == "stab.str" else None
    if ext in ("ttf", "icm"):
        return base
    if ext == "bmp":
        return base if lower == "egc.bmp" else None
    if ext == "avi":
        return base if lower.startswith("ad_") else None
    return None


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 22), b""):
            h.update(block)
    return h.hexdigest()


def write_if_changed(path, data):
    if os.path.exists(path):
        with open(path, "rb") as f:
            if f.read() == data:
                return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    return True


def cmd_static(args):
    copied = 0
    for root, _dirs, files in os.walk(args.src):
        for name in files:
            if not name.endswith((".html", ".css", ".js", ".svg")):
                continue            # package.json is for node, not the page
            src = os.path.join(root, name)
            rel = os.path.relpath(src, args.src)
            with open(src, "rb") as f:
                if write_if_changed(os.path.join(args.out, rel), f.read()):
                    copied += 1
    with open(os.path.join(args.out, ".static.stamp"), "w") as f:
        f.write(datetime.datetime.now(datetime.timezone.utc).isoformat() + "\n")
    print(f"web_datapack static: {copied} file(s) updated in {args.out}")


def make_tar(members):
    """members: [(arcname, bytes)] -> deterministic ustar bytes."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for arcname, data in members:
            info = tarfile.TarInfo(arcname)
            info.size = len(data)
            info.mtime = 946684800          # 2000-01-01: stable output
            info.mode = 0o644
            tar.addfile(info, io.BytesIO(data))
    return buf.getvalue()


def link_or_copy(src, dst):
    if os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(src) \
            and os.path.getmtime(dst) >= os.path.getmtime(src):
        return
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst):
        os.remove(dst)
    try:
        os.link(src, dst)                   # 157 MB: do not copy when a link will do
    except OSError:
        shutil.copyfile(src, dst)


def cmd_data(args):
    main = os.path.join(args.gamedata, "main")
    disc = os.path.join(args.gamedata, "disc")
    os.makedirs(args.out, exist_ok=True)

    core, seen = [], set()
    for name in sorted(os.listdir(main), key=str.lower):
        src = os.path.join(main, name)
        if not os.path.isfile(src):
            continue
        dest = install_path(name)
        if not dest or dest.lower() in seen:
            continue
        seen.add(dest.lower())
        with open(src, "rb") as f:
            core.append((dest, f.read()))
    advisor = 0
    if args.advisor and os.path.isdir(args.advisor):
        for name in sorted(os.listdir(args.advisor)):
            if name.lower().endswith(".llv"):
                with open(os.path.join(args.advisor, name), "rb") as f:
                    core.append(("advisor/" + name, f.read()))
                advisor += 1
    if not any(d.lower() == "strings/stab.str" for d, _ in core):
        sys.exit("web_datapack: gamedata/main has no stab.str")

    speech = []
    speech_dir = os.path.join(disc, "Speech")
    if os.path.isdir(speech_dir):
        for name in sorted(os.listdir(speech_dir), key=str.lower):
            if name.lower().endswith(".wav"):
                with open(os.path.join(speech_dir, name), "rb") as f:
                    speech.append(("speech/" + name, f.read()))

    packs = []
    for pack_id, title, members, required in (
            ("core", "Install files", core, True),
            ("speech", "Voice narration", speech, False)):
        if not members:
            continue
        data = make_tar(members)
        write_if_changed(os.path.join(args.out, pack_id + ".tar"), data)
        packs.append({
            "id": pack_id, "kind": "tar", "title": title, "url": pack_id + ".tar",
            "bytes": len(data), "files": len(members),
            "installBytes": sum(len(d) for _, d in members),
            "sha256": hashlib.sha256(data).hexdigest(), "required": required})

    sizes = {}
    for vol in VOLUMES:
        src = os.path.join(disc, vol)
        if not os.path.isfile(src):
            sys.exit(f"web_datapack: {src} is missing")
        dst = os.path.join(args.out, "volumes", vol)
        link_or_copy(src, dst)
        sizes[vol.lower()] = os.path.getsize(src)
        packs.append({
            "id": vol, "kind": "file", "title": vol, "url": "volumes/" + vol,
            "dest": "volumes/" + vol, "bytes": sizes[vol.lower()], "files": 1,
            "installBytes": sizes[vol.lower()], "sha256": sha256_file(src), "required": True})

    edition = dict(EDITION) if sizes == EN_SIZES else {"id": "unknown", "label": "LEGOLAND"}
    manifest = {
        "format": 1,
        "edition": edition,
        "advisorFrames": advisor,
        "generated": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "packs": packs,
    }
    write_if_changed(os.path.join(args.out, "manifest.json"),
                     (json.dumps(manifest, indent=2) + "\n").encode())
    total = sum(p["bytes"] for p in packs)
    print(f"web_datapack data: {len(core)} install files ({advisor} advisor clips), "
          f"{len(speech)} speech files, 3 volumes; {total / 1048576:.1f} MB in {args.out}")


def cmd_stamp(args):
    wasm = os.path.join(args.out, "legoland.wasm")
    if not os.path.isfile(wasm):
        sys.exit(f"web_datapack stamp: {wasm} is missing")
    h = hashlib.sha256()
    for name in ("legoland.wasm", "legoland.js"):
        with open(os.path.join(args.out, name), "rb") as f:
            h.update(f.read())
    build = h.hexdigest()[:16]
    info = {"build": build,
            "built": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")}
    with open(os.path.join(args.out, "version.json"), "w") as f:
        json.dump(info, f, indent=2)
        f.write("\n")
    print(f"web_datapack stamp: build {build}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("static")
    s.add_argument("--src", required=True)
    s.add_argument("--out", required=True)
    d = sub.add_parser("data")
    d.add_argument("--gamedata", required=True)
    d.add_argument("--advisor")
    d.add_argument("--out", required=True)
    t = sub.add_parser("stamp")
    t.add_argument("--out", required=True)
    args = ap.parse_args()
    {"static": cmd_static, "data": cmd_data, "stamp": cmd_stamp}[args.cmd](args)


if __name__ == "__main__":
    main()
