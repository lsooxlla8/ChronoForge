# ChronoForge — техническое задание следующей фазы

## Glitch Lab: одноразовые сессии, быстрый эксперимент и новые семейства эффектов

Статус документа: рабочее ТЗ для реализации  
Базовая версия: ChronoForge 0.9.0  
Целевая линия версий: 1.0–1.2  
Платформа: macOS 14+, Apple Silicon  
Режим работы: только офлайн-обработка видеофайлов

---

## 1. Цель фазы

Превратить ChronoForge в небольшой, быстрый и намеренно непредсказуемый инструмент для создания нестандартных цифровых и аналогоподобных глитчей.

Основной сценарий:

1. Пользователь запускает приложение с чистого листа.
2. Импортирует один или несколько видеофайлов либо image sequence.
3. Добавляет эффекты вручную или заменяет текущий стек случайным стеком из 1–3 эффектов.
4. Быстро меняет параметры, Amount и порядок эффектов, сравнивает результат с исходником и при необходимости отменяет изменения.
5. Экспортирует MP4 либо последовательность изображений.
6. После нормального закрытия творческое состояние сессии исчезает.

Фаза считается успешной, если экспериментировать стало заметно быстрее, цепочки из нескольких эффектов перестали неизбежно превращаться в одинаковую кашу, а добавление каждого следующего семейства эффектов требует предсказуемого объёма работы.

## 2. Продуктовые принципы

### 2.1. Обязательные принципы

- ChronoForge — офлайн-инструмент для обработки файлов, не live VJ software.
- Разнообразие действительно разных методов глитча важнее единой концепции вокруг времени.
- Каждый обычный запуск начинается с пустого состояния.
- Пользователь сохраняет результаты, но не обязан управлять проектами.
- Интерфейс не должен требовать изучения node graph, модульной системы или композитинга.
- Случайность должна помогать находить новые результаты, но не подменяться библиотекой пресетов.
- Preview остаётся интерактивным proxy-путём, full render всегда пересчитывается из оригинальных медиа.
- Одинаковый набор явных параметров и скрытых seed обязан давать детерминированный результат внутри каждого render path; различия proxy/full допустимы только из-за разрешения и временной выборки proxy.
- Новые эффекты должны работать в RAM-прокси и out-of-core full render либо явно не попадать в релиз.

### 2.2. Явные нецели

В эту продуктовую линию не входят:

- сохранение и открытие пользовательских проектов;
- пресеты, коллекции, избранное и галерея готовых образов;
- поиск по эффектам до тех пор, пока категоризированное меню остаётся обозримым;
- MIDI, OSC, LFO, аудиореактивность и live input;
- сцены, макросы и performance mode;
- marketplace, плагины и сторонний SDK;
- полноценный монтаж, многослойный compositor и ручная ротоскопия;
- temporal brush;
- timecode и ProRes export;
- полная история действий как отдельный пользовательский экран;
- полная поддержка произвольных FFmpeg-кодеков в macOS-версии.

## 3. Состав следующей фазы

Работа делится на три последовательных этапа. Каждый этап должен завершаться самостоятельно используемой и протестированной версией.

### Этап A — 1.0: Ephemeral Workflow

- удалить пользовательскую модель проектов;
- добавить Undo/Redo;
- добавить постоянный bypass и моментальное сравнение Before/After;
- добавить Amount каждому совместимому эффекту;
- добавить Replace with Random;
- добавить всегда включённое автоматическое обновление preview;
- сгруппировать эффекты по семействам;
- централизовать метаданные эффектов в реестре;
- сохранить render queue как состояние только текущего запуска.

### Этап B — 1.1: Frames, FPS & Alpha

- импорт PNG image sequence;
- экспорт PNG image sequence;
- сохранение альфа-канала в последовательности изображений;
- выбор интерпретации FPS;
- просмотр прозрачности на checkerboard;
- явное поведение MP4 при наличии альфы;
- подготовка расширяемого контракта форматов для TIFF/EXR в будущем.

### Этап C — 1.2: Effect Expansion Wave A

- добавить первую волну одноклиповых signal/data/memory-эффектов;
- добавить первую волну двухклиповых weave/graft/transplant-эффектов;
- для каждого эффекта определить контролируемое случайное распределение;
- проверить сочетания новых эффектов с Amount и случайными стеками;
- зафиксировать кандидатов Wave B без включения их в scope 1.2.

---

## 4. Состояние сессии и удаление проектов

### 4.1. Пользовательское поведение

- Удалить команды New Project, Open Project, Save Project и Save Project As.
- Удалить `.chronoforge` из обычного пользовательского потока и обработчика Open With.
- Удалить название проекта и статус `Edited` из toolbar.
- При нормальном запуске всегда показывать пустой workspace.
- При нормальном закрытии не сохранять медиа, стек, параметры, очередь и позицию playhead.
- Кэш decoded proxy и результатов может сохраняться между запусками, так как он не является творческим состоянием.
- Настройки самого приложения могут сохраняться: тема, proxy quality по умолчанию и фон preview.

### 4.2. Аварийное восстановление

