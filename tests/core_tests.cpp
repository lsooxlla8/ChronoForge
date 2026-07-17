#include "chronoforge/core/cache_store.hpp"
#include "chronoforge/core/effects.hpp"
#include "chronoforge/core/file_executor.hpp"
#include "chronoforge/core/mapped_tensor.hpp"
#include "chronoforge/core/node_graph.hpp"
#include "chronoforge/core/resource_planner.hpp"
#include "chronoforge/core/spectral.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_near(float actual, float expected, const std::string& message) {
    require(std::abs(actual - expected) < 0.0001F, message + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

chronoforge::VideoTensor numbered(chronoforge::TensorShape shape) {
    chronoforge::VideoTensor result(shape);
    for (std::size_t t = 0; t < shape.t; ++t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                for (std::size_t c = 0; c < shape.c; ++c) {
                    result.at(t, y, x, c) = static_cast<float>(100 * t + 10 * y + x + c);
                }
            }
        }
    }
    return result;
}

void test_transpose() {
    const chronoforge::VideoTensorMetadata metadata{24, 1, chronoforge::ColorTransfer::Rec709, chronoforge::AlphaRepresentation::Straight};
    auto input = numbered({2, 3, 4, 1});
    input = chronoforge::VideoTensor(input.shape(), input.values(), metadata);
    const auto output = chronoforge::space_time_transpose(input, chronoforge::SpatialAxis::X);
    require(output.shape() == chronoforge::TensorShape{4, 3, 2, 1}, "X/T transpose shape");
    require_near(output.at(3, 2, 1, 0), input.at(1, 2, 3, 0), "X/T transpose value");
    require(output.metadata() == metadata, "Effects preserve tensor colour and playback metadata");

    const auto fitted = chronoforge::space_time_transpose(
        input,
        {chronoforge::SpatialAxis::X, chronoforge::TransposeResolution::FitSourceCanvas});
    require(fitted.shape() == chronoforge::TensorShape{4, 3, 4, 1}, "Fitted X/T transpose uses the source canvas width");
    require_near(
        fitted.at(3, 2, 1, 0),
        input.at(0, 2, 3, 0) + (input.at(1, 2, 3, 0) - input.at(0, 2, 3, 0)) / 3.0F,
        "Fitted transpose interpolates along source time");
}

void test_time_shift_and_funnel() {
    chronoforge::VideoTensor input({4, 1, 2, 1});
    for (std::size_t t = 0; t < 4; ++t) {
        for (std::size_t x = 0; x < 2; ++x) {
            input.at(t, 0, x, 0) = static_cast<float>(t) / 3.0F;
        }
    }
    const auto shifted = chronoforge::luma_time_shift(input, {3.0F, chronoforge::ShiftSource::Luma, chronoforge::EdgeBehavior::Clamp});
    require_near(shifted.at(3, 0, 0, 0), input.at(0, 0, 0, 0), "Luma shift samples an earlier frame");

    const auto funnel = chronoforge::radial_chrono_funnel(
        input,
        {0.0F, 0.0F, 0.25F, chronoforge::EdgeBehavior::Wrap, 0.0F, chronoforge::RadialTopology::TimeLoom});
    require_near(funnel.at(3, 0, 1, 0), input.at(0, 0, 1, 0), "Radial funnel wraps time at an outer pixel");

    // Regression: fractional wrap can round a tiny negative coordinate to
    // exactly the extent on smaller proxy tensors. Every sampled coordinate
    // must remain within [0, extent), regardless of proxy dimensions.
    for (std::size_t frames = 2; frames <= 11; ++frames) {
        for (std::size_t height = 2; height <= 9; ++height) {
            chronoforge::VideoTensor proxy({frames, height, 13, 1}, 0.5F);
            const auto wrapped = chronoforge::radial_chrono_funnel(
                proxy,
                {0.5F, 0.5F, 0.08F, chronoforge::EdgeBehavior::Wrap, 0.75F,
                 chronoforge::RadialTopology::TimeLoom});
            require(wrapped.shape() == proxy.shape(), "Wrapped polar warp stays inside a small proxy tensor");
        }
    }
}

void test_rgb_time_slip() {
    chronoforge::VideoTensor input({3, 1, 1, 4}, 0.0F);
    input.at(0, 0, 0, 0) = 0.25F;
    input.at(0, 0, 0, 3) = 0.25F;
    input.at(1, 0, 0, 1) = 0.5F;
    input.at(1, 0, 0, 3) = 0.5F;
    input.at(2, 0, 0, 2) = 1.0F;
    input.at(2, 0, 0, 3) = 1.0F;
    const auto output = chronoforge::rgb_time_slip(
        input,
        {1.0F, 0.0F, -1.0F, 0.0F, chronoforge::SplitAxis::Horizontal, chronoforge::EdgeBehavior::Clamp});
    require_near(output.at(1, 0, 0, 0), 0.5F, "RGB Time Slip reads red from its independent frame and re-premultiplies it");
    require_near(output.at(1, 0, 0, 1), 0.5F, "RGB Time Slip keeps the stationary green channel");
    require_near(output.at(1, 0, 0, 2), 0.5F, "RGB Time Slip reads blue from its independent frame and re-premultiplies it");
    require_near(output.at(1, 0, 0, 3), 0.5F, "RGB Time Slip keeps alpha on the current frame");
}

void test_horizontal_sync_loss() {
    const auto input = numbered({3, 6, 12, 4});
    const chronoforge::HorizontalSyncLossParams params{
        0.75F, 0.25F, 0.5F, 1.0F, chronoforge::SyncLossDriver::DeterministicNoise,
        chronoforge::EdgeBehavior::Wrap, 0xC0FFEE};
    const auto first = chronoforge::horizontal_sync_loss(input, params);
    const auto second = chronoforge::horizontal_sync_loss(input, params);
    require(first.values() == second.values(), "Horizontal Sync Loss is deterministic for a stored seed");
    auto changed_params = params;
    changed_params.random_seed = 7;
    require(
        chronoforge::horizontal_sync_loss(input, changed_params).values() != first.values(),
        "Horizontal Sync Loss Reseed changes deterministic noise bands");
    auto dry_params = params;
    dry_params.tear_density = 0.0F;
    require(
        chronoforge::horizontal_sync_loss(input, dry_params).values() == input.values(),
        "Zero tear density is an exact visual identity before Amount blending");
    auto vertical_params = params;
    vertical_params.axis = chronoforge::SyncLossAxis::Vertical;
    require(
        chronoforge::horizontal_sync_loss(input, vertical_params).values() != first.values(),
        "Vertical Sync Loss shifts column bands on the orthogonal axis");
}

