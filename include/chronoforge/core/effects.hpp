#pragma once

#include "chronoforge/core/video_tensor.hpp"

#include <cstddef>

namespace chronoforge {

enum class SpatialAxis { X, Y };
enum class EdgeBehavior { Clamp, Wrap, Mirror };
enum class ShiftSource { Luma, Red, Green, Blue, Alpha };
enum class SortCriterion { Luma, Hue, Saturation };
enum class SortDirection { Ascending, Descending };
enum class FillMode { Black, Transparent, Repeat };

struct LumaTimeShiftParams {
    float shift_multiplier{};
    ShiftSource source{ShiftSource::Luma};
    EdgeBehavior edge_behavior{EdgeBehavior::Clamp};
};

struct RadialChronoFunnelParams {
    // Normalized canvas coordinates. (0, 0) is top-left, (1, 1) bottom-right.
    float center_x{0.5F};
    float center_y{0.5F};
    float intensity{};
    EdgeBehavior edge_behavior{EdgeBehavior::Wrap};
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

VideoTensor space_time_transpose(const VideoTensor& input, SpatialAxis axis);
VideoTensor luma_time_shift(const VideoTensor& input, const LumaTimeShiftParams& params);
VideoTensor radial_chrono_funnel(const VideoTensor& input, const RadialChronoFunnelParams& params);
VideoTensor temporal_pixel_sort(const VideoTensor& input, const TemporalPixelSortParams& params);
VideoTensor tensor_3d_rotation(const VideoTensor& input, const TensorRotationParams& params);

}  // namespace chronoforge