Разрешена только страховка от аварийного завершения:

- во время открытой сессии хранить скрытый recovery snapshot;
- при нормальном завершении приложения удалять snapshot;
- при следующем запуске после crash/force quit показать один диалог: `Recover Interrupted Session` / `Start Fresh`;
- при `Start Fresh` snapshot удаляется немедленно;
- восстановленную сессию нельзя сохранить как проект;
- формат recovery считается внутренним и не обязан быть долговременно совместимым между мажорными версиями.

### 4.3. Изменения кода

- Переименовать `ProjectStore` в `SessionStore`, удалить `projectURL`, `isDirty`, обычный `hasRecovery` и связанные команды.
- Заменить `ProjectPersistence` внутренним `SessionRecoveryStore` либо сузить существующий тип до recovery-only.
- Добавить обработку clean termination через AppDelegate/application lifecycle.
- Удалить UTType пользовательского проекта и регистрацию расширения из Info.plist.
- Обновить README, architecture и self-test, исключив save/open project.

---

## 5. Undo/Redo

### 5.1. Пользовательское поведение

- `⌘Z` отменяет последнее творческое изменение.
- `⇧⌘Z` повторяет отменённое изменение.
- Отдельная панель истории не создаётся.
- История существует только в текущем запуске.
- Максимальная глубина: 100 пользовательских операций.

### 5.2. Что входит в историю

- добавление, удаление, дублирование и перестановка эффекта;
- изменение enabled/bypass;
- изменение параметра, option, Amount, driver B и режима size matching;
- Clear Effect Stack одной операцией;
- Replace with Random Stack одной операцией;
- смена primary A;
- удаление медиа, если decoded proxy ещё доступен в памяти;
- reseed стохастического эффекта;
- будущие простые маски Apply To.

### 5.3. Что не входит в историю

- playback, playhead и выбранный кадр;
- выбор строки в sidebar;
- запуск, завершение и отмена preview/export;
- прогресс рендера;
- добавление и удаление элементов render queue;
- открытие save panels;
- изменение внешнего вида приложения.

### 5.4. Группировка операций

- Полный drag одного slider регистрируется как одна операция с исходным и конечным значением.
- Набор числа и подтверждение Return регистрируется как одна операция.
- Изменение Picker или Toggle регистрируется сразу.
- Серия внутренних нормализаций параметров, например ремонт осей Space-Time Map, входит в ту же операцию.
- Undo/Redo не должно автоматически запускать множество preview; оно вызывает единственную инвалидацию и один debounce Auto Update.

### 5.5. Рекомендуемая реализация

Создать отдельный session history слой с небольшими командами либо snapshot текущего лёгкого creative state. Не включать в snapshot массивы пикселей, decoder objects, output tensors и render queue.

Лёгкий snapshot должен содержать:

- primary media ID;
- порядок и полное состояние `EffectNode`;
- output settings, влияющие на изображение;
- selected node ID только при необходимости корректного UI после structural undo.

Использовать `UndoManager` для интеграции с меню macOS, но не позволять view-компонентам напрямую определять семантику операций.

---

## 6. Bypass и сравнение

### 6.1. Постоянный bypass

- Существующий `Enabled` остаётся постоянным свойством узла.
- Bypass участвует в Undo/Redo.
- Disabled-узел не участвует в cache key как активная операция, но его состояние остаётся в стеке.

### 6.2. Source Before/After

- В viewer добавить кнопку compare.
- Пока кнопка удерживается, показывать исходный A proxy на соответствующей позиции времени.
- После отпускания моментально возвращать последний готовый result preview.
- Сравнение ничего не меняет в creative state и не попадает в Undo.
- Добавить удерживаемый keyboard shortcut, окончательную клавишу выбрать при реализации после проверки конфликтов macOS.
- При stale preview сравнивается source с последним готовым результатом; stale badge остаётся видимым.
- Если source и result имеют разное число кадров, позиция сопоставляется по нормализованному времени `0...1`, а не по одинаковому номеру frame.

### 6.3. Selected Node Before/After

- После каждого proxy render сохранять только две дополнительные лёгкие ссылки для выбранного узла: непосредственный input узла и output узла.
- Compare в Inspector выбранного эффекта показывает его локальный input/output, а не пересчитанный финальный стек без этого эффекта.
- UI обязан назвать режим `Selected Effect Input`, чтобы не создавать ложное ожидание пересчёта downstream.
- Если выбранный эффект изменился после последнего render, кнопка недоступна до следующего preview.
- Если input и output выбранного узла имеют разную длину, compare также использует нормализованную позицию времени.

---

## 7. Автоматическое обновление preview

### 7.1. Поведение

- Auto Update всегда включён и не имеет пользовательского Toggle или ручной кнопки Update Preview.
- Slider не запускает render во время drag.
- После отпускания slider, подтверждения числа, изменения Picker/Toggle, Undo/Redo либо structural edit запускать debounce.
- Обычная задержка debounce: 450 мс.
- Для global-cost эффектов допустима задержка 800 мс без отдельной настройки пользователя.
- Новое изменение отменяет только ещё не начатый debounce либо активный proxy render; export не затрагивается.

