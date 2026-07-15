# ChronoForge

**ChronoForge** is a native macOS-first editor for treating video as a space-time tensor `(T, H, W, C)`, not a stack of isolated frames. It imports real video, evaluates a sequential five-family C++ effect stack, previews a bounded proxy and renders full-resolution H.264 MP4 through memory-mapped SSD tensors.

## What is included

- Five coherent effect families: Tensor Transform (Axis Swap/3D Rotation), Channel Time Shift, Polar Time Warp, Temporal Pixel Sort and Spectral Transform (Swap/Rotate).
- A directed acyclic node graph that rejects cycles.
- SSD cache chunks written atomically, so cancelled or interrupted renders do not create valid-looking partial cache entries.
- A tile planner designed for complete per-pixel time series: temporal effects split across `H × W`, while retaining all `T` samples needed to operate correctly.
- Arbitrary-length full-resolution 3D FFT that maps complex tensors to SSD and keeps only individual frequency lines in RAM.
- A native macOS 14+ SwiftUI editor with real video import, frame preview, editable effect parameters, reordering/bypass, render cancellation and MP4 export.
- A content-addressed proxy cache keyed by source fingerprint, graph parameters and engine version.
- A native H.264 integration check that generates a movie, decodes it and sends it through the C++ bridge.
- Full-resolution out-of-core decode, processing and encode. Intermediate tensors are mapped from SSD and each completed node replaces the preceding scratch file.
- A sequential UI stack that automatically reconnects inputs after drag reordering; the core dependency graph still validates and rejects cycles.
- Project save/open, security-scoped source bookmarks, autosave recovery, automatic 8 GB cache trimming and optional original-audio muxing.
- Full-quality sequential render queue with a settings snapshot per item, automatic cache cleanup between jobs, ⌘⇧R start and completion sound.
- Always-proxy preview with Standard/High quality choices; direct export and render queue always decode the original at full quality.
- Independent spatial and temporal output prefilters with Off/Light/Strong levels, applied identically to proxy and SSD-backed full renders.
- Exact numeric parameter entry, contextual default reset, Space play/pause and in-editor media replacement/removal.
- CPU thread pools for proxy and full local/temporal effects, granular progress and cancellation with partial-file cleanup.

## Key technical decisions

The original brief is strong, but a few details make the difference between a demo and a dependable editor:

| Decision | Why it matters |
| --- | --- |
| Linear float working space with explicit transfer/alpha metadata | Brightness, interpolation and FFT are mathematically meaningful only after decoding video out of display-encoded sRGB/Rec.709. |
| Logical playback rate is metadata, not an axis size | Swapping `T` and `X` changes the tensor extents; the desired export FPS remains an explicit Output choice. |
| Tile contracts declare their temporal footprint | Pixel sort needs every time sample for a pixel. A scheduler must not accidentally run it on independent frame chunks. |
| Cache key includes graph signature, node parameters, proxy scale and source fingerprint | Otherwise a changed node could display stale cached output. Proxy and full-render caches use content-addressed keys. |
| FFT is a global operation | Graphs containing FFT automatically use separable arbitrary-length lines over memory-mapped complex tensors for both proxy and full render, so tensor volume consumes SSD rather than RAM. |
| Effects sample backward into source coordinates | It avoids holes in radial/rotation transforms, and 3D rotation uses trilinear interpolation. |

The complete operating model is in [docs/architecture.md](docs/architecture.md).

## Build the core on macOS

Prerequisites: Xcode Command Line Tools and CMake 3.24+.

```bash
cmake -S . -B build -DCHRONOFORGE_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/chronoforge-cli
```

Build or run the native macOS application from the repository root:

```bash
swift run ChronoForgeMac
```

Create a signed local application bundle:

```bash
./scripts/package_macos.sh release
./scripts/create_dmg.sh
open dist/ChronoForge.app
```

Run the end-to-end application diagnostic (rotated H.264 input, proxy decode, project round-trip, disk tensor render and MP4 validation):

```bash
.build/arm64-apple-macosx/release/ChronoForgeMac --self-test
```

## Repository layout

```text
apps/
  cli/             Core diagnostic executable
  macos/           Native SwiftUI editor and AVFoundation pipeline (macOS 14+)
include/           Public C++20 core interfaces
src/               Core implementation
tests/             Deterministic core tests
docs/              Architecture and delivery milestones
```

## Practical limits in this milestone

The bundled local build targets Apple Silicon and macOS 14+. A universal Intel/Apple Silicon package requires full Xcode (`xcbuild`), which is not present in the current build environment. Local builds are ad-hoc signed; public distribution still requires the owner's Apple Developer ID certificate and notarization.

3D FFT remains computationally expensive and requires temporary SSD space for two complex tensors, but full-quality processing no longer requires the complete volume to fit in RAM. Original audio can be preserved explicitly; when an axis-changing effect makes the video longer, audio ends at its original duration.
