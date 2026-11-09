#!/bin/bash
# Prototype 2 0.4.0: make the NVK driver's memalign/free calls go to p2_nvk_memalign/p2_nvk_free
# (project/src/switch_applet_memory.cpp), so its GPU memory can come from the applet pool. Run inside the
# devkitA64 image after the Mesa archive is merged; idempotent (always starts from libvulkan.a.orig).
set -euo pipefail
LIB="${1:-/work/out/mesa-sdk/opt/devkitpro/portlibs/switch/lib/libvulkan.a}"
OBJCOPY=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-objcopy
[ -f "$LIB.orig" ] || cp "$LIB" "$LIB.orig"
"$OBJCOPY" --redefine-sym memalign=p2_nvk_memalign --redefine-sym free=p2_nvk_free "$LIB.orig" "$LIB.tmp"
mv "$LIB.tmp" "$LIB"
echo "libvulkan.a: memalign/free redirected to p2_nvk_memalign/p2_nvk_free"
