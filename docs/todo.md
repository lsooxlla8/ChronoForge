# Сделать — ChronoForge после 1.1

Это единственный актуальный backlog после релиза 1.1. Реализованное состояние находится в [done.md](done.md), подробная история — в [CHANGELOG.md](../CHANGELOG.md).

ChronoForge остаётся офлайн-инструментом для быстрых нестандартных глитчей. Новые задачи не должны превращать его в project manager, монтажную систему, compositor или live-performance платформу.

## Приоритеты

1. Профилирование и поэтапный Metal/GPU backend.
2. Нормализация цвета после Amount, затем простой `Apply To` и минимальная анимация параметров, только если не перегружают workflow.
3. Effect Wave B.
4. Дополнительные image-sequence форматы.

Порядок может меняться после пользовательской проверки 1.0 и профилирования реальных клипов.

## 1. Metal/GPU и дальнейшая оптимизация

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

## 2. Цвет после Amount, Apply To и простая анимация

### Нормализация цвета после Amount

Текущий premultiplied-alpha clamp гарантирует только технически допустимый RGBA (`0 ≤ RGB ≤ alpha`). Он не решает художественную проблему: Add, Screen, Difference и XOR Glitch после Amount могут выбивать цвет из рабочего диапазона, жёстко срезать отдельные каналы и менять hue или воспринимаемую яркость.

Нужно спроектировать общий post-Amount colour stage:

- выполнять его в linear premultiplied RGBA после blend, одинаково в Preview и full render;
- сравнить обычный channel clamp с hue-preserving gamut compression, где RGB масштабируются совместно, а не независимо;
- не использовать нормализацию min/max по целому кадру или клипу: она создаёт temporal pumping и делает один кадр зависимым от посторонних экстремумов;
- не менять результирующую alpha и не поднимать полностью прозрачный RGB;
- определить UI без лишней сложности: вероятные варианты `Clip`, `Preserve Hue` и `Preserve Luma`, либо один безопасный automatic default;
- включить выбранную policy в descriptor/cache compatibility и реализовать одинаково в RAM, SSD-backed и будущем Metal backend.

Приёмка: synthetic saturated RGB/alpha fixtures покрывают все семь Amount Blend modes и Amount 0/0.5/1; output остаётся finite и premultiplied, hue/luma отклонение измеряется документированно, transparent pixels не получают цвет, статичный input не flicker/pump, Preview и full render совпадают в tolerance.

### Apply To и простая анимация

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

## 3. Effect Wave B

Wave B — не набор вариаций обычного Wave Warp. В первую очередь добавить эффекты, использующие необычное представление видео: события, ошибку квантования, граф времени, wavelet-поддиапазоны и структуру движения. Все они должны работать офлайн, без обучения или инференса нейросетей; допустимы только детерминированные CPU-алгоритмы и уже существующий dense Lucas–Kanade flow.

### Общая реализация Wave B

Перед включением любого эффекта из списка:

- расширить `EffectKind`, `EffectRegistry`, C bridge, `EffectSpec`, RAM executor, file executor, proxy/full cache key и Swift state как одну миграцию; не заводить Swift-only параметры;
- описать для каждого узла access pattern: local, sequential temporal или global, точный dependency range и нужный SSD preflight;
- все случайные поля генерировать из stored 64-bit seed и координат/номера кадра, без глобального RNG и без недетерминированных шумов;
- `Amount = 0` — bit-exact bypass. Shape остаётся исходным, поэтому Amount и все стандартные blend modes поддерживаются всеми эффектами;
- использовать linear premultiplied RGBA: guide/scalar metrics считаются по straight/unpremultiplied luma, но цветовой результат всегда re-premultiply; alpha не превращать в случайную маску без отдельного режима;
- добавить в Inspector короткое plain-language объяснение, безопасный default и cost badge; global effects получают 800 ms Auto Update и отменяемый proxy analysis;
- для каждого эффекта: unit tests границ/alpha/seed, proxy–full parity, file-backed fixture, visual-regression fixture, проверка cache invalidation и performance-smoke. Не объединять это с Metal-работой.

### 3.1 Event Scar

**Идея.** Симулировать event camera без нейросети: пиксель «срабатывает», только когда разность логарифмической luma между двумя кадрами превышает порог. Положительные и отрицательные события получают разные цвета и могут затухать, накапливаться или становиться картой temporal displacement.

Реализация:

