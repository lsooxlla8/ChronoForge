#pragma once

#include "chronoforge/core/video_tensor.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string_view>

namespace chronoforge {

enum class EffectOperation : std::int32_t {
    SpaceTimeTranspose = 0,
    LumaTimeShift = 1,
    RadialChronoFunnel = 2,
    TemporalPixelSort = 3,
    Tensor3dRotation = 4,
    SpectralFftSwap = 5,
    SelectivePrefilter = 6,
};

struct EffectSpec {
    EffectOperation kind;
    std::array<float, 4> values{};
    std::array<std::int32_t, 4> options{};
};

struct FileRenderResult {
    TensorShape shape;
    VideoTensorMetadata metadata;
};

using FileRenderProgress = std::function<bool(double fraction, std::string_view stage)>;

FileRenderResult render_file_effect_chain(
    const std::filesystem::path& input_path,
    const std::filesystem::path& output_path,
    const std::filesystem::path& scratch_directory,
    TensorShape input_shape,
    VideoTensorMetadata metadata,
    std::span<const EffectSpec> effects,
    std::size_t max_working_set_bytes,
    const FileRenderProgress& progress = {});

}  // namespace chronoforge
