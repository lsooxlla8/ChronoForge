# ChronoForge

**ChronoForge** is a native macOS-first editor for treating video as a space-time tensor `(T, H, W, C)`, not a stack of isolated frames. The current macOS application imports real video through AVFoundation, builds a bounded proxy, evaluates a six-effect C++ graph, caches its result and exports H.264 MP4.

## What works in this first foundation

- Six CPU reference effects: space-time transpose, luma-time shift, radial chrono-funnel, temporal pixel sort, trilinear 3D rotation, and a bounded 3D FFT frequency-axis swap.
- A directed acyclic node graph that rejects cycles.
- SSD cache chunks written atomically, so cancelled or interrupted renders do not create valid-looking partial cache entries.
- A tile planner designed for complete per-pixel time series: temporal effects split across `H × W`, while retaining all `T` samples needed to operate correctly.
- CPU safety gate for 3D FFT. It supports power-of-two proxy tensors only and refuses requests before allocating a working set above the configured budget.
- A native macOS 14+ SwiftUI editor with real video import, frame preview, editable effect parameters, reordering/bypass, render cancellation and MP4 export.
- A content-addressed proxy cache keyed by source fingerprint, graph parameters and engine version.
- A native H.264 integration check that generates a movie, decodes it and sends it through the C++ bridge.

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

Build or run the native macOS application from the repository root:

```bash
swift run ChronoForgeMac
```

Create a signed local application bundle:

```bash
./scripts/package_macos.sh release
open dist/ChronoForge.app
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

Proxy editing and proxy MP4 export are functional. Full-resolution export is still being moved to the file-backed executor; selecting Full Quality does not yet bypass the bounded proxy. Audio is intentionally omitted because time-axis transformations require an explicit audio mapping policy.
