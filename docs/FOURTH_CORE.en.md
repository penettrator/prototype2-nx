# Fourth CPU core — Prototype 2, 0.1.0

[Русский](FOURTH_CORE.ru.md)

Switch cores are0–3; the fourth is core3. Access depends on process permissions and game thread masks.

## Ready-to-use NSP — recommended

Download `Prototype2-forwarder.nsp` from [Releases](https://github.com/penettrator/Prototype2-NX/releases), install it using DBI and start the game from HOME. It already permits cores0–3. The included TOML is prepared; no `atmosphere/hbl.nsp` replacement is needed.

## Game settings

Check existing lines in `SD:/switch/Prototype2/prototype2.toml`:

```toml
p2_guest_core_mask = 15
p2_audio_core_mask = 8
p2_main_thread_mask = 14
p2_host_core_mask = 6
p2_skinning_helper_mask = 8
p2_skinning_parallel_min = 128
p2_audio_engine_mask = 8
p2_audio_engine_priority = 41
```

`config/prototype2.toml` already contains these values. Masks15,8,14,6 mean cores0–3, core3 only, cores1–3 and cores1–2 respectively. Fully close and relaunch after changes.

## R/hbmenu launch

Your `SD:/atmosphere/hbl.nsp` controls permissions. If it permits only cores0–2, editing TOML alone cannot add core3.

Use the ready-to-use NSP to access four cores without replacing the system loader. To build your own forwarder, see [BUILD.en.md](BUILD.en.md).

## Disabling core3 use

Set these TOML values:

```toml
p2_guest_core_mask = 7
p2_audio_engine_mask = 0
p2_audio_engine_priority = 0
p2_audio_core_mask = 6
p2_main_thread_mask = 6
p2_host_core_mask = 6
p2_skinning_helper_mask = 6
```

Fully close and relaunch. System services also use core3; permission does not grant exclusive ownership of the core.
