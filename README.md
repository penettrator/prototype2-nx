# Prototype 2 для Nintendo Switch — 0.1.0

[English](README.en.md)

Автор порта: [penettrator](https://github.com/penettrator). Поддержка: [Boosty](https://boosty.to/itgenius).

Нативный порт Xbox 360 версии Prototype 2 на Nintendo Switch. Нужна собственная официальная копия игры; игровые данные в скачиваемые файлы порта не входят.

## Что нужно

- Nintendo Switch с Atmosphère.
- Не менее 10 ГБ свободного места на SD; FAT32 подходит.
- DBI или другой менеджер установки NSP.

## Установка

### 1. Скачайте файлы порта

Скачайте готовые файлы из раздела [Releases](https://github.com/penettrator/Prototype2-NX/releases):

| Файл | Назначение |
|---|---|
| `Prototype2-NX.zip` | Готовый порт и настройки: папка `switch/Prototype2/` |
| `Prototype2-forwarder.nsp` | Иконка игры в HOME; разрешает использование четырёх ядер |

Распакуйте `Prototype2-NX.zip` в корень SD-карты. Получится `SD:/switch/Prototype2/`. Самостоятельная сборка для установки не нужна.

### 2. Добавьте свою игру

Если игра уже распакована, назовите папку с `default.xex` **`game_root`**. Иначе распакуйте свою Xbox 360 ISO с помощью [extract-xiso](https://github.com/XboxDev/extract-xiso/releases):

```text
extract-xiso.exe -x "Prototype 2.iso" -d game_root
```

Подставьте имя своего ISO. Скопируйте всю папку `game_root` в `SD:/switch/Prototype2/`. Папка `$SystemUpdate` не нужна.

```text
SD:/switch/Prototype2/
  prototype2.nro
  prototype2.toml
  game_root/
    default.xex
    boot.rcf
    cells.rcf
    ...остальные файлы вашей игры
```

### 3. Установите и запустите

Установите **`Prototype2-forwarder.nsp`** через DBI и запустите Prototype 2 с иконки в HOME. NSP запускает NRO с SD, поэтому оставьте папку `switch/Prototype2/` на карте.

**Четвёртое ядро уже разрешено этим NSP, настройки подготовлены. Заменять `atmosphere/hbl.nsp` или править системные файлы не нужно.**

Альтернатива: удерживайте **R** при запуске обычной установленной игры, дождитесь hbmenu и выберите Prototype 2. Доступ к четвёртому ядру при таком запуске зависит от вашего загрузчика. **Через Альбом не запускайте:** памяти недостаточно.

## Настройки

Файл: **`SD:/switch/Prototype2/prototype2.toml`**. После изменений полностью закройте и снова запустите игру.

| Параметр | По умолчанию | Значение |
|---|---|---|
| `user_language` | `12` | `12` — русский, `1` — английский; доступность зависит от издания игры |
| `p2_render_width`, `p2_render_height` | `640`, `352` | Внутреннее разрешение |
| `present_effect` | `"fsr"` | Масштабирование изображения через FSR 1 |

При обновлении заменяйте файлы порта. `game_root` копировать повторно не нужно. Перед заменой TOML сохраните свои изменённые настройки.

## Известные проблемы

- Просадки при загрузке данных и создании графических pipeline.

## Исходники и лицензии

Порт использует статическую рекомпиляцию ReXGlue под ARM64 и Vulkan-рендерер на NVK/Mesa. Инструкции для разработчиков: [сборка](docs/BUILD.ru.md). Дополнительно: [установка и проверка издания](docs/INSTALL.ru.md), [четвёртое ядро](docs/FOURTH_CORE.ru.md).

Код порта — GPL-3.0: [LICENSE](LICENSE). Сторонние компоненты: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Благодарности ReXGlue/Xenia, masseffect-nx, project8-sw, NFSMW-NX, Mesa и AMD FidelityFX. Проект не связан с Radical Entertainment, Activision, Microsoft или Nintendo.
