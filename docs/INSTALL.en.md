# Installing Prototype 2 on Nintendo Switch — 0.1.0

[Русский](INSTALL.ru.md)

## Requirements

- Nintendo Switch running Atmosphère.
- At least 10 GB free on SD; FAT32 works.
- An official Xbox 360 copy of Prototype 2.
- DBI for NSP installation or hbmenu in application mode.

For installation download the ready-to-use `Prototype2-NX.zip` and `Prototype2-forwarder.nsp` from [Releases](https://github.com/penettrator/Prototype2-NX/releases). Add game data from your own copy.

## 1. Port files

Extract `Prototype2-NX.zip` to the SD root. You will get `prototype2.nro` and `prototype2.toml` in `SD:/switch/Prototype2/`. No compilation is required. Source code and the [build guide](BUILD.en.md) are intended for developers.

## 2. Game data

Extract your Xbox 360 ISO, for example using [extract-xiso](https://github.com/XboxDev/extract-xiso/releases):

```text
extract-xiso.exe -x "Prototype 2.iso" -d game_root
```

Skip extraction if you already have the extracted files. GOD containers need an appropriate extractor; renaming GOD to ISO does not convert them.

Copy `game_root` to `SD:/switch/Prototype2/`. The `$SystemUpdate` folder is unnecessary. The tested default.xex SHA-256 is:

```text
f6caf82c433b0bb20fe701912388b1e7ee8d6b529d8fc275af960ac2e9ddcab8
```

Expected layout:

```text
SD:/switch/Prototype2/
  prototype2.nro
  prototype2.toml
  game_root/
    default.xex
    boot.rcf
    cells.rcf
    ...remaining game files
```

## 3. Launch

**From HOME:** install `Prototype2-forwarder.nsp` using DBI and start Prototype 2 from its icon. You may delete the NSP installer afterwards; keep the game folder on SD. The forwarder opens `sdmc:/switch/Prototype2/prototype2.nro`.

**Through hbmenu:** fully close the running application, hold R while starting a normal installed game and keep holding it until hbmenu opens. Select Prototype 2. Do not use Album: applet memory is insufficient.

## 4. Settings

Edit `SD:/switch/Prototype2/prototype2.toml`: `user_language = 12` selects Russian; `user_language = 1` selects English. Available text/audio depends on the edition's data. Fully close and relaunch after changes.

The ready-to-use `Prototype2-forwarder.nsp` permits all four cores and 39-bit address space; settings are already prepared. No system `hbl.nsp` replacement is needed for HOME launch. [More details](FOURTH_CORE.en.md).

For clocks use your existing manager, such as sys-clk, and values supported by your console revision. Forwarder Title ID: `0150524F54320000`.

## 5. Updates

Back up your settings and saves before updating. Replace only the port files from the new release; normally no need to copy `game_root` again. Keep user data and caches.
