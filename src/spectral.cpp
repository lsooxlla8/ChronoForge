#include "chronoforge/core/spectral.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace chronoforge {
namespace {

using Complex = std::complex<float>;

[[nodiscard]] std::size_t next_power_of_two(std::size_t value) {
    if (value == 0) {
        throw std::invalid_argument("FFT dimension cannot be zero");
    }
    std::size_t result = 1;
    while (result < value) {
        if (result > std::numeric_limits<std::size_t>::max() / 2) {
            throw std::overflow_error("FFT padded dimension overflows size_t");
        }
        result <<= 1U;
    }
    return result;
}

[[nodiscard]] std::size_t linear_index(
    std::size_t t,
    std::size_t y,
    std::size_t x,
    std::size_t c,
    const TensorShape& shape) {
    return (((t * shape.h) + y) * shape.w + x) * shape.c + c;
}

void fft_1d(std::vector<Complex>& values, bool inverse) {
    const auto count = values.size();
    for (std::size_t index = 1, reversed = 0; index < count; ++index) {
        auto bit = count >> 1U;
        for (; (reversed & bit) != 0; bit >>= 1U) {
            reversed ^= bit;
        }
        reversed ^= bit;
        if (index < reversed) {
            std::swap(values[index], values[reversed]);
        }
    }

    for (std::size_t length = 2; length <= count; length <<= 1U) {
        const auto angle = (inverse ? 2.0F : -2.0F) * std::numbers::pi_v<float> / static_cast<float>(length);
        const Complex phase_step{std::cos(angle), std::sin(angle)};
        for (std::size_t base = 0; base < count; base += length) {
            Complex phase{1.0F, 0.0F};
            const auto half = length >> 1U;
            for (std::size_t offset = 0; offset < half; ++offset) {
                const auto even = values[base + offset];
                const auto odd = values[base + offset + half] * phase;
                values[base + offset] = even + odd;
                values[base + offset + half] = even - odd;
                phase *= phase_step;
            }
        }
    }

    if (inverse) {
        for (auto& value : values) {
            value /= static_cast<float>(count);
        }
    }
}

enum class FftAxis { Time, Height, Width };

void fft_along_axis(std::vector<Complex>& values, const TensorShape& shape, FftAxis axis, bool inverse) {
    const auto extent = axis == FftAxis::Time ? shape.t : (axis == FftAxis::Height ? shape.h : shape.w);
    std::vector<Complex> line(extent);

    for (std::size_t t = 0; t < shape.t; ++t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                if ((axis == FftAxis::Time && t != 0) || (axis == FftAxis::Height && y != 0) ||
                    (axis == FftAxis::Width && x != 0)) {
                    continue;
                }
                for (std::size_t c = 0; c < shape.c; ++c) {
                    for (std::size_t i = 0; i < extent; ++i) {
                        const auto source_t = axis == FftAxis::Time ? i : t;
                        const auto source_y = axis == FftAxis::Height ? i : y;
                        const auto source_x = axis == FftAxis::Width ? i : x;
                        line[i] = values[linear_index(source_t, source_y, source_x, c, shape)];
                    }
                    fft_1d(line, inverse);
                    for (std::size_t i = 0; i < extent; ++i) {
                        const auto destination_t = axis == FftAxis::Time ? i : t;
                        const auto destination_y = axis == FftAxis::Height ? i : y;
                        const auto destination_x = axis == FftAxis::Width ? i : x;
                        values[linear_index(destination_t, destination_y, destination_x, c, shape)] = line[i];
                    }
                }
            }
        }
    }
}

void fft_3d(std::vector<Complex>& values, const TensorShape& shape, bool inverse) {
    fft_along_axis(values, shape, FftAxis::Width, inverse);
    fft_along_axis(values, shape, FftAxis::Height, inverse);
    fft_along_axis(values, shape, FftAxis::Time, inverse);
}

[[nodiscard]] TensorShape transposed_shape(const TensorShape& input, SpectralSwapAxis axis) {
    switch (axis) {
        case SpectralSwapAxis::XTime:
            return {input.w, input.h, input.t, input.c};
        case SpectralSwapAxis::YTime:
            return {input.h, input.t, input.w, input.c};
        case SpectralSwapAxis::AllAxes:
            return {input.h, input.w, input.t, input.c};
    }
    throw std::invalid_argument("Unknown spectral swap axis");
}

[[nodiscard]] std::vector<Complex> transpose_spectrum(
    const std::vector<Complex>& input,
    const TensorShape& input_shape,
    SpectralSwapAxis axis) {
    const auto output_shape = transposed_shape(input_shape, axis);
    std::vector<Complex> output(output_shape.element_count());
    for (std::size_t t = 0; t < output_shape.t; ++t) {
        for (std::size_t y = 0; y < output_shape.h; ++y) {
            for (std::size_t x = 0; x < output_shape.w; ++x) {
                for (std::size_t c = 0; c < output_shape.c; ++c) {
                    std::size_t source_t{};
                    std::size_t source_y{};
                    std::size_t source_x{};
                    switch (axis) {
                        case SpectralSwapAxis::XTime:
                            source_t = x;
                            source_y = y;
                            source_x = t;
                            break;
                        case SpectralSwapAxis::YTime:
                            source_t = y;
                            source_y = t;
                            source_x = x;
                            break;
                        case SpectralSwapAxis::AllAxes:
                            source_t = x;
                            source_y = t;
                            source_x = y;
                            break;
                    }
                    output[linear_index(t, y, x, c, output_shape)] = input[linear_index(source_t, source_y, source_x, c, input_shape)];
                }
            }
        }
    }
    return output;
}