### 7.2. Состояния UI

- `Waiting to update` во время debounce показывать не нужно.
- Во время render сохранить нынешний progress/cancel UX.
- Stale badge не нужен: preview сам обновляется после debounce.
- Ошибка preview показывается в обычном error UX; автоматическое обновление остаётся активным.

### 7.3. Домены отмены

- Debounced preview, running preview, media import, full export и render queue должны оставаться отдельными cancellation domains.
- Изменение параметра не имеет права отменить full export, который уже захватил snapshot стека.

---

## 8. Amount / Wet-Dry

### 8.1. Модель данных

Добавить в каждый `EffectNode`:

```swift
var amount: Float = 1.0
var amountBlendMode: AmountBlendMode = .normal
var randomSeed: UInt64
```

`randomSeed` обязателен для эффектов со стохастической геометрией и может оставаться неиспользованным остальными эффектами.

### 8.2. UI

- В Inspector каждого поддерживаемого эффекта после Enabled показать `Amount`.
- Диапазон: 0–100%, значение по умолчанию 100%.
- Поле точного значения и slider используют существующий стиль.
- Контекстное меню slider: Reset to 100%.
- Для случайных стеков Amount выбирается по effect-specific distribution, обычно в диапазоне 25–100%.
- `Amount Blend` предлагает Normal / Add / Screen / Multiply / Difference / Displace / XOR Glitch.

### 8.3. Семантика

Для shape-preserving эффекта:

```text
output = input × (1 - amount) + effected × amount
```

Формула выше описывает Normal. Остальные режимы сначала вычисляют выбранную compositing-операцию между input и effected, затем Amount интерполирует от input к этому результату. Displace использует effected как трёхмерное поле смещения input; XOR Glitch квантует и XOR-комбинирует значения для намеренно жёстких цифровых разломов.

- Смешивание выполняется в linear premultiplied RGBA.
- Amount 0 обязан быть точной identity-операцией.
- Amount 1 обязан совпадать с прежним эффектом бит-в-бит либо в пределах существующего float tolerance.
- Cache key включает Amount.
- Bypass сильнее Amount: disabled-узел не вычисляется независимо от значения Amount.

### 8.4. Эффекты, меняющие форму

- Если output shape отличается от input shape, частичное Amount недоступно.
- Inspector показывает Amount = 100% disabled и короткое пояснение.
- При выборе Fit Source Size Amount снова становится доступным.
- Random Stack обязан выбирать только действительно shape-preserving варианты для всех узлов с Amount меньше 100%; сохранение spatial canvas недостаточно, если меняется `T`.

### 8.5. Реализация full render

- Proxy может смешивать исходный и effected tensor после вычисления узла.
- Full renderer должен смешивать mapped input и mapped output in-place после вычисления эффекта либо fused внутри write pass.
- Не создавать третий полноразмерный tensor только ради Amount.
- Input scratch нельзя удалять до завершения blend pass.
- Disk preflight учитывает дополнительный проход по данным, но не дополнительный объём файла.

---

## 9. Replace with Random

### 9.1. UX

- В нижней части sidebar рядом с Add Effect добавить кнопку с dice-иконкой `Random`.
- Кнопка полностью заменяет текущий стек без confirmation dialog.
- Операция всегда отменяется одним `⌘Z`.
- Доступна только после импорта primary A.
- После генерации выбирается первый либо самый визуально значимый узел; точное правило должно быть стабильным.
- Preview запускается через обычный debounce.

### 9.2. Базовые правила генерации

- Длина: 1, 2 или 3 узла с весами 35%, 45%, 20%.
- Повторы одного EffectKind разрешены с низкой вероятностью.
- Не более одного global-cost эффекта.
- Двухвходовые эффекты исключаются без отдельного B.
- Seamless Loop, если выбран, всегда последний.
- Невалидные combinations осей и size modes не генерируются.
- Native Tensor не используется, если следующий эффект не умеет принимать изменившуюся форму.
- Стек должен быть полностью валидирован до замены текущего; частичная генерация запрещена.

### 9.3. Распределения параметров

Нельзя использовать один uniform random для всех параметров. Реестр эффектов должен поддерживать:

- uniform range;
- triangular distribution с preferred center;
- log distribution для масштабов и вероятностей;
- signed magnitude с dead zone вокруг нуля;
- weighted discrete options;
- fixed safe value;
- зависимые правила между параметрами;
- custom generator для сложного эффекта.

Каждый EffectDefinition обязан иметь проверенный `RandomizationProfile`. Отсутствие профиля означает, что эффект не участвует в Random Stack.

### 9.4. Seed и повторяемость

- Пользователь не видит и не вводит seed Random Stack.
- После генерации каждый стохастический эффект хранит свой `randomSeed`.
- Proxy и full render обязаны использовать один seed.
- Для стохастических эффектов в Inspector разрешена одна кнопка `Reseed`; это отдельная Undo-операция.
- Тесты используют injectable deterministic RNG.

