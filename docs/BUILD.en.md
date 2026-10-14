# Building Prototype 2 — 0.1.0

[Русский](BUILD.ru.md)

Requires Windows, Git/Git Bash, PowerShell, Python3, Docker Desktop with Linux containers, enough disk/RAM for devkitA64/LTO, and your official game copy.

## 1. Pinned dependencies

Run from `Prototype2-NX`. Patches target these commits; do not substitute versions arbitrarily.

```sh
git clone https://github.com/nebadasSwifty/masseffect-nx.git reference/masseffect-nx
git -C reference/masseffect-nx checkout a85b0b419ec3e332165ae1617c980bd399856a44
git -C reference/masseffect-nx apply ../../patches/rexglue-sdk-masseffect-nx.patch

git clone --recursive https://github.com/rexglue/rexglue-sdk.git reference/rexglue-upstream
git -C reference/rexglue-upstream checkout f5337cdc947ff6d4c4196737e2c807a48f2a1fc2
git -C reference/rexglue-upstream submodule update --init --recursive

git clone https://github.com/danfromtico/mesa-switch.git reference/mesa-switch-src
git -C reference/mesa-switch-src checkout d4a00ea0ab3f59afb967cc5d779e4263d237bd77
git -C reference/mesa-switch-src apply ../../patches/mesa-switch.patch
python tools/materialize_dependency_links.py
```

The SDK patch includes56 changed/new files for ARM64, threads, rendering and ordered presentation. Compiled SDK/Mesa libraries are not included in the source package. Preserve global `REX_HAS_FIDELITYFX_SDK=1` in switch/CMakeLists: it changes Presenter layout and needs a full rebuild of dependent targets when changed.

## 2. Game data and code generation

Extract your disc into `assets/game_root/`; default.xex must match README's SHA. Download [ReXGlue SDK0.10.0 for Windows](https://github.com/rexglue/rexglue-sdk/releases), arranging files so `tools/bin/rexglue-0.10.0/win-amd64/bin/rexglue.exe` exists.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/codegen.ps1
```

Output is `project/generated/`. Never commit it or `assets/`. The manifest includes extra addresses/XAPI bindings; prepare_* steps protect handwritten overrides.

## 3. Mesa/NVK, once

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_mesa.ps1
```

Docker prepares the toolchain image and static driver in `out/mesa-sdk/opt/devkitpro/portlibs/switch`. This can take a long time and needs network access when preparing the image. No console keys required.

## 4. NRO

In Git Bash:

```sh
bash tools/build_switch.sh
```

Default8compile jobs; adjust with `JOBS=4 bash tools/build_switch.sh`. Uses guest LTO, protected direct calls, ARM integer VMX and leaf locals; stack audit off. Output: `build/switch/prototype2.nro`, version0.1.0. Derived hot code is generated before compilation. For a clean rebuild remove only your own build directory, not game data.

## 5. Packaging

Place the NRO and `config/prototype2.toml` in `switch/Prototype2/`, add VERSION0.1.0, licenses and bilingual instructions. Public releases must exclude game_root, prod.keys/title.keys and personal saves/logs. For your installation add your own disc data separately. Title takeover needs no NSP forwarder. Distribute checksums with prepared packages.

## Source layout

`project/src`: handwritten hooks/native renderer; `switch`: CMake/Horizon; `tools`: code generation/build/checks; `patches`: SDK/Mesa changes; `config`:0.1.0 settings; `docs`:RU/EN. Building never automatically connects to the Switch or overwrites the SD.

## NSP forwarder 0.1.0

After building an NRO with an icon/NACP, use Git Bash, Git, Docker and your own prod.keys:

```sh
SWITCH_PROD_KEYS=/path/to/your/prod.keys bash tools/build_forwarder.sh build/switch/prototype2.nro build/forwarder
```

Output: `build/forwarder/Prototype2-forwarder.nsp`. Title ID `0150524F54320000`, target `sdmc:/switch/Prototype2/prototype2.nro`,39-bit, cores0–3. The script checks both NPDM permission tables and NACP version0.1.0. Keys are mounted read-only and excluded from releases; packer output containing keys is not printed. Forwarder-Mod/hacBrewPack source commits are pinned in the script.

Defaults: `P2_RELEASE_DIAGNOSTICS_OFF=ON`, `P2_TASK_LIFETIME_AUDIT=OFF`, `P2_GUEST_STACK_AUDIT=OFF`. `switch/release_perf.cpp` provides basic FPS/mode integration without reports or stack sampling. Protected hooks are listed in `config/strong-overrides.json`; no private NRO archive is required to build.
