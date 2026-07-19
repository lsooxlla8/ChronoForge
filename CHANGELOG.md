# Changelog

## 1.1.0 — 2026-07-19

- Added Export Current Frame as a full-resolution 8-bit RGBA PNG using the exact full-render graph and cache rather than the Viewer proxy.
- Map the selected Viewer position to the rendered timeline after shape-changing effects, preserving first, middle and last positions without artificial upscaling.
- Added overwrite confirmation through the macOS save panel, six-digit suggested frame names, cancellable progress and byte-for-byte parity coverage against PNG Sequence export.
- Added searchable built-in Help covering the disposable session workflow, Preview, A/B, comparison, AA, FPS, queue/export, Amount blend modes and a glossary.
- Added contextual Inspector Help plus a visual principle, safe starting point and control reference for every production effect; general Help stays in the standard macOS menu.
- Added self-test validation that the Help catalog and centralized EffectDefinition registry remain in sync.
- Added an exhaustive Current Frame release gate covering 2,149 configurations: every effect-specific discrete mode and valid mode combination, hidden 3D Rotation and Output Prefilter path, plus every Amount Blend combined with every compatible effect configuration. It runs across movie and alpha-bearing PNG sources, first/middle/last positions, two-input graphs and cold/warm cache paths for 12,894 byte-for-byte PNG comparisons.
- Fixed premultiplied-alpha output in Cubic Space-Time Map, non-displacement Time Feedback blends and the shared Amount compositor after the exhaustive mode matrix exposed invalid RGBA combinations.
- Canonicalized cache signature JSON before hashing so unchanged sources and effect graphs always reuse the same full-render cache.
- Bumped app, core and render-cache compatibility to 1.1.0.

## 1.0.1 — 2026-07-19

- Added Affinity Migration as a production single-source effect with normalized cell sizing, neighbour-majority migration, motion response and palette-class controls.
- Kept temporal state limited to cell-coordinate offsets: final colour and alpha are always sampled from the current input frame, preventing feedback blur and ghosting.
- Made Reseed deterministically select among compatible neighbouring source cells with matching RAM and SSD-backed rendering.
- Added Affinity Migration to the categorized effect menu, Inspector, Random Stack, bridge validation, visual regression, performance smoke and full release self-tests.
- Bumped app, core and render-cache compatibility to 1.0.1.

## 1.0.0 — 2026-07-18

- Clear proxy and full-render caches automatically after a normal app termination while retaining crash recovery after abnormal exits.
- Reduced Chroma Carrier Drift sampling work by sharing each interpolated alpha lookup across RGB channels in both proxy and full-render paths.
- Made the backslash Before/After shortcut a latched toggle by ignoring key-repeat events while the key is held.
- Expanded Seamless Loop with start/end transition placement plus Spectral Amount, Frequency Blur and phase-timing controls for Spectral Morph.
- Raised High Preview to 30 fps, kept proxy rate at or below the source rate, and moved playback to monotonic deadlines with overdue-frame catch-up.
- Added Polar Time Warp coordinate rotation and an optional periodic angular time field that removes the Time Loom branch-cut seam.
- Added Spectral Morph and Difference Weave to Seamless Loop with matching in-memory and SSD-backed rendering.
- Fixed Random Stack assigning Partial Amount to Fit Source Size Space-Time Transform, which preserves the canvas but still changes timeline extent.
- Stabilized the workspace with a fixed-width scrolling Inspector and adaptive Preview controls; compacted the sidebar actions to one row, fixed startup focus styling, removed the viewer-background option, and moved queue/export beside the appearance control.
- Added the standard backslash Before/After toggle and shortened the audio option to Original.
- Made Preview automatic and non-optional, removed the manual Update Preview control and stale badge, defaulted new sessions to High Preview, and compacted the toolbar around a top-left appearance button.
- Fixed the sidebar cache-label overlap and shortened its contextual controls to Add effect, Random and Clear.
- Refined Preview toolbar labels and layout, added stable progress geometry, automatic rerender after quality changes, and shortcuts for sequence import, queueing, Random Stack and stack clearing.
- Expanded Pixel Sort with hue-key rotation, Zigzag and Center Out ordering; expanded Axis Datamosh with dark-side luma triggering.
- Renamed Sync Loss and added vertical operation plus normalized resolution-independent sizing for Sync Loss, Block Address Corruption, Signal Weave and Block Graft.
- Added Normal, Add, Screen, Multiply, Difference, Displace and XOR Glitch Amount compositing with proxy/full-render parity.

