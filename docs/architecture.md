# ChronoForge architecture — macOS application

## Scope

The macOS build is a complete CPU reference application: it imports and previews real media, persists editable projects, evaluates a branched graph, renders through SSD-backed tensors, and exports H.264 MP4. It does not claim real-time 4K processing; proxy quality is the interactive path and full quality is an offline render.

## Data contract

`VideoTensor` has canonical memory order `T, H, W, C` and stores `float32` samples in a linear working space. Nodes may change `T`, `H` or `W`; `C` is preserved unless a future format-conversion node explicitly changes it. Tensor metadata carries playback frame rate, colour transfer, and alpha representation.

Input Decode performs these operations before a tensor enters the graph:

1. Decode compressed media to a known pixel format through AVFoundation on macOS.
2. Convert transfer function to linear light and preserve source colour-space metadata.
3. Convert to `RGB` or premultiplied `RGBA` float32.
4. Persist a source fingerprint from the canonical path, file metadata, media dimensions and timeline.

`T` is a logical sequence index, not an assumption that frames are exactly `1 / fps` apart. Full decode retains presentation timestamps; axis-changing effects deliberately establish a new uniform output timeline.

## Node and execution model

```mermaid
flowchart LR
    source["Input / AVFoundation decode"] --> proxy["Proxy sampler"]
    proxy --> graph["Validated DAG"]
    graph --> scheduler["Tile & dependency scheduler"]
    scheduler --> cache["SSD content-addressed cache"]
    cache --> preview["SwiftUI preview"]
    cache --> export["AVFoundation encode"]
```

Every node declares:

- parameter schema and a canonical serialisation for cache hashing;
- input/output tensor descriptor;
- access pattern: local, finite temporal halo, or complete temporal vector/global volume;
- deterministic backend capability: CPU reference, Metal kernel, or global FFT;
- cacheability and cache version.

The editor presents one ordered effect stack and automatically reconnects every input after insertion, duplication, deletion or drag reordering. The core retains explicit validated edges and cycle rejection. Proxy and full caches are keyed by source fingerprint, stack signature, quality and engine version.

## Out-of-core strategy

Proxy cache files use an atomic versioned container. Full renders use mapped linear-float tensors with sidecar metadata and delete incomplete scratch results on cancellation. Cache keys include:

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
| Per-pixel temporal | Luma-Time Shift, Radial Time Loom, Temporal Pixel Sort | Keep the complete `T` vector for every pixel within an `H × W` tile. |
| Global | 3D rotation with temporal mixing, 3D FFT | Preflight SSD space. FFT maps complex tensors and transforms independent lines with bounded RAM. |

The supplied `TilePlanner` implements the per-pixel-temporal class. It will be generalised after executor integration.

## The six effects and implementation notes

1. **Space-Time Transpose** is an extent permutation. `X↔T` changes `(T,H,W,C)` to `(W,H,T,C)`, while `Y↔T` changes it to `(H,T,W,C)`. Fit Source Size resamples the new frame canvas but intentionally keeps the swapped time extent. At a fixed output FPS, duration therefore becomes the new frame count divided by FPS.
2. **Luma-Time Shift** determines the source frame from the input sample at the output coordinate. It supports luma/R/G/B/alpha and clamp, wrap or mirror edges. The UI must constrain the multiplier in user-facing frame units.
3. **Radial Time Loom** jointly warps fractional time, radius and polar angle into animated braids with trilinear sampling. Kaleido Fold creates moving spatial sectors; Event Horizon bends radius into orbiting temporal echoes while retaining proxy/full parity.
4. **Temporal Pixel Sort** sorts complete colour vectors by a scalar key along `T`. Pixels below threshold retain their time slots; selected samples are stably sorted into the selected slots.
5. **Tensor 3D Rotation** samples backward with trilinear interpolation in normalised `T/H/W` space. Fill Fit computes an inscribed inverse-rotation scale so every output frame is covered without empty corners.
6. **3D FFT Transform** uses Bluestein arbitrary-length transforms over memory-mapped tensors. It can swap frequency axes or rotate the complex spectrum by an arbitrary angle in X–Time, Y–Time or X–Y. Proxy graphs containing this node automatically switch to the same safe disk executor. Fit Source Size is the default and preserves the input `T/H/W` extents.

## macOS-specific plan

| Phase | Deliverable |
| --- | --- |
| 0 — complete | C++20 tensor rules, CPU references, cache format and deterministic tests. |
| 1 — complete | C bridge, native AVFoundation proxy decode, editable SwiftUI graph, preview cache and H.264 MP4 export. |
| 2 — complete | File-backed full-resolution decoder/executor, thread pools, cancellation, project persistence and recovery. |
| 3 — optional optimization | Metal kernels for local/per-pixel effects and a tile atlas for preview. |
| 4 — optional optimization | Metal FFT/global transform acceleration. Disk reservation, diagnostics and recovery already ship in the CPU path. |
| 5 | Windows frontend / shared GPU abstraction only after parity tests pass. |

## Guardrails that must remain non-negotiable

- Measure both host RAM and free cache-disk space before a render starts. Reserve cache space, do not just estimate it.
- Keep decoding, render scheduling and encoding in separate cancellation domains. Export cancellation must leave the source and cache valid.
- Cap concurrency by RAM/GPU budget, not core count alone.
- Treat alpha intentionally. Colour interpolation in premultiplied linear RGB avoids fringes.
- Do not claim preview equals final render unless the same node backend and colour policy were used.
- Record engine version and GPU backend in render metadata so a project can explain a changed cache result.

## Shipping implementation notes

The macOS codec backend is AVFoundation so the application bundle is self-contained and uses hardware H.264 decode/encode without a Homebrew dependency. The portable core and C ABI do not depend on AVFoundation. The Windows port will attach FFmpeg at the same decoded-tensor/file-tensor boundary.

Full renders use headerless linear-RGBA float files plus JSON metadata. They are mapped with `mmap`; local nodes read one mapped input and write one mapped output, then remove the prior scratch result. Temporal Pixel Sort retains only one pixel's complete time vector per worker. Space is checked before decode and before every node, with a 512 MiB filesystem reserve.

Frequency transformation is intentionally different: it is global, but full render writes two temporary complex tensors to SSD and performs separable width, height and time transforms one line at a time. Swap permutes this spectrum; Rotate periodically resamples its complex values before the inverse FFT. The RAM budget controls concurrent line buffers, not total video size. Temporary frequency files are removed on success, failure or cancellation.