void test_chroma_carrier_drift() {
    chronoforge::VideoTensor grayscale({3, 1, 3, 4}, 0.0F);
    for (std::size_t t = 0; t < 3; ++t) {
        for (std::size_t x = 0; x < 3; ++x) {
            const auto alpha = 0.5F;
            const auto gray = 0.1F + 0.1F * static_cast<float>(t * 3 + x);
            for (std::size_t c = 0; c < 3; ++c) grayscale.at(t, 0, x, c) = gray * alpha;
            grayscale.at(t, 0, x, 3) = alpha;
        }
    }
    const auto neutral = chronoforge::chroma_carrier_drift(
        grayscale,
        {2.0F, 0.0F, 1.0F, 3.0F, chronoforge::ChromaDriftMode::SplitCbCr, chronoforge::EdgeBehavior::Wrap});
    for (std::size_t index = 0; index < grayscale.values().size(); ++index) {
        require_near(neutral.values()[index], grayscale.values()[index], "Chroma drift leaves neutral grayscale unchanged");
    }

    chronoforge::VideoTensor colour({2, 1, 1, 4}, 0.0F);
    colour.at(0, 0, 0, 0) = 0.25F;
    colour.at(0, 0, 0, 3) = 0.25F;
    colour.at(1, 0, 0, 2) = 0.75F;
    colour.at(1, 0, 0, 3) = 0.75F;
    const auto drifted = chronoforge::chroma_carrier_drift(
        colour,
        {0.0F, 0.0F, 1.0F, 0.0F, chronoforge::ChromaDriftMode::Together, chronoforge::EdgeBehavior::Wrap});
    require_near(drifted.at(0, 0, 0, 3), 0.25F, "Chroma drift keeps current-frame alpha");
    require(
        drifted.at(0, 0, 0, 0) <= 0.25F && drifted.at(0, 0, 0, 1) <= 0.25F && drifted.at(0, 0, 0, 2) <= 0.25F,
        "Chroma drift output remains premultiplied");
}

void test_stride_error() {
    const auto input = numbered({3, 4, 7, 4});
    const auto identity = chronoforge::stride_error(
        input, {0.0F, 0.0F, 0.0F, chronoforge::StrideChannelMode::SeparateChannels, chronoforge::AddressEdge::Mirror});
    require(identity.values() == input.values(), "Zero Stride Error parameters are identity in every channel mode");
    const chronoforge::StrideErrorParams params{
        -0.5F, 0.93F, -0.77F, chronoforge::StrideChannelMode::AlphaIncluded, chronoforge::AddressEdge::Mirror};
    const auto first = chronoforge::stride_error(input, params);
    const auto second = chronoforge::stride_error(input, params);
    require(first.shape() == input.shape() && first.values() == second.values(),
            "Stride Error safely resolves extreme deterministic addresses inside each frame");
}

void test_block_address_corruption() {
    const auto input = numbered({5, 7, 11, 4});
    const chronoforge::BlockAddressCorruptionParams params{
        0.3F, 0.8F, 2, 2, chronoforge::BlockCorruptionMapping::Cascade,
        chronoforge::EdgeBehavior::Mirror, 0xC0FFEE};
    const auto first = chronoforge::block_address_corruption(input, params);
    require(first.shape() == input.shape(), "Block Address Corruption preserves the tensor shape at partial edge blocks");
    require(first.values() == chronoforge::block_address_corruption(input, params).values(),
            "Block Address Corruption is deterministic for a stored seed");
    auto changed = params;
    changed.random_seed = 7;
    require(first.values() != chronoforge::block_address_corruption(input, changed).values(),
            "Block Address Corruption Reseed changes the address map");
    auto dry = params;
    dry.corruption = 0.0F;
    require(chronoforge::block_address_corruption(input, dry).values() == input.values(),
            "Zero corruption is exact identity even when Time Reach is nonzero");
}

void test_bitplane_forge() {
    chronoforge::VideoTensor impulse({1, 1, 1, 1}, 1.0F / 255.0F);
    const auto rotated = chronoforge::bitplane_forge(
        impulse, {8, 0x00FF, 1, chronoforge::BitplaneOperation::Rotate,
                  chronoforge::BitplaneChannel::Luma, 0});
    require_near(rotated.at(0, 0, 0, 0), 2.0F / 255.0F, "Bitplane Forge rotates the working integer planes");

    chronoforge::VideoTensor premultiplied({2, 2, 3, 4}, 0.0F);
    for (std::size_t t = 0; t < 2; ++t) for (std::size_t y = 0; y < 2; ++y) for (std::size_t x = 0; x < 3; ++x) {
        const auto alpha = 0.2F + 0.1F * static_cast<float>(t + y + x);
        premultiplied.at(t, y, x, 0) = 0.8F * alpha;
        premultiplied.at(t, y, x, 1) = 0.4F * alpha;
        premultiplied.at(t, y, x, 2) = 0.1F * alpha;
        premultiplied.at(t, y, x, 3) = alpha;
    }
    const chronoforge::BitplaneForgeParams xor_params{
        10, 0x03FF, 0, chronoforge::BitplaneOperation::Xor,
        chronoforge::BitplaneChannel::RgbTogether, 41};
    const auto first = chronoforge::bitplane_forge(premultiplied, xor_params);
    require(first.values() == chronoforge::bitplane_forge(premultiplied, xor_params).values(),
            "Bitplane XOR is deterministic for a stored seed");
    auto changed = xor_params;
    changed.random_seed = 42;
    require(first.values() != chronoforge::bitplane_forge(premultiplied, changed).values(),
            "Bitplane XOR responds to Reseed");
    for (std::size_t index = 0; index < first.values().size(); index += 4) {
        require(first.values()[index] <= first.values()[index + 3] &&
                first.values()[index + 1] <= first.values()[index + 3] &&
                first.values()[index + 2] <= first.values()[index + 3],
                "Bitplane colour operations preserve premultiplied alpha");
    }
}

