# Prototype 2 — 0.4.0

## Русский

- Стабильность: игра берёт 256 МБ простаивающей памяти апплетов для драйвера GPU. Это делает модуль `p2mem.nsp` из пакета: игра сама копирует его в `atmosphere/contents/010000000050324D/` и запускает, он закрывается вместе с игрой. Основная куча раньше в тяжёлых местах доходила до предела, теперь у неё ~250 МБ запаса. HOME, скриншоты и клавиатура работают. Выключение: `p2_applet_memory_mb = 0`.
- Производительность: поток, который переводит команды игры для GPU, тратит на кадр около 19 % меньше времени процессора. Не менявшиеся вершинные данные и текстуры перепроверяются реже (`p2_ring_light_checks`, `p2_texture_light_trust`).
- Драйвер Mesa/NVK собран с `-O2` вместо `-O1`.

## English

- Stability: the game borrows 256 MB of idle applet memory for the GPU driver. The helper `p2mem.nsp` from the package does this: the game copies it to `atmosphere/contents/010000000050324D/` and starts it, and it exits with the game. The main heap used to reach its limit in heavy scenes; it now has ~250 MB of headroom. HOME, screenshots and the keyboard keep working. To turn it off: `p2_applet_memory_mb = 0`.
- Performance: the thread that translates the game's commands for the GPU spends about 19 % less CPU time per frame. Vertex data and textures that never change are re-checked less often (`p2_ring_light_checks`, `p2_texture_light_trust`).
- The Mesa/NVK driver is built with `-O2` instead of `-O1`.

# Prototype 2 — 0.3.0

## Русский

- Производительность: выше частота кадров в городе и в бою, заметно реже длинные подвисания (кадры дольше 50 мс). Самые частые функции системы задач игры работают без глобального мьютекса, у главного потока игры выше приоритет, анимацию персонажей считают два вспомогательных потока, SDK не будит потоки на семафорах, которых никто не ждёт.
- Переработана кодогенерация: прямые вызовы для самых частых косвенных переходов, вычисления одинарной точности без лишних преобразований, встроенные короткие memcpy/memset, свёртка констант из данных только для чтения.
- Пул памяти текстур и обновлённые профили PGO.
- Исправлен зелёный кадр перед каждой видеовставкой.
- Исправлено свечение солнца и луны сквозь мосты и здания.
- Улучшена совместимость сохранений; размер файла обновляется после записи. Дополнительная диагностика выключена.
- Сборка из исходников повторяет релизную: шаги производительности выполняет `tools/prepare_p2_perf.py`, см. [сборка](docs/BUILD.ru.md).

## English

- Performance: higher frame rate in the city and in combat, far fewer long hitches (frames over 50 ms). The game's most frequent job-system functions run without the global mutex, the game's main thread has a higher priority, character animation runs on two helper threads, and the SDK no longer wakes threads on semaphores nobody waits on.
- Reworked code generation: direct calls for the most frequent indirect branches, single-precision arithmetic without extra conversions, inlined short memcpy/memset, folding of constants from read-only data.
- Texture memory pool and updated PGO profiles.
- Fixed the green frame before every video.
- Fixed the sun and moon glow showing through bridges and buildings.
- Improved save compatibility; the file size is refreshed after writes. Optional diagnostics are off.
- Building from source reproduces the release: `tools/prepare_p2_perf.py` applies the performance steps, see the [build guide](docs/BUILD.en.md).

# Prototype 2 — 0.2.0

## Русский

- Статическая рекомпиляция Xbox 360 под ARM64 через ReXGlue, нативный Vulkan-рендерер NVK/Mesa.
- PGO для гостевого кода и нативной части; профили и воспроизводимый способ сборки включены.
- Повторное использование анализа входных данных skinning и кеша результатов. Обновление анимации толпы через кадр выключено.
- Внутреннее разрешение 800×448 с масштабированием FSR 1.
- Исправлены проваливания персонажей в геометрию и исчезновение материалов после длительного открытия карты.
- Обновлена обработка запросов видимости: объединение GPU-запросов без принудительного видимого результата после большого числа отрисовок.
- Всплывающие уведомления достижений выключены; получение и сохранение достижений сохранены.
- L+R+ZR сохраняет скриншот независимо от меню отладки.
- Релизная сборка без stack/task audit и SDK-профилировщика; базовая интеграция FPS сохранена.
- RU/EN инструкции установки/сборки, патчи зависимостей и HOME-форвардер с разрешением ядер 0–3.

## English

- ReXGlue static Xbox 360 recompilation for ARM64 and a native NVK/Mesa Vulkan renderer.
- PGO for guest code and the native application, with profiles and a reproducible build workflow included.
- Reused skinning input scans and cached results. Half-rate crowd animation remains disabled.
- Internal resolution of 800×448 with FSR 1 upscaling.
- Fixed characters falling through geometry and missing materials after leaving the map open.
- Updated visibility-query handling: batched GPU queries without forcing a visible result after many draws.
- Achievement popups disabled; achievement unlocking and saving preserved.
- L+R+ZR screenshots work independently of debug menus.
- Release build excludes stack/task audits and the SDK profiler, retaining basic FPS integration.
- RU/EN installation/build guides, dependency patches and a HOME forwarder allowing cores 0–3.
