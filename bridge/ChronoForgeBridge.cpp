#include "ChronoForgeBridge.h"

#include "chronoforge/core/effects.hpp"
#include "chronoforge/core/spectral.hpp"
#include "chronoforge/core/video_tensor.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
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

void enforce_budget(const VideoTensor& tensor, uint64_t budget) {
    if (budget == 0) {
        throw std::invalid_argument("Working-set budget must be greater than zero");
    }
    const auto bytes = tensor.shape().byte_count();
    if (bytes > budget / 2) {
        throw std::runtime_error("Effect chain needs two tensor buffers and exceeds the configured memory budget");
    }
}

VideoTensor apply_effect(const VideoTensor& input, const CFEffectDescriptor& descriptor, uint64_t budget) {
    const auto kind = checked_enum<CFEffectKind>(descriptor.kind, CF_EFFECT_SPECTRAL_FFT_SWAP, "effect kind");
    switch (kind) {
        case CF_EFFECT_SPACE_TIME_TRANSPOSE:
            return chronoforge::space_time_transpose(
                input,
                checked_enum<chronoforge::SpatialAxis>(descriptor.options[0], 1, "transpose axis"));
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
                });
        case CF_EFFECT_TEMPORAL_PIXEL_SORT:
            return chronoforge::temporal_pixel_sort(
                input,
                {
                    checked_enum<chronoforge::SortCriterion>(descriptor.options[0], 2, "sort criterion"),
                    checked_enum<chronoforge::SortDirection>(descriptor.options[1], 1, "sort direction"),
                    descriptor.values[0],
                });
        case CF_EFFECT_TENSOR_3D_ROTATION:
            return chronoforge::tensor_3d_rotation(
                input,
                {
                    descriptor.values[0],
                    descriptor.values[1],
                    descriptor.values[2],
                    checked_enum<chronoforge::FillMode>(descriptor.options[0], 2, "fill mode"),
                });
        case CF_EFFECT_SPECTRAL_FFT_SWAP:
            return chronoforge::spectral_fft_swap(
                input,
                {
                    checked_enum<chronoforge::SpectralSwapAxis>(descriptor.options[0], 2, "FFT swap axis"),
                    descriptor.options[1] != 0,
                    static_cast<std::size_t>(std::min<uint64_t>(budget, std::numeric_limits<std::size_t>::max())),
                });
    }
    throw std::invalid_argument("Unsupported effect kind");
}

}  // namespace

extern "C" {

CFEffectDescriptor cf_effect_descriptor_make(
    int32_t kind,
    float value0,
    float value1,
    float value2,
    float value3,
    int32_t option0,
    int32_t option1,
    int32_t option2,
    int32_t option3) {
    return {kind, {value0, value1, value2, value3}, {option0, option1, option2, option3}};
}

const char* cf_core_version(void) { return "0.2.0"; }

int32_t cf_render_effect_chain(
    const float* input,
    uint64_t frames,
    uint64_t height,
    uint64_t width,
    uint64_t channels,
    uint32_t frame_rate_numerator,
    uint32_t frame_rate_denominator,
    const CFEffectDescriptor* effects,
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
            current = apply_effect(current, effects[index], max_working_set_bytes);
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