- Added the centralized EffectDefinition registry and categorized Add Effect menu.
- Introduced the versioned eight-slot effect descriptor with validated logical parameter counts, Amount and deterministic seed fields.
- Added per-effect Amount controls with bit-exact zero, linear wet/dry blending in proxy and mapped full renders, and explicit rejection for shape-changing modes.
- Added core and AVFoundation integration coverage for Amount identity, mapped blending and deterministic repeated renders.
- Added 100-level session Undo/Redo with coalesced slider gestures and lightweight creative-state snapshots.
- Added persistent Auto Update with cost-aware debounce and a proxy-only cancellation domain that cannot cancel full export.
- Added held Source A comparison with normalized timeline mapping and a keyboard hold shortcut.
- Added Inspector comparison for the selected effect's immediate input and output, including normalized timeline mapping for shape-changing effects and cache-hit capture recovery.
- Removed project commands, `.chronoforge` registration, project title/Edited UI and normal-session persistence; only crash recovery remains.
- Added deterministic Random Stack replacement with 1–3 effects, effect-specific distributions, driver/cost/shape validation and single-operation Undo.
- Added a real seed path and Reseed action for Axis Datamosh with matching RAM and out-of-core patterns.
- Added a shared movie/frame-sequence `MediaSource` boundary across proxy decoding, full decoding, recovery and cache signatures; sequence fingerprints include every frame plus playback FPS.
- Added numbered PNG sequence import with natural sorting, dimension validation, gap warnings, selectable/custom FPS, premultiplied-alpha proxy decoding and one-frame-at-a-time full decoding.
- Added streaming 8-bit RGBA PNG sequence export with six-digit names, non-empty-folder protection and alpha-preserving round-trip coverage.
- Added Result/preset/custom Playback FPS reinterpretation, output-duration feedback, audio sync restrictions and explicit black viewer compositing.
- Added RGB Time Slip with independent R/G/B frame offsets, horizontal/vertical/radial spatial separation, current-frame premultiplied alpha, Amount, effect-aware randomization and matching RAM/mapped render paths.
- Added Horizontal Sync Loss with drifting row bands, deterministic Noise/Luma/Edges drivers, Wrap/Clamp/Mirror edges, Reseed, Amount and RAM/mapped parity.
- Added Chroma Carrier Drift with stable current-frame luma, independent Cb/Cr spatial and temporal offsets, five-tap bleed, Together/Split/Alternating modes and premultiplied-alpha parity.
- Added Stride Error with wrong-row-length addressing, base and temporal drift, RGB/separate/alpha channel modes, safe per-frame Wrap/Mirror resolution and deterministic RAM/mapped parity.
- Added Block Address Corruption with held deterministic block maps, spatial and temporal address replacement, four mapping modes, safe edge resolution and seed-stable RAM/mapped parity.
- Added Bitplane Forge with 2–16-bit working precision, selectable plane masks, Shuffle/Rotate/Invert/XOR operations, luma/RGB/channel/alpha targeting and premultiplied-alpha-safe RAM/mapped parity.
- Added Signal Weave for deterministic two-source Lines/Fields/Bands/Checker patterns, phase drift, irregularity, B time offsets and Clamp/Stretch/Crop matching in proxy and full renders.
- Added Block Graft with held B-block replacement, Random/A Luma/B Luma/Difference/A Edges triggers, time offset, size matching and deterministic proxy/full parity.
- Added Channel Transplant with compact per-component A/B routing in RGB or YCbCr, independent B time/spatial offsets, A-alpha preservation and proxy/full parity.
- Bumped cache compatibility for Amount, seed and Wave A nodes, and expanded Random Stack acceptance coverage to 10,000 seeded graphs plus driver-free generation.
- Expanded sequence and session acceptance coverage for concrete mismatch filenames, cancellation-safe completed PNG frames and immutable render-queue snapshots across later Undo.
- Added a procedural, copyright-free visual regression corpus and generated Wave A contact sheet covering standard and alternate/seeded results for every new effect.
- Added an Auto Update debounce diagnostic proving a rapid burst of creative edits launches exactly one proxy preview.
- Added a 10-second Standard-proxy performance smoke executable with per-effect 1-second local/channel and 4-second temporal/memory budget enforcement.

## 0.9.0 — seamless loops and interaction fixes

- Added Seamless Loop with Crossfade, detail-driven Luma Weave and guaranteed Ping-Pong modes in both proxy and out-of-core full rendering.
- Made Return confirm Clear Effect Stack.
- Prevented invalid Space-Time Map axis combinations in the UI and automatically repairs duplicates found in older projects instead of throwing an error.
- Fixed numeric parameter fields retaining keyboard focus after Return, Escape, picker changes or slider interaction.

## 0.8.0 — clear names, real mapping and dark mode

