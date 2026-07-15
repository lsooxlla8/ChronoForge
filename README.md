# ChronoForge

**ChronoForge** is a native macOS-first editor for treating video as a space-time tensor `(T, H, W, C)`, not a stack of isolated frames. It is an early, tested foundation: a C++20 processing core and a SwiftUI workspace shell are present; FFmpeg decode/encode and Metal execution are the next integration milestones.

## What works in this first foundation

- Six CPU reference effects: space-time transpose, luma-time shift, radial chrono-funnel, temporal pixel sort, trilinear 3D rotation, and a bounded 3D FFT frequency-axis swap.
- A directed acyclic node graph that rejects cycles.
- SSD cache chunks written atomically, so cancelled or interrupted renders do not create valid-looking partial cache entries.
- A tile planner designed for complete per-pixel time series: temporal effects split across `H × W`, while retaining all `T` samples needed to operate correctly.
- CPU safety gate for 3D FFT. It supports power-of-two proxy tensors only and refuses requests before allocating a working set above the configured budget.
- A native macOS 14+ SwiftUI workspace shell showing the intended node graph, preview, inspector and proxy/export modes.

## Key technical decisions

The original brief is strong, but a few details make the difference between a demo and a dependable editor:

| Decision | Why it matters |
| --- | --- |
| Linear float working space with explicit transfer/alpha metadata | Brightness, interpolation and FFT are mathematically meaningful only after decoding video out of display-encoded sRGB/Rec.709. |
| Logical playback rate is metadata, not an axis size | Swapping `T` and `X` changes the tensor extents; the desired export FPS remains an explicit Output choice. |
| Tile contracts declare their temporal footprint | Pixel sort needs every time sample for a pixel. A scheduler must not accidentally run it on independent frame chunks. |
| Cache key includes graph signature, node parameters, proxy scale and source fingerprint | Otherwise a changed node may display stale cached output. The store is ready for such keys; graph hashing is the next executor step. |
| FFT is a global operation | It is never silently tiled. The proxy CPU implementation is bounded; full render will route to Metal/MetalFFT or an external-memory algorithm after a resource estimate. |
| Effects sample backward into source coordinates | It avoids holes in radial/rotation transforms, and 3D rotation uses trilinear interpolation. |

The complete operating model and the remaining production milestones are in [docs/architecture.md](docs/architecture.md).

## Build the core on macOS

Prerequisites: Xcode Command Line Tools and CMake 3.24+.

```bash
cmake -S . -B build -DCHRONOFORGE_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/chronoforge-cli
```

The SwiftUI shell is an independent macOS package:

```bash
cd apps/macos
swift run ChronoForgeMac
```

## Repository layout

```text
apps/
  cli/             Core diagnostic executable
  macos/           Native SwiftUI workspace shell (macOS 14+)
include/           Public C++20 core interfaces
src/               Core implementation
tests/             Deterministic core tests
docs/              Architecture and delivery milestones
```

## Practical limits in this milestone

This is deliberately not yet a production video editor. The core runs uncompressed `float` tensors in memory for its reference effects; the executor that feeds them from FFmpeg-backed SSD cache tiles is the next implementation step. The on-screen macOS workspace is intentionally decoupled from the core until its thin C/Objective-C++ bridge lands.

That separation is purposeful: it allows the tensor rules, caching format and graph API to be tested now, before GPU and codec complexity obscure errors.
