# Установка Prototype 2 на Nintendo Switch — 0.1.0

[English](INSTALL.en.md)

## Что нужно

- Nintendo Switch с Atmosphère.
- Минимум 10 ГБ свободного места на SD; FAT32 подходит.
- Официальная копия Prototype 2 для Xbox 360.
- DBI для установки NSP либо hbmenu в режиме приложения.

Для установки скачайте готовые `Prototype2-NX.zip` и `Prototype2-forwarder.nsp` из раздела [Releases](https://github.com/penettrator/Prototype2-NX/releases). Игровые данные добавьте из собственной копии игры.

## 1. Файлы порта

Распакуйте `Prototype2-NX.zip` в корень SD. В `SD:/switch/Prototype2/` появятся `prototype2.nro` и `prototype2.toml`. Собирать порт самостоятельно не требуется. Исходники и [инструкция сборки](BUILD.ru.md) предназначены для разработчиков.

## 2. Данные игры

Распакуйте ISO своей Xbox 360 копии, например с помощью [extract-xiso](https://github.com/XboxDev/extract-xiso/releases):

```text
extract-xiso.exe -x "Prototype 2.iso" -d game_root
```

Если файлы уже распакованы, этот шаг пропустите. Формат GOD требует подходящего для него распаковщика; переименование GOD в ISO не выполняет конвертацию.

Скопируйте `game_root` в `SD:/switch/Prototype2/`. Папка `$SystemUpdate` не нужна. Проверенное издание имеет SHA-256 `default.xex`:

```text
f6caf82c433b0bb20fe701912388b1e7ee8d6b529d8fc275af960ac2e9ddcab8
```

Итоговая структура:

```text
SD:/switch/Prototype2/
  prototype2.nro
  prototype2.toml
  game_root/
    default.xex
    boot.rcf
    cells.rcf
    ...остальные файлы игры
```

## 3. Запуск

**С HOME:** установите `Prototype2-forwarder.nsp` через DBI и запустите Prototype 2 с появившейся иконки. Установочный NSP можно удалить после установки; папку игры на SD оставьте. Форвардер запускает `sdmc:/switch/Prototype2/prototype2.nro`.

**Через hbmenu:** полностью закройте запущенное приложение, удерживайте R при запуске обычной установленной игры и держите до открытия hbmenu. Выберите Prototype 2. Через Альбом запускать нельзя: памяти недостаточно.

## 4. Настройки

Язык меняется в `SD:/switch/Prototype2/prototype2.toml`: `user_language = 12` — русский, `user_language = 1` — английский. Наличие текста и озвучки зависит от данных издания. После правок полностью закройте и снова запустите игру.

Готовый `Prototype2-forwarder.nsp` разрешает все четыре ядра и 39-битное адресное пространство; настройки уже выставлены. Для запуска с HOME заменять системный `hbl.nsp` не нужно. [Подробнее](FOURTH_CORE.ru.md).

Для частот используйте свой менеджер, например sys-clk, и значения, поддерживаемые вашей ревизией консоли. Title ID форвардера: `0150524F54320000`.

## 5. Обновление

Перед обновлением сохраните свои настройки и сохранения. Заменяйте только файлы порта из нового релиза; повторно копировать `game_root` обычно не требуется. Не удаляйте пользовательские данные и кеши.
