# ChronoForge architecture — macOS foundation

## Scope of this milestone

The first macOS build establishes a reliable, testable contract between the editor and processing core. It does not claim real-time 4K rendering yet. Its job is to make future FFmpeg, Metal and export work safe to add.

## Data contract

`VideoTensor` has canonical memory order `T, H, W, C` and stores `float32` samples in a linear working space. Nodes may change `T`, `H` or `W`; `C` is preserved unless a future format-conversion node explicitly changes it. Tensor metadata carries playback frame rate, colour transfer, and alpha representation.

Input Decode performs these operations before a tensor enters the graph:

1. Decode compressed media to a known pixel format through FFmpeg.
2. Convert transfer function to linear light and preserve source colour-space metadata.
3. Convert to `RGB` or premultiplied `RGBA` float32.
4. Persist a source fingerprint: canonical source path, stream index, duration, dimensions, frame timestamps, colour metadata and a fast content hash.

`T` is a logical sequence index, not an assumption that frames are exactly `1 / fps` apart. The first FFmpeg integration must store a PTS table. Variable-frame-rate sources are sampled onto a declared proxy/export timeline; this is required for meaningful temporal shifts.

## Node and execution model

```mermaid
flowchart LR
    source["Input / FFmpeg decode"] --> proxy["Proxy sampler"]
    proxy --> graph["Validated DAG"]
    graph --> scheduler["Tile & dependency scheduler"]
    scheduler --> cache["SSD content-addressed cache"]
    cache --> preview["Metal preview texture"]
    cache --> export["FFmpeg encode"]
```

Every node declares:

- parameter schema and a canonical serialisation for cache hashing;
- input/output tensor descriptor;
- access pattern: local, finite temporal halo, or complete temporal vector/global volume;
- deterministic backend capability: CPU reference, Metal kernel, or global FFT;
- cacheability and cache version.

The current `NodeGraph` handles ordering and cycle prevention. The next executor will evaluate graph outputs by `(node signature, quality, tile extent)` rather than materialising an entire video.

## Out-of-core strategy

Cache files are binary float chunks with a versioned header and an atomic temporary-file rename. The final cache key must be SHA-256 over:

```text
engine/cache format version
node type + canonical parameters
ordered upstream cache keys
input source fingerprint
proxy scale + output timeline + colour policy
tile extent and halo
```

Nodes fall into three scheduling classes:

| Class | Examples | Scheduling rule |
| --- | --- | --- |
| Local | Colour conversion, transform with bounded sample footprint | Process independent tiles plus halo. |
| Per-pixel temporal | Luma-Time Shift, Radial Chrono-Funnel, Temporal Pixel Sort | Keep the complete `T` vector for every pixel within an `H × W` tile. |
| Global | 3D rotation with temporal mixing, 3D FFT | Preflight RAM/SSD/GPU budget. Use slab algorithms only where mathematically valid; otherwise force proxy or refuse. |

The supplied `TilePlanner` implements the per-pixel-temporal class. It will be generalised after executor integration.

## The six effects and implementation notes

1. **Space-Time Transpose** is a lossless extent permutation. `X↔T` changes `(T,H,W,C)` to `(W,H,T,C)`, while `Y↔T` changes it to `(H,T,W,C)`.
2. **Luma-Time Shift** determines the source frame from the input sample at the output coordinate. It supports luma/R/G/B/alpha and clamp, wrap or mirror edges. The UI must constrain the multiplier in user-facing frame units.
3. **Radial Chrono-Funnel** computes distance in source pixel space. Its center is normalised so proxy and full-quality compositions align.
4. **Temporal Pixel Sort** sorts complete colour vectors by a scalar key along `T`. Pixels below threshold retain their time slots; selected samples are stably sorted into the selected slots.
5. **Tensor 3D Rotation** samples backward with trilinear interpolation in normalised `T/H/W` space. Normalisation prevents a long duration from visually overpowering spatial axes. Output bounds remain fixed in this milestone; a future fit/crop node will expose canvas bounds explicitly.
6. **3D FFT Swap** uses a Cooley–Tukey reference implementation for small power-of-two proxies. Production implementation must use Metal FFT or FFTW/FFmpeg-adjacent CPU worker constraints, tracked as a global resource job.

## macOS-specific plan

| Phase | Deliverable |
| --- | --- |
| 0 — complete | C++20 tensor rules, CPU references, cache format and deterministic tests. |
| 1 — complete | C bridge, native AVFoundation proxy decode, editable SwiftUI graph, preview cache and H.264 MP4 export. |
| 2 — active | File-backed full-resolution decoder/executor, render progress and project persistence. |
| 3 | Metal kernels for local/per-pixel effects and a tile atlas for preview. |
| 4 | Metal FFT/global transform policy, render estimates, disk reservation, diagnostics and recovery. |
| 5 | Windows frontend / shared GPU abstraction only after parity tests pass. |

## Guardrails that must remain non-negotiable

- Measure both host RAM and free cache-disk space before a render starts. Reserve cache space, do not just estimate it.
- Keep decoding, render scheduling and encoding in separate cancellation domains. Export cancellation must leave the source and cache valid.
- Cap concurrency by RAM/GPU budget, not core count alone.
- Treat alpha intentionally. Colour interpolation in premultiplied linear RGB avoids fringes.
- Do not claim preview equals final render unless the same node backend and colour policy were used.
- Record engine version and GPU backend in render metadata so a project can explain a changed cache result.