---

## 10. Категории и реестр эффектов

### 10.1. Категории Add Effect

Меню Add Effect получает фиксированные секции:

1. **Time & Motion**
2. **Space & Geometry**
3. **Signal & Analog**
4. **Memory & Compression**
5. **Data & Channels**
6. **Multi-Source · A + B**
7. **Output & Utility**

Предлагаемое распределение существующих эффектов:

| Эффект | Категория |
| --- | --- |
| Self Time Displacement | Time & Motion |
| Pixel Sort (Time) | Time & Motion |
| Optical Flow Time Warp | Time & Motion |
| Space-Time Transform | Space & Geometry |
| Polar Time Warp | Space & Geometry |
| 3D FFT Transform | Data & Channels |
| Time Feedback | Memory & Compression |
| Axis Datamosh | Memory & Compression |
| Space-Time Map | Multi-Source · A + B |
| Space-Time Displacement | Multi-Source · A + B |
| Seamless Loop | Output & Utility |

Пустые категории до появления новых эффектов можно не показывать.

### 10.2. EffectDefinition

Убрать продуктовые метаданные из разрозненных switch-блоков в единый Swift registry. Минимальный контракт:

```swift
struct EffectDefinition {
    let kind: EffectKind
    let title: String
    let symbol: String
    let tint: EffectTint
    let category: EffectCategory
    let inputArity: EffectInputArity
    let costClass: EffectCostClass
    let shapeBehavior: EffectShapeBehavior
    let usesRandomSeed: Bool
    let defaultNode: () -> EffectNode
    let randomization: RandomizationProfile?
}
```

Допускается сохранить custom SwiftUI Inspector для сложных эффектов. Реестр обязан централизованно обслуживать:

- Add Effect;
- заголовки, символы и категории;
- default node;
- Random Stack;
- поддержку B;
- поддержку Amount;
- предупреждение о стоимости;
- выбор безопасного shape mode.

Core registry/enum остаётся статическим и не превращается в plugin ABI.

### 10.3. Версионированный bridge descriptor

Текущий `CFEffectDescriptor` ограничен четырьмя float values и четырьмя integer options. Wave A уже требует больше параметров, поэтому Amount и seed нельзя прятать в существующие свободные слоты.

До добавления новых эффектов ввести внутренний descriptor следующей версии:

```c
typedef struct CFEffectDescriptorV2 {
    int32_t kind;
    uint32_t descriptor_version;
    float amount;
    uint64_t random_seed;
    uint32_t value_count;
    uint32_t option_count;
    float values[8];
    int32_t options[8];
} CFEffectDescriptorV2;
```

Требования:

- descriptor version проверяется на C boundary;
- неиспользуемые slots всегда обнуляются;
- `value_count` и `option_count` валидируются против EffectDefinition;
- Swift helper обязан безопасно дополнять массивы до восьми slots и не обращаться по отсутствующему индексу;
- C++ `EffectSpec` расширяется тем же количеством slots и получает Amount/seed как отдельные поля;
- proxy, full render и cross-tensor functions используют один descriptor;
- старый descriptor можно удалить после одновременной миграции всех внутренних callers, так как публичный plugin ABI не поддерживается;
- cache serialization использует логические counts и значения, а не padding bytes структуры.

Восемь slots считаются внутренним лимитом этой продуктовой линии. Если будущему эффекту требуется больше, нужно вводить специализированный parameter block либо новый descriptor version, а не перегружать slots неочевидной упаковкой.

### 10.4. Контракт каждого нового эффекта

Новый эффект нельзя считать завершённым без:

- CPU reference для RAM tensor;
- out-of-core file-backed реализации;
- C bridge mapping;
- EffectKind/EffectDefinition;
- Inspector с понятными диапазонами;
- proxy/full cache serialization;
- RandomizationProfile либо явного исключения;
- Amount-поведения;
- alpha-тестов;
- unit tests и integration coverage;
- короткого описания в README.

---

## 11. Image sequence, FPS и alpha

### 11.1. Импорт PNG sequence

- Добавить отдельную команду `Import Image Sequence…`.
- Пользователь выбирает папку либо первый PNG; предпочтительный UX определить прототипом, но результатом всегда является одна sequence.
- Файлы сортируются natural numeric order.
- Поддерживаются RGB и RGBA PNG одинакового размера.
- Перед импортом показать маленький sheet: detected frame count, dimensions и FPS.
- FPS по умолчанию: 24; варианты 12, 15, 23.976, 24, 25, 29.97, 30, 50, 59.94, 60 и Custom.
- Пропуски в номерах показываются предупреждением, но существующие файлы импортируются подряд без вставки пустых кадров.
- Разный размер кадров является ошибкой импорта с указанием первого несовместимого файла.
- Orientation берётся из фактических пикселей; EXIF rotation нормализуется на decode.
- Color profile преобразуется в существующее linear working space.
- Alpha преобразуется в premultiplied representation.

### 11.2. Proxy и full decode sequence