void test_sort_and_rotation() {
    chronoforge::VideoTensor input({3, 1, 1, 1});
    input.at(0, 0, 0, 0) = 0.8F;
    input.at(1, 0, 0, 0) = 0.2F;
    input.at(2, 0, 0, 0) = 0.5F;
    const auto sorted = chronoforge::temporal_pixel_sort(input, {chronoforge::SortCriterion::Luma, chronoforge::SortDirection::Ascending, 0.4F});
    require_near(sorted.at(0, 0, 0, 0), 0.5F, "Sort moves eligible darkest sample first");
    require_near(sorted.at(1, 0, 0, 0), 0.2F, "Sort preserves threshold-excluded frame");
    require_near(sorted.at(2, 0, 0, 0), 0.8F, "Sort moves eligible brightest sample last");

    chronoforge::VideoTensor four({4, 1, 1, 1});
    four.at(0, 0, 0, 0) = 0.1F;
    four.at(1, 0, 0, 0) = 0.2F;
    four.at(2, 0, 0, 0) = 0.3F;
    four.at(3, 0, 0, 0) = 0.4F;
    const auto zigzag = chronoforge::temporal_pixel_sort(
        four, {chronoforge::SortCriterion::Luma, chronoforge::SortDirection::Zigzag, 0.0F, 0.0F});
    require_near(zigzag.at(0, 0, 0, 0), 0.1F, "Zigzag begins with the lowest eligible key");
    require_near(zigzag.at(1, 0, 0, 0), 0.4F, "Zigzag alternates to the highest eligible key");
    const auto center_out = chronoforge::temporal_pixel_sort(
        four, {chronoforge::SortCriterion::Luma, chronoforge::SortDirection::CenterOut, 0.0F, 0.0F});
    require_near(center_out.at(0, 0, 0, 0), 0.2F, "Center Out begins at the lower median");
    require_near(center_out.at(1, 0, 0, 0), 0.3F, "Center Out expands toward the upper median");

    chronoforge::VideoTensor hues({3, 1, 1, 3});
    hues.at(0, 0, 0, 0) = 1.0F;
    hues.at(1, 0, 0, 1) = 1.0F;
    hues.at(2, 0, 0, 2) = 1.0F;
    const auto hue_normal = chronoforge::temporal_pixel_sort(
        hues, {chronoforge::SortCriterion::Hue, chronoforge::SortDirection::Ascending, 0.0F, 0.0F});
    const auto hue_shifted = chronoforge::temporal_pixel_sort(
        hues, {chronoforge::SortCriterion::Hue, chronoforge::SortDirection::Ascending, 0.0F, 140.0F});
    require(hue_shifted.values() != hue_normal.values(), "Hue Key Shift changes ordering without altering source colour values");

    const auto rotation = chronoforge::tensor_3d_rotation(input, {});
    require(rotation.values() == input.values(), "Zero 3D rotation is identity");

    chronoforge::VideoTensor canvas({2, 3, 3, 1}, 1.0F);
    const auto black_rotation = chronoforge::tensor_3d_rotation(
        canvas,
        {45.0F, 0, 0, chronoforge::FillMode::Black});
    const auto fit_rotation = chronoforge::tensor_3d_rotation(
        canvas,
        {45.0F, 0, 0, chronoforge::FillMode::Fit});
    require_near(black_rotation.at(0, 0, 0, 0), 0.0F, "Black rotation exposes an empty corner");
    require_near(fit_rotation.at(0, 0, 0, 0), 1.0F, "Fit rotation zooms the tensor to cover the frame");

    chronoforge::VideoTensor impulse({3, 3, 3, 1}, 0.0F);
    impulse.at(1, 1, 1, 0) = 1.0F;
    const auto spatial_filter = chronoforge::selective_prefilter(
        impulse, {chronoforge::PrefilterStrength::Strong, chronoforge::PrefilterStrength::Off});
    require_near(spatial_filter.at(1, 1, 1, 0), 0.6F, "Strong spatial prefilter applies an efficient axial low-pass");
    require_near(spatial_filter.at(0, 1, 1, 0), 0.0F, "Spatial prefilter does not blend time");
    const auto temporal_filter = chronoforge::selective_prefilter(
        impulse, {chronoforge::PrefilterStrength::Off, chronoforge::PrefilterStrength::Strong});
    require_near(temporal_filter.at(1, 1, 1, 0), 0.6F, "Strong temporal prefilter weights the current frame");
    require_near(temporal_filter.at(0, 1, 1, 0), 0.2F, "Temporal prefilter blends adjacent frames");
}

void test_fft_swap() {
    const auto input = numbered({3, 5, 6, 1});
    const auto output = chronoforge::spectral_fft_swap(input, {chronoforge::SpectralSwapAxis::XTime, false, 1024 * 1024});
    const auto expected = chronoforge::space_time_transpose(input, chronoforge::SpatialAxis::X);
    require(output.shape() == expected.shape(), "FFT swap has transposed shape");
    for (std::size_t i = 0; i < output.values().size(); ++i) {
        require_near(output.values()[i], expected.values()[i], "FFT swap matches axis permutation for distinct extents");
    }
    const auto fitted = chronoforge::spectral_fft_swap(
        input,
        {chronoforge::SpectralSwapAxis::XTime, false, 1024 * 1024, chronoforge::SpectralResolution::FitSourceTensor});
    require(fitted.shape() == input.shape(), "Fitted FFT preserves the complete input tensor shape");

    const auto zero_rotation = chronoforge::spectral_fft_swap(
        input,
        {chronoforge::SpectralSwapAxis::XTime, false, 1024 * 1024,
         chronoforge::SpectralResolution::FitSourceTensor, chronoforge::SpectralTransform::Rotate, 0.0F});
    require(zero_rotation.shape() == input.shape(), "Zero-degree spectral rotation preserves shape");
    for (std::size_t i = 0; i < input.values().size(); ++i) {
        require_near(zero_rotation.values()[i], input.values()[i], "Zero-degree spectral rotation is identity");
    }
    const auto rotated = chronoforge::spectral_fft_swap(
        input,
        {chronoforge::SpectralSwapAxis::XTime, false, 1024 * 1024,
         chronoforge::SpectralResolution::FitSourceTensor, chronoforge::SpectralTransform::Rotate, 27.0F});
    require(rotated.shape() == input.shape(), "Angled spectral rotation keeps the source tensor shape");
    require(rotated.values() != input.values(), "Angled spectral rotation changes frequency geometry");
}

