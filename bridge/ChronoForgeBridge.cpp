#include "ChronoForgeBridge.h"

#include "chronoforge/core/effects.hpp"
#include "chronoforge/core/file_executor.hpp"
#include "chronoforge/core/spectral.hpp"
#include "chronoforge/core/video_tensor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using chronoforge::VideoTensor;

struct CFVideoBuffer {
    explicit CFVideoBuffer(VideoTensor value) : tensor(std::move(value)) {}
    VideoTensor tensor;
};

namespace {

void set_error(char* destination, uint64_t capacity, const std::string& message) {
    if (destination == nullptr || capacity == 0) {
        return;
    }
    const auto safe_capacity = static_cast<std::size_t>(std::min<uint64_t>(capacity, std::numeric_limits<std::size_t>::max()));
    std::snprintf(destination, safe_capacity, "%s", message.c_str());
}

template <typename Enum>
Enum checked_enum(int32_t value, int32_t maximum, const char* name) {
    if (value < 0 || value > maximum) {
        throw std::invalid_argument(std::string("Invalid ") + name + " option");
    }
    return static_cast<Enum>(value);
}

bool checked_flag(int32_t value, const char* name) {
    if (value != 0 && value != 1) {
        throw std::invalid_argument(std::string("Invalid ") + name + " option");
    }
    return value != 0;
}

void enforce_budget(const VideoTensor& tensor, uint64_t budget) {
    if (budget == 0) {
        throw std::invalid_argument("Working-set budget must be greater than zero");
    }
    const auto bytes = tensor.shape().byte_count();
    if (bytes > budget / 2) {
        throw std::runtime_error("Effect chain needs two tensor buffers and exceeds the configured memory budget");
    }
}

[[nodiscard]] std::pair<uint32_t, uint32_t> expected_parameter_counts(CFEffectKind kind) {
    switch (kind) {
        case CF_EFFECT_SPACE_TIME_TRANSPOSE: return {0, 2};
        case CF_EFFECT_LUMA_TIME_SHIFT: return {1, 2};
        case CF_EFFECT_RADIAL_CHRONO_FUNNEL: return {5, 3};
        case CF_EFFECT_TEMPORAL_PIXEL_SORT: return {2, 2};
        case CF_EFFECT_TENSOR_3D_ROTATION: return {3, 1};
        case CF_EFFECT_SPECTRAL_FFT_SWAP: return {1, 4};
        case CF_EFFECT_SELECTIVE_PREFILTER: return {0, 2};
        case CF_EFFECT_DIMENSIONAL_SPLICER: return {0, 4};
        case CF_EFFECT_TENSOR_DISPLACEMENT: return {3, 3};
        case CF_EFFECT_OPTICAL_FLOW_TIME_WARP: return {4, 1};
        case CF_EFFECT_CHRONO_FEEDBACK: return {4, 1};
        case CF_EFFECT_STRUCTURAL_DATAMOSH: return {3, 3};
        case CF_EFFECT_SEAMLESS_LOOP: return {4, 3};
        case CF_EFFECT_RGB_TIME_SLIP: return {4, 2};
        case CF_EFFECT_HORIZONTAL_SYNC_LOSS: return {4, 3};
        case CF_EFFECT_CHROMA_CARRIER_DRIFT: return {4, 2};
        case CF_EFFECT_STRIDE_ERROR: return {3, 2};
        case CF_EFFECT_BLOCK_ADDRESS_CORRUPTION: return {4, 2};
        case CF_EFFECT_BITPLANE_FORGE: return {3, 2};
        case CF_EFFECT_SIGNAL_WEAVE: return {4, 2};
        case CF_EFFECT_BLOCK_GRAFT: return {4, 2};
        case CF_EFFECT_CHANNEL_TRANSPLANT: return {3, 5};
        case CF_EFFECT_AFFINITY_MIGRATION: return {4, 1};
    }
    throw std::invalid_argument("Unsupported effect kind");
}

CFEffectKind validate_descriptor(const CFEffectDescriptorV2& descriptor) {
    if (descriptor.descriptor_version != CF_EFFECT_DESCRIPTOR_VERSION) {
        throw std::invalid_argument("Unsupported effect descriptor version");
    }
    if (!std::isfinite(descriptor.amount) || descriptor.amount < 0.0F || descriptor.amount > 1.0F) {
        throw std::invalid_argument("Effect amount must be between zero and one");
    }
    checked_enum<chronoforge::AmountBlendMode>(descriptor.amount_blend_mode, 6, "Amount blend mode");
    const auto kind = checked_enum<CFEffectKind>(descriptor.kind, CF_EFFECT_AFFINITY_MIGRATION, "effect kind");
    const auto [values, options] = expected_parameter_counts(kind);
    if (descriptor.value_count != values || descriptor.option_count != options) {
        throw std::invalid_argument("Effect descriptor parameter counts do not match its kind");
    }
    for (uint32_t index = descriptor.value_count; index < CF_EFFECT_PARAMETER_CAPACITY; ++index) {
        if (descriptor.values[index] != 0.0F) {
            throw std::invalid_argument("Unused effect value slots must be zero");
        }
    }
    for (uint32_t index = descriptor.option_count; index < CF_EFFECT_PARAMETER_CAPACITY; ++index) {
        if (descriptor.options[index] != 0) {
            throw std::invalid_argument("Unused effect option slots must be zero");
        }
    }
    return kind;
}

void blend_amount(
    const VideoTensor& input,
    VideoTensor& effected,
    float amount,
    chronoforge::AmountBlendMode mode) {
    if (input.shape() != effected.shape()) {
        throw std::invalid_argument("Partial Amount requires an effect that preserves tensor shape");
    }
    auto& output = effected.values();
    const auto& source = input.values();
    if (mode == chronoforge::AmountBlendMode::Displace) {
        const auto field = output;
        const auto& shape = input.shape();
        const auto field_at = [&](std::size_t t, std::size_t y, std::size_t x, std::size_t c) {
            return field[(((t * shape.h + y) * shape.w + x) * shape.c) + std::min(c, shape.c - 1)];
        };
        for (std::size_t t = 0; t < shape.t; ++t) {
            for (std::size_t y = 0; y < shape.h; ++y) {
                for (std::size_t x = 0; x < shape.w; ++x) {
                    const auto st = static_cast<std::size_t>(std::clamp(
                        std::llround(static_cast<double>(t) + (field_at(t, y, x, 2) - 0.5F) * amount * 0.15F * static_cast<float>(shape.t)),
                        0LL, static_cast<long long>(shape.t - 1)));
                    const auto sy = static_cast<std::size_t>(std::clamp(
                        std::llround(static_cast<double>(y) + (field_at(t, y, x, 1) - 0.5F) * amount * 0.15F * static_cast<float>(shape.h)),
                        0LL, static_cast<long long>(shape.h - 1)));
                    const auto sx = static_cast<std::size_t>(std::clamp(
                        std::llround(static_cast<double>(x) + (field_at(t, y, x, 0) - 0.5F) * amount * 0.15F * static_cast<float>(shape.w)),
                        0LL, static_cast<long long>(shape.w - 1)));
                    for (std::size_t c = 0; c < shape.c; ++c) {
                        output[(((t * shape.h + y) * shape.w + x) * shape.c) + c] = input.at(st, sy, sx, c);
                    }
                }
            }
        }
        return;
    }
    const auto composite = [mode](float base, float layer) {
        switch (mode) {
            case chronoforge::AmountBlendMode::Normal: return layer;
            case chronoforge::AmountBlendMode::Add: return base + layer;
            case chronoforge::AmountBlendMode::Screen: return 1.0F - (1.0F - base) * (1.0F - layer);
            case chronoforge::AmountBlendMode::Multiply: return base * layer;
            case chronoforge::AmountBlendMode::Difference: return std::abs(base - layer);
            case chronoforge::AmountBlendMode::XorGlitch: {
                constexpr auto maximum = 4095U;
                const auto a = static_cast<std::uint32_t>(std::llround(std::clamp(base, 0.0F, 1.0F) * maximum));
                const auto b = static_cast<std::uint32_t>(std::llround(std::clamp(layer, 0.0F, 1.0F) * maximum));
                return static_cast<float>((a ^ b) & maximum) / static_cast<float>(maximum);
            }
            case chronoforge::AmountBlendMode::Displace: return base;
        }
        return layer;
    };
    for (std::size_t index = 0; index < output.size(); ++index) {
        output[index] = source[index] + (composite(source[index], output[index]) - source[index]) * amount;
    }
}

VideoTensor apply_effect(const VideoTensor& input, const CFEffectDescriptorV2& descriptor, uint64_t budget) {
    const auto kind = validate_descriptor(descriptor);
    switch (kind) {
        case CF_EFFECT_SPACE_TIME_TRANSPOSE:
            return chronoforge::space_time_transpose(
                input,
                {
                    checked_enum<chronoforge::SpatialAxis>(descriptor.options[0], 1, "transpose axis"),
                    checked_enum<chronoforge::TransposeResolution>(descriptor.options[1], 1, "transpose resolution"),
                });
        case CF_EFFECT_LUMA_TIME_SHIFT:
            return chronoforge::luma_time_shift(
                input,
                {
                    descriptor.values[0],
                    checked_enum<chronoforge::ShiftSource>(descriptor.options[0], 4, "shift source"),
                    checked_enum<chronoforge::EdgeBehavior>(descriptor.options[1], 2, "edge behavior"),
                });
        case CF_EFFECT_RADIAL_CHRONO_FUNNEL:
            return chronoforge::radial_chrono_funnel(
                input,
                {
                    descriptor.values[0],
                    descriptor.values[1],
                    descriptor.values[2],
                    checked_enum<chronoforge::EdgeBehavior>(descriptor.options[0], 2, "edge behavior"),
                    descriptor.values[3],
                    checked_enum<chronoforge::RadialTopology>(descriptor.options[1], 2, "radial topology"),
                    descriptor.values[4],
                    checked_enum<chronoforge::RadialSeamMode>(descriptor.options[2], 1, "radial seam mode"),
                });
        case CF_EFFECT_TEMPORAL_PIXEL_SORT:
            return chronoforge::temporal_pixel_sort(
                input,
                {
                    checked_enum<chronoforge::SortCriterion>(descriptor.options[0], 2, "sort criterion"),
                    checked_enum<chronoforge::SortDirection>(descriptor.options[1], 3, "sort order"),
                    descriptor.values[0],
                    descriptor.values[1],
                });
        case CF_EFFECT_TENSOR_3D_ROTATION:
            return chronoforge::tensor_3d_rotation(
                input,
                {
                    descriptor.values[0],
                    descriptor.values[1],
                    descriptor.values[2],
                    checked_enum<chronoforge::FillMode>(descriptor.options[0], 3, "fill mode"),
                });
        case CF_EFFECT_SPECTRAL_FFT_SWAP:
            return chronoforge::spectral_fft_swap(
                input,
                {
                    checked_enum<chronoforge::SpectralSwapAxis>(descriptor.options[0], 2, "FFT swap axis"),
                    descriptor.options[1] != 0,
                    static_cast<std::size_t>(std::min<uint64_t>(budget, std::numeric_limits<std::size_t>::max())),
                    checked_enum<chronoforge::SpectralResolution>(descriptor.options[2], 1, "FFT resolution"),
                    checked_enum<chronoforge::SpectralTransform>(descriptor.options[3], 1, "FFT transform"),
                    descriptor.values[0],
                });
        case CF_EFFECT_SELECTIVE_PREFILTER:
            return chronoforge::selective_prefilter(
                input,
                {
                    checked_enum<chronoforge::PrefilterStrength>(descriptor.options[0], 2, "spatial prefilter"),
                    checked_enum<chronoforge::PrefilterStrength>(descriptor.options[1], 2, "temporal prefilter"),
                });
        case CF_EFFECT_OPTICAL_FLOW_TIME_WARP:
            return chronoforge::optical_flow_time_warp(
                input,
                {
                    descriptor.values[0], descriptor.values[1], descriptor.values[2], descriptor.values[3],
                    checked_enum<chronoforge::EdgeBehavior>(descriptor.options[0], 2, "flow edge behavior"),
                });
        case CF_EFFECT_CHRONO_FEEDBACK:
            return chronoforge::chrono_feedback(
                input,
                {
                    static_cast<std::size_t>(std::max(0.0F, std::round(descriptor.values[0]))), descriptor.values[1],
                    static_cast<std::size_t>(std::max(0.0F, std::round(descriptor.values[2]))), descriptor.values[3],
                    checked_enum<chronoforge::FeedbackBlendMode>(descriptor.options[0], 5, "feedback blend mode"),
                });
        case CF_EFFECT_STRUCTURAL_DATAMOSH:
            return chronoforge::structural_datamosh(
                input,
                {
                    checked_enum<chronoforge::FreezeAxis>(descriptor.options[0], 2, "freeze axis"),
                    checked_enum<chronoforge::FreezeTrigger>(descriptor.options[1], 2, "freeze trigger"),
                    descriptor.values[0],
                    static_cast<std::size_t>(std::max(0.0F, std::round(descriptor.values[1]))),
                    descriptor.values[2],
                    descriptor.random_seed,
                    checked_flag(descriptor.options[2], "freeze trigger polarity"),
                });
        case CF_EFFECT_SEAMLESS_LOOP:
            return chronoforge::seamless_loop(
                input,
                {
                    static_cast<std::size_t>(std::max(1.0F, std::round(descriptor.values[0]))),
                    descriptor.values[1],
                    checked_enum<chronoforge::SeamlessLoopMode>(descriptor.options[0], 4, "seamless loop mode"),
                    descriptor.values[2],
                    descriptor.values[3],
                    checked_enum<chronoforge::LoopTransitionPlacement>(descriptor.options[1], 1, "loop transition placement"),
                    checked_enum<chronoforge::SpectralPhaseMode>(descriptor.options[2], 2, "spectral phase mode"),
                });
        case CF_EFFECT_RGB_TIME_SLIP:
            return chronoforge::rgb_time_slip(
                input,
                {
                    descriptor.values[0], descriptor.values[1], descriptor.values[2], descriptor.values[3],
                    checked_enum<chronoforge::SplitAxis>(descriptor.options[0], 2, "RGB split axis"),
                    checked_enum<chronoforge::EdgeBehavior>(descriptor.options[1], 2, "RGB time slip edge behavior"),
                });
        case CF_EFFECT_HORIZONTAL_SYNC_LOSS:
            return chronoforge::horizontal_sync_loss(
                input,
                {
                    descriptor.values[0],
                    descriptor.values[1],
                    descriptor.values[2], descriptor.values[3],
                    checked_enum<chronoforge::SyncLossDriver>(descriptor.options[0], 2, "sync loss driver"),
                    checked_enum<chronoforge::EdgeBehavior>(descriptor.options[1], 2, "sync loss edge behavior"),
                    descriptor.random_seed,
                    checked_enum<chronoforge::SyncLossAxis>(descriptor.options[2], 1, "sync loss axis"),
                });
        case CF_EFFECT_CHROMA_CARRIER_DRIFT:
            return chronoforge::chroma_carrier_drift(
                input,
                {
                    descriptor.values[0], descriptor.values[1], descriptor.values[2], descriptor.values[3],
                    checked_enum<chronoforge::ChromaDriftMode>(descriptor.options[0], 2, "chroma drift mode"),
                    checked_enum<chronoforge::EdgeBehavior>(descriptor.options[1], 2, "chroma drift edge behavior"),
                });
        case CF_EFFECT_STRIDE_ERROR:
            return chronoforge::stride_error(
                input,
                {
                    descriptor.values[0], descriptor.values[1], descriptor.values[2],
                    checked_enum<chronoforge::StrideChannelMode>(descriptor.options[0], 2, "stride channel mode"),
                    checked_enum<chronoforge::AddressEdge>(descriptor.options[1], 1, "stride address edge"),
                });
        case CF_EFFECT_BLOCK_ADDRESS_CORRUPTION:
            return chronoforge::block_address_corruption(
                input,
                {
                    descriptor.values[0],
                    descriptor.values[1],
                    static_cast<std::size_t>(std::max(0.0F, std::round(descriptor.values[2]))),
                    static_cast<std::size_t>(std::max(1.0F, std::round(descriptor.values[3]))),
                    checked_enum<chronoforge::BlockCorruptionMapping>(descriptor.options[0], 3, "block corruption mapping"),
                    checked_enum<chronoforge::EdgeBehavior>(descriptor.options[1], 2, "block corruption edge"),
                    descriptor.random_seed,
                });
        case CF_EFFECT_BITPLANE_FORGE:
            return chronoforge::bitplane_forge(
                input,
                {
                    static_cast<std::size_t>(std::clamp(std::round(descriptor.values[0]), 2.0F, 16.0F)),
                    static_cast<std::uint16_t>(std::clamp(std::round(descriptor.values[1]), 0.0F, 65535.0F)),
                    static_cast<int>(std::clamp(std::round(descriptor.values[2]), -15.0F, 15.0F)),
                    checked_enum<chronoforge::BitplaneOperation>(descriptor.options[0], 3, "bitplane operation"),
                    checked_enum<chronoforge::BitplaneChannel>(descriptor.options[1], 5, "bitplane channel"),
                    descriptor.random_seed,
                });
        case CF_EFFECT_AFFINITY_MIGRATION:
            return chronoforge::affinity_migration(
                input,
                {descriptor.values[0], descriptor.values[1], descriptor.values[2], descriptor.values[3],
                 std::clamp(descriptor.options[0] + 2, 2, 8), descriptor.random_seed});
        case CF_EFFECT_DIMENSIONAL_SPLICER:
        case CF_EFFECT_TENSOR_DISPLACEMENT:
        case CF_EFFECT_SIGNAL_WEAVE:
        case CF_EFFECT_BLOCK_GRAFT:
        case CF_EFFECT_CHANNEL_TRANSPLANT:
            throw std::invalid_argument("This effect requires a driver video");
    }
    throw std::invalid_argument("Unsupported effect kind");
}

VideoTensor apply_cross_effect(
    const VideoTensor& source,
    const VideoTensor& driver,
    const CFEffectDescriptorV2& descriptor) {
    const auto kind = validate_descriptor(descriptor);
    switch (kind) {
        case CF_EFFECT_DIMENSIONAL_SPLICER:
            return chronoforge::dimensional_splicer(
                source,
                driver,
                {
                    checked_enum<chronoforge::TensorAxisSource>(descriptor.options[0], 5, "output X axis source"),
                    checked_enum<chronoforge::TensorAxisSource>(descriptor.options[1], 5, "output Y axis source"),
                    checked_enum<chronoforge::TensorAxisSource>(descriptor.options[2], 5, "output Time axis source"),
                    checked_enum<chronoforge::TensorInterpolation>(descriptor.options[3], 2, "splicer interpolation"),
                });
        case CF_EFFECT_TENSOR_DISPLACEMENT:
            return chronoforge::tensor_displacement(
                source,
                driver,
                {
                    descriptor.values[0], descriptor.values[1], descriptor.values[2],
                    checked_enum<chronoforge::ShiftSource>(descriptor.options[0], 4, "displacement source"),
                    checked_enum<chronoforge::TensorBroadcast>(descriptor.options[1], 2, "tensor broadcast"),
                    checked_enum<chronoforge::EdgeBehavior>(descriptor.options[2], 2, "displacement edge behavior"),
                });
        case CF_EFFECT_SIGNAL_WEAVE:
            return chronoforge::signal_weave(
                source,
                driver,
                {
                    checked_enum<chronoforge::SignalWeavePattern>(descriptor.options[0], 3, "signal weave pattern"),
                    descriptor.values[0],
                    descriptor.values[1], descriptor.values[2],
                    static_cast<int>(std::clamp(std::round(descriptor.values[3]), -240.0F, 240.0F)),
                    checked_enum<chronoforge::TensorBroadcast>(descriptor.options[1], 2, "signal weave size matching"),
                    descriptor.random_seed,
                });
        case CF_EFFECT_BLOCK_GRAFT:
            return chronoforge::block_graft(
                source,
                driver,
                {
                    descriptor.values[0],
                    descriptor.values[1],
                    static_cast<std::size_t>(std::max(1.0F, std::round(descriptor.values[2]))),
                    static_cast<int>(std::clamp(std::round(descriptor.values[3]), -240.0F, 240.0F)),
                    checked_enum<chronoforge::BlockGraftTrigger>(descriptor.options[0], 4, "block graft trigger"),
                    checked_enum<chronoforge::TensorBroadcast>(descriptor.options[1], 2, "block graft size matching"),
                    descriptor.random_seed,
                });
        case CF_EFFECT_CHANNEL_TRANSPLANT:
            return chronoforge::channel_transplant(
                source,
                driver,
                {
                    {checked_flag(descriptor.options[0], "channel transplant component source"),
                     checked_flag(descriptor.options[1], "channel transplant component source"),
                     checked_flag(descriptor.options[2], "channel transplant component source")},
                    static_cast<int>(std::clamp(std::round(descriptor.values[0]), -240.0F, 240.0F)),
                    static_cast<int>(std::round(descriptor.values[1])),
                    static_cast<int>(std::round(descriptor.values[2])),
                    checked_enum<chronoforge::ChannelTransplantColourModel>(descriptor.options[3], 1, "channel transplant colour model"),
                    checked_enum<chronoforge::TensorBroadcast>(descriptor.options[4], 2, "channel transplant size matching"),
                });
        default:
            throw std::invalid_argument("The selected effect does not accept a driver video");
    }
}

}  // namespace

