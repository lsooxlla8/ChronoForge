# ChronoForge

**ChronoForge** is a native macOS-first editor for treating video as a space-time tensor `(T, H, W, C)`, not a stack of isolated frames. It imports real video, evaluates a branched six-effect C++ graph, previews a bounded proxy and renders full-resolution H.264 MP4 through memory-mapped SSD tensors.

## What is included

- Six CPU reference effects: space-time transpose, luma-time shift, radial chrono-funnel, temporal pixel sort, trilinear 3D rotation, and a bounded 3D FFT frequency-axis swap.
- A directed acyclic node graph that rejects cycles.
- SSD cache chunks written atomically, so cancelled or interrupted renders do not create valid-looking partial cache entries.
- A tile planner designed for complete per-pixel time series: temporal effects split across `H × W`, while retaining all `T` samples needed to operate correctly.
- CPU safety gate for 3D FFT. Arbitrary tensor extents are padded safely and requests are refused before allocating a working set above the configured budget.
- A native macOS 14+ SwiftUI editor with real video import, frame preview, editable effect parameters, reordering/bypass, render cancellation and MP4 export.
- A content-addressed proxy cache keyed by source fingerprint, graph parameters and engine version.
- A native H.264 integration check that generates a movie, decodes it and sends it through the C++ bridge.
- Full-resolution out-of-core decode, processing and encode. Intermediate tensors are mapped from SSD and each completed node replaces the preceding scratch file.
- Explicit graph inputs and Output selection: branches are supported, cycles are rejected, and only ancestors of Output are evaluated.
- Project save/open, security-scoped source bookmarks, autosave recovery, cache management and optional original-audio muxing.
- CPU thread pools for proxy and full local/temporal effects, granular progress and cancellation with partial-file cleanup.

## Key technical decisions

The original brief is strong, but a few details make the difference between a demo and a dependable editor:

| Decision | Why it matters |
| --- | --- |
| Linear float working space with explicit transfer/alpha metadata | Brightness, interpolation and FFT are mathematically meaningful only after decoding video out of display-encoded sRGB/Rec.709. |
| Logical playback rate is metadata, not an axis size | Swapping `T` and `X` changes the tensor extents; the desired export FPS remains an explicit Output choice. |
| Tile contracts declare their temporal footprint | Pixel sort needs every time sample for a pixel. A scheduler must not accidentally run it on independent frame chunks. |
| Cache key includes graph signature, node parameters, proxy scale and source fingerprint | Otherwise a changed node could display stale cached output. Proxy and full-render caches use content-addressed keys. |
| FFT is a global operation | It is never silently tiled. Proxy and full render use a bounded CPU implementation and stop before allocation when the estimated working set exceeds the configured RAM budget. |
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

3D FFT remains a global operation. Full-quality FFT is allowed only when its padded complex working set fits the configured RAM budget; otherwise the app stops before allocation and asks for Proxy or a smaller tensor. Original audio can be preserved explicitly; when the transformed video becomes longer, audio ends at its original duration.
