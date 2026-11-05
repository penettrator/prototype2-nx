#!/usr/bin/env python3
"""Refuse a PGO=USE build whose GCC differs from the one that wrote the .gcda profiles.

GCC silently ignores (or only warns about) profiles stamped by another compiler version,
so a moved docker tag (devkitpro/devkita64:latest) would quietly produce a non-PGO build.
The .gcda header is: magic "gcda" + 4-byte version stamp, both written as one native
(little-endian) word, so the bytes read "adcg" and e.g. "*25B" for GCC 15.2 ("B52*").
GCC >= 11 stamp: chr('A' + major // 10), chr('0' + major % 10), chr('0' + minor), '*'.

Usage: check_pgo_compiler.py --profiles config/pgo-data --gcc-version 15.2.0
Exit 0 = match, 2 = mismatch / unreadable profiles (message on stderr).
"""
import argparse
import pathlib
import sys


def stamp_for(version: str) -> str:
    parts = version.strip().split(".")
    major, minor = int(parts[0]), int(parts[1]) if len(parts) > 1 else 0
    if major < 11 or minor > 9:
        raise ValueError(f"unsupported GCC version for stamp check: {version}")
    return chr(ord("A") + major // 10) + chr(ord("0") + major % 10) + chr(ord("0") + minor) + "*"


def read_stamp(path: pathlib.Path) -> str:
    head = path.read_bytes()[:8]
    if len(head) < 8 or head[:4] != b"adcg":
        raise ValueError(f"not a little-endian gcda file: {path.name}")
    return head[4:8][::-1].decode("ascii", "replace")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--profiles", required=True)
    ap.add_argument("--gcc-version", required=True, help="output of aarch64-none-elf-gcc -dumpversion")
    args = ap.parse_args()
    files = sorted(pathlib.Path(args.profiles).glob("*.gcda"))
    if not files:
        print(f"[pgo-check] ERROR: no .gcda files in {args.profiles}", file=sys.stderr)
        return 2
    try:
        want = stamp_for(args.gcc_version)
        stamps = {}
        for f in files:
            stamps.setdefault(read_stamp(f), []).append(f.name)
    except ValueError as e:
        print(f"[pgo-check] ERROR: {e}", file=sys.stderr)
        return 2
    if len(stamps) != 1 or want not in stamps:
        found = ", ".join(f"{s} x{len(v)}" for s, v in stamps.items())
        print(f"[pgo-check] ERROR: compiler GCC {args.gcc_version.strip()} (stamp {want}) does not match "
              f"profile stamps [{found}] in {args.profiles}. PGO=USE would be silently lost. "
              f"Pin the docker image used for the profiles (DEVKITA64_IMAGE=devkitpro/devkita64@sha256:...), "
              f"or retrain profiles (skill cycle-release), or build with PGO=OFF for a separate comparison.",
              file=sys.stderr)
        return 2
    print(f"[pgo-check] OK: GCC {args.gcc_version.strip()} matches {len(files)} profiles (stamp {want})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