extern "C" {

CFEffectDescriptorV2 cf_effect_descriptor_v2_make(
    int32_t kind,
    float amount,
    int32_t amount_blend_mode,
    uint64_t random_seed,
    const float* values,
    uint32_t value_count,
    const int32_t* options,
    uint32_t option_count) {
    CFEffectDescriptorV2 descriptor{};
    descriptor.kind = kind;
    descriptor.descriptor_version = CF_EFFECT_DESCRIPTOR_VERSION;
    descriptor.amount = amount;
    descriptor.amount_blend_mode = amount_blend_mode;
    descriptor.random_seed = random_seed;
    descriptor.value_count = value_count;
    descriptor.option_count = option_count;
    if (values != nullptr && value_count <= CF_EFFECT_PARAMETER_CAPACITY) {
        std::copy_n(values, value_count, descriptor.values);
    }
    if (options != nullptr && option_count <= CF_EFFECT_PARAMETER_CAPACITY) {
        std::copy_n(options, option_count, descriptor.options);
    }
    return descriptor;
}

const char* cf_core_version(void) { return "1.1.0"; }

int32_t cf_render_effect_chain(
    const float* input,
    uint64_t frames,
    uint64_t height,
    uint64_t width,
    uint64_t channels,
    uint32_t frame_rate_numerator,
    uint32_t frame_rate_denominator,
    const CFEffectDescriptorV2* effects,
    uint64_t effect_count,
    uint64_t max_working_set_bytes,
    CFVideoBuffer** output,
    char* error_message,
    uint64_t error_message_capacity) {
    if (output != nullptr) {
        *output = nullptr;
    }
    try {
        if (input == nullptr || output == nullptr) {
            throw std::invalid_argument("Input and output pointers are required");
        }
        if (effect_count > 0 && effects == nullptr) {
            throw std::invalid_argument("Effect descriptors are required when effect_count is non-zero");
        }
        if (frame_rate_numerator == 0 || frame_rate_denominator == 0) {
            throw std::invalid_argument("Frame rate must be a positive rational number");
        }

        const chronoforge::TensorShape shape{
            static_cast<std::size_t>(frames),
            static_cast<std::size_t>(height),
            static_cast<std::size_t>(width),
            static_cast<std::size_t>(channels),
        };
        const auto count = shape.element_count();
        std::vector<float> values(input, input + count);
        VideoTensor current(
            shape,
            std::move(values),
            {frame_rate_numerator, frame_rate_denominator, chronoforge::ColorTransfer::Linear,
             channels == 4 ? chronoforge::AlphaRepresentation::Premultiplied : chronoforge::AlphaRepresentation::None});
        enforce_budget(current, max_working_set_bytes);

        for (uint64_t index = 0; index < effect_count; ++index) {
            const auto& descriptor = effects[index];
            validate_descriptor(descriptor);
            if (descriptor.amount == 0.0F) {
                continue;
            }
            auto effected = apply_effect(current, descriptor, max_working_set_bytes);
            const auto amount_mode = static_cast<chronoforge::AmountBlendMode>(descriptor.amount_blend_mode);
            if (descriptor.amount < 1.0F || amount_mode != chronoforge::AmountBlendMode::Normal) {
                blend_amount(current, effected, descriptor.amount, amount_mode);
            }
            current = std::move(effected);
            enforce_budget(current, max_working_set_bytes);
        }
        *output = new CFVideoBuffer(std::move(current));
        set_error(error_message, error_message_capacity, "");
        return 0;
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return 1;
    } catch (...) {
        set_error(error_message, error_message_capacity, "Unknown ChronoForge core failure");
        return 2;
    }
}

int32_t cf_render_cross_tensor_effect(
    const float* source,
    uint64_t source_frames,
    uint64_t source_height,
    uint64_t source_width,
    uint64_t source_channels,
    uint32_t frame_rate_numerator,
    uint32_t frame_rate_denominator,
    const float* driver,
    uint64_t driver_frames,
    uint64_t driver_height,
    uint64_t driver_width,
    uint64_t driver_channels,
    CFEffectDescriptorV2 effect,
    uint64_t max_working_set_bytes,
    CFVideoBuffer** output,
    char* error_message,
    uint64_t error_message_capacity) {
    if (output != nullptr) {
        *output = nullptr;
    }
    try {
        if (source == nullptr || driver == nullptr || output == nullptr) {
            throw std::invalid_argument("Source, driver and output pointers are required");
        }
        if (frame_rate_numerator == 0 || frame_rate_denominator == 0) {
            throw std::invalid_argument("Frame rate must be a positive rational number");
        }
        const chronoforge::TensorShape source_shape{
            static_cast<std::size_t>(source_frames), static_cast<std::size_t>(source_height),
            static_cast<std::size_t>(source_width), static_cast<std::size_t>(source_channels),
        };
        const chronoforge::TensorShape driver_shape{
            static_cast<std::size_t>(driver_frames), static_cast<std::size_t>(driver_height),
            static_cast<std::size_t>(driver_width), static_cast<std::size_t>(driver_channels),
        };
        const chronoforge::VideoTensorMetadata metadata{
            frame_rate_numerator, frame_rate_denominator, chronoforge::ColorTransfer::Linear,
            source_channels == 4 ? chronoforge::AlphaRepresentation::Premultiplied : chronoforge::AlphaRepresentation::None,
        };
        VideoTensor source_tensor(source_shape, std::vector<float>(source, source + source_shape.element_count()), metadata);
        VideoTensor driver_tensor(
            driver_shape,
            std::vector<float>(driver, driver + driver_shape.element_count()),
            {frame_rate_numerator, frame_rate_denominator, chronoforge::ColorTransfer::Linear,
             driver_channels == 4 ? chronoforge::AlphaRepresentation::Premultiplied : chronoforge::AlphaRepresentation::None});
        if (max_working_set_bytes == 0 || source_shape.byte_count() + driver_shape.byte_count() > max_working_set_bytes) {
            throw std::runtime_error("Proxy tensors exceed the configured working memory budget");
        }
        validate_descriptor(effect);
        if (effect.amount == 0.0F) {
            *output = new CFVideoBuffer(std::move(source_tensor));
            set_error(error_message, error_message_capacity, "");
            return 0;
        }
        auto result = apply_cross_effect(source_tensor, driver_tensor, effect);
        const auto amount_mode = static_cast<chronoforge::AmountBlendMode>(effect.amount_blend_mode);
        if (effect.amount < 1.0F || amount_mode != chronoforge::AmountBlendMode::Normal) {
            blend_amount(source_tensor, result, effect.amount, amount_mode);
        }
        *output = new CFVideoBuffer(std::move(result));
        set_error(error_message, error_message_capacity, "");
        return 0;
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return 1;
    } catch (...) {
        set_error(error_message, error_message_capacity, "Unknown ChronoForge cross-tensor failure");
        return 2;
    }
}

int32_t cf_render_file_effect_chain(
    const char* input_path,
    const char* output_path,
    const char* scratch_directory,
    CFFileTensorInfo input_info,
    const CFEffectDescriptorV2* effects,
    uint64_t effect_count,
    uint64_t max_working_set_bytes,
    CFRenderProgressCallback progress,
    void* progress_context,
    CFFileTensorInfo* output_info,
    char* error_message,
    uint64_t error_message_capacity) {
    try {
        if (input_path == nullptr || output_path == nullptr || scratch_directory == nullptr || output_info == nullptr) {
            throw std::invalid_argument("File render paths and output info are required");
        }
        if (effect_count > 0 && effects == nullptr) {
            throw std::invalid_argument("Effect descriptors are required when effect_count is non-zero");
        }
        if (input_info.frame_rate_numerator == 0 || input_info.frame_rate_denominator == 0) {
            throw std::invalid_argument("Frame rate must be a positive rational number");
        }
        const chronoforge::TensorShape shape{
            static_cast<std::size_t>(input_info.frames),
            static_cast<std::size_t>(input_info.height),
            static_cast<std::size_t>(input_info.width),
            static_cast<std::size_t>(input_info.channels),
        };
        static_cast<void>(shape.element_count());
        std::vector<chronoforge::EffectSpec> specifications;
        specifications.reserve(static_cast<std::size_t>(effect_count));
        for (uint64_t index = 0; index < effect_count; ++index) {
            validate_descriptor(effects[index]);
            const auto kind = static_cast<chronoforge::EffectOperation>(effects[index].kind);
            specifications.push_back({
                kind,
                {effects[index].values[0], effects[index].values[1], effects[index].values[2], effects[index].values[3],
                 effects[index].values[4], effects[index].values[5], effects[index].values[6], effects[index].values[7]},
                {effects[index].options[0], effects[index].options[1], effects[index].options[2], effects[index].options[3],
                 effects[index].options[4], effects[index].options[5], effects[index].options[6], effects[index].options[7]},
                effects[index].amount,
                effects[index].random_seed,
                checked_enum<chronoforge::AmountBlendMode>(effects[index].amount_blend_mode, 6, "Amount blend mode"),
            });
        }
        const chronoforge::VideoTensorMetadata metadata{
            input_info.frame_rate_numerator,
            input_info.frame_rate_denominator,
            chronoforge::ColorTransfer::Linear,
            input_info.channels == 4 ? chronoforge::AlphaRepresentation::Premultiplied
                                     : chronoforge::AlphaRepresentation::None,
        };
        const auto callback = [&](double fraction, std::string_view stage) {
            if (progress == nullptr) {
                return true;
            }
            const std::string owned_stage(stage);
            return progress(fraction, owned_stage.c_str(), progress_context) != 0;
        };
        const auto result = chronoforge::render_file_effect_chain(
            input_path,
            output_path,
            scratch_directory,
            shape,
            metadata,
            specifications,
            static_cast<std::size_t>(std::min<uint64_t>(max_working_set_bytes, std::numeric_limits<std::size_t>::max())),
            callback);
        *output_info = {
            result.shape.t,
            result.shape.h,
            result.shape.w,
            result.shape.c,
            result.metadata.frame_rate_numerator,
            result.metadata.frame_rate_denominator,
        };
        set_error(error_message, error_message_capacity, "");
        return 0;
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return std::string(error.what()) == "Render cancelled" ? 3 : 1;
    } catch (...) {
        set_error(error_message, error_message_capacity, "Unknown ChronoForge file render failure");
        return 2;
    }
}

int32_t cf_render_file_cross_tensor_effect(
    const char* source_path,
    CFFileTensorInfo source_info,
    const char* driver_path,
    CFFileTensorInfo driver_info,
    const char* output_path,
    CFEffectDescriptorV2 effect,
    CFRenderProgressCallback progress,
    void* progress_context,
    CFFileTensorInfo* output_info,
    char* error_message,
    uint64_t error_message_capacity) {
    try {
        if (source_path == nullptr || driver_path == nullptr || output_path == nullptr || output_info == nullptr) {
            throw std::invalid_argument("Cross-tensor file paths and output info are required");
        }
        const chronoforge::TensorShape source_shape{
            static_cast<std::size_t>(source_info.frames), static_cast<std::size_t>(source_info.height),
            static_cast<std::size_t>(source_info.width), static_cast<std::size_t>(source_info.channels),
        };
        const chronoforge::TensorShape driver_shape{
            static_cast<std::size_t>(driver_info.frames), static_cast<std::size_t>(driver_info.height),
            static_cast<std::size_t>(driver_info.width), static_cast<std::size_t>(driver_info.channels),
        };
        validate_descriptor(effect);
        const auto kind = static_cast<chronoforge::EffectOperation>(effect.kind);
        const chronoforge::EffectSpec specification{
            kind,
            {effect.values[0], effect.values[1], effect.values[2], effect.values[3],
             effect.values[4], effect.values[5], effect.values[6], effect.values[7]},
            {effect.options[0], effect.options[1], effect.options[2], effect.options[3],
             effect.options[4], effect.options[5], effect.options[6], effect.options[7]},
            effect.amount,
            effect.random_seed,
            checked_enum<chronoforge::AmountBlendMode>(effect.amount_blend_mode, 6, "Amount blend mode"),
        };
        const chronoforge::VideoTensorMetadata metadata{
            source_info.frame_rate_numerator, source_info.frame_rate_denominator,
            chronoforge::ColorTransfer::Linear,
            source_info.channels == 4 ? chronoforge::AlphaRepresentation::Premultiplied
                                      : chronoforge::AlphaRepresentation::None,
        };
        const chronoforge::VideoTensorMetadata driver_metadata{
            driver_info.frame_rate_numerator, driver_info.frame_rate_denominator,
            chronoforge::ColorTransfer::Linear,
            driver_info.channels == 4 ? chronoforge::AlphaRepresentation::Premultiplied
                                      : chronoforge::AlphaRepresentation::None,
        };
        const auto callback = [&](double fraction, std::string_view stage) {
            if (progress == nullptr) return true;
            const std::string owned(stage);
            return progress(fraction, owned.c_str(), progress_context) != 0;
        };
        const auto result = chronoforge::render_file_cross_tensor_effect(
            source_path, driver_path, output_path, source_shape, driver_shape, metadata, driver_metadata, specification, callback);
        *output_info = {
            result.shape.t, result.shape.h, result.shape.w, result.shape.c,
            result.metadata.frame_rate_numerator, result.metadata.frame_rate_denominator,
        };
        set_error(error_message, error_message_capacity, "");
        return 0;
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return std::string(error.what()) == "Render cancelled" ? 3 : 1;
    } catch (...) {
        set_error(error_message, error_message_capacity, "Unknown ChronoForge cross-tensor file failure");
        return 2;
    }
}

uint64_t cf_video_buffer_frames(const CFVideoBuffer* buffer) {
    return buffer == nullptr ? 0 : buffer->tensor.shape().t;
}

uint64_t cf_video_buffer_height(const CFVideoBuffer* buffer) {
    return buffer == nullptr ? 0 : buffer->tensor.shape().h;
}

uint64_t cf_video_buffer_width(const CFVideoBuffer* buffer) {
    return buffer == nullptr ? 0 : buffer->tensor.shape().w;
}

uint64_t cf_video_buffer_channels(const CFVideoBuffer* buffer) {
    return buffer == nullptr ? 0 : buffer->tensor.shape().c;
}

uint64_t cf_video_buffer_value_count(const CFVideoBuffer* buffer) {
    return buffer == nullptr ? 0 : buffer->tensor.values().size();
}

const float* cf_video_buffer_values(const CFVideoBuffer* buffer) {
    return buffer == nullptr ? nullptr : buffer->tensor.values().data();
}

void cf_video_buffer_destroy(CFVideoBuffer* buffer) { delete buffer; }

}  // extern "C"
