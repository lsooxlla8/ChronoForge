# Сделано — ChronoForge 1.0.1

Релиз: **1.0.1**, 19 июля 2026 года.

Платформа: Apple Silicon, macOS 14 Sonoma и новее.

Подробная история изменений: [CHANGELOG.md](../CHANGELOG.md).

Этот файл фиксирует фактическое состояние приложения. Он заменяет завершённую часть прежнего `next-development-phase.md` и не является списком обещаний.

## Продукт

ChronoForge — офлайн-приложение для экспериментального глитчинга видео и PNG sequence. Это не монтажная система, не project manager и не live-VJ инструмент.

- Сессия одноразовая: обычного Save/Open Project нет.
- Нормальный запуск всегда начинается с пустого workspace.
- Во время работы сохраняется только скрытый crash-recovery snapshot.
- После нормального выхода recovery snapshot и весь proxy/full-render cache удаляются.
- После crash или force quit приложение предлагает восстановить прерванную сессию.
- Исходные файлы не изменяются; постоянным результатом работы является экспорт.

## Рабочий процесс и интерфейс

- Импорт одного или нескольких видео; основной источник обозначается как A, дополнительный источник эффекта — B.
- Импорт нумерованных PNG sequences с natural sorting, проверкой размеров, предупреждением о пропусках и выбором FPS.
- Стек эффектов с Add Effect, icon-only Random и Clear.
- Категории эффектов: Time, Space, Signal, Memory, Data, Organic Systems, Multi-Source и Output.
- Автоматическое обновление Preview после committed edit; ручных Auto Update и Update Preview нет.
- High Preview выбран по умолчанию и сохраняет частоту источника до 30 FPS; Standard ограничен 10 FPS.
- Image AA, Time AA, Result/custom FPS и Audio Original/None.
- Before/After по кнопке и latched-переключателю `\`; compare не мутирует стек.
- Selected Effect compare показывает непосредственный input/output выбранного узла.
- Undo/Redo до 100 операций с объединением slider drag в одну запись.
- Random заменяет весь стек из 1–3 совместимых эффектов одной Undo-операцией.
- Inspector имеет фиксированную ширину и scroll; выбор эффекта не меняет геометрию toolbar.
- Светлая и тёмная темы; Viewer всегда композитится на чёрный фон.

## Amount, random и детерминизм

- Каждый shape-compatible эффект поддерживает Amount от identity до полного результата.
- Blend modes: Normal, Add, Screen, Multiply, Difference, Displace и XOR Glitch.
- Shape-changing режимы не предлагают Partial Amount, если невозможно сохранить размер tensor.
- Стохастические эффекты используют сохранённый 64-bit seed и действие Reseed.
- Random Stack применяет effect-specific распределения, не создаёт two-input эффект без B, не сочетает несколько global-cost узлов и держит Seamless Loop последним.
- Одинаковый source fingerprint, graph, параметры и seed дают одинаковый результат независимо от порядка worker threads.

## Media и экспорт

- Общая модель `MediaSource` для movie и frame sequence.
- AVFoundation proxy/full decode для видео.
- Потоковый full-resolution decode PNG sequence без загрузки всей последовательности в RAM.
- Экспорт H.264 MP4 с опциональным оригинальным аудио.
- Экспорт PNG Sequence в 8-bit RGBA с сохранением alpha.
- Result, preset и custom FPS переинтерпретируют готовые кадры без скрытого resampling.
- При изменённом FPS Original Audio отключается, чтобы не создавать рассинхронизацию.
- Render Queue хранит immutable snapshot источников, стека и настроек каждого задания.
- Отмена Preview не отменяет уже запущенный export.

## Render engine

- C++20 tensor core с каноническим порядком `T, H, W, C` и linear float32 premultiplied RGBA.
- Версионированный `CFEffectDescriptorV2`: восемь value/option slots, logical counts, Amount и seed.
- Один effect graph используется Swift Preview, C bridge и file-backed full render.
- Full-resolution intermediates обрабатываются как memory-mapped SSD tensors.
- Перед full render проверяются требуемый объём и свободное место с обязательным filesystem reserve.
- Незавершённые scratch-файлы удаляются после cancel; пользовательские output-файлы не удаляются молча.
- Cache key включает source fingerprints, graph, параметры, options, Amount, seed и engine version.
- Preview cache и full-render cache переиспользуются внутри сессии и удаляются при нормальном завершении приложения.
- Монотонный playback scheduler не накапливает задержку: при опоздании UI перескакивает к актуальному кадру.

## Эффекты одного видео

- **Space-Time Transform** — swap Width/Time или Height/Time и 3D rotation видеообъёма.
- **Self Time Displacement** — смещение времени яркостью или отдельным каналом текущего клипа.
- **Polar Time Warp** — полярные temporal braid/fold/orbit структуры, rotation координат и бесшовный angular field.
- **Pixel Sort (Time)** — сортировка по времени через luma, saturation или hue key; Ascending, Descending, Zigzag и Center Out.
- **3D FFT Transform** — swap и rotation пространственно-временного спектра.
- **RGB Time Slip** — независимые временные offsets RGB и spatial split.
- **Sync Loss** — горизонтальные или вертикальные tearing bands с нормализованным размером.
- **Chroma Carrier Drift** — независимый drift Cb/Cr при сохранении текущей luma.
- **Stride Error** — безопасная имитация неверной длины строки и адресации памяти.
- **Block Address Corruption** — детерминированная замена spatial/temporal block addresses.
- **Bitplane Forge** — quantize и Shuffle/Rotate/Invert/XOR выбранных bitplanes.
- **Optical Flow Time Warp** — смещение времени на основе локального движения.
- **Time Feedback** — recursive past/future feedback, включая coordinate displacement.
- **Axis Datamosh** — удержание и протягивание данных по Time/X/Y с bright/dark luma trigger.
- **Seamless Loop** — Crossfade, Luma Weave, Difference Weave, Spectral Morph и Ping-Pong; переход Start/End.
- **Affinity Migration** — motion-responsive миграция cell regions между соседними colour classes без temporal RGB feedback.

## Эффекты двух видео

- **Space-Time Map** — RGB источника B задаёт координаты X/Y/Time чтения A.
- **Space-Time Displacement** — B смещает A по X/Y/Time.
- **Signal Weave** — Lines, Fields, Bands и Checker между A/B.
- **Block Graft** — перенос удерживаемых блоков B по random/luma/difference/edge trigger.
- **Channel Transplant** — независимый routing RGB или YCbCr компонентов A/B.

## Проверка качества 1.0

- Unit tests для маленьких tensor, edge modes, alpha и shape changes.
- Proxy/file-backed parity для всех эффектов и Amount modes.
- 10 000 seeded Random Stack generations без invalid graph.
- AVFoundation integration, sequence round-trip, cancellation и queue snapshot tests.
- Процедурный visual-regression corpus без чужих copyrighted assets.
- Release performance-smoke на Standard proxy; локальные и temporal эффекты укладываются в установленные бюджеты.
- Self-test полного пути decode → proxy → effect → mapped render → MP4/PNG export.

Команды релизной проверки:

```bash
cmake -S . -B build -DCHRONOFORGE_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
swift build
swift run ChronoForgeIntegration
swift run ChronoForgeMac --self-test
```

## Definition of Done 1.0

- [x] Ephemeral workflow без обычных проектов.
- [x] Crash-only recovery и очистка cache при clean quit.
- [x] Undo/Redo, Before/After, Selected Effect compare и automatic Preview.
- [x] Amount, blend modes, deterministic Random Stack и categorized Add Effect.
- [x] Multi-source A/B workflow.
- [x] PNG sequence import/export, FPS reinterpretation и alpha-safe processing.
- [x] Wave A и расширенные temporal/global glitch effects.
- [x] Render Queue, SSD guardrails и cancellation domains.
- [x] Core, integration, visual, performance и full-pipeline проверки.
