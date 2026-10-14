# Prototype 2 — 0.1.0

## Русский

- Первый релиз исходников порта Xbox360 на Nintendo Switch.
- Статическая рекомпиляция ReXGlue под ARM64 и нативный Vulkan-рендерер NVK/Mesa.
- Конфигурация640×352 с FSR1, исправления коллизий и обработки запросов видимости солнца.
- Инструкции установки и сборки RU/EN, патчи зависимостей и скрипт сборки NSP-форвардера.

## English

- Initial source release of the Xbox360-to-Nintendo Switch port.
- ReXGlue static recompilation for ARM64 with a native Vulkan renderer using NVK/Mesa.
- 640×352/FSR1 configuration, collision and sun-visibility query handling fixes.
- RU/EN installation/build guides, dependency patches and an NSP forwarder build script.

- Кеш результатов skinning, актуализация скелетных буферов и настройка аудиопотока на core3. / Skinning result cache, fresh skinned-buffer identities and audio-thread core3 placement.
- Релизная интеграция FPS без профилирования; самостоятельные данные защищённых функций для сборки. / Release FPS integration without profiling; standalone protected-hook data for building.
