# Четвёртое ядро — Prototype 2, 0.1.0

[English](FOURTH_CORE.en.md)

У Switch ядра0–3; четвёртое — core3. Доступ к нему задаётся разрешениями процесса и масками потоков игры.

## Готовый NSP — рекомендуемый способ

Скачайте `Prototype2-forwarder.nsp` из [Releases](https://github.com/penettrator/Prototype2-NX/releases), установите через DBI и запускайте игру с HOME. Он уже разрешает ядра0–3. Настройки в комплектном TOML подготовлены; заменять `atmosphere/hbl.nsp` не нужно.

## Настройки игры

В `SD:/switch/Prototype2/prototype2.toml` проверьте существующие строки:

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

Конфигурация `config/prototype2.toml` уже содержит эти значения. Маски15,8,14,6 означают соответственно ядра0–3, только3, ядра1–3 и ядра1–2. После изменения полностью закройте и снова запустите игру.

## Запуск через R/hbmenu

Разрешения определяет ваш `SD:/atmosphere/hbl.nsp`. Если он допускает только ядра0–2, редактирование TOML само по себе не добавит core3.

Чтобы использовать четыре ядра без замены системного загрузчика, запускайте игру через готовый NSP. Самостоятельная сборка форвардера описана в [BUILD.ru.md](BUILD.ru.md).

## Если требуется отключить core3

В TOML задайте:

```toml
p2_guest_core_mask = 7
p2_audio_engine_mask = 0
p2_audio_engine_priority = 0
p2_audio_core_mask = 6
p2_main_thread_mask = 6
p2_host_core_mask = 6
p2_skinning_helper_mask = 6
```

Полностью закройте и снова запустите игру. Ядро3 используется и системными службами; разрешение на него не означает монопольное владение ядром.
