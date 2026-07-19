# ChronoForge

**Break video across space and time.**

ChronoForge 1.1 is a native offline macOS app for glitch art.
Import movies or PNG image sequences, stack strange time-based effects, watch a fast proxy preview, then export the result at full resolution.

The 1.0 workflow is deliberately session-based: every normal launch starts empty, and a clean quit discards media, effect settings and the render queue. Only finished videos remain the thing you save.

No formulas required. If you have used an effect stack, displacement map or render queue before, you already know the workflow.

## What can it do?

- Turn time into a visible image axis, or rotate the whole clip through space and time.
- Sort every pixel through the duration of a video instead of across one frame.
- Bend time with brightness, motion, polar shapes or another video.
- Use a second clip as an RGB coordinate map or a space-time displacement map.
- Build recursive past/future feedback, spectral FFT glitches and directional datamosh trails.
- Queue several full-quality renders and let ChronoForge process them one after another.
- Export the exact Viewer position as a full-resolution alpha-preserving PNG and learn every control from searchable built-in Help.

## Why it exists

ChronoForge began with one idea: treat a video as a three-dimensional volume and exchange a spatial axis with time. The original inspiration was foo52ru ТехноШаман's video [«Меняем пространство и время местами»](https://www.youtube.com/watch?v=n4tbdFD18vs). That experiment became **Space-Time Transform**, then grew into an offline multitool for tearing apart time, memory, colour channels, signal structure and relationships between one or more videos.

## Effects

### One video

| Effect | What you see |
| --- | --- |
| **Space-Time Transform** | Swap width or height with time, or rotate the video volume in 3D. |
| **Self Time Displacement** | Bright or coloured parts of the same clip jump forward or backward in time. |
| **Polar Time Warp** | Braided, folded and orbiting time structures with a rotatable polar field and optional seamless angle. |
| **Pixel Sort (Time)** | Sort through frames by luma, saturation or a shifted hue key using ascending, descending, zigzag or center-out order. |
| **3D FFT Transform** | Swap or rotate spatial and temporal frequencies for spectral textures. |
| **RGB Time Slip** | Pull red, green and blue from independent frames and split them horizontally, vertically or radially. |
| **Sync Loss** | Tear coherent row or column bands horizontally or vertically with resolution-independent band sizing. |
| **Chroma Carrier Drift** | Keep luma stable while Cb/Cr drift, delay and bleed together or in opposite directions. |
| **Stride Error** | Read each frame with a deliberately wrong row stride and safe wrapped or mirrored memory addresses. |
| **Block Address Corruption** | Replace held spatial blocks with deterministic addresses from elsewhere in space and nearby time. |
| **Bitplane Forge** | Quantize selected luma, colour or alpha channels and shuffle, rotate, invert or XOR chosen bitplanes. |
| **Optical Flow Time Warp** | Moving objects bend time more than the static background. |
| **Time Feedback** | Recursive past and future echoes with colour and displacement blend modes. |
| **Axis Datamosh** | Freeze and drag image data along time, horizontal or vertical lines, including bright- or dark-side luma triggering. |
| **Seamless Loop** | Close a clip with start/end crossfade, luma/difference weave, controllable FFT spectral morph or a guaranteed ping-pong pass. |
| **Affinity Migration** | Let colour-class neighbourhoods pull cell-sized regions toward nearby source coordinates, with motion-responsive migration and deterministic reseeding. |

### Two videos

| Effect | What A and B do |
| --- | --- |
| **Space-Time Map** | A supplies the picture. B's red, green and blue channels choose where ChronoForge reads X, Y and Time from A. |
| **Space-Time Displacement** | A is the target. Brightness or a channel from B pushes A through X, Y and Time. |
| **Signal Weave** | Alternate moving lines, interlaced fields, bands or checker cells between A and B. |
| **Block Graft** | Implant held blocks from B into A using random, luma, difference or edge triggers. |
| **Channel Transplant** | Replace individual RGB or Y/Cb/Cr components of A with offset samples from B. |

## Five-minute workflow

1. Import a video or numbered PNG sequence. This becomes the primary source, **A**. Sequence import reports dimensions, numbering gaps and lets you choose FPS.
2. Add effects from the sidebar. They run from top to bottom.
3. Adjust **Amount** and its Normal/Add/Screen/Multiply/Difference/Displace/XOR Glitch mode, or press **Random** to replace the chain with 1–3 compatible effects. One Undo restores the previous stack.
4. Preview updates automatically after a short pause. It is always a smaller proxy so experimentation stays responsive.
5. For a two-video effect, add another clip to Media and choose it as **Driver video (B)**.
6. Use **Export** for the current frame, an H.264 MP4, or an alpha-preserving PNG sequence. Current Frame evaluates the same full-resolution stack and cache as a complete render. Choose FPS to reinterpret finished frames without resampling them. **Add to Queue** remains available for MP4 batches; press **Shift–Command–R** to start the queue.

Useful shortcuts: **Space** plays or pauses Preview, **Command–Z** / **Shift–Command–Z** undo and redo, **\\** toggles Before/After, **Shift–S** toggles the sidebar, **Shift–R** generates a random stack, **Shift–Delete** clears it, **Shift–Command–I** imports a PNG sequence, **Shift–Command–E** exports the current frame, and **Option–Command–R** adds the current render to the queue.

Open **Help → ChronoForge Help** (Command–?) for a searchable workflow guide, control reference, glossary and a page for every production effect. Contextual `?` buttons in the toolbar and Inspector open the relevant section.

## Download and requirements

Download the latest macOS build from [Releases](https://github.com/lsooxlla8/ChronoForge/releases/latest).

- Apple Silicon Mac
- macOS 14 Sonoma or newer
- Free SSD space for full-quality temporary renders

The local build is ad-hoc signed. On first launch, macOS may ask you to confirm that you want to open it.

## Build from source

For developers: install Xcode Command Line Tools and CMake 3.24+, then run:

```bash
cmake -S . -B build -DCHRONOFORGE_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
swift run ChronoForgeMac
```

For a reproducible UI acceptance pass without opening system file pickers, launch the
packaged debug app with `--ui-acceptance`. ChronoForge creates two small procedural
movies, loads them as A/B, and disables recovery reads, writes and cleanup for that run:

```bash
./scripts/package_macos.sh debug
open -na dist/ChronoForge.app --args --ui-acceptance
```

Create the app and DMG:

```bash
./scripts/package_macos.sh release
./scripts/create_dmg.sh
```

The processing core is C++20; the macOS interface is SwiftUI and AVFoundation. Full-resolution intermediate tensors are memory-mapped to SSD. More implementation detail lives in [docs/architecture.md](docs/architecture.md).