- Proxy decoder выбирает кадры по той же temporal sampling policy, что и видео proxy.
- Full decoder пишет sequence напрямую в mapped tensor и не хранит все изображения в RAM.
- Fingerprint включает список имён, размеры файлов, modification dates, chosen FPS и sequence settings.
- Изменение любого кадра инвалидирует cache sequence.

### 11.3. Экспорт PNG sequence

- В Export добавить выбор `MP4 Video` / `PNG Sequence`.
- Для sequence пользователь выбирает новую или пустую папку назначения.
- Имя по умолчанию: `ChronoForge_000001.png` с padding 6.
- Существующие совпадающие имена нельзя молча перезаписывать.
- MVP поддерживает PNG 8-bit RGBA; PNG 16-bit добавляется только после подтверждённого корректного round-trip.
- Экспорт каждого кадра выполняется потоково из mapped tensor.
- Частично записанная sequence при cancel остаётся в папке с явным сообщением; автоматически удалять пользовательские файлы нельзя.

### 11.4. FPS export

- Добавить output setting `FPS`: Result либо перечисленные стандартные/custom значения.
- В 1.1 FPS только переинтерпретирует готовые кадры: frame count не меняется, duration изменяется.
- UI сразу показывает итоговую duration.
- При FPS, отличном от source/effect result, Audio: Original отключается с пояснением о рассинхронизации.
- Frame duplication, dropping, optical interpolation и time-stretch audio не входят в 1.1.
- Output FPS входит в cache/export signature, но не меняет cache вычисленного пиксельного tensor, если меняется только metadata.

### 11.5. Alpha

- Все новые эффекты обязаны сохранять premultiplied alpha осмысленно.
- Viewer всегда использует чёрный фон.
- PNG sequence по умолчанию сохраняет alpha.
- H.264 MP4 не заявляет поддержку прозрачности и композитит результат на чёрный фон.
- Если effect Fill = Transparent, preview и PNG обязаны показывать/сохранять прозрачную область.
- Alpha не используется как случайный цветовой канал, кроме эффектов, где пользователь явно выбирает Alpha как source.

### 11.6. Будущие форматы

- TIFF sequence и EXR sequence оставить на следующую фазу.
- Форматный слой проектировать через общий `MediaSource`/`FrameSequenceSource`, не через проверки расширения в `SessionStore`.
- FFmpeg не добавлять только ради image sequence.

---

## 12. Новые эффекты — Wave A

Все названия рабочие. Перед UI freeze допускается одно переименование ради ясности, но не объединение разных алгоритмов в перегруженный mega-effect.

### 12.1. RGB Time Slip

Категория: Data & Channels  
Входы: A  
Shape: сохраняется

Каждый RGB-канал читает независимый момент времени. Альфа по умолчанию читается из текущего кадра.

Параметры:

- Red Offset: −240…240 frames;
- Green Offset: −240…240 frames;
- Blue Offset: −240…240 frames;
- Spatial Split: −200…200 px;
- Split Axis: Horizontal / Vertical / Radial;
- Edge: Clamp / Wrap / Mirror;
- Amount.

Randomization должна часто оставлять один канал почти неподвижным, а два других разводить в разные стороны.

### 12.2. Sync Loss

Категория: Signal & Analog  
Входы: A  
Shape: сохраняется

Группы строк или колонок получают ортогональный сдвиг, дрейфующий во времени. Эффект должен напоминать потерю синхронизации, а не обычный displacement noise.

Параметры:

- Shift: 0…100% width;
- Direction: Horizontal / Vertical;
- Band Size: 0…1, где 0 означает один pixel, а 1 — одну полосу на весь frame;
- Drift Speed: −10…10 bands/frame;
- Tear Density: 0…1;
- Driver: Deterministic Noise / Luma / Edges;
- Edge: Wrap / Clamp / Mirror;
- Amount;
- Reseed для Noise.

### 12.3. Chroma Carrier Drift

Категория: Signal & Analog  
Входы: A  
Shape: сохраняется

RGB преобразуется в luminance/chroma. Luma остаётся относительно стабильной, Cb/Cr смещаются, задерживаются и растекаются независимо.

Параметры:

- Chroma X Offset: −500…500 px;
- Chroma Y Offset: −500…500 px;
- Chroma Time Offset: −120…120 frames;
- Bleed: 0…100 px;
- Mode: Together / Split Cb-Cr / Alternating;
- Edge;
- Amount.

### 12.4. Stride Error

Категория: Memory & Compression  
Входы: A  
Shape: сохраняется

Кадр читается из линейного буфера с намеренно неверной длиной строки. Индексы всегда безопасно оборачиваются внутри текущего frame buffer.

Параметры:

- Stride Delta: −50%…50% width;
- Base Offset: −1…1 frame lengths;
- Temporal Drift: −1…1 frame lengths/frame;
- Channel Mode: RGB Together / Separate Channels / Alpha Included;
- Address Edge: Wrap / Mirror;
- Amount.

Эффект обязан быть deterministic и не обращаться за пределы mapped frame.

### 12.5. Block Address Corruption

Категория: Memory & Compression  
Входы: A  
Shape: сохраняется