- для каждого `t > 0` вычислять `d = log(eps + luma_t) - log(eps + luma_{t-1})`; пороговать отдельно `d > +threshold` и `d < -threshold`;
- поддержать `Single Frame`, `Decay Accumulate` (sequential ring buffer) и `Event Displace`; последний использует event density как bounded X/Y/T offset, не как feedback цвета;
- параметры: `Threshold`, `Sensitivity`, `Decay`, `Positive/Negative balance`, `Event size`, `Mode`; options: `Both/Positive/Negative`, `Monochrome/Dual colour/Source colour`, `Clamp/Wrap/Mirror`;
- temporal halo — один предыдущий кадр; `Decay Accumulate` — sequential temporal, остальные режимы — finite halo;
- не имитировать «noise overlay»: на неподвижном кадре output обязан стабильно затухнуть в source/transparent event layer согласно mode.

Приёмка: нулевое движение не создаёт событий; смена знака меняет только polarity colour; одинаковые вход/seed дают byte-identical proxy/full; alpha edge не рождает белого fringe.

### 3.2 Causal Dither

**Идея.** Квантование с error diffusion в трёх измерениях: остаток ошибки течёт в соседние пиксели и в следующий кадр. Это даёт «память» растра вместо независимой постеризации и случайного flicker.

Реализация:

- начать с палитры равномерных уровней в RGB или YCbCr; квантовать straight colour, затем диффундировать error по нормализованному stencil;
- реализовать три fixed stencils: `Scanline` (X/Y), `Time Carry` (часть ошибки в `t+1` того же/смещённого pixel) и `Interlaced` (чётные/нечётные строки имеют разный temporal destination);
- параметры: `Levels`, `Spatial diffusion`, `Temporal carry`, `Temporal offset`, `Palette bias`, `Colour separation`; options: `RGB/YCbCr/Luma`, `Scanline/Time Carry/Interlaced`, `Serpentine/Forward`;
- sequential temporal executor хранит только текущий error tile и следующий error tile, а не полный extra video tensor; file executor обязан обрабатывать tiles в детерминированном scan order;
- Amount смешивает конечный dithered RGBA с source, не частично меняет error weights.

Приёмка: `Temporal carry = 0` не читает/не зависит от соседнего кадра; ошибка сохраняет bounded energy и не выходит в NaN/Inf; одинаковый клип при разном tile size даёт совпадающий результат.

### 3.3 Motion Barcode Raster

**Идея.** Каждая строка или колонка получает временной бинарный штрихкод: пересекала ли её заметная перемена. Эти barcodes либо непосредственно рисуются, либо управляют выборкой source из другого времени.

Реализация:

- получать activity map из `abs(luma_t - luma_{t-1})` с optional small box blur и порогом; не требовать segmentation model;
- для выбранной оси сворачивать activity по перпендикулярной координате в один bit на line/frame; сглаживать/hold бит только в пределах заданного history;
- режимы: `Raster` (полосы/битовые столбцы поверх source), `Time Address` (barcode выбирает bounded temporal offset), `Gate` (оставляет/прячет source line), `RGB Encode` (history разбит между каналами);
- параметры: `Axis`, `Activity threshold`, `History frames`, `Band width`, `Hold`, `Temporal range`; options: `Raster/Time Address/Gate/RGB Encode`, `Rows/Columns`, `Black/Source/Palette`;
- `Raster`/`Gate` имеют halo `history`; `Time Address` должен декларировать расширенный bounded temporal range. History — ring buffer на tile/line, не полный boolean volume.

Приёмка: статичный клип даёт пустой/стабильный barcode; одна движущаяся вертикальная граница активирует предсказуемый диапазон строк; mode не меняет размеры и не разрушает alpha.

### 3.4 Continuity Jump Graph

**Идея.** В proxy-преданализе построить граф похожих кадров, затем заменить прямой ход времени детерминированным walk по этому графу. Видео прыгает в прошлое и будущее почти без обычного монтажного cut.

Реализация:

- на downsampled luma (например, 64×36) строить frame descriptor: low-frequency thumbnail + edge energy + mean chroma; normalise его;
- для каждого frame искать ограниченное число ближайших соседей только за пределами exclusion window вокруг исходного `t`; не создавать полный `T × T` matrix для длинных клипов — использовать temporal bins и top-K candidate search;
- сохранить graph как content-addressed analysis cache, зависящий от source fingerprint, proxy scale и analysis version; full render использует те же normalized frame indices, а не пересчитывает другой graph;
- path начинается с `t`, далее с вероятностью `Jump rate` выбирает weighted compatible neighbor, иначе идёт к следующему output step. Если кандидат слабый, fallback — обычный `t+1`; seed полностью определяет walk;
- параметры: `Jump rate`, `Similarity tolerance`, `Exclusion window`, `Max leap`, `Hold length`, `Edge weight`; options: `Forward only/Bidirectional`, `Smoothest/Most distant`, `Loop/Clamp`;
- global effect: before preview/export выполнить cancellable graph analysis, проверить cache disk и ограничить display/proxy length. Не выдавать это за seamless-loop guarantee.

