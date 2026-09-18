#!/usr/bin/env python3
"""Write build_info.js: the commits a browser build was made from.

The page's "version" cell (src/browser/index.html) reads `globalThis.llBuildInfo`
and links each commit to its repository. cmake/browser.cmake runs this on every
build and passes the output to emcc as --pre-js; the file is rewritten only when
its content changes, so an unchanged tree does not relink.

    python3 tools/build_info.py --root . --decomp decomp --out build-wasm/build_info.js
"""
import argparse
import json
import os
import subprocess

BROWSER_URL = 'https://github.com/adamXbot/brickpark-browser'
DECOMP_URL = 'https://github.com/adamXbot/brickpark-decomp'


def git(repo, *args):
    try:
        return subprocess.run(['git', '-C', repo, *args], capture_output=True,
                              text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ''


def describe(repo, url):
    sha = git(repo, 'rev-parse', 'HEAD')
    if not sha:
        return None
    return {
        'repo': url,
        'commit': sha,
        'date': git(repo, 'log', '-1', '--format=%cI'),
        'subject': git(repo, 'log', '-1', '--format=%s'),
        # Tracked files only: build trees and the game data links are untracked.
        'dirty': bool(git(repo, 'status', '--porcelain', '--untracked-files=no')),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', required=True, help='the brickpark-browser checkout')
    ap.add_argument('--decomp', required=True, help='the brickpark-decomp checkout')
    ap.add_argument('--out', required=True)
    args = ap.parse_args()

    info = {
        'browser': describe(args.root, BROWSER_URL),
        'decomp': describe(args.decomp, DECOMP_URL),
    }
    text = 'globalThis.llBuildInfo = %s;\n' % json.dumps(info, sort_keys=True)
    try:
        with open(args.out) as f:
            if f.read() == text:
                return
    except OSError:
        pass
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, 'w') as f:
        f.write(text)


if __name__ == '__main__':
    main()
