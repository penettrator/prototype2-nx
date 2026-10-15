# Prototype 2 for Nintendo Switch — 0.2.0

[Русский](README.md)

Port author: [penettrator](https://github.com/penettrator). Support: [Boosty](https://boosty.to/itgenius).

A native port of the Xbox 360 version of Prototype 2 to Nintendo Switch. You need your own official copy of the game; port downloads do not include game data.

## Requirements

- Nintendo Switch running Atmosphère.
- At least 10 GB free on SD; FAT32 works.
- DBI or another NSP installer.

## Installation

### 1. Download the port

Download the ready-to-use files from [Releases](https://github.com/penettrator/Prototype2-NX/releases):

| File | Purpose |
|---|---|
| `Prototype2-NX.zip` | Ready-to-use port and settings: `switch/Prototype2/` folder |
| `Prototype2-forwarder.nsp` | HOME icon; permits all four CPU cores |

Extract `Prototype2-NX.zip` to the SD card root. You should get `SD:/switch/Prototype2/`. You do not need to compile the port to install it.

### 2. Add your game

If your game is already extracted, name the folder containing `default.xex` **`game_root`**. Otherwise extract your Xbox 360 ISO with [extract-xiso](https://github.com/XboxDev/extract-xiso/releases):

```text
extract-xiso.exe -x "Prototype 2.iso" -d game_root
```

Use your ISO filename. Copy the entire `game_root` folder to `SD:/switch/Prototype2/`. The `$SystemUpdate` folder is unnecessary.

```text
SD:/switch/Prototype2/
  prototype2.nro
  prototype2.toml
  game_root/
    default.xex
    boot.rcf
    cells.rcf
    ...remaining files from your game
```

### 3. Install and start

Install **`Prototype2-forwarder.nsp`** using DBI and start Prototype 2 from its HOME icon. The NSP loads the NRO from SD, so keep the `switch/Prototype2/` folder on the card.

**This NSP already permits the fourth core and the settings are prepared. There is no need to replace `atmosphere/hbl.nsp` or edit system files.**

Alternatively, hold **R** while starting a normal installed game, wait for hbmenu and select Prototype 2. Fourth-core access through this method depends on your loader. **Do not launch through Album:** applet memory is insufficient.

## Settings

File: **`SD:/switch/Prototype2/prototype2.toml`**. Fully close and restart the game after edits.

| Setting | Default | Meaning |
|---|---|---|
| `user_language` | `12` | `12` — Russian, `1` — English; availability depends on your game edition |
| `p2_render_width`, `p2_render_height` | `800`, `448` | Internal resolution |
| `present_effect` | `"fsr"` | FSR 1 upscaling |
| `achievement_notifications` | `false` | Achievement popups are disabled |
| `p2_native_texture_cache_mb` | `128` | Retention budget for idle texture payload, MiB |

When updating, replace the port files. You do not need to copy `game_root` again. Back up your edited settings before replacing the TOML.

## Known issues

- Hitches during streaming and graphics pipeline creation.

## Source and licenses

The port uses ReXGlue static recompilation for ARM64 and a Vulkan renderer on NVK/Mesa. Developer instructions: [build guide](docs/BUILD.en.md). Further details: [installation and edition check](docs/INSTALL.en.md), [fourth CPU core](docs/FOURTH_CORE.en.md).

Port code: GPL-3.0, see [LICENSE](LICENSE). Third-party components: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Credits: ReXGlue/Xenia, masseffect-nx, project8-sw, NFSMW-NX, Mesa and AMD FidelityFX. Not affiliated with Radical Entertainment, Activision, Microsoft or Nintendo.