Приёмка: seed повторяет path; `Jump rate = 0` — exact identity; нет self-jump и запрещённых близких соседей; cache reuse не меняет path; graph не растёт квадратично в RAM.

### 3.5 3D Wavelet Contraband

**Идея.** Разложить volume по X/Y/T на coarse approximation и detail subbands, затем портить только выбранные пространственно-временные детали. Это не существующий 3D FFT: эффект локальнее, многомасштабнее и сохраняет форму при ломании движения.

Реализация:

- первая версия — separable reversible 3D Haar transform на power-of-two padded groups of frames; symmetric boundary extension и crop до исходного shape после inverse transform;
- группировать по `Group size` (8/16/32 frames) с deterministic overlap-add/crossfade на границах групп, чтобы не было hard seam; spatial dimensions pad/crop только во внутренних scratch tensors;
- адресовать subbands как `LLL` и выбранные temporal/spatial detail masks; операции: `Gain`, `Sign Flip`, `Quantize`, `Coefficient Shuffle`, `Magnitude Swap` между разрешёнными detail bands;
- параметры: `Temporal levels`, `Spatial levels`, `Detail gain`, `Quantize`, `Shuffle amount`, `Group overlap`; options: `Temporal/Spatial/Mixed detail`, `Gain/Flip/Quantize/Shuffle/Magnitude Swap`, `Haar v1`;
- file executor пишет transform scratch на SSD и обрабатывает группы с bounded buffers. В v1 не добавлять arbitrary wavelet families и motion-compensated wavelets;
- global cost; Amount применяется только после complete inverse transform.

Приёмка: все operation amounts на нейтральных значениях дают reconstruction в установленном float tolerance; detail-only mode не изменяет DC/средний цвет; group boundaries не видны на constant/ramp fixtures; alpha проходит тем же transform path без fringe.

### 3.6 Prediction-Residual Datamosh

**Идея.** Синтетически разложить frame на motion-predicted прошлый кадр и residual, затем независимо ломать predictor или residual. Результат напоминает codec datamosh, но остаётся безопасным и воспроизводимым — без порчи H.264/HEVC bitstream.

Реализация:

- для `t > 0` оценивать существующий deterministic Lucas–Kanade flow между `t-1` и `t`; получить `pred_t = warp(output_or_input_{t-1}, flow)` и `res_t = input_t - pred_t` в straight linear colour с explicit alpha policy;
- режимы: `Hold Predictor`, `Vector Quantize`, `Block Readdress`, `Residual Drop`, `Residual Amplify`, `Crossfade Predictor`; в v1 predictor строится от предыдущего **input**, чтобы effect не был скрытым неконтролируемым feedback loop;
- параметры: `Block size`, `Flow scale`, `Predictor hold`, `Residual amount`, `Corruption`, `Keyframe reset`; options: `Predictor/Residual/Both`, перечисленные corruption modes, `Clamp/Wrap/Mirror`;
- `Keyframe reset` периодически подставляет чистый input frame и ограничивает накопление ошибки. Stateful режимы объявить sequential temporal; `Residual Drop` без hold — finite halo;
- не использовать реальный compressed bitstream и не заявлять совместимость с codec artifacts beyond visual inspiration.

Приёмка: `Predictor hold = 0`, `Residual amount = 1`, `Corruption = 0` восстанавливает input в tolerance; reset возвращает чистый кадр; первый кадр identity; proxy/full одинаково трактуют flow, block edges и alpha.

### 3.7 Flow Anatomy

**Идея.** Разложить уже вычисленный optical flow на curl-free (источник/сток), divergence-free (вихрь) и harmonic components. Пользователь управляет time/space warp не общей «скоростью движения», а его физической формой.

Реализация:

- reuse dense Lucas–Kanade flow из Optical Flow Time Warp и вычислять discrete divergence/curl на proxy/full одинаковыми finite differences;
- решить два scalar Poisson equations для potentials на каждом frame pair итеративным deterministic Jacobi/red-black Gauss–Seidel solver с fixed iteration count и явным boundary condition; собрать `sourceSink`, `vortex` и residual `harmonic`;
- mode выбирает component для bounded X/Y/T displacement; `Visualize` выводит signed component как colour map, но не становится отдельным diagnostic UI;
- параметры: `Strength`, `Temporal bend`, `Spatial bend`, `Smoothing`, `Component mix`, `Solver iterations`; options: `Vortex/Source-Sink/Harmonic/Mix/Visualize`, `Clamp/Wrap/Mirror`;
- finite temporal halo в один frame pair; cache flow/component fields как derived analysis keyed by source + parameters, если profiling покажет повторные reads. Не вводить neural flow backend как prerequisite.

