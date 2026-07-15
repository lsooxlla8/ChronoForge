# Changelog

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