Блоки кадра читаются из других пространственных адресов и, опционально, из соседних моментов времени.

Параметры:

- Block Size: 0…1, где 0 означает один pixel, а 1 покрывает весь frame;
- Corruption: 0…1;
- Time Reach: 0…240 frames;
- Hold: 1…240 frames;
- Mapping: Swap / Repeat / Offset / Cascade;
- Edge;
- Amount;
- Reseed.

### 12.6. Bitplane Forge

Категория: Data & Channels  
Входы: A  
Shape: сохраняется

Linear float временно квантуется в целочисленное представление, после чего bitplanes переставляются, вращаются, инвертируются либо XOR-смешиваются.

Параметры:

- Working Bits: 2…16;
- Plane Range/Mask;
- Shift: −15…15;
- Operation: Shuffle / Rotate / Invert / XOR;
- Channel: Luma / RGB Together / R / G / B / Alpha;
- Amount;
- Reseed только для Shuffle/XOR.

После операции значение возвращается в linear float. При Amount 0 результат обязан быть точной identity, независимо от Working Bits.

### 12.7. Signal Weave

Категория: Multi-Source · A + B  
Входы: A + B  
Shape: A по умолчанию

Строки, fields либо небольшие полосы поочерёдно берутся из A и B, образуя движущуюся ткань двух сигналов.

Параметры:

- Pattern: Lines / Interlaced Fields / Bands / Checker;
- Band Size: 0…1, где 0 означает один pixel, а 1 охватывает frame;
- Phase Drift: −20…20 units/frame;
- Irregularity: 0…1;
- B Time Offset: −240…240 frames;
- Size Matching: Clamp / Stretch / Crop;
- Amount;
- Reseed для irregular pattern.

### 12.8. Block Graft

Категория: Multi-Source · A + B  
Входы: A + B  
Shape: A

Пространственные блоки B внедряются в A по выбранному trigger и могут удерживаться несколько кадров.

Параметры:

- Block Size: 0…1, где 0 означает один pixel, а 1 покрывает весь frame;
- Density/Threshold: 0…1;
- Hold: 1…240 frames;
- B Time Offset: −240…240 frames;
- Trigger: Random / A Luma / B Luma / Difference / A Edges;
- Size Matching;
- Amount;
- Reseed для Random.

### 12.9. Channel Transplant

Категория: Multi-Source · A + B  
Входы: A + B  
Shape: A

Выбранные цветовые компоненты A заменяются компонентами B с независимым временным смещением.

Параметры:

- Source mapping для R/G/B либо Y/Cb/Cr;
- B Time Offset: −240…240 frames;
- B Spatial Offset X/Y;
- Colour Model: RGB / YCbCr;
- Size Matching;
- Amount.

UI mapping должен быть компактным: три строки назначения с Picker A/B, без node routing.

---

## 13. Эффекты — Wave B, не входит в 1.2

Кандидаты следующей волны:

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

Причина отсрочки: этим эффектам нужны дополнительные stateful, motion-vector, palette либо codec-like примитивы. Их не следует имитировать упрощёнными версиями только ради количества.

---

## 14. Простые маски и keyframes — отложенный контракт

### 14.1. Apply To

Не входит в обязательный scope 1.0–1.2, но модель Amount не должна препятствовать будущему mask weight:

```text
effectiveAmount(t, y, x) = amount × mask(t, y, x)
```

Разрешённый будущий UI: один Picker `Apply To` со значениями Entire Image, Shadows, Highlights, Edges, Moving Areas, Driver B. Рисуемые маски и mask timeline запрещены текущей продуктовой рамкой.

### 14.2. Простая анимация параметра

Полноценный curve editor не планируется. Если keyframes будут реализованы позднее, первая версия ограничивается:

- static;
- Start → End;
- Linear / Ease / Ping-Pong;
- нормализованным временем текущего node input.

До реализации требуется отдельное решение для эффектов, меняющих ось T.

### 14.3. Встроенная Help — отложенный контракт

Не входит в текущий обязательный пакет. Следующая продуктовая фаза должна добавить доступную из меню Help документацию для пользователей без опыта в tensor/video processing:

- краткое объяснение session-based workflow, A/B media, Preview, Image AA, Time AA, FPS, Before/After, queue и export;
- отдельную страницу каждого эффекта с визуальным принципом действия;
- описание каждой кнопки, option, slider, единиц измерения, диапазона и безопасной стартовой точки;
- объяснение Amount и всех Amount Blend modes;
- контекстные ссылки `?` из Inspector и toolbar в соответствующий раздел;
- searchable glossary терминов alpha, premultiplied, proxy, luma, chroma, FFT, stride, block address и datamosh;
- документацию, сгенерированную или проверяемую против EffectDefinition, чтобы UI и Help не расходились.

---

## 15. Кэш, детерминизм и совместимость рендера

