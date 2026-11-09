#!/bin/bash
# Prototype 2 0.4.0: build the p2mem helper (lends the game memory from the applet pool, see
# project/src/switch_applet_memory.cpp). Output: out/p2mem/exefs.nsp, shipped as switch/Prototype2/p2mem.nsp.
# Usage (Git Bash + Docker, from the repository root): bash tools/build_p2mem.sh
set -euo pipefail
IMAGE="${DEVKITA64_IMAGE:-devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282}"
cd "$(dirname "$0")/.."
MSYS_NO_PATHCONV=1 docker run --rm --mount "type=bind,source=$(pwd -W),target=/work" "$IMAGE" bash -lc '
set -e
export DEVKITPRO=/opt/devkitpro
rm -rf /tmp/p2mem && mkdir -p /tmp/p2mem/source
cp /work/tools/p2mem/source/main.c /tmp/p2mem/source/
cp /work/tools/p2mem/p2mem.json /tmp/p2mem/p2mem.json
cp /opt/devkitpro/examples/switch/templates/sysmodule/Makefile /tmp/p2mem/Makefile
make -C /tmp/p2mem -j4
mkdir -p /work/out/p2mem
cp /tmp/p2mem/p2mem.nsp /work/out/p2mem/exefs.nsp
sha256sum /work/out/p2mem/exefs.nsp'
