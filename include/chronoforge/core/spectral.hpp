#pragma once

#include "chronoforge/core/video_tensor.hpp"

#include <cstddef>

namespace chronoforge {

enum class SpectralSwapAxis { XTime, YTime, AllAxes };
enum class SpectralResolution { NativeTensor, FitSourceTensor };
enum class SpectralTransform { Swap, Rotate };

struct SpectralSwapParams {
    SpectralSwapAxis axis{SpectralSwapAxis::XTime};
    bool normalize{true};
    // The in-memory proxy reference is capped. Full renders use the disk-backed executor.
    std::size_t max_working_set_bytes{512ULL * 1024ULL * 1024ULL};
    SpectralResolution resolution{SpectralResolution::NativeTensor};
    SpectralTransform transform{SpectralTransform::Swap};
    float angle_degrees{0.0F};
};

// In-process CPU reference. Arbitrary dimensions are zero-padded to the next
// powers of two and cropped after the inverse transform. It fails before
// allocation if the padded complex working set exceeds the safety budget.
VideoTensor spectral_fft_swap(const VideoTensor& input, const SpectralSwapParams& params);

}  // namespace chronoforge