- Reworked Space-Time Map so B's red, green and blue values genuinely map A's X, Y and Time coordinates instead of only borrowing B's dimensions.
- Split the Add Effect menu into clear One Video and Two Videos groups and added an available SF Symbol for Optical Flow Time Warp.
- Renamed the effect families around familiar VFX terms: Space-Time Transform, Self Time Displacement, Pixel Sort, Space-Time Displacement, Optical Flow Time Warp, Time Feedback and Axis Datamosh.
- Added Difference and coordinate-driven Displace modes to Time Feedback.
- Added a persistent light/dark appearance switch in the top-right toolbar.
- Rewrote the README as a practical product introduction and quick-start guide.

## 0.7.0 — Cross-Tensor & Flow

- Added a persistent multi-video media pool with primary A and per-effect driver B routing, including add, replace, remove and primary-source controls.
- Added Dimensional Splicer with A/B axis geometry and true nearest, trilinear and tricubic sampling.
- Added Tensor Displacement with time/X/Y movement, channel selection, Clamp/Stretch/Crop size matching and temporal edge modes.
- Added Motion Time Warp with dense local optical-flow estimation, motion threshold and directional focus.
- Added recursive past/future Chrono Feedback with Add, Screen, Multiply and Lighten modes.
- Added Structural Datamosh along Time, Horizontal or Vertical axes with Edge, Luma and deterministic Random triggers.
- Extended proxy, full-resolution SSD rendering, cache signatures, project persistence and render-queue snapshots to all new effects.
- Restored Space as a global Play/Pause shortcut even when a slider, picker or list owns keyboard focus.

## 0.6.0 — transform families and anti-aliasing

- Fixed the low-quality proxy crash in Polar Time Warp with Wrap edges by keeping rounded fractional coordinates strictly inside the tensor.
- Combined Axis Swap and 3D Rotation into one user-facing Tensor Transform family while preserving old project compatibility.
- Renamed the five effect families consistently: Tensor Transform, Channel Time Shift, Polar Time Warp, Temporal Pixel Sort and Spectral Transform.
- Added independent Off/Light/Strong Spatial and Temporal Prefilter settings to proxy preview, direct export and queued full renders.
- Removed redundant toolbar captions and added Shift+S for Toggle Sidebar.
- Added active-mode subtitles to effect rows, Bypass/Enable to their context menu and an explicit stale-preview badge.

## 0.5.0 — proxy, stack and spectral rotation

- Made `Fit Source Size` the consistent default for every effect that can change the visible frame dimensions.
- Simplified the editor to one sequential effect stack with automatic reconnection after reordering, right-click Delete/Duplicate, and a clearly labelled Clear Effect Stack action.
- Removed the duplicate graph strip below the preview and debounced recovery writes to keep stack gestures responsive.
- Made preview rendering permanently proxy-based and every direct or queued export permanently full-quality.
- Added Standard and High proxy preview modes with a persistent, visible Proxy Preview badge.
- Extended Spectral FFT from axis swapping to arbitrary-angle frequency rotation in X–Time, Y–Time and X–Y planes.
- Rebuilt Radial Time Loom as a joint spatial-temporal polar warp with animated braids, real kaleidoscopic folding and orbiting event-horizon echoes.
- Added ⌘⇧R for Start Render Queue and a sound notification when the whole queue finishes.

## 0.4.0 — workflow and memory update

- Fixed the SwiftUI stale-index crash when deleting the selected effect.
- Renamed Render to Update Preview and made Export recalculate the current graph independently.
- Added an eight-gigabyte automatic cache limit and per-item cleanup in batch rendering.
- Added a sequential render queue that captures different sources, graphs, quality and audio settings.
- Added Replace/Remove media controls and exact source frame counts where the container exposes them.
- Added typed numeric parameter fields, right-click slider reset and Space play/pause.
- Replaced Radial Chrono-Funnel with animated Radial Time Loom, Kaleido Fold and Event Horizon topologies.
- Added Tensor 3D Rotation Fill Fit and Spectral FFT Fit Source Tensor modes.
- Replaced full-volume in-memory FFT with arbitrary-length, line-by-line memory-mapped SSD processing.

## 0.3.0 — macOS usable build

- Real AVFoundation import with orientation-aware proxy and full-resolution decoding.
- Explicit branched node graph with six tensor effects and selectable Output.
- Native or source-canvas resolution modes for space-time transpose.
- Memory-mapped out-of-core full render, CPU thread pools, progress and cancellation.
- Content-addressed proxy/full caches with disk preflight and cache clearing.
- H.264 MP4 export with optional original-audio preservation.
- `.chronoforge` project save/open, security-scoped bookmarks and crash recovery.
- Signed local `.app` packaging, custom icon and end-to-end self-test.

## 0.1.0 — processing foundation

- C++20 tensor, CPU reference effects, DAG validation, cache chunks and resource planning.