- Cache format version увеличить при добавлении Amount, seed, новых effects либо alpha policy.
- Cache key каждого узла включает kind, parameters, options, Amount, seed, source fingerprints и engine version.
- Output FPS не инвалидирует пиксельный tensor, если меняется только playback metadata.
- Random Stack сам по себе не входит в cache key; в него входят получившиеся явные узлы.
- Все noise/random-address эффекты используют coordinate hash/seed, а не глобальный mutable RNG внутри worker threads.
- Результат не зависит от числа worker threads и порядка tiles.
- Proxy/full допускают различие из-за разрешения proxy, но не из-за разных seed или разной формулы эффекта.
- Alpha всегда остаётся premultiplied во внутренних tensors.

---

## 16. UI-компоновка

Следующая фаза не должна превращать toolbar в панель самолёта.

Рекомендуемое размещение:

- Sidebar bottom: Add Effect и icon-only Random/Clear в одной строке.
- Main toolbar: тема слева, Export и Add to Queue справа; строка с названием приложения удалена.
- Viewer controls: Before/After и output settings; фон всегда чёрный.
- Inspector: фиксированная ширина, вертикальный scroll, Enabled, Amount, effect-specific controls, Driver B при необходимости, Reseed только для stochastic effects.
- Output settings: Preview Quality, Image AA, Time AA, FPS, Audio.
- Add Effect menu: только категории и эффекты, без поиска, presets и favorites.

Если ширины toolbar не хватает, output settings адаптивно переносятся на следующую строку без обрезки подписей; выбор эффекта не меняет геометрию toolbar или Inspector.

---

## 17. Тестирование и критерии приёмки

### 17.1. Автоматические тесты

Для каждого существующего и нового эффекта:

- известный маленький tensor даёт ожидаемые значения;
- Amount 0 — identity;
- Amount 1 — reference result;
- одинаковый seed даёт одинаковый result;
- другой seed меняет stochastic result;
- alpha остаётся корректно premultiplied;
- proxy и file-backed path совпадают в допустимом tolerance;
- edge modes не выходят за границы на маленьких tensors;
- cancellation удаляет только незавершённые scratch files.

Для Random Stack:

- 10 000 seeded generations не создают invalid graph;
- без B никогда не появляется two-input effect;
- не появляется более одного global effect;
- Seamless Loop всегда последний;
- все Amount совместимы с shapes;
- одинаковый injected seed создаёт одинаковый стек.

Для Undo/Redo:

- каждая заявленная операция обратима;
- slider drag создаёт одну undo entry;
- Random Stack и Clear Stack создают по одной entry;
- Undo во время Auto Update не запускает render storm;
- export snapshot не меняется после последующего Undo.

Для image sequences:

- natural numeric sorting;
- RGB/RGBA round-trip;
- alpha edge без fringe;
- mismatch dimensions сообщает конкретный файл;
- missing frame numbers дают warning;
- cancel export не удаляет ранее записанные frames;
- custom FPS корректно меняет duration metadata.

### 17.2. Ручная приёмка

- После нормального перезапуска workspace пуст.
- После искусственного crash предлагается recovery.
- Пользователь может создать random stack, оценить его и вернуть прежний стек одним `⌘Z`.
- Before/After работает без render и без изменения состояния.
- Amount визуально возвращает часть оригинальной структуры в цепочке из трёх эффектов.
- Auto Update не начинает render на каждом промежуточном значении slider.
- Export очереди использует snapshot, даже если текущий стек изменился.
- PNG с прозрачными областями показывает checkerboard и экспортируется с alpha.
- MP4 с тем же tensor композитится на чёрный без цветных fringe.

### 17.3. Команды проверки перед каждым релизом

```bash
cmake -S . -B build -DCHRONOFORGE_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
swift build
swift run ChronoForgeIntegration
swift run ChronoForgeMac --self-test
```

### 17.4. Визуальный regression corpus

Числовых unit tests недостаточно для оценки глитч-эффектов. Создать небольшой набор собственных или процедурно сгенерированных fixtures без чужих авторских материалов:

- RGB/alpha gradient с полупрозрачными краями;
- движущиеся геометрические фигуры на статичном фоне;
- короткий портретоподобный силуэт с различимым лицом;
- шум, мелкая текстура и резкие одно-pixel линии;
- два клипа A/B с разным движением, цветом и длительностью;
- sequence с пропущенным номером и один несовместимый frame для error paths.

Для каждого нового эффекта сохранять внутреннюю contact sheet стандартных параметров и несколько seeded random результатов. Contact sheets используются для ручной проверки перед релизом и не показываются пользователю как presets.

Не копировать работы референсных художников в тесты, bundle или документацию без отдельного разрешения. Их работы задают направление визуального исследования, но не являются test assets.

### 17.5. Бюджеты интерактивности

На базовом Apple Silicon Mac и 10-секундном Standard proxy целевые ориентиры:

- изменение creative state и регистрация Undo: до 50 мс без учёта render;
- Random Stack generation: до 20 мс;
- переключение готового Before/After: в пределах одного UI frame;
- local/channel effect: до 1 секунды;
- temporal/memory effect: до 4 секунд;
- global FFT effect может превышать 4 секунды, но обязан оставаться cancellable;
- Auto Update не блокирует main thread и не накапливает очередь устаревших renders.