void test_cross_tensor_and_flow_effects() {
    const auto source = numbered({2, 2, 3, 1});
    chronoforge::VideoTensor driver(
        {4, 1, 5, 1}, std::vector<float>(20, 1.0F),
        {8, 1, chronoforge::ColorTransfer::Linear, chronoforge::AlphaRepresentation::None});
    const auto spliced = chronoforge::dimensional_splicer(source, driver, {});
    require(spliced.shape() == chronoforge::TensorShape{4, 2, 3, 1}, "Splicer takes Time extent from driver B");
    require_near(spliced.at(3, 1, 2, 0), source.at(1, 1, 2, 0), "Splicer normalizes B geometry into A pixels");
    require(spliced.metadata().frame_rate_numerator == 8, "Splicer inherits driver FPS when output Time comes from B Time");

    chronoforge::VideoTensor coordinate_map({2, 2, 3, 3}, 0.0F);
    const auto mapped = chronoforge::dimensional_splicer(
        source, coordinate_map,
        {chronoforge::TensorAxisSource::BX, chronoforge::TensorAxisSource::BY,
         chronoforge::TensorAxisSource::BT, chronoforge::TensorInterpolation::Nearest});
    require(mapped.shape() == chronoforge::TensorShape{2, 2, 3, 1}, "Space-Time Map can take every output extent from B");
    require_near(mapped.at(1, 1, 2, 0), source.at(0, 0, 0, 0), "B RGB values map A's X, Y and Time coordinates");
    const auto repaired_axes = chronoforge::dimensional_splicer(
        source, coordinate_map,
        {chronoforge::TensorAxisSource::AX, chronoforge::TensorAxisSource::BX,
         chronoforge::TensorAxisSource::BT, chronoforge::TensorInterpolation::Nearest});
    require(repaired_axes.shape() == chronoforge::TensorShape{2, 2, 3, 1}, "Space-Time Map repairs duplicate axis semantics without throwing");

    chronoforge::VideoTensor timeline({3, 1, 1, 1});
    timeline.at(0, 0, 0, 0) = 0.0F;
    timeline.at(1, 0, 0, 0) = 0.5F;
    timeline.at(2, 0, 0, 0) = 1.0F;
    chronoforge::VideoTensor map({1, 1, 1, 1}, 1.0F);
    const auto displaced = chronoforge::tensor_displacement(
        timeline, map,
        {1.0F, 0, 0, chronoforge::ShiftSource::Luma, chronoforge::TensorBroadcast::Stretch,
         chronoforge::EdgeBehavior::Clamp});
    require_near(displaced.at(0, 0, 0, 0), 0.5F, "Tensor displacement uses B to move A through time");

    chronoforge::VideoTensor weave_a({2, 4, 3, 1}, 0.0F);
    chronoforge::VideoTensor weave_b({2, 4, 3, 1}, 1.0F);
    const auto woven = chronoforge::signal_weave(
        weave_a, weave_b,
        {chronoforge::SignalWeavePattern::Lines, 0, 0, 0, 0,
         chronoforge::TensorBroadcast::Clamp, 9});
    require_near(woven.at(0, 0, 0, 0), 0.0F, "Signal Weave keeps even lines from A");
    require_near(woven.at(0, 1, 0, 0), 1.0F, "Signal Weave takes odd lines from B");
    const auto irregular_a = chronoforge::signal_weave(
        weave_a, weave_b,
        {chronoforge::SignalWeavePattern::Checker, 0.2F, 0, 1, 0,
         chronoforge::TensorBroadcast::Clamp, 10});
    const auto irregular_b = chronoforge::signal_weave(
        weave_a, weave_b,
        {chronoforge::SignalWeavePattern::Checker, 0.2F, 0, 1, 0,
         chronoforge::TensorBroadcast::Clamp, 11});
    require(irregular_a.values() != irregular_b.values(), "Signal Weave irregularity responds to Reseed");
    const auto grafted = chronoforge::block_graft(
        weave_a, weave_b,
        {0.3F, 0.5F, 2, 0, chronoforge::BlockGraftTrigger::Difference,
         chronoforge::TensorBroadcast::Clamp, 0});
    require(grafted.values() == weave_b.values(), "Block Graft replaces complete B blocks when Difference crosses threshold");
    chronoforge::VideoTensor transplant_a({1, 1, 1, 4}, 0.0F);
    transplant_a.at(0, 0, 0, 0) = 0.5F;
    transplant_a.at(0, 0, 0, 3) = 0.5F;
    chronoforge::VideoTensor transplant_b({1, 1, 1, 4}, 0.0F);
    transplant_b.at(0, 0, 0, 1) = 1.0F;
    transplant_b.at(0, 0, 0, 2) = 1.0F;
    transplant_b.at(0, 0, 0, 3) = 1.0F;
    const auto transplanted = chronoforge::channel_transplant(
        transplant_a, transplant_b,
        {{false, true, false}, 0, 0, 0, chronoforge::ChannelTransplantColourModel::Rgb,
         chronoforge::TensorBroadcast::Clamp});
    require_near(transplanted.at(0, 0, 0, 0), 0.5F, "Channel Transplant keeps selected A components");
    require_near(transplanted.at(0, 0, 0, 1), 0.5F, "Channel Transplant re-premultiplies a B component to A alpha");
    require_near(transplanted.at(0, 0, 0, 3), 0.5F, "Channel Transplant preserves A alpha");

    chronoforge::VideoTensor still({3, 3, 3, 1}, 0.4F);
    const auto flow = chronoforge::optical_flow_time_warp(still, {});
    require(flow.values() == still.values(), "Motion time warp preserves a static tensor");

    chronoforge::VideoTensor pulse({3, 1, 1, 1});
    pulse.at(1, 0, 0, 0) = 1.0F;
    const auto feedback = chronoforge::chrono_feedback(
        pulse, {1, 0.5F, 0, 0, chronoforge::FeedbackBlendMode::Add});
    require_near(feedback.at(1, 0, 0, 0), 0.5F, "Feedback blends a recursive past sample");
    require_near(feedback.at(2, 0, 0, 0), 0.25F, "Feedback recursively decays through later frames");

    chronoforge::VideoTensor layers({2, 1, 1, 1});
    layers.at(0, 0, 0, 0) = 0.2F;
    layers.at(1, 0, 0, 0) = 0.7F;
    const auto difference = chronoforge::chrono_feedback(
        layers, {0, 0, 1, 1.0F, chronoforge::FeedbackBlendMode::Difference});
    require_near(difference.at(0, 0, 0, 0), 0.5F, "Difference feedback blends the absolute channel difference");

    chronoforge::VideoTensor displacement_layers({2, 3, 1, 1});
    for (std::size_t y = 0; y < 3; ++y) displacement_layers.at(0, y, 0, 0) = static_cast<float>(y) / 2.0F;
    for (std::size_t y = 0; y < 3; ++y) displacement_layers.at(1, y, 0, 0) = 1.0F;
    const auto displaced_feedback = chronoforge::chrono_feedback(
        displacement_layers, {1, 0, 1, 1.0F, chronoforge::FeedbackBlendMode::Displace});
    require(displaced_feedback.at(0, 0, 0, 0) > 0.0F, "Displace feedback uses the future layer to move the current frame vertically");

    chronoforge::VideoTensor edge({1, 1, 3, 1});
    edge.at(0, 0, 0, 0) = 0.0F;
    edge.at(0, 0, 1, 0) = 0.6F;
    edge.at(0, 0, 2, 0) = 1.0F;
    const auto datamosh = chronoforge::structural_datamosh(
        edge, {chronoforge::FreezeAxis::Horizontal, chronoforge::FreezeTrigger::Edge, 0.2F, 2, 0});
    require_near(datamosh.at(0, 0, 2, 0), 0.0F, "Structural datamosh holds an edge along the selected axis");
    const auto dark_datamosh = chronoforge::structural_datamosh(
        edge, {chronoforge::FreezeAxis::Horizontal, chronoforge::FreezeTrigger::Luma, 0.7F, 1, 0, 0, true});
    require_near(dark_datamosh.at(0, 0, 1, 0), 0.0F, "Inverted luma trigger freezes from darker values");

    const auto loop_source = numbered({8, 1, 1, 1});
    const auto crossfade_loop = chronoforge::seamless_loop(
        loop_source, {3, 0.12F, chronoforge::SeamlessLoopMode::Crossfade});
    require(crossfade_loop.shape() == chronoforge::TensorShape{5, 1, 1, 1}, "Crossfade loop removes the overlapped tail frames");
    require_near(crossfade_loop.at(0, 0, 0, 0), loop_source.at(5, 0, 0, 0), "Loop boundary continues into the source tail without a cut");
    require_near(crossfade_loop.at(2, 0, 0, 0), loop_source.at(2, 0, 0, 0), "Crossfade finishes on the original beginning");
    const auto woven_loop = chronoforge::seamless_loop(
        loop_source, {3, 0.15F, chronoforge::SeamlessLoopMode::LumaWeave});
    require(woven_loop.shape() == crossfade_loop.shape(), "Luma Weave uses the same loop duration as Crossfade");
    require_near(woven_loop.at(0, 0, 0, 0), loop_source.at(5, 0, 0, 0), "Luma Weave preserves the loop-side boundary frame");
    const auto ping_pong = chronoforge::seamless_loop(
        loop_source, {3, 0.12F, chronoforge::SeamlessLoopMode::PingPong});
    require(ping_pong.shape() == chronoforge::TensorShape{14, 1, 1, 1}, "Ping-Pong loop appends the reverse pass without duplicate endpoints");
    require_near(ping_pong.at(13, 0, 0, 0), loop_source.at(1, 0, 0, 0), "Ping-Pong ends one frame before its loop start");
}