Приёмка: synthetic rotation активирует vortex сильнее source/sink; radial expand активирует source/sink; uniform translation даёт почти нулевые curl/divergence; solver совпадает в proxy/full tolerance и не оставляет border seam.

### 3.8 Flow Ridges

**Идея.** Найти в flow границы, по разные стороны которых близкие траектории быстро расходятся (FTLE/Lagrangian coherent-structure ridges), и использовать их как живые линии разрыва времени или сигнала.

Реализация:

- reuse flow pairs; для каждого seed pixel численно advect две близкие trajectories на `Integration window` кадров с bilinear flow sampling;
- оценить deformation gradient конечной разностью, затем scalar FTLE; не требовать physical-unit calibration — это художественная flow metric;
- ridge map: percentile/threshold + optional non-max suppression после небольшого blur. Map управляет `Tear` (offset/hold по обе стороны), `Freeze Edge`, `Palette Edge` или `Gate`;
- параметры: `Integration window`, `Flow scale`, `Ridge threshold`, `Ridge width`, `Tear amount`, `Edge persistence`; options: `Forward/Backward flow`, `Tear/Freeze Edge/Palette Edge/Gate`, `Rows/Columns/Normal direction`;
- это global/sequential analysis по окну: proxy сначала считает cached low-res ridge maps, full render — соответствующие full-res maps с bounded window. Показывать high cost и отменяемый analysis progress;
- v1 ограничить window (например, 2–16 frames), не вводить 3D LCS solver или deep flow.

Приёмка: uniform translation не создаёт ridge; synthetic divergent field создаёт предсказуемую линию; threshold выше max FTLE возвращает identity; maps/cache/seed совпадают в proxy/full по выбранной tolerance.

### 3.9 Voronoi Time Cells

**Идея.** Разделить кадр на устойчивые, но не регулярные ячейки Вороного и дать каждой ячейке собственный маленький сдвиг времени, дрейф или hold. В отличие от block corruption и Scan Slice Field, границы здесь органические, а не прямоугольные или полосовые.

Реализация:

- генерировать `Cell count` детерминированных 2D sites из stored seed: в v1 — jittered grid или Poisson-like rejection с минимальной дистанцией, без тяжёлой 3D tessellation; для каждого output pixel назначать nearest site через spatial bins, не через случайную текстуру;
- sites остаются фиксированными либо получают bounded advection из уже существующего Lucas–Kanade flow. При flow-advection ограничить число substeps и возвращать site в canonical domain по выбранной edge policy;
- на site хранить deterministic attributes: temporal offset, hold length, X/Y drift, colour/opacity weight. Режимы: `Cell Time`, `Cell Hold`, `Neighbour Borrow`, `Flat Colour Map`;
- параметры: `Cell count`, `Cell jitter`, `Temporal range`, `Hold`, `Spatial drift`, `Boundary softness`, `Flow follow`; options: `Fixed/Advected`, перечисленные режимы, `Clamp/Wrap/Mirror`;
- fixed `Flat Colour Map` — local; `Cell Time` декларирует bounded temporal range; `Advected` flow — finite flow-pair halo. В v1 не добавлять 3D Voronoi, segmentation и физическую симуляцию клеток.

Приёмка: `Temporal range = Hold = Spatial drift = 0` даёт identity в режимах выборки; seed воспроизводит sites и attributes; cell boundary не даёт alpha fringe; proxy/full используют одинаковую нормализованную cell topology.

### 3.10 Reaction-Diffusion Injector

**Идея.** Запустить поверх видео малую детерминированную Gray–Scott reaction-diffusion систему. Видеосигнал не «стилизуется нейросетью»: luma, edge или motion только инжектируют реагент, из которого возникают живые пятна, жилы и химические фронты.

Реализация:

