#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace chronoforge {

// Phase-aware spatial FFT interpolation between two interleaved image frames.
// RGB is processed straight and returned premultiplied when an alpha channel is present.
std::vector<float> spectral_morph_frames(
    std::span<const float> tail,
    std::span<const float> head,
    std::size_t height,
    std::size_t width,
    std::size_t channels,
    float mix);

}  // namespace chronoforge