void normalize_range(VideoTensor& output) {
    auto& values = output.values();
    if (values.empty()) {
        return;
    }
    const auto [minimum, maximum] = std::minmax_element(values.begin(), values.end());
    const auto minimum_value = *minimum;
    const auto span = *maximum - minimum_value;
    if (std::abs(span) < std::numeric_limits<float>::epsilon()) {
        return;
    }
    for (auto& value : values) {
        value = (value - minimum_value) / span;
    }
}

[[nodiscard]] float scaled_coordinate(std::size_t coordinate, std::size_t destination_extent, std::size_t source_extent) {
    if (destination_extent <= 1 || source_extent <= 1) {
        return 0.0F;
    }
    return static_cast<float>(coordinate) * static_cast<float>(source_extent - 1) /
           static_cast<float>(destination_extent - 1);
}

[[nodiscard]] VideoTensor resample_tensor(const VideoTensor& input, const TensorShape& output_shape) {
    VideoTensor output(output_shape, 0.0F, input.metadata());
    const auto& source = input.shape();
    for (std::size_t t = 0; t < output_shape.t; ++t) {
        const auto source_t = scaled_coordinate(t, output_shape.t, source.t);
        const auto t0 = static_cast<std::size_t>(std::floor(source_t));
        const auto t1 = std::min(t0 + 1, source.t - 1);
        const auto ft = source_t - static_cast<float>(t0);
        for (std::size_t y = 0; y < output_shape.h; ++y) {
            const auto source_y = scaled_coordinate(y, output_shape.h, source.h);
            const auto y0 = static_cast<std::size_t>(std::floor(source_y));
            const auto y1 = std::min(y0 + 1, source.h - 1);
            const auto fy = source_y - static_cast<float>(y0);
            for (std::size_t x = 0; x < output_shape.w; ++x) {
                const auto source_x = scaled_coordinate(x, output_shape.w, source.w);
                const auto x0 = static_cast<std::size_t>(std::floor(source_x));
                const auto x1 = std::min(x0 + 1, source.w - 1);
                const auto fx = source_x - static_cast<float>(x0);
                for (std::size_t c = 0; c < output_shape.c; ++c) {
                    const auto c00 = input.at(t0, y0, x0, c) + (input.at(t0, y0, x1, c) - input.at(t0, y0, x0, c)) * fx;
                    const auto c01 = input.at(t0, y1, x0, c) + (input.at(t0, y1, x1, c) - input.at(t0, y1, x0, c)) * fx;
                    const auto c10 = input.at(t1, y0, x0, c) + (input.at(t1, y0, x1, c) - input.at(t1, y0, x0, c)) * fx;
                    const auto c11 = input.at(t1, y1, x0, c) + (input.at(t1, y1, x1, c) - input.at(t1, y1, x0, c)) * fx;
                    output.at(t, y, x, c) = (c00 + (c01 - c00) * fy) +
                                             ((c10 + (c11 - c10) * fy) - (c00 + (c01 - c00) * fy)) * ft;
                }
            }
        }
    }
    return output;
}

}  // namespace

VideoTensor spectral_fft_swap(const VideoTensor& input, const SpectralSwapParams& params) {
    const auto& shape = input.shape();
    const TensorShape padded_shape{
        next_power_of_two(shape.t),
        next_power_of_two(shape.h),
        next_power_of_two(shape.w),
        shape.c,
    };
    const auto elements = padded_shape.element_count();
    if (elements > std::numeric_limits<std::size_t>::max() / (2 * sizeof(Complex)) ||
        elements * 2 * sizeof(Complex) > params.max_working_set_bytes) {
        throw std::runtime_error("3D FFT exceeds the configured working-set budget; render a smaller proxy or use the GPU backend");
    }

    std::vector<Complex> spectrum(elements, Complex{0.0F, 0.0F});
    for (std::size_t t = 0; t < shape.t; ++t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                for (std::size_t c = 0; c < shape.c; ++c) {
                    spectrum[linear_index(t, y, x, c, padded_shape)] = input.at(t, y, x, c);
                }
            }
        }
    }
    fft_3d(spectrum, padded_shape, false);

    const auto padded_output_shape = transposed_shape(padded_shape, params.axis);
    auto transposed = transpose_spectrum(spectrum, padded_shape, params.axis);
    fft_3d(transposed, padded_output_shape, true);

    const auto output_shape = transposed_shape(shape, params.axis);
    std::vector<float> values(output_shape.element_count());
    for (std::size_t t = 0; t < output_shape.t; ++t) {
        for (std::size_t y = 0; y < output_shape.h; ++y) {
            for (std::size_t x = 0; x < output_shape.w; ++x) {
                for (std::size_t c = 0; c < output_shape.c; ++c) {
                    values[linear_index(t, y, x, c, output_shape)] =
                        transposed[linear_index(t, y, x, c, padded_output_shape)].real();
                }
            }
        }
    }
    VideoTensor output(output_shape, std::move(values), input.metadata());
    if (params.resolution == SpectralResolution::FitSourceTensor) {
        auto fitted = resample_tensor(output, shape);
        if (params.normalize) {
            normalize_range(fitted);
        }
        return fitted;
    }
    if (params.normalize) {
        normalize_range(output);
    }
    return output;
}

}  // namespace chronoforge