- хранить два float-поля `U/V` на отдельной низкой рабочей resolution; на output frame выполнять фиксированное число finite-difference Euler substeps с 4- или 9-neighbour Laplacian и clamp конечного состояния;
- source guide формировать из straight luma, Sobel edge либо density существующего flow; guide добавляет `V`/убирает `U` с bounded strength. Начальное поле и optional sparse seeds получают stored 64-bit seed;
- после upscale использовать state как `Mask`, `Palette`, `Time Displace` или `Source Replace`. `Time Displace` читает только явно ограниченный temporal range, остальные варианты могут не читать соседний frame кроме motion-guide;
- параметры: `Feed`, `Kill`, `Diffusion ratio`, `Injection`, `Substeps`, `Scale`, `Decay`; options: `Luma/Edges/Motion/Seed`, `Mask/Palette/Time Displace/Replace`, `Reset on cut/Continuous`;
- sequential temporal effect: checkpoints для proxy/file executor через установленный interval, чтобы изменение параметра не требовало скрытого full-video tensor; cut reset определять простым luma histogram delta, не моделью scene detection.

Приёмка: fixed seed/parameters повторяют state; `Injection = 0` и neutral composite сохраняют source; U/V не становятся NaN/Inf; reset на synthetic cut воспроизводим; full render совпадает с proxy при одинаковом initial checkpoint в tolerance.

### 3.11 Distance-Field Topography

**Идея.** Превратить выбранный видеосигнал в signed distance field, а затем в рельеф, изобанды, террасы или кольца времени. Это даёт геометрический эффект, не сводящийся к обычному edge detect или displacement map.

Реализация:

- сначала получить binary inside/outside mask из thresholded luma, Sobel edge или motion activity; вычислить расстояния до обеих областей точным separable Euclidean distance transform и сформировать `SDF = dOutside - dInside`;
- квантовать/модулировать SDF в bands, затем применять его как `Contours` colour/mask, `Terraces` spatial displacement, `Ring Time` temporal offset или `Emboss` lighting. Sample mask/guide только в straight-alpha domain;
- параметры: `Threshold`, `Edge width`, `Band spacing`, `Band sharpness`, `Relief`, `Temporal range`, `Smoothing`; options: `Luma/Edges/Motion`, `Contours/Terraces/Ring Time/Emboss`, `Inside/Outside/Both`, `Clamp/Wrap/Mirror`;
- `Contours` и `Emboss` local, `Ring Time` имеет bounded temporal halo. Для thin edge mode вводить ясный fallback на empty/full mask, а не деление на ноль;
- v1 использует 2D per-frame SDF. Не подменять это 3D volume distance transform, ML depth или сторонним geometry engine.

Приёмка: constant black/white input корректно даёт empty/full fallback; spacing выше максимальной distance возвращает один предсказуемый band; identity amount не меняет source; signed bands устойчивы на alpha edges и proxy/full сходятся в установленной tolerance.

### 3.12 Flow Ink / LIC

**Идея.** Использовать optical flow не только для варпа: провести через поле короткие streamlines и собрать по ним яркость, edge или deterministic noise. Получатся «чернила», волокна и следы движения, живущие по траекториям, а не типичный motion blur.

Реализация:

- reuse dense Lucas–Kanade flow; для каждого output pixel интегрировать `N` bounded шагов вперед/назад с bilinear flow sampling и bilinear sampling выбранного signal. Для стабильности clamp magnitude и применить fixed step size;
- signal: source luma/chroma, Sobel edge или seeded zero-mean noise. Собрать samples symmetric weighted kernel и вывести `Source Smear`, `Noise Threads`, `Edge Ink`, `Counterflow` или `Time Thread`;
- параметры: `Line length`, `Steps`, `Flow scale`, `Step size`, `Contrast`, `Edge mix`, `Temporal range`; options: перечисленные режимы, `Forward/Backward/Both`, `Clamp/Wrap/Mirror`;
- spatial modes имеют finite flow-pair halo, `Time Thread` — явно bounded temporal halo. Рабочая resolution/proxy scale должна влиять на число шагов в physical-pixel terms, а не создавать другой характер;
- избегать feedback как скрытой составляющей: каждый frame строить от source и cached flow. Не называть эффект настоящей научной визуализацией flow.

Приёмка: нулевой flow не рисует направленных линий; uniform flow даёт согласованные прямые streamlines; seed фиксирует noise threads; короткая длина/neutral mix возвращают source в tolerance; alpha не расползается по линии без выбранного opaque mode.

### 3.13 Signal Plot Field

**Идея.** Сжать кадр вдоль одной оси в одномерный сигнал, обработать его как волну/график, а затем снова развернуть в пиксельное поле. Это переносит в видео визуальный язык осциллографа и псевдоданных без аудио-реактивности или внешних данных.

Реализация:

- для каждой строки либо колонки строить 1D signal из mean/peak luma, edge density или motion energy по перпендикулярной оси; затем применять box/Gaussian smoothing, finite difference, quantization, threshold и bounded temporal hold;
- rasterize signal в `Plot Lines`, `Bar Field`, `Data Gate`, `Time Address` или `Displace`. Для line mode использовать deterministic antialias width, а не platform-dependent vector drawing;
- параметры: `Axis`, `Signal source`, `Smoothing`, `Gain`, `Quantize`, `Threshold`, `History`, `Line width`, `Temporal range`; options: `Mean/Peak/Edges/Motion`, перечисленные output modes, `Rows/Columns`, `Black/Source/Palette`;
- local output modes читают только текущий frame; `History`/`Time Address` используют ring buffer и декларируют bounded range. Никакого микрофона, MIDI или live CHOP input;
- edge/motion source считают до axis reduction в straight luma и только final composite re-premultiply.

Приёмка: constant frame порождает постоянный предсказуемый signal; синтетическая вертикальная граница даёт ровно ожидаемую реакцию для выбранной оси; `Gain = 0` или Amount zero даёт identity согласно mode; history не зависит от tile size.

### 3.14 Contour Relay

**Идея.** Из luma, edges или motion получить несколько iso-contours, а потом использовать их как автономные линии, colour relay или адреса времени. Это не фильтр Sobel: важны непрерывные уровни, порядок contour bands и возможность переносить их через время.

Реализация:

- сформировать scalar field из straight luma, Sobel magnitude или motion density; взять равномерные/логарифмические iso levels и извлечь линии Marching Squares на рабочей grid. Для raster output можно сразу классифицировать cells/isobands, а для clean line mode хранить bounded polyline segments;
- режимы: `Contour Lines`, `Isobands`, `Contour Delay` (линии выбирают time offset), `Relay Palette` (level адресует palette/source channel), `Erode/Expand`;
- параметры: `Levels`, `Level spacing`, `Threshold`, `Line width`, `Band fill`, `Temporal range`, `Persistence`; options: `Luma/Edges/Motion`, перечисленные режимы, `Linear/Log spacing`, `Clamp/Wrap/Mirror`;
- `Contour Lines`/`Isobands` local; `Contour Delay` имеет bounded temporal range; optional persistence — sequential ring buffer или flow-advected previous contour map с fixed decay, без optical-flow tracker/ML segmentation;
- degenerate saddle cells должны иметь фиксированное правило tie-break, чтобы proxy/full не расходились при одинаковом scalar field.

Приёмка: monotonic gradient создаёт ожидаемое число прямых contours; constant field не выдаёт случайных segments; одинаковые iso levels/seed дают одинаковую topology; alpha и Amount корректны на boundary; `Persistence = 0` не читает прошлый кадр.

### 3.15 Histogram Time Graft

**Идея.** Сохранить геометрию и детали текущего кадра, но заставить его распределение яркости или цвета соответствовать другому времени клипа. Например, лицо в текущем кадре остаётся лицом, но его тени, насыщенность и цветовые массы берутся из кадра на 2 секунды раньше: получается не обычный colour grade, а «цветовая пересадка времени».

Реализация:

- для source frame `t` и bounded reference frame `t + offset` строить 1D cumulative histograms (CDF) выбранных компонент: `Luma`, `Chroma`, `RGB` или `YCbCr`; из пары CDF детерминированно строить monotonic LUT `source value -> reference value`;
- применять LUT к straight colour текущего кадра. В `Chroma` mode сохранить Y/luma и переносить только Cb/Cr; в `Luma` mode сохранить chroma. Это делает результат читаемым и резко отличает его от RGB Time Slip;
- режимы: `Direct Graft` (пара текущий/reference), `Quantile Bands` (переносятся только выбранные тени/середины/света), `History Reservoir` (reference CDF — экспоненциально сглаженная история), `Palette Leak` (LUT квантована в небольшое число ступеней);
- параметры: `Time offset`, `Strength`, `Bins`, `Shadow range`, `Highlight range`, `Temporal smoothing`, `Quantize`; options: `Luma/Chroma/RGB/YCbCr`, перечисленные modes, `Past/Future/Alternating`, `Clamp/Loop`;
- `Direct Graft` имеет bounded temporal halo; `History Reservoir` — sequential temporal и хранит только несколько histogram bins, а не прошлые frames. Reference frame выбирается от исходного video, не от уже обработанного output;
- строить CDF по unpremultiplied pixels с alpha-weight; не включать transparent black как массовый colour sample. После colour transform re-premultiply и оставить source alpha неизменной.

