#pragma once

#include "chronoforge/core/video_tensor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace chronoforge {

enum class SpatialAxis { X, Y };
enum class TransposeResolution { NativeTensor, FitSourceCanvas };
enum class EdgeBehavior { Clamp, Wrap, Mirror };
enum class ShiftSource { Luma, Red, Green, Blue, Alpha };
enum class SortCriterion { Luma, Hue, Saturation };
enum class SortDirection { Ascending, Descending, Zigzag, CenterOut };
enum class FillMode { Black, Transparent, Repeat, Fit };
enum class RadialTopology { TimeLoom, KaleidoFold, EventHorizon };
enum class RadialSeamMode { Open, Periodic };
enum class PrefilterStrength { Off, Light, Strong };
enum class TensorAxisSource { AX, AY, AT, BX, BY, BT };
enum class TensorInterpolation { Nearest, Linear, Cubic };
enum class TensorBroadcast { Clamp, Stretch, Crop };
enum class FeedbackBlendMode { Add, Screen, Multiply, Lighten, Difference, Displace };
enum class FreezeAxis { Time, Horizontal, Vertical };
enum class FreezeTrigger { Edge, Luma, Random };
enum class SeamlessLoopMode { Crossfade, LumaWeave, PingPong, SpectralMorph, DifferenceWeave };
enum class LoopTransitionPlacement { Start, End };
enum class SpectralPhaseMode { Even, TailBiased, HeadBiased };
enum class SplitAxis { Horizontal, Vertical, Radial };
enum class SyncLossDriver { DeterministicNoise, Luma, Edges };
enum class SyncLossAxis { Horizontal, Vertical };
enum class ChromaDriftMode { Together, SplitCbCr, Alternating };
enum class StrideChannelMode { RgbTogether, SeparateChannels, AlphaIncluded };
enum class AddressEdge { Wrap, Mirror };
enum class BlockCorruptionMapping { Swap, Repeat, Offset, Cascade };
enum class BitplaneOperation { Shuffle, Rotate, Invert, Xor };
enum class BitplaneChannel { Luma, RgbTogether, Red, Green, Blue, Alpha };
enum class SignalWeavePattern { Lines, InterlacedFields, Bands, Checker };
enum class BlockGraftTrigger { Random, ALuma, BLuma, Difference, AEdges };
enum class ChannelTransplantColourModel { Rgb, YCbCr };

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
    // Rotates the polar coordinate system without rotating the output canvas.
    float rotation_degrees{};
    // Periodic removes the atan2 branch-cut discontinuity from time mapping.
    RadialSeamMode seam_mode{RadialSeamMode::Open};
};

struct TemporalPixelSortParams {
    SortCriterion criterion{SortCriterion::Luma};
    SortDirection direction{SortDirection::Ascending};
    // Frames whose key is smaller than the threshold stay in their original slot.
    float threshold{0.0F};
    // Rotates only the hue key used for ordering; output colour is unchanged.
    float hue_shift_degrees{};
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
    std::uint64_t random_seed{};
    bool invert_trigger{};
};

struct SeamlessLoopParams {
    std::size_t transition_frames{15};
    float weave_softness{0.12F};
    SeamlessLoopMode mode{SeamlessLoopMode::Crossfade};
    float spectral_amount{1.0F};
    float frequency_blur{};
    LoopTransitionPlacement placement{LoopTransitionPlacement::Start};
    SpectralPhaseMode phase_mode{SpectralPhaseMode::Even};
};

struct RGBTimeSlipParams {
    float red_offset{};
    float green_offset{};
    float blue_offset{};
    float spatial_split{};
    SplitAxis split_axis{SplitAxis::Horizontal};
    EdgeBehavior edge_behavior{EdgeBehavior::Clamp};
};

struct HorizontalSyncLossParams {
    float shift_fraction{0.2F};
    float band_size{0.08F};
    float drift_speed{};
    float tear_density{0.35F};
    SyncLossDriver driver{SyncLossDriver::DeterministicNoise};
    EdgeBehavior edge_behavior{EdgeBehavior::Wrap};
    std::uint64_t random_seed{};
    SyncLossAxis axis{SyncLossAxis::Horizontal};
};

struct ChromaCarrierDriftParams {
    float x_offset{};
    float y_offset{};
    float time_offset{};
    float bleed{};
    ChromaDriftMode mode{ChromaDriftMode::Together};
    EdgeBehavior edge_behavior{EdgeBehavior::Clamp};
};

struct StrideErrorParams {
    float stride_delta{};
    float base_offset{};
    float temporal_drift{};
    StrideChannelMode channel_mode{StrideChannelMode::RgbTogether};
    AddressEdge address_edge{AddressEdge::Wrap};
};

struct BlockAddressCorruptionParams {
    float block_size{0.1F};
    float corruption{0.3F};
    std::size_t time_reach{};
    std::size_t hold{1};
    BlockCorruptionMapping mapping{BlockCorruptionMapping::Swap};
    EdgeBehavior edge_behavior{EdgeBehavior::Clamp};
    std::uint64_t random_seed{};
};

struct BitplaneForgeParams {
    std::size_t working_bits{8};
    std::uint16_t plane_mask{0x00FF};
    int shift{};
    BitplaneOperation operation{BitplaneOperation::Invert};
    BitplaneChannel channel{BitplaneChannel::RgbTogether};
    std::uint64_t random_seed{};
};

struct SignalWeaveParams {
    SignalWeavePattern pattern{SignalWeavePattern::Bands};
    float band_size{0.05F};
    float phase_drift{};
    float irregularity{};
    int b_time_offset{};
    TensorBroadcast size_matching{TensorBroadcast::Clamp};
    std::uint64_t random_seed{};
};

struct BlockGraftParams {
    float block_size{0.1F};
    float density_or_threshold{0.35F};
    std::size_t hold{1};
    int b_time_offset{};
    BlockGraftTrigger trigger{BlockGraftTrigger::Random};
    TensorBroadcast size_matching{TensorBroadcast::Clamp};
    std::uint64_t random_seed{};
};

struct ChannelTransplantParams {
    std::array<bool, 3> source_from_b{false, false, false};
    int b_time_offset{};
    int b_spatial_offset_x{};
    int b_spatial_offset_y{};
    ChannelTransplantColourModel colour_model{ChannelTransplantColourModel::Rgb};
    TensorBroadcast size_matching{TensorBroadcast::Clamp};
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
VideoTensor seamless_loop(const VideoTensor& input, const SeamlessLoopParams& params);
VideoTensor rgb_time_slip(const VideoTensor& input, const RGBTimeSlipParams& params);
VideoTensor horizontal_sync_loss(const VideoTensor& input, const HorizontalSyncLossParams& params);
VideoTensor chroma_carrier_drift(const VideoTensor& input, const ChromaCarrierDriftParams& params);
VideoTensor stride_error(const VideoTensor& input, const StrideErrorParams& params);
VideoTensor block_address_corruption(const VideoTensor& input, const BlockAddressCorruptionParams& params);
VideoTensor bitplane_forge(const VideoTensor& input, const BitplaneForgeParams& params);
VideoTensor signal_weave(const VideoTensor& source, const VideoTensor& driver, const SignalWeaveParams& params);
VideoTensor block_graft(const VideoTensor& source, const VideoTensor& driver, const BlockGraftParams& params);
VideoTensor channel_transplant(const VideoTensor& source, const VideoTensor& driver, const ChannelTransplantParams& params);

}  // namespace chronoforge
