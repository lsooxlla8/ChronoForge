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
    DimensionalSplicer = 7,
    TensorDisplacement = 8,
    OpticalFlowTimeWarp = 9,
    ChronoFeedback = 10,
    StructuralDatamosh = 11,
    SeamlessLoop = 12,
    RgbTimeSlip = 13,
};

struct EffectSpec {
    EffectOperation kind;
    std::array<float, 8> values{};
    std::array<std::int32_t, 8> options{};
    float amount{1.0F};
    std::uint64_t random_seed{};
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

FileRenderResult render_file_cross_tensor_effect(
    const std::filesystem::path& source_path,
    const std::filesystem::path& driver_path,
    const std::filesystem::path& output_path,
    TensorShape source_shape,
    TensorShape driver_shape,
    VideoTensorMetadata metadata,
    VideoTensorMetadata driver_metadata,
    const EffectSpec& effect,
    const FileRenderProgress& progress = {});

}  // namespace chronoforge