Приёмка: `Strength = 0` даёт bit-exact identity; `Time offset = 0` даёт identity в Direct Graft; LUT монотонна и не даёт NaN/Inf; Chroma mode сохраняет luma в tolerance; одинаковые source/reference дают одинаковый result в proxy/full; прозрачная граница не получает fringe.

### 3.16 Hilbert Time Weave

**Идея.** Это **не рисование кривой Гильберта**. Кривая только один раз задаёт порядок вложенных квадратных ячеек кадра. Целые группы соседних по этому порядку ячеек получают разные source times, поэтому кадр собирается из фрактально-вложенных квадратных областей прошлого/будущего, а не из горизонтальных scanlines или случайных blocks.

Реализация:

- для grid `2^n × 2^n` precompute bijective `Hilbert index` map: каждому output cell соответствует один index обхода. Map масштабируется до resolution кадра nearest/box policy и кэшируется по `Curve order`/shape; в runtime curve никогда не rasterize и не выводится как line;
- разбить индекс на contiguous `Packet length` ranges. Для packet `p` детерминированно выбрать bounded `timeOffset(p, seed)`; все пиксели его cell/sample group читают один из кадров `t + offset`. Это создаёт крупные вложенные time-islands, а не pixel noise;
- режимы: `Time Packets` (source time на packet), `Alternating Past/Future`, `Packet Hold` (несколько output frames держат source time), `Channel Weave` (R/G/B получают согласованные разные offsets). В `Channel Weave` alpha всегда берётся единообразно из base source, не по каналам;
- параметры: `Curve order`, `Packet length`, `Temporal range`, `Hold`, `Cell softness`, `Offset correlation`, `Seed`; options: перечисленные modes, `Past/Future/Both`, `Nearest/Box cells`, `Clamp/Loop`;
- finite bounded temporal halo равен `Temporal range`; все offsets берутся из input, не из result. В v1 не добавлять arbitrary space-filling families и line-render mode: если пользователь видит нарисованную кривую, это bug дизайна.

Приёмка: `Temporal range = 0` и `Hold = 0` дают exact identity; map bijective на каждой supported grid; одинаковый seed повторяет packets; synthetic colour-by-frame fixture явно показывает nested square regions, а не полосы; alpha не получает RGB fringe и proxy/full используют ту же normalized map.

### 3.17 DCT Lattice Leak

**Идея.** Искусственно сломать **внутреннюю частотную текстуру** маленьких блоков, а не переставлять сами блоки. Край может стать косой рябью внутри своей 8×8 ячейки, мелкая текстура — превратиться в цветной кварц/«комариный» шум, а плавный градиент — в лоскутную DCT-сетку. Это codec-like вид без повреждения H.264/HEVC или файла.

Реализация:

- в v1 выполнять deterministic 2D DCT-II/IDCT на independent blocks `8×8` или `16×16` в straight YCbCr (или linear RGB). DC coefficient по умолчанию сохранять, чтобы эффект портил детали/границы, а не просто менял exposure;
- над AC coefficients применять `Quantize`, `Band Limit`, `Band Swap` между blocks, `Zig-Zag Shuffle`, `Sign Flip` и `Coefficient Hold` на ограниченное число input frames. Shuffle выбирает только разрешённые frequency bands и полностью определяется seed/block/frame index;
- режимы: `Quilt` (quantize/band limit), `Ringing` (high-band gain/sign), `Spectral Swap` (AC между соседними blocks), `Mosquito` (edge-weighted high-band perturbation), `Temporal Hold` (сохранить AC прошлого frame при свежем DC);
- параметры: `Block size`, `Quantize`, `Band range`, `Shuffle amount`, `High-band gain`, `Temporal hold`, `Edge protect`; options: перечисленные modes, `YCbCr/RGB`, `Luma/Chroma/Both`, `Fixed/Seeded lattice`;
- local без `Temporal Hold`; temporal mode использует один bounded previous-frame coefficient buffer и никогда не кодирует/декодирует compressed bitstream. Alpha не DCT-трансформировать в v1: сохранить source alpha и re-premultiply final colour.

Приёмка: neutral coefficients (`Quantize = 0`, gain 1, shuffle 0, hold 0) дают reconstruction в float tolerance; flat colour не получает случайной сетки в защищённом mode; single diagonal edge даёт локальный ringing в ожидаемых blocks; seed стабилен; DC preserve сохраняет mean luma в tolerance; нет alpha fringe.

### 3.18 Temporal Morphology Relay

