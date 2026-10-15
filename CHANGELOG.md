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
