#pragma once

#include "chronoforge/core/video_tensor.hpp"

#include <cstddef>

namespace chronoforge {

enum class SpectralSwapAxis { XTime, YTime, AllAxes };

struct SpectralSwapParams {
    SpectralSwapAxis axis{SpectralSwapAxis::XTime};
    bool normalize{true};
    // The CPU reference is intentionally capped. Production renders use Metal/cufft.
    std::size_t max_working_set_bytes{512ULL * 1024ULL * 1024ULL};
};

// In-process CPU reference. Arbitrary dimensions are zero-padded to the next
// powers of two and cropped after the inverse transform. It fails before
// allocation if the padded complex working set exceeds the safety budget.
VideoTensor spectral_fft_swap(const VideoTensor& input, const SpectralSwapParams& params);

}  // namespace chronoforge
