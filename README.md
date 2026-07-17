# ChronoForge

**Break video across space and time.**

ChronoForge is a native macOS video-effects app for VJs, glitch artists and motion designers. Import movies or PNG image sequences, stack strange time-based effects, watch a fast proxy preview, then export the result at full resolution.

The 1.0 workflow is deliberately session-based: every normal launch starts empty, and a clean quit discards media, effect settings and the render queue. ChronoForge keeps only a hidden crash-recovery snapshot while the app is running; finished videos remain the thing you save.

No formulas required. If you have used an effect stack, displacement map or render queue before, you already know the workflow.

## What can it do?

- Turn time into a visible image axis, or rotate the whole clip through space and time.
- Sort every pixel through the duration of a video instead of across one frame.
- Bend time with brightness, motion, polar shapes or another video.
- Use a second clip as an RGB coordinate map or a space-time displacement map.
- Build recursive past/future feedback, spectral FFT glitches and directional datamosh trails.
- Queue several full-quality renders and let ChronoForge process them one after another.

## Effects

### One video

| Effect | What you see |
| --- | --- |
| **Space-Time Transform** | Swap width or height with time, or rotate the video volume in 3D. |
| **Self Time Displacement** | Bright or coloured parts of the same clip jump forward or backward in time. |
| **Polar Time Warp** | Braided, folded and orbiting time structures around a movable centre. |
| **Pixel Sort (Time)** | Pixel sorting through frames, so shadows and highlights flow through time. |
| **3D FFT Transform** | Swap or rotate spatial and temporal frequencies for spectral textures. |
| **RGB Time Slip** | Pull red, green and blue from independent frames and split them horizontally, vertically or radially. |
| **Horizontal Sync Loss** | Tear coherent row bands sideways with drifting deterministic noise, luma or edge drivers. |
| **Chroma Carrier Drift** | Keep luma stable while Cb/Cr drift, delay and bleed together or in opposite directions. |
| **Stride Error** | Read each frame with a deliberately wrong row stride and safe wrapped or mirrored memory addresses. |
| **Block Address Corruption** | Replace held spatial blocks with deterministic addresses from elsewhere in space and nearby time. |
| **Optical Flow Time Warp** | Moving objects bend time more than the static background. |
| **Time Feedback** | Recursive past and future echoes with colour and displacement blend modes. |
| **Axis Datamosh** | Freeze and drag image data along time, horizontal or vertical lines. |
| **Seamless Loop** | Close a non-looping clip with a crossfade, a detail-driven luma weave or a guaranteed ping-pong pass. |

### Two videos

| Effect | What A and B do |
| --- | --- |
| **Space-Time Map** | A supplies the picture. B's red, green and blue channels choose where ChronoForge reads X, Y and Time from A. |
| **Space-Time Displacement** | A is the target. Brightness or a channel from B pushes A through X, Y and Time. |

## Five-minute workflow

1. Import a video or numbered PNG sequence. This becomes the primary source, **A**. Sequence import reports dimensions, numbering gaps and lets you choose FPS.
2. Add effects from the sidebar. They run from top to bottom.
3. Adjust **Amount** to blend any shape-compatible effect with its input, or press **Random Stack** to replace the chain with 1–3 compatible effects. One Undo restores the previous stack.
4. Click **Update Preview**, or leave **Auto Update** enabled. Preview is always a smaller proxy so experimentation stays responsive.
5. For a two-video effect, add another clip to Media and choose it as **Driver video (B)**.
6. Use **Export** for an H.264 MP4 or alpha-preserving PNG sequence. Choose Playback FPS to reinterpret the finished frames without resampling them. **Add to Queue** remains available for MP4 batches; press **Shift–Command–R** to start the queue.

Useful shortcuts: **Space** plays or pauses the preview, **Command–Z** / **Shift–Command–Z** undo and redo creative changes, **\\** temporarily shows Source A, and **Shift–S** hides or shows the sidebar.

## Why it is different

Most video tools treat time as a playhead. ChronoForge treats time like another direction you can swap, rotate, sort, map and displace. The result is closer to a playable glitch instrument than a conventional editor.

Large movies and image sequences are processed through temporary SSD files instead of being loaded into RAM all at once. Preview always uses proxy media; export always goes back to the original files. The render cache trims itself automatically.

The 1.0 development line groups effects by Time, Space, Signal, Memory, Data, Multi-Source and Output families. Its implementation plan is tracked in [docs/next-development-phase.md](docs/next-development-phase.md).

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

Create the app and DMG:

```bash
./scripts/package_macos.sh release
./scripts/create_dmg.sh
```

The processing core is C++20; the macOS interface is SwiftUI and AVFoundation. Full-resolution intermediate tensors are memory-mapped to SSD. More implementation detail lives in [docs/architecture.md](docs/architecture.md).
