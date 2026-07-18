# Сделать — ChronoForge после 1.0

Это единственный актуальный backlog после релиза 1.0. Реализованное состояние находится в [done.md](done.md), подробная история — в [CHANGELOG.md](../CHANGELOG.md).

ChronoForge остаётся офлайн-инструментом для быстрых нестандартных глитчей. Новые задачи не должны превращать его в project manager, монтажную систему, compositor или live-performance платформу.

## Приоритеты

1. Export Current Frame as PNG.
2. Встроенная Help.
3. Профилирование и поэтапный Metal/GPU backend.
4. Простой `Apply To` и минимальная анимация параметров, только если не перегружают workflow.
5. Effect Wave B.
6. Дополнительные image-sequence форматы.

Порядок может меняться после пользовательской проверки 1.0 и профилирования реальных клипов.

## 1. Export Current Frame as PNG

Добавить рядом с основным Export действие `Export Current Frame…`, сохраняющее выбранный во Viewer кадр как один PNG в максимальном доступном разрешении результата.

Требования:

- экспортировать full-resolution output после всего effect stack, а не proxy bitmap Viewer;
- нормализованно отображать текущую Viewer position на timeline результата после shape-changing эффектов;
- не делать искусственный upscale выше настоящего output shape;
- сохранять PNG 8-bit RGBA с той же alpha policy, что и PNG Sequence;
- предлагать имя `ChronoForge_Frame_000123.png` и не перезаписывать файл молча;
- не менять session state, current frame, Undo/Redo или Render Queue;
- переиспользовать валидный full-render cache;
- показывать отдельный cancellable progress.

Для local/spatial узлов renderer может вычислять только нужный frame/tile. Temporal, recursive, FFT, sorting и shape-changing узлы обязаны получить весь необходимый dependency range. Single-frame export не имеет права приблизительно имитировать full render.

Приёмка:

- PNG совпадает с тем же кадром PNG Sequence/full render;
- первый, средний и последний кадры правильно отображаются после изменения T;
- alpha edges сохраняются без fringe;
- повторный экспорт из cache заметно быстрее первого.

## 2. Встроенная Help

Добавить документацию из меню Help для пользователей без опыта в tensor/video processing:

- краткое объяснение session workflow, A/B, Preview, Image AA, Time AA, FPS, compare, queue и export;
- отдельная страница каждого эффекта с визуальным принципом действия;
- описание каждой кнопки, picker и slider, единиц, диапазона и безопасной стартовой точки;
- объяснение Amount и всех blend modes;
- контекстные ссылки `?` из Inspector и toolbar;
- searchable glossary: alpha, premultiplied, proxy, luma, chroma, FFT, stride, address и datamosh;
- автоматическая проверка документации против `EffectDefinition`, чтобы Help не расходилась с UI.

## 3. Metal/GPU и дальнейшая оптимизация

CPU backend остаётся reference implementation и fallback. Переносить код на GPU следует только после end-to-end профилирования, включающего upload/download, SSD I/O и cache serialization.

Архитектурные правила:

- descriptor, effect semantics, cache key и premultiplied-linear alpha не зависят от backend;
- CPU/GPU совпадают в установленном tolerance при одинаковых параметрах и seed;
- adjacent GPU-compatible nodes выполняются без промежуточного возврата tensor на CPU;
- pipeline states создаются и кэшируются заранее;
- Apple Silicon использует shared/unified-memory buffers и zero-copy `CVPixelBuffer`/IOSurface interop там, где копирование действительно исключается;
- full render работает tiles/chunks через bounded staging buffers и не требует всего клипа в памяти;
- double/triple buffering перекрывает SSD read, GPU execution и SSD write;
- command-buffer concurrency ограничивается memory working-set budget;
- memory pressure, unsupported hardware или kernel failure автоматически включают CPU fallback.

Порядок переноса:

1. Prefilter, Amount, blend modes, alpha flatten и общие resize/sample primitives.
2. Sync Loss, RGB Time Slip, Chroma Carrier Drift, Bitplane Forge, Block Address Corruption, Signal Weave, Block Graft и простые displacement modes.
3. Axis Datamosh, Optical Flow Time Warp и Time Feedback с bounded temporal ring buffer.
4. Spectral Morph, 3D FFT Transform, Pixel Sort (Time) и Space-Time Transform только после отдельных замеров transpose, FFT scratch, sorting и SSD exchange.

Параллельные CPU-задачи:

- representative Standard/High release benchmarks;
- устранение повторных coordinate/alpha samples и временных allocations с обязательным parity-test;
- reuse scratch buffers и FFT plans, если allocations видны в профиле;
- bounded persistent thread pool вместо создания потоков на каждый pass, если это ускоряет реальные короткие задачи.

Приёмка GPU-этапа:

- cold/warm benchmarks измеряют import → Preview и full render → export;
- regression corpus сравнивает CPU/GPU для alpha, edges, seed, shape changes и B drivers;
- ускорение устойчиво с учётом передачи данных и не нарушает peak-memory budget;
- Preview cancellation не отменяет export;
- backend boundary позволяет будущую Windows-реализацию без переписывания эффектов.

## 4. Apply To и простая анимация

Будущий Amount может быть умножен на простой mask weight:

```text
effectiveAmount(t, y, x) = amount × mask(t, y, x)
```

Допустимый первый UI `Apply To`: Entire Image, Shadows, Highlights, Edges, Moving Areas и Driver B. Рисуемые маски и mask timeline пока не планируются.

Если параметрам понадобится анимация, первая версия ограничивается:

- Static или Start → End;
- Linear, Ease или Ping-Pong;
- нормализованным временем текущего node input;
- без curve editor.

До реализации требуется однозначная семантика для узлов, меняющих ось T.

## 5. Effect Wave B

Кандидаты:

- Field Rupture;
- Palette/Index Corruption;
- Synthetic Codec Datamosh;
- Temporal Interleave;
- Difference Infection;
- Motion Graft;
- Memory Exchange;
- Motion-Vector Transplant;
- Vertical Roll / Head Switching Noise;
- Slice Repeat / Band Infection.

Им нужны новые stateful, motion-vector, palette или codec-like primitives. Не добавлять упрощённые имитации только ради количества эффектов.

Каждый новый эффект обязан получить:

- proxy и file-backed реализацию;
- Amount/shape policy;
- deterministic seed, если есть randomness;
- alpha и edge tests;
- proxy/full parity;
- visual-regression fixture и performance classification.

## 6. Будущие форматы

- PNG 16-bit — только после корректного alpha round-trip.
- TIFF sequence.
- EXR sequence.
- Форматный слой расширять через `MediaSource`/`FrameSequenceSource`, не через проверки расширений в `SessionStore`.
- Не добавлять FFmpeg только ради ещё одного image-sequence формата.

## Не входит в ближайший план

Без отдельного пересмотра продуктовой рамки не добавлять:

- обычные Save/Open Project, presets, galleries и collections;
- live VJ, MIDI/OSC и audio-reactive controls;
- plugin/SDK ecosystem;
- сложные рисуемые masks, compositor и полноценный curve editor;
- temporal brush и timecode workflow;
- ProRes export.

## Общие guardrails

- Не ухудшать алгоритм ради benchmark без визуального сравнения.
- Не утверждать parity Preview/full, если backend или colour policy различаются.
- Проверять RAM и свободное место до full render.
- Сохранять независимые cancellation domains для Preview, decode и export.
- Не удалять пользовательские output-файлы автоматически.
- Любая новая задача должна сохранять быстрый одноразовый workflow и разнообразие эффектов.
