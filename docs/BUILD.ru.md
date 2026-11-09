# Сборка Prototype 2 — 0.4.0

[English](BUILD.en.md)

Нужны Windows, Git/Git Bash, PowerShell, Python3, Docker Desktop с Linux-контейнерами, достаточно диска/RAM для devkitA64/LTO и ваша официальная копия игры.

## 1. Зависимости на зафиксированных коммитах

Из корня `Prototype2-NX` выполните команды ниже. Не меняйте версии произвольно: патчи рассчитаны на эти базы.

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

SDK-патч включает 65 изменённых/добавленных файлов, в том числе совместимые ARM64/потоки/рендерер/очередь вывода. SDK и Mesa не входят готовыми библиотеками в исходный пакет. Сохраняйте глобальное определение `REX_HAS_FIDELITYFX_SDK=1` в switch/CMakeLists: оно влияет на layout класса Presenter, поэтому при изменении требуется полная пересборка всех зависимых целей.

## 2. Данные и кодогенерация

Распакуйте свой диск в `assets/game_root/`; default.xex должен иметь SHA из README. Скачайте [ReXGlue SDK0.10.0 для Windows](https://github.com/rexglue/rexglue-sdk/releases), распакуйте так, чтобы существовал `tools/bin/rexglue-0.10.0/win-amd64/bin/rexglue.exe`.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/codegen.ps1
```

Результат — `project/generated/`. Не добавляйте его или `assets/` в Git. Манифест содержит дополнительные адреса и привязки XAPI; применяемые prepare_* шаги защищают ручные переопределения.

### Шаги производительности 0.3.0

После стандартных `prepare_*` скрипт `tools/build_switch.sh` запускает `tools/prepare_p2_perf.py` — шаги, с которыми собран релиз: наборы msr-регионов без глобального мьютекса, прямые вызовы горячих косвенных переходов по `config/icall_census_pass13.csv` (функции из `config/direct_call_keep.txt` остаются переопределяемыми), одинарная точность для вещественных загрузок и операций, встраивание коротких memcpy/memset, журналы отказов аллокатора. Шаги выполняются один раз на каждую свежую кодогенерацию: в конце `prototype2_pch.h` ставится метка, повторный запуск ничего не меняет.

Свёртка констант `.rdata` требует образ загруженного XEX: `build/image_dump.bin` (гостевые адреса с 0x82000000, 0xFD0000 байт). Это игровые данные, в репозитории их нет. Образ записывает PC-сборка порта (`project/`), если запустить её с переменной `PROTOTYPE2_DUMP_IMAGE=build\image_dump.bin`. Без образа шаг пропускается с предупреждением: сборка корректна, но константы остаются загрузками из памяти и NRO немного отличается от релизного. Другой путь: `python tools/prepare_p2_perf.py --image <файл>`.

## 3. Mesa/NVK, один раз

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_mesa.ps1
```

Docker создаёт toolchain image и статический драйвер в `out/mesa-sdk/opt/devkitpro/portlibs/switch`. Эта стадия может быть долгой и требует сети при подготовке образа. Ключи консоли не нужны.

С 0.4.0 драйвер собирается с `-Doptimization=2`, а в конце `tools/redirect_libvulkan.sh` переименовывает в `libvulkan.a` вызовы `memalign`/`free` в `p2_nvk_memalign`/`p2_nvk_free` (`project/src/switch_applet_memory.cpp`): так память GPU драйвера может браться из памяти апплетов. Без этого шага NRO не соберётся. Исходный архив остаётся рядом как `libvulkan.a.orig`.

## 4. NRO

В Git Bash:

```sh
bash tools/build_switch.sh
```

По умолчанию8compile jobs, изменить можно `JOBS=4 bash tools/build_switch.sh`. Используются guest LTO, защищённые direct calls, ARM integer VMX и leaf locals; stack audit выключен. Скрипт создаёт `build/switch/prototype2.nro` версии 0.4.0. Производный hot-код генерируется перед сборкой. Для full-rebuild удаляйте только собственный каталог build, а не игровые данные.

## 5. Пакет

Поместите NRO и `config/prototype2.toml` в `switch/Prototype2/`, добавьте `p2mem.nsp` (`bash tools/build_p2mem.sh` → `out/p2mem/exefs.nsp`, переименовать в `p2mem.nsp`), VERSION 0.4.0, лицензии и двуязычные инструкции. Для публичного релиза не добавляйте game_root, prod.keys/title.keys, личные сохранения/журналы. Для собственной установки добавьте данные своего диска отдельно. Метод title takeover не требует NSP-форвардера. 

## Структура исходников

`project/src` — ручные хуки/нативный renderer; `switch` — CMake/Horizon; `tools` — кодогенерация/сборка/проверки; `patches` — изменения SDK/Mesa; `config` — профиль 0.4.0; `docs` — RU/EN. Ни одна сборка автоматически не подключается к Switch и не перезаписывает SD.

## NSP-форвардер 0.4.0

После сборки NRO с иконкой/NACP используйте Git Bash, Git, Docker и собственный prod.keys:

```sh
SWITCH_PROD_KEYS=/path/to/your/prod.keys bash tools/build_forwarder.sh build/switch/prototype2.nro build/forwarder
```

Получится `build/forwarder/Prototype2-forwarder.nsp`. Title ID `0150524F54320000`, путь `sdmc:/switch/Prototype2/prototype2.nro`, 39-bit, ядра0–3. Скрипт проверяет обе таблицы NPDM и версию NACP 0.4.0. Ключи подключаются к контейнеру только для чтения и не входят в релиз; вывод упаковщика с ключами не печатается. Исходники Forwarder-Mod/hacBrewPack закреплены на коммитах в скрипте.

По умолчанию используются `P2_RELEASE_DIAGNOSTICS_OFF=ON`, `P2_TASK_LIFETIME_AUDIT=OFF` и `P2_GUEST_STACK_AUDIT=OFF`. Модуль `switch/release_perf.cpp` передаёт только базовый FPS/режим; записи профилей и sampler не запускаются. Защищённые функции перечислены в `config/strong-overrides.json`; приватный архив NRO для сборки не требуется.


## PGO

`tools/build_switch.sh` по умолчанию использует `PGO=USE` и 228 профилей GCC из `config/pgo-data`. Оптимизируются гостевой код и нативная часть приложения; в готовом NRO нет обучающего дампера gcov. Для совместимости профилей используйте GCC 15.2 и Docker-разметку `/work`, заданную скриптом. Если версия GCC в образе не совпадает с профилями, сборка останавливается (`tools/check_pgo_compiler.py`). Релиз собран в образе `DEVKITA64_IMAGE=devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282`. Файлы профилей содержат счётчики компилятора, игровых данных в них нет.

Обычная сборка без PGO: `PGO=OFF bash tools/build_switch.sh`. Для самостоятельного обучения: `PGO=GEN bash tools/build_switch.sh`, запустите полученный NRO и поиграйте в разных сценах; профили записываются в `SD:/pgo/` раз в три минуты. Скопируйте `.gcda` в `config/pgo-data/` и повторите сборку с `PGO=USE`. Стадии GEN/USE должны использовать совместимые исходники, компилятор и один каталог сборки. GEN использует только счётчики переходов (`-fno-profile-values`), а USE — коррекцию непоследовательных счётчиков. Обучающая запись может надолго задерживать игру; её FPS не сравнивают с готовой сборкой.
