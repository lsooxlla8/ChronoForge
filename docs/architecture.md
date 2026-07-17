# ChronoForge architecture — macOS application

## Scope

The macOS build is a complete CPU reference application: it imports and previews multiple media sources, evaluates a routed effect stack in an ephemeral session, renders through SSD-backed tensors, and exports H.264 MP4. It does not claim real-time 4K processing; proxy quality is the interactive path and full quality is an offline render.

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

Product metadata is centralized in the Swift `EffectRegistry`. Each `EffectDefinition` owns the title, symbol, category, input arity, cost class, shape behavior, logical parameter counts and default node. The Add Effect menu and render descriptor construction consume this registry instead of maintaining parallel switch blocks.

The internal C boundary uses `CFEffectDescriptorV2`: a versioned packet with eight value slots, eight option slots, logical counts, `Amount` and a 64-bit random seed. The boundary validates the version, counts and zeroed padding before dispatch. `Amount = 0` is a bit-exact identity; partial Amount is accepted only when the node preserves tensor shape. Proxy rendering blends in linear premultiplied tensor space, while full rendering blends directly into the mapped output after the effect pass and does not allocate a third full-size tensor.

The editor presents one ordered effect stack and automatically reconnects every A input after insertion, duplication, deletion or drag reordering. Cross-tensor effects also store an independent media-pool B identifier. The core retains explicit validated edges and cycle rejection. Proxy and full caches include fingerprints for A and every referenced B source.

`SessionStore` separates lightweight `CreativeSessionState` from decoded media, render tasks, playback and queue state. `UndoManager` keeps at most 100 creative operations; slider drags are coalesced into one operation. Auto Update waits 450 ms after a committed edit, or 800 ms for global-cost effects, and owns cancellation only for its debounce and proxy render. A full export retains an independent immutable stack snapshot.

`RandomStackGenerator` uses injectable SplitMix64 state, the per-effect `RandomizationProfile`, and reusable uniform, triangular, logarithmic, signed-magnitude and fixed distributions. It produces one to three nodes with 35/45/20 percent length weights, rejects a second global-cost node, requires a real B source for two-input effects, forces shape-safe modes for partial Amount and keeps Seamless Loop last. Axis Datamosh incorporates its stored seed in both RAM and mapped implementations, so Reseed changes the pattern without breaking proxy/full parity.

There is no user project format in the 1.0 workflow. A hidden `SessionRecoverySnapshot` is written while a session is active, removed by normal application termination, and offered once after a crash or force quit. The internal recovery JSON is not registered with Launch Services and is not a long-term document contract.

## Out-of-core strategy

Proxy cache files use an atomic versioned container. Full renders use mapped linear-float tensors with sidecar metadata and delete incomplete scratch results on cancellation. Cache keys include:

```text
engine/cache format version
node type + canonical parameters
amount + random seed
ordered upstream cache keys
input source fingerprint
proxy scale + output timeline + colour policy
tile extent and halo
```

Nodes fall into three scheduling classes:

| Class | Examples | Scheduling rule |
| --- | --- | --- |
| Local | Colour conversion, transform with bounded sample footprint | Process independent tiles plus halo. |
| Per-pixel temporal | Self Time Displacement, Polar Time Warp, Pixel Sort (Time) | Keep the complete `T` vector for every pixel within an `H × W` tile. |
| Global | 3D rotation with temporal mixing, 3D FFT | Preflight SSD space. FFT maps complex tensors and transforms independent lines with bounded RAM. |

The supplied `TilePlanner` implements the per-pixel-temporal class. It will be generalised after executor integration.

## Effect families and implementation notes

1. **Space-Time Transform** groups geometric tensor operations. Axis Swap permutes `X↔T` or `Y↔T`; 3D Rotation samples backward with trilinear interpolation in normalised `T/H/W`. Fit Source Size is the default for both modes.
2. **Self Time Displacement** determines the source frame from luma/R/G/B/alpha at the output coordinate and supports clamp, wrap or mirror edges.
3. **Polar Time Warp** jointly warps fractional time, radius and polar angle into animated braids. Kaleido Fold creates moving spatial sectors; Event Horizon bends radius into orbiting temporal echoes.
4. **Pixel Sort (Time)** sorts complete colour vectors by a scalar key along `T`. Pixels below threshold retain their time slots; selected samples are stably sorted into the selected slots.
5. **3D FFT Transform** uses Bluestein arbitrary-length transforms over memory-mapped tensors. It swaps frequency axes or rotates the complex spectrum in X–Time, Y–Time or X–Y while keeping the full volume off RAM.
6. **Space-Time Map** takes colour from A and output extents from the selected A/B axes. When a B axis is selected, B's red, green or blue value is an absolute normalized lookup coordinate for A's X, Y or Time respectively. Volumetric interpolation is nearest, trilinear or tricubic.
7. **Space-Time Displacement** samples A at time/X/Y coordinates offset by a channel from B. Clamp, Stretch and Crop define mismatched tensor sizes explicitly.
8. **Optical Flow Time Warp** estimates a dense local Lucas–Kanade flow field from adjacent frames and bends time by motion magnitude, threshold and direction.
9. **Time Feedback** is sequential along Time: it reads prior output for recursive echoes and future input for look-ahead echoes, with colour blends plus a coordinate-displacement mode.
10. **Axis Datamosh** carries samples along Time, X or Y after deterministic edge, luma or random triggers.
11. **Seamless Loop** overlaps the tail with the beginning, optionally staggers the transition per pixel by luma, or emits a forward/reverse ping-pong tensor. Crossfade modes shorten `T` by the overlap; Ping-Pong emits `2T-2` frames.

Spatial and Temporal Prefilter are project-level output settings rather than editable nodes. The renderer injects one deterministic hidden low-pass stage after the visible stack, so preview, direct export and queued renders share the same cache signature and result.

## macOS-specific plan

| Phase | Deliverable |
| --- | --- |
| 0 — complete | C++20 tensor rules, CPU references, cache format and deterministic tests. |
| 1 — complete | C bridge, native AVFoundation proxy decode, editable SwiftUI graph, preview cache and H.264 MP4 export. |
| 2 — complete | File-backed full-resolution decoder/executor, thread pools, cancellation and crash recovery. |
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

Full renders use headerless linear-RGBA float files plus JSON metadata. They are mapped with `mmap`; local nodes read one mapped input and write one mapped output, then remove the prior scratch result. Pixel Sort (Time) retains only one pixel's complete time vector per worker. Space is checked before decode and before every node, with a 512 MiB filesystem reserve.

Two-input full renders decode each referenced B source once into its own content-addressed mapped tensor. Space-Time Map and Space-Time Displacement read A and B concurrently through virtual-memory paging and write only the current output tensor; neither input volume is copied into process RAM. Optical Flow Time Warp currently uses the deterministic CPU reference on macOS. Metal/Vision acceleration is an interchangeable future backend, not a different node format.

Frequency transformation is intentionally different: it is global, but full render writes two temporary complex tensors to SSD and performs separable width, height and time transforms one line at a time. Swap permutes this spectrum; Rotate periodically resamples its complex values before the inverse FFT. The RAM budget controls concurrent line buffers, not total video size. Temporary frequency files are removed on success, failure or cancellation.
