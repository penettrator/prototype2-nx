# PGO profiles / Профили PGO

GCC 15.2 arc-count profiles collected on Nintendo Switch, for the generated guest code and the native application. These are compiler branch counters, not game assets or generated game source. Used by `tools/build_switch.sh` with `PGO=USE` (default). Keep filenames unchanged. Build inside the supplied Docker layout `/work`.

Счётчики ветвлений GCC 15.2 для гостевого кода и нативной части, собранные на Nintendo Switch. Это профили компилятора, не игровые данные и не сгенерированные исходники игры. Скрипт сборки использует их по умолчанию (`PGO=USE`). Не переименовывайте файлы; используйте Docker-разметку `/work`.