**Идея.** Из luma/edge/motion строится жёсткая binary/soft guide, и по маленькому X/Y/T-объёму выбирается не среднее, а **minimum, maximum или rank**. Поэтому движущийся силуэт может разрастись в плотный шлейф, быть съеден до пересечения своих прошлых положений или получить ползущие дырки. Это не blur и не cellular automaton: каждый output pixel выбирает экстремум/ранг из ограниченного соседства.

Реализация:

- сформировать guide из thresholded straight luma, Sobel edge или frame-difference motion. Применить 2D spatial + bounded temporal structuring element `Cross`, `Box` или `Diamond`; `Dilation = max`, `Erosion = min`, `Rank = kth ordered sample`;
- использовать resulting map как `Hard Gate`, `Palette Ink`, `Time Stencil` (выбирает current/oldest/most-active source time) или `Silhouette Relay` (color/alpha-like layer поверх source). Не выполнять component-wise min/max RGB: morphology работает на scalar guide;
- параметры: `Spatial radius`, `Temporal radius`, `Rank`, `Threshold`, `Softness`, `Persistence`, `Temporal range`; options: `Luma/Edges/Motion`, `Dilation/Erosion/Rank`, перечисленные output modes, `Cross/Box/Diamond`, `Past/Both`;
- radius до небольшого fixed maximum в v1; на CPU считать separable min/max pass для dilation/erosion, а rank ограничить small neighbourhood, чтобы не строить весь 3D volume. `Past` mode — causal sequential/finite halo, symmetric `Both` разрешён только как явно global bounded-window mode;
- guide и mask считаются с alpha-weighted straight luma; финальный composite сохраняет source alpha, кроме отдельного `Silhouette Relay` с явной alpha policy.

Приёмка: единичная moving white square в `Dilation` образует предсказуемый hard trail, в `Erosion` — intersection; `Rank` выбирает документированную kth sample без flicker; radius/temporal radius 0 дают identity в выборочных modes; static frame стабилен; tile size не меняет output и alpha edge не даёт fringe.

### 3.19 Scan Slice Field — эффект по разбору референса

Референс: [GENERATIVE SCAN GLITCH EFFECT TOUCHDESIGNER TUTORIAL](https://www.youtube.com/watch?v=wV8lRkusDq4), PPPANIK. В ролике видны несколько слоёв независимых scan-полос: крупные растянутые срезы, тонкие сдвинутые фрагменты и пустоты. Это не один uniform displacement и не codec error; характер строится на процедурном поле, которое адресует и масштабирует каждый slice отдельно.

**Идея ChronoForge.** `Scan Slice Field` берёт строку/колонку или ограниченную полосу source, назначает ей детерминированные offset, scale, gap и optional time offset из низкочастотного quantized noise field, затем пересобирает кадр из этих независимых срезов.

Реализация:

- создать 1D guide вдоль выбранной scan axis из seeded value-noise/fBm: `g(s, t)`. Перед применением квантовать guide в plateaus/bands, чтобы слайсы были материальными, а не гладким Wave Warp;
- для каждой output полосы вычислять source coordinate: перпендикулярный `s` остаётся в band, продольный `u` получает `u' = centre + (u - centre) * scale(g) + offset(g)`; optional `t' = t + timeOffset(g)`;
- рисовать до трёх independent layers: `Macro` (широкие stretch/smear bands), `Micro` (тонкие резкие slices) и `Gaps` (source/transparent/black holes). Composite только в linear premultiplied space;
- параметры: `Axis`, `Band size`, `Macro density`, `Micro density`, `Offset`, `Stretch`, `Gap amount`, `Guide roughness`, `Time slip`; options: `Horizontal/Vertical/Alternating`, `Fixed/Crawl guide`, `Source/Transparent/Black gaps`, `Clamp/Wrap/Mirror`;
- `Fixed guide` — spatial local effect; `Crawl` — guide зависит от `t`, но не читает соседние кадры; `Time slip` объявляет bounded temporal halo. Не вводить feedback или сторонние TouchDesigner assets;
- default: horizontal, large Macro + sparse Micro, low gap, `Time slip = 0`, чтобы эффект был узнаваемым на still/video и не выглядел как обычный Sync Loss.

Приёмка: `Offset = 0`, `Stretch = 1`, `Gap = 0`, `Time slip = 0` дают identity; seed стабильно повторяет guide; band boundaries не дают alpha fringe; horizontal и vertical modes зеркально согласованы на synthetic grid; output сохраняет shape и поддерживает Amount.

Каждый новый эффект обязан получить:

- proxy и file-backed реализацию;
- Amount/shape policy;
- deterministic seed, если есть randomness;
- alpha и edge tests;
- proxy/full parity;
- visual-regression fixture и performance classification.

## 4. Будущие форматы

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
