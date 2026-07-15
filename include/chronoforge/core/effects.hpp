#pragma once

#include "chronoforge/core/video_tensor.hpp"

#include <cstddef>

namespace chronoforge {

enum class SpatialAxis { X, Y };
enum class TransposeResolution { NativeTensor, FitSourceCanvas };
enum class EdgeBehavior { Clamp, Wrap, Mirror };
enum class ShiftSource { Luma, Red, Green, Blue, Alpha };
enum class SortCriterion { Luma, Hue, Saturation };
enum class SortDirection { Ascending, Descending };
enum class FillMode { Black, Transparent, Repeat, Fit };
enum class RadialTopology { TimeLoom, KaleidoFold, EventHorizon };
enum class PrefilterStrength { Off, Light, Strong };
enum class TensorAxisSource { AX, AY, AT, BX, BY, BT };
enum class TensorInterpolation { Nearest, Linear, Cubic };
enum class TensorBroadcast { Clamp, Stretch, Crop };
enum class FeedbackBlendMode { Add, Screen, Multiply, Lighten };
enum class FreezeAxis { Time, Horizontal, Vertical };
enum class FreezeTrigger { Edge, Luma, Random };

struct LumaTimeShiftParams {
    float shift_multiplier{};
    ShiftSource source{ShiftSource::Luma};
    EdgeBehavior edge_behavior{EdgeBehavior::Clamp};
};

struct SpaceTimeTransposeParams {
    SpatialAxis axis{SpatialAxis::X};
    TransposeResolution resolution{TransposeResolution::NativeTensor};
};

struct RadialChronoFunnelParams {
    // Normalized canvas coordinates. (0, 0) is top-left, (1, 1) bottom-right.
    float center_x{0.5F};
    float center_y{0.5F};
    float intensity{};
    EdgeBehavior edge_behavior{EdgeBehavior::Wrap};
    float twist{0.75F};
    RadialTopology topology{RadialTopology::TimeLoom};
};

struct TemporalPixelSortParams {
    SortCriterion criterion{SortCriterion::Luma};
    SortDirection direction{SortDirection::Ascending};
    // Frames whose key is smaller than the threshold stay in their original slot.
    float threshold{0.0F};
};

struct TensorRotationParams {
    // Degrees in X-Y, X-T and Y-T planes respectively.
    float xy_degrees{};
    float xt_degrees{};
    float yt_degrees{};
    FillMode fill_mode{FillMode::Black};
};

struct SelectivePrefilterParams {
    PrefilterStrength spatial{PrefilterStrength::Off};
    PrefilterStrength temporal{PrefilterStrength::Off};
};

struct DimensionalSplicerParams {
    TensorAxisSource output_x{TensorAxisSource::AX};
    TensorAxisSource output_y{TensorAxisSource::AY};
    TensorAxisSource output_t{TensorAxisSource::BT};
    TensorInterpolation interpolation{TensorInterpolation::Linear};
};

struct TensorDisplacementParams {
    float time_multiplier{};
    float x_multiplier{};
    float y_multiplier{};
    ShiftSource source{ShiftSource::Luma};
    TensorBroadcast broadcast{TensorBroadcast::Stretch};
    EdgeBehavior edge_behavior{EdgeBehavior::Clamp};
};

struct OpticalFlowTimeWarpParams {
    float sensitivity{0.02F};
    float intensity{4.0F};
    float direction_degrees{};
    float direction_tolerance{180.0F};
    EdgeBehavior edge_behavior{EdgeBehavior::Clamp};
};

struct ChronoFeedbackParams {
    std::size_t past_delay{2};
    float past_amount{0.35F};
    std::size_t future_delay{2};
    float future_amount{0.15F};
    FeedbackBlendMode blend_mode{FeedbackBlendMode::Screen};
};

struct StructuralDatamoshParams {
    FreezeAxis axis{FreezeAxis::Time};
    FreezeTrigger trigger{FreezeTrigger::Edge};
    float threshold{0.2F};
    std::size_t max_hold{8};
    float probability{0.05F};
};

VideoTensor space_time_transpose(const VideoTensor& input, SpatialAxis axis);
VideoTensor space_time_transpose(const VideoTensor& input, const SpaceTimeTransposeParams& params);
VideoTensor luma_time_shift(const VideoTensor& input, const LumaTimeShiftParams& params);
VideoTensor radial_chrono_funnel(const VideoTensor& input, const RadialChronoFunnelParams& params);
VideoTensor temporal_pixel_sort(const VideoTensor& input, const TemporalPixelSortParams& params);
VideoTensor tensor_3d_rotation(const VideoTensor& input, const TensorRotationParams& params);
VideoTensor selective_prefilter(const VideoTensor& input, const SelectivePrefilterParams& params);
VideoTensor dimensional_splicer(
    const VideoTensor& source,
    const VideoTensor& driver,
    const DimensionalSplicerParams& params);
VideoTensor tensor_displacement(
    const VideoTensor& target,
    const VideoTensor& displacement,
    const TensorDisplacementParams& params);
VideoTensor optical_flow_time_warp(const VideoTensor& input, const OpticalFlowTimeWarpParams& params);
VideoTensor chrono_feedback(const VideoTensor& input, const ChronoFeedbackParams& params);
VideoTensor structural_datamosh(const VideoTensor& input, const StructuralDatamoshParams& params);

}  // namespace chronoforge