void test_file_backed_cross_tensor() {
    const auto root = std::filesystem::temp_directory_path() / "chronoforge-cross-tensor-file-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto source = numbered({2, 2, 3, 1});
    const chronoforge::VideoTensor driver(
        {4, 1, 5, 3}, std::vector<float>(60, 1.0F),
        {8, 1, chronoforge::ColorTransfer::Linear, chronoforge::AlphaRepresentation::None});
    const auto source_path = root / "source.raw";
    const auto driver_path = root / "driver.raw";
    {
        auto mapped = chronoforge::MappedTensor::create(source_path, source.shape());
        std::copy(source.values().begin(), source.values().end(), mapped.mutable_data());
        mapped.sync();
    }
    {
        auto mapped = chronoforge::MappedTensor::create(driver_path, driver.shape());
        std::copy(driver.values().begin(), driver.values().end(), mapped.mutable_data());
        mapped.sync();
    }
    const chronoforge::EffectSpec effect{
        chronoforge::EffectOperation::DimensionalSplicer, {}, {3, 4, 5, 0}};
    const auto output_path = root / "output.raw";
    const auto result = chronoforge::render_file_cross_tensor_effect(
        source_path, driver_path, output_path, source.shape(), driver.shape(), source.metadata(), driver.metadata(), effect, {});
    require(result.shape == chronoforge::TensorShape{4, 1, 5, 1}, "Out-of-core Space-Time Map reports B-driven extents");
    require(result.metadata.frame_rate_numerator == 8, "Out-of-core Splicer reports driver FPS for B Time");
    const auto output = chronoforge::MappedTensor::open(output_path, result.shape, chronoforge::MappedTensor::Access::ReadOnly);
    require_near(output.data()[((3 * 1 + 0) * 5 + 4)], source.at(1, 1, 2, 0), "Out-of-core Space-Time Map uses B RGB coordinates");
    const auto weave_path = root / "weave.raw";
    const chronoforge::EffectSpec weave_effect{
        chronoforge::EffectOperation::SignalWeave, {0.2F, 0.5F, 0.35F, 1}, {3, 2}, 1.0F, 37};
    const auto weave_result = chronoforge::render_file_cross_tensor_effect(
        source_path, driver_path, weave_path, source.shape(), driver.shape(),
        source.metadata(), driver.metadata(), weave_effect, {});
    const auto weave_expected = chronoforge::signal_weave(
        source, driver,
        {chronoforge::SignalWeavePattern::Checker, 0.2F, 0.5F, 0.35F, 1,
         chronoforge::TensorBroadcast::Crop, 37});
    require(weave_result.shape == weave_expected.shape(), "Out-of-core Signal Weave reports cropped A/B extents");
    const auto weave_output = chronoforge::MappedTensor::open(weave_path, weave_result.shape, chronoforge::MappedTensor::Access::ReadOnly);
    for (std::size_t index = 0; index < weave_expected.values().size(); ++index) {
        require_near(weave_output.data()[index], weave_expected.values()[index], "Out-of-core Signal Weave matches RAM reference");
    }
    const auto graft_path = root / "graft.raw";
    const chronoforge::EffectSpec graft_effect{
        chronoforge::EffectOperation::BlockGraft, {0.3F, 0.4F, 2, -1}, {3, 1}, 1.0F, 71};
    const auto graft_result = chronoforge::render_file_cross_tensor_effect(
        source_path, driver_path, graft_path, source.shape(), driver.shape(),
        source.metadata(), driver.metadata(), graft_effect, {});
    const auto graft_expected = chronoforge::block_graft(
        source, driver,
        {0.3F, 0.4F, 2, -1, chronoforge::BlockGraftTrigger::Difference,
         chronoforge::TensorBroadcast::Stretch, 71});
    const auto graft_output = chronoforge::MappedTensor::open(graft_path, graft_result.shape, chronoforge::MappedTensor::Access::ReadOnly);
    for (std::size_t index = 0; index < graft_expected.values().size(); ++index) {
        require_near(graft_output.data()[index], graft_expected.values()[index], "Out-of-core Block Graft matches RAM reference");
    }
    const auto transplant_path = root / "transplant.raw";
    const chronoforge::EffectSpec transplant_effect{
        chronoforge::EffectOperation::ChannelTransplant, {1, 1, 0}, {1, 0, 1, 0, 1}};
    const auto transplant_result = chronoforge::render_file_cross_tensor_effect(
        source_path, driver_path, transplant_path, source.shape(), driver.shape(),
        source.metadata(), driver.metadata(), transplant_effect, {});
    const auto transplant_expected = chronoforge::channel_transplant(
        source, driver,
        {{true, false, true}, 1, 1, 0, chronoforge::ChannelTransplantColourModel::Rgb,
         chronoforge::TensorBroadcast::Stretch});
    const auto transplant_output = chronoforge::MappedTensor::open(
        transplant_path, transplant_result.shape, chronoforge::MappedTensor::Access::ReadOnly);
    for (std::size_t index = 0; index < transplant_expected.values().size(); ++index) {
        require_near(transplant_output.data()[index], transplant_expected.values()[index], "Out-of-core Channel Transplant matches RAM reference");
    }
    std::filesystem::remove_all(root);
}