Это не публичные marketing promises. Если эффект не укладывается в ориентир, решение принимается по профилированию: оптимизация, более редкий Auto Update или честная cost classification — но не ухудшение алгоритма без визуального сравнения.

---

## 18. Порядок реализации

### 18.1. Foundation

1. Ввести EffectDefinition registry и effect metadata.
2. Разделить creative state, transient render state и app preferences.
3. Ввести `CFEffectDescriptorV2` и расширенный C++ `EffectSpec`.
4. Добавить Amount и seed в EffectNode/cache/bridge paths.
5. Добавить blend path в proxy и full renderer.
6. Покрыть Amount и determinism тестами.

### 18.2. Interaction

7. Реализовать Undo/Redo с coalescing sliders.
8. Добавить Source и Selected Effect compare.
9. Добавить Auto Update с отдельным cancellation domain.
10. Добавить RandomizationProfile и Random Stack.
11. Перегруппировать Add Effect.

### 18.3. Ephemeral lifecycle

12. Переименовать ProjectStore в SessionStore и удалить project commands/UI/persistence.
13. Переделать recovery в crash-only.
14. Обновить README, architecture, tests и package metadata.

### 18.4. Formats

15. Ввести MediaSource abstraction для movie/sequence.
16. Добавить PNG sequence proxy/full decode.
17. Добавить PNG sequence export.
18. Добавить FPS reinterpretation и audio restriction.
19. Добавить checkerboard и MP4 alpha flatten.

### 18.5. Effect Wave A

20. RGB Time Slip.
21. Stride Error.
22. Sync Loss.
23. Chroma Carrier Drift.
24. Bitplane Forge.
25. Block Address Corruption.
26. Signal Weave.
27. Block Graft.
28. Channel Transplant.

Порядок внутри Wave A выбран так, чтобы сначала проверить channel, address, signal и two-source primitives на более простых эффектах.

---

## 19. Риски и защитные решения

### Риск: Amount удваивает стоимость каждого узла

Решение: fused blend либо один дополнительный sequential pass без третьего tensor. Amount 100% пропускает blend pass, Amount 0% пропускает effect целиком.

### Риск: Auto Update делает тяжёлые эффекты раздражающими

Решение: запуск только после commit, debounce, агрессивная отмена proxy и сохранение ручного Toggle.

### Риск: Random Stack постоянно выдаёт кашу

Решение: effect-specific distributions, Amount, cost/compatibility rules, не более трёх узлов и статистический seeded test.

### Риск: рост числа эффектов раздувает Models.swift и Inspector

Решение: EffectDefinition registry для общей метаинформации, небольшие отдельные Inspector sections и переиспользуемые controls. Не строить универсальную schema-driven UI ценой ухудшения ясности сложных эффектов.

### Риск: альфа выглядит правильно в PNG, но даёт fringe после эффектов

Решение: только premultiplied linear processing, отдельные тесты полупрозрачных цветных краёв и явный unpremultiply только при записи формата, который этого требует.

### Риск: image sequence занимает слишком много SSD

Решение: preflight по ожидаемому mapped tensor и приблизительному output size, потоковая запись и сохранение существующего filesystem reserve.

### Риск: удаление проектов воспринимается как потеря функции

Решение: ясно описать философию одноразовой сессии в README, сохранить crash recovery и сделать экспорт результата заметным основным действием.

---

## 20. Definition of Done всей фазы

Фаза Glitch Lab завершена, когда:

- приложение не предлагает сохранять или открывать проекты;
- clean launch всегда пустой, crash recovery работает отдельно;
- Undo/Redo покрывает весь заявленный creative state;
- compare не мутирует стек;
- Auto Update работает без render storm;
- каждый shape-compatible эффект имеет корректный Amount;
- Random Stack создаёт валидные, контролируемо разнообразные цепочки;
- Add Effect организован по согласованным семействам;
- PNG sequence импортируется и экспортируется потоково;
- FPS можно переинтерпретировать с явным изменением duration;
- alpha виден в viewer и сохраняется в PNG;
- Wave A effects работают в proxy и full render;
- render queue, cache trim, cancellation и SSD guardrails не регрессировали;
- core tests, Swift build, integration и full pipeline self-test проходят;
- README и architecture описывают фактическое поведение версии.

## 21. Принятые рабочие допущения

Эти решения считаются утверждёнными для начала реализации, если не будут отдельно изменены:

- обычные проекты удаляются полностью;
- crash recovery сохраняется только как страховка;
- Auto Update всегда включён и не настраивается;
- Random Stack заменяет стек без confirmation и отменяется одним Undo;
- случайный стек содержит 1–3 эффекта;
- Amount реализуется раньше простых масок;
- первая image sequence реализация — PNG;
- смена FPS в первой версии переинтерпретирует кадры, не конвертирует их;
- MP4 всегда непрозрачный и композитится на чёрный;
- ProRes import остаётся возможностью AVFoundation, ProRes export не добавляется;
- keyframes, Apply To masks, Temporal Brush, встроенная Help и Wave B отложены.
