#!/usr/bin/env bash
# Prototype 2 0.4.0; Windows Git Bash + Docker.
# PERF pass 1: DEVKITA64_IMAGE pins the toolchain image (default unchanged: devkitpro/devkita64:latest);
# with PGO=USE the GCC version inside the image must match the stamp of config/pgo-data/*.gcda
# (tools/check_pgo_compiler.py), otherwise the build stops instead of silently losing PGO.
set -euo pipefail
cd "$(dirname "$0")/.."
IMAGE=${DEVKITA64_IMAGE:-devkitpro/devkita64:latest}
echo "[build_switch] image $IMAGE digest $(docker image inspect --format '{{join .RepoDigests ","}}' "$IMAGE" 2>/dev/null || echo unknown)"
if [ "${PGO:-USE}" = "USE" ]; then
  GCC_VERSION=$(MSYS_NO_PATHCONV=1 docker run --rm "$IMAGE" /opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc -dumpversion | tr -d '\r')
  python tools/check_pgo_compiler.py --profiles config/pgo-data --gcc-version "$GCC_VERSION"
fi
python tools/prepare_switch_codegen.py
python tools/prepare_no_volatile.py
python tools/prepare_switch_hot.py
# 0.3.0: the performance and diagnostic steps the release was built with (idempotent; tools/prepare_p2_perf.py).
python tools/prepare_p2_perf.py
MSYS_NO_PATHCONV=1 docker run --rm --mount "type=bind,source=$(pwd -W),target=/work" -w /work "$IMAGE" bash -lc "cmake -S switch -B build/switch -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/work/switch/cmake/switch-devkitA64.cmake -DREXSDK_DIR=/work/reference/masseffect-nx/sdk -DREXGLUE_SWITCH_NVK_SDK=/work/out/mesa-sdk/opt/devkitpro/portlibs/switch -DP2_PGO=${PGO:-USE} -DP2_PGO_DATA=/work/config/pgo-data -DP2_RELEASE_DIAGNOSTICS_OFF=ON -DP2_GUEST_STACK_AUDIT=OFF -DP2_TASK_LIFETIME_AUDIT=OFF -DP2_DIRECT_CALLS=ON -DP2_GUEST_LTO=ON -DP2_LEAF_LOCALS=ON -DP2_ARM_VMX=ON && cmake --build build/switch --target prototype2 -j ${JOBS:-8}"