void test_cache_and_graph() {
    const auto root = std::filesystem::temp_directory_path() / "chronoforge-core-test-cache";
    std::filesystem::remove_all(root);
    chronoforge::DiskCacheStore cache(root);
    const chronoforge::ChunkExtent extent{0, 2, 3, 4, 5, 7};
    const std::vector<float> values{1.0F, 2.0F, 3.0F};
    cache.write("0123abcd", extent, values);
    const auto hit = cache.read("0123abcd", extent);
    require(hit.has_value() && hit->values == values, "Disk cache returns its atomic write");
    std::filesystem::remove_all(root);

    chronoforge::NodeGraph graph;
    graph.add_node({"input", "Input", chronoforge::NodeKind::Input});
    graph.add_node({"shift", "Shift", chronoforge::NodeKind::LumaTimeShift});
    graph.add_node({"output", "Output", chronoforge::NodeKind::Output});
    graph.connect("input", "shift");
    graph.connect("shift", "output");
    require(graph.topological_order().size() == 3, "Graph topologically orders a chain");
    bool cycle_rejected = false;
    try {
        graph.connect("output", "input");
    } catch (const std::invalid_argument&) {
        cycle_rejected = true;
    }
    require(cycle_rejected, "Graph rejects a dependency cycle");
}

void test_tiling() {
    const auto tiles = chronoforge::TilePlanner::temporal_tiles({4, 10, 10, 1}, {4 * 4 * 10, 1024}, {});
    require(!tiles.empty(), "Temporal tile planner creates tiles");
    require(tiles.front().t_begin == 0 && tiles.front().t_end == 4, "Temporal tiles keep full time vectors");
}

