# Changelog

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