void test_file_backed_effect_chain() {
    const auto root = std::filesystem::temp_directory_path() / "chronoforge-file-executor-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto input_path = root / "input.raw";
    const auto output_path = root / "output.raw";
    const auto input = numbered({3, 2, 4, 4});
    {
        auto mapped = chronoforge::MappedTensor::create(input_path, input.shape());
        std::copy(input.values().begin(), input.values().end(), mapped.mutable_data());
        mapped.sync();
    }

    const std::vector<chronoforge::EffectSpec> effects{
        {chronoforge::EffectOperation::LumaTimeShift, {2.0F, 0, 0, 0}, {0, 1, 0, 0}},
        {chronoforge::EffectOperation::RgbTimeSlip, {1.0F, 0.0F, -1.0F, 1.0F}, {0, 1}},
        {chronoforge::EffectOperation::HorizontalSyncLoss, {0.4F, 0.25F, 0.5F, 0.75F}, {0, 1, 0}, 1.0F, 0xC0FFEE},
        {chronoforge::EffectOperation::ChromaCarrierDrift, {1.0F, 0.0F, 1.0F, 2.0F}, {1, 1}},
        {chronoforge::EffectOperation::StrideError, {0.1F, 0.07F, 0.013F}, {1, 1}},
        {chronoforge::EffectOperation::BlockAddressCorruption, {0.3F, 0.75F, 2.0F, 2.0F}, {3, 2}, 1.0F, 0xC0FFEE},
        {chronoforge::EffectOperation::BitplaneForge, {8.0F, 255.0F, 1.0F}, {3, 1}, 1.0F, 0xC0FFEE},
        {chronoforge::EffectOperation::RadialChronoFunnel, {0.5F, 0.5F, 0.2F, 0}, {2, 0, 0, 0}},
        {chronoforge::EffectOperation::TemporalPixelSort, {0.1F, 0, 0, 0}, {0, 0, 0, 0}},
        {chronoforge::EffectOperation::Tensor3dRotation, {5.0F, 10.0F, 0, 0}, {3, 0, 0, 0}},
        {chronoforge::EffectOperation::SpaceTimeTranspose, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {chronoforge::EffectOperation::SpectralFftSwap, {0, 0, 0, 0}, {1, 0, 0, 0}},
        {chronoforge::EffectOperation::SelectivePrefilter, {0, 0, 0, 0}, {1, 1, 0, 0}},
    };
    std::size_t progress_calls = 0;
    double last_progress = 0.0;
    const auto result = chronoforge::render_file_effect_chain(
        input_path,
        output_path,
        root / "scratch",
        input.shape(),
        input.metadata(),
        effects,
        64 * 1024 * 1024,
        [&](double fraction, std::string_view) {
            ++progress_calls;
            require(fraction >= last_progress, "File executor progress is monotonic");
            last_progress = fraction;
            return true;
        });

    auto expected = chronoforge::luma_time_shift(input, {2.0F, chronoforge::ShiftSource::Luma, chronoforge::EdgeBehavior::Wrap});
    expected = chronoforge::rgb_time_slip(
        expected, {1.0F, 0.0F, -1.0F, 1.0F, chronoforge::SplitAxis::Horizontal, chronoforge::EdgeBehavior::Wrap});
    expected = chronoforge::horizontal_sync_loss(
        expected, {0.4F, 0.25F, 0.5F, 0.75F, chronoforge::SyncLossDriver::DeterministicNoise,
                   chronoforge::EdgeBehavior::Wrap, 0xC0FFEE});
    expected = chronoforge::chroma_carrier_drift(
        expected, {1.0F, 0.0F, 1.0F, 2.0F, chronoforge::ChromaDriftMode::SplitCbCr,
                   chronoforge::EdgeBehavior::Wrap});
    expected = chronoforge::stride_error(
        expected, {0.1F, 0.07F, 0.013F, chronoforge::StrideChannelMode::SeparateChannels,
                   chronoforge::AddressEdge::Mirror});
    expected = chronoforge::block_address_corruption(
        expected, {0.3F, 0.75F, 2, 2, chronoforge::BlockCorruptionMapping::Cascade,
                   chronoforge::EdgeBehavior::Mirror, 0xC0FFEE});
    expected = chronoforge::bitplane_forge(
        expected, {8, 255, 1, chronoforge::BitplaneOperation::Xor,
                   chronoforge::BitplaneChannel::RgbTogether, 0xC0FFEE});
    expected = chronoforge::radial_chrono_funnel(
        expected,
        {0.5F, 0.5F, 0.2F, chronoforge::EdgeBehavior::Mirror, 0.0F, chronoforge::RadialTopology::TimeLoom});
    expected = chronoforge::temporal_pixel_sort(expected, {chronoforge::SortCriterion::Luma, chronoforge::SortDirection::Ascending, 0.1F});
    expected = chronoforge::tensor_3d_rotation(expected, {5.0F, 10.0F, 0, chronoforge::FillMode::Fit});
    expected = chronoforge::space_time_transpose(expected, chronoforge::SpatialAxis::X);
    expected = chronoforge::spectral_fft_swap(expected, {chronoforge::SpectralSwapAxis::YTime, false, 64 * 1024 * 1024});
    expected = chronoforge::selective_prefilter(
        expected, {chronoforge::PrefilterStrength::Light, chronoforge::PrefilterStrength::Light});

    require(result.shape == expected.shape(), "File executor reports the final transposed shape");
    require(progress_calls >= effects.size() + 2 && last_progress == 1.0, "File executor reports granular progress through completion");
    const auto mapped_output = chronoforge::MappedTensor::open(output_path, result.shape, chronoforge::MappedTensor::Access::ReadOnly);
    for (std::size_t i = 0; i < expected.values().size(); ++i) {
        require_near(mapped_output.data()[i], expected.values()[i], "File-backed chain matches in-memory reference");
    }

    const auto fitted_path = root / "fitted.raw";
    const std::vector<chronoforge::EffectSpec> fitted_effects{
        {chronoforge::EffectOperation::SpaceTimeTranspose, {0, 0, 0, 0}, {0, 1, 0, 0}},
    };
    const auto fitted_result = chronoforge::render_file_effect_chain(
        input_path,
        fitted_path,
        root / "fit-scratch",
        input.shape(),
        input.metadata(),
        fitted_effects,
        64 * 1024 * 1024,
        {});
    const auto fitted_expected = chronoforge::space_time_transpose(
        input,
        {chronoforge::SpatialAxis::X, chronoforge::TransposeResolution::FitSourceCanvas});
    require(fitted_result.shape == fitted_expected.shape(), "File-backed fitted transpose reports the source canvas shape");
    const auto fitted_output = chronoforge::MappedTensor::open(
        fitted_path,
        fitted_result.shape,
        chronoforge::MappedTensor::Access::ReadOnly);
    for (std::size_t i = 0; i < fitted_expected.values().size(); ++i) {
        require_near(fitted_output.data()[i], fitted_expected.values()[i], "File-backed fitted transpose matches preview");
    }

    const auto fft_path = root / "fft-fit.raw";
    const std::vector<chronoforge::EffectSpec> fft_effects{
        {chronoforge::EffectOperation::SpectralFftSwap, {0, 0, 0, 0}, {0, 0, 1, 0}},
    };
    const auto fft_result = chronoforge::render_file_effect_chain(
        input_path,
        fft_path,
        root / "fft-scratch",
        input.shape(),
        input.metadata(),
        fft_effects,
        1024,
        {});
    const auto fft_expected = chronoforge::spectral_fft_swap(
        input,
        {chronoforge::SpectralSwapAxis::XTime, false, 64 * 1024 * 1024, chronoforge::SpectralResolution::FitSourceTensor});
    require(fft_result.shape == input.shape(), "Out-of-core fitted FFT keeps the source tensor shape");
    const auto fft_output = chronoforge::MappedTensor::open(fft_path, fft_result.shape, chronoforge::MappedTensor::Access::ReadOnly);
    for (std::size_t i = 0; i < fft_expected.values().size(); ++i) {
        require_near(fft_output.data()[i], fft_expected.values()[i], "Out-of-core FFT matches the in-memory reference with a tiny RAM budget");
    }

    const auto fft_rotate_path = root / "fft-rotate.raw";
    const std::vector<chronoforge::EffectSpec> fft_rotate_effects{
        {chronoforge::EffectOperation::SpectralFftSwap, {0, 0, 0, 0}, {0, 0, 1, 1}},
    };
    const auto fft_rotate_result = chronoforge::render_file_effect_chain(
        input_path,
        fft_rotate_path,
        root / "fft-rotate-scratch",
        input.shape(),
        input.metadata(),
        fft_rotate_effects,
        1024,
        {});
    require(fft_rotate_result.shape == input.shape(), "Out-of-core spectral rotation keeps the source shape");
    const auto fft_rotate_output = chronoforge::MappedTensor::open(
        fft_rotate_path, fft_rotate_result.shape, chronoforge::MappedTensor::Access::ReadOnly);
    for (std::size_t i = 0; i < input.values().size(); ++i) {
        require_near(fft_rotate_output.data()[i], input.values()[i], "Zero-degree out-of-core spectral rotation is identity");
    }

    const auto loop_input = numbered({8, 2, 2, 1});
    const auto loop_input_path = root / "loop-input.raw";
    {
        auto mapped = chronoforge::MappedTensor::create(loop_input_path, loop_input.shape());
        std::copy(loop_input.values().begin(), loop_input.values().end(), mapped.mutable_data());
        mapped.sync();
    }
    const auto loop_path = root / "loop.raw";
    const std::vector<chronoforge::EffectSpec> loop_effects{
        {chronoforge::EffectOperation::SeamlessLoop, {3, 0.15F, 0, 0}, {1, 0, 0, 0}},
    };
    const auto loop_result = chronoforge::render_file_effect_chain(
        loop_input_path, loop_path, root / "loop-scratch", loop_input.shape(), loop_input.metadata(),
        loop_effects, 64 * 1024 * 1024, {});
    const auto loop_expected = chronoforge::seamless_loop(
        loop_input, {3, 0.15F, chronoforge::SeamlessLoopMode::LumaWeave});
    require(loop_result.shape == loop_expected.shape(), "Out-of-core Seamless Loop reports its shortened duration");
    const auto loop_output = chronoforge::MappedTensor::open(loop_path, loop_result.shape, chronoforge::MappedTensor::Access::ReadOnly);
    for (std::size_t i = 0; i < loop_expected.values().size(); ++i) {
        require_near(loop_output.data()[i], loop_expected.values()[i], "Out-of-core Seamless Loop matches proxy processing");
    }

    const auto half_path = root / "amount-half.raw";
    const std::vector<chronoforge::EffectSpec> half_effects{
        {chronoforge::EffectOperation::LumaTimeShift, {2.0F}, {0, 1}, 0.5F, 42},
    };
    const auto half_result = chronoforge::render_file_effect_chain(
        input_path, half_path, root / "amount-half-scratch", input.shape(), input.metadata(),
        half_effects, 64 * 1024 * 1024, {});
    const auto fully_shifted = chronoforge::luma_time_shift(
        input, {2.0F, chronoforge::ShiftSource::Luma, chronoforge::EdgeBehavior::Wrap});
    const auto half_output = chronoforge::MappedTensor::open(
        half_path, half_result.shape, chronoforge::MappedTensor::Access::ReadOnly);
    for (std::size_t i = 0; i < input.values().size(); ++i) {
        require_near(
            half_output.data()[i],
            input.values()[i] + (fully_shifted.values()[i] - input.values()[i]) * 0.5F,
            "Out-of-core Amount blends in-place in linear tensor space");
    }

    const std::array blend_modes{
        chronoforge::AmountBlendMode::Add,
        chronoforge::AmountBlendMode::Screen,
        chronoforge::AmountBlendMode::Multiply,
        chronoforge::AmountBlendMode::Difference,
        chronoforge::AmountBlendMode::XorGlitch,
    };
    const auto composite = [](float base, float layer, chronoforge::AmountBlendMode mode) {
        if (mode == chronoforge::AmountBlendMode::Add) return base + layer;
        if (mode == chronoforge::AmountBlendMode::Screen) return 1.0F - (1.0F - base) * (1.0F - layer);
        if (mode == chronoforge::AmountBlendMode::Multiply) return base * layer;
        if (mode == chronoforge::AmountBlendMode::Difference) return std::abs(base - layer);
        constexpr auto maximum = 4095U;
        const auto a = static_cast<std::uint32_t>(std::llround(std::clamp(base, 0.0F, 1.0F) * maximum));
        const auto b = static_cast<std::uint32_t>(std::llround(std::clamp(layer, 0.0F, 1.0F) * maximum));
        return static_cast<float>((a ^ b) & maximum) / static_cast<float>(maximum);
    };
    for (const auto mode : blend_modes) {
        const auto mode_path = root / ("amount-mode-" + std::to_string(static_cast<int>(mode)) + ".raw");
        const std::vector<chronoforge::EffectSpec> mode_effects{
            {chronoforge::EffectOperation::LumaTimeShift, {2.0F}, {0, 1}, 0.6F, 42, mode},
        };
        const auto mode_result = chronoforge::render_file_effect_chain(
            input_path, mode_path, root / ("amount-mode-scratch-" + std::to_string(static_cast<int>(mode))),
            input.shape(), input.metadata(), mode_effects, 64 * 1024 * 1024, {});
        const auto mode_output = chronoforge::MappedTensor::open(
            mode_path, mode_result.shape, chronoforge::MappedTensor::Access::ReadOnly);
        for (std::size_t i = 0; i < input.values().size(); ++i) {
            const auto expected_composite = composite(input.values()[i], fully_shifted.values()[i], mode);
            require_near(mode_output.data()[i], input.values()[i] + (expected_composite - input.values()[i]) * 0.6F,
                         "Out-of-core Amount blend mode matches its compositing formula");
        }
    }

    const auto displace_path = root / "amount-displace.raw";
    const std::vector<chronoforge::EffectSpec> displace_effects{
        {chronoforge::EffectOperation::LumaTimeShift, {2.0F}, {0, 1}, 1.0F, 42, chronoforge::AmountBlendMode::Displace},
    };
    const auto displace_result = chronoforge::render_file_effect_chain(
        input_path, displace_path, root / "amount-displace-scratch", input.shape(), input.metadata(),
        displace_effects, 64 * 1024 * 1024, {});
    const auto displace_output = chronoforge::MappedTensor::open(
        displace_path, displace_result.shape, chronoforge::MappedTensor::Access::ReadOnly);
    require(!std::equal(input.values().begin(), input.values().end(), displace_output.data()),
            "Displace Amount mode uses the effect output as a spatial-temporal displacement field");

    const auto dry_path = root / "amount-zero.raw";
    const std::vector<chronoforge::EffectSpec> dry_effects{
        {chronoforge::EffectOperation::LumaTimeShift, {200.0F}, {0, 1}, 0.0F, 99},
    };
    const auto dry_result = chronoforge::render_file_effect_chain(
        input_path, dry_path, root / "amount-zero-scratch", input.shape(), input.metadata(),
        dry_effects, 64 * 1024 * 1024, {});
    const auto dry_output = chronoforge::MappedTensor::open(
        dry_path, dry_result.shape, chronoforge::MappedTensor::Access::ReadOnly);
    require(
        std::equal(input.values().begin(), input.values().end(), dry_output.data()),
        "Amount zero is a bit-exact identity and skips effect evaluation");

    const auto seeded_input = numbered({5, 5, 12, 1});
    const auto seeded_input_path = root / "seeded-input.raw";
    {
        auto mapped = chronoforge::MappedTensor::create(seeded_input_path, seeded_input.shape());
        std::copy(seeded_input.values().begin(), seeded_input.values().end(), mapped.mutable_data());
        mapped.sync();
    }
    const auto seeded_path = root / "seeded-datamosh.raw";
    const std::vector<chronoforge::EffectSpec> seeded_effects{
        {chronoforge::EffectOperation::StructuralDatamosh, {0.2F, 3.0F, 0.37F}, {1, 2, 0}, 1.0F, 0xC0FFEE},
    };
    const auto seeded_result = chronoforge::render_file_effect_chain(
        seeded_input_path, seeded_path, root / "seeded-datamosh-scratch", seeded_input.shape(), seeded_input.metadata(),
        seeded_effects, 64 * 1024 * 1024, {});
    const auto seeded_expected = chronoforge::structural_datamosh(
        seeded_input,
        {chronoforge::FreezeAxis::Horizontal, chronoforge::FreezeTrigger::Random, 0.2F, 3, 0.37F, 0xC0FFEE});
    const auto seeded_output = chronoforge::MappedTensor::open(
        seeded_path, seeded_result.shape, chronoforge::MappedTensor::Access::ReadOnly);
    require(
        std::equal(seeded_expected.values().begin(), seeded_expected.values().end(), seeded_output.data()),
        "Random seed produces identical RAM and out-of-core datamosh patterns");
    const auto different_seed = chronoforge::structural_datamosh(
        seeded_input,
        {chronoforge::FreezeAxis::Horizontal, chronoforge::FreezeTrigger::Random, 0.2F, 3, 0.37F, 0});
    require(different_seed.values() != seeded_expected.values(), "Changing the random seed changes the datamosh pattern");

    bool partial_shape_change_rejected = false;
    try {
        const std::vector<chronoforge::EffectSpec> invalid_amount{
            {chronoforge::EffectOperation::SeamlessLoop, {3}, {0}, 0.5F, 0},
        };
        static_cast<void>(chronoforge::render_file_effect_chain(
            loop_input_path, root / "invalid-amount.raw", root / "invalid-amount-scratch",
            loop_input.shape(), loop_input.metadata(), invalid_amount, 64 * 1024 * 1024, {}));
    } catch (const std::invalid_argument&) {
        partial_shape_change_rejected = true;
    }
    require(partial_shape_change_rejected, "Partial Amount rejects shape-changing effects");

    bool cancelled = false;
    try {
        const std::vector<chronoforge::EffectSpec> cancellable{
            {chronoforge::EffectOperation::LumaTimeShift, {2.0F, 0, 0, 0}, {0, 1, 0, 0}},
        };
        static_cast<void>(chronoforge::render_file_effect_chain(
            input_path,
            root / "cancelled.raw",
            root / "cancel-scratch",
            input.shape(),
            input.metadata(),
            cancellable,
            64 * 1024 * 1024,
            [](double fraction, std::string_view) { return fraction < 0.1; }));
    } catch (const std::runtime_error&) {
        cancelled = true;
    }
    require(cancelled && !std::filesystem::exists(root / "cancelled.raw"), "Cancelled file render removes partial output");
    std::filesystem::remove_all(root);
}

}  // namespace

int main() {
    try {
        test_transpose();
        test_time_shift_and_funnel();
        test_rgb_time_slip();
        test_horizontal_sync_loss();
        test_chroma_carrier_drift();
        test_stride_error();
        test_block_address_corruption();
        test_bitplane_forge();
        test_sort_and_rotation();
        test_fft_swap();
        test_cross_tensor_and_flow_effects();
        test_file_backed_cross_tensor();
        test_cache_and_graph();
        test_tiling();
        test_file_backed_effect_chain();
        std::cout << "All ChronoForge core tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
