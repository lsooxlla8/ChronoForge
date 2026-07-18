#include "chronoforge/core/spectral_morph.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>

namespace chronoforge {
namespace {

using Complex = std::complex<float>;

std::size_t next_power_of_two(std::size_t value) {
    std::size_t result = 1;
    while (result < value) result <<= 1U;
    return result;
}

void fft(std::vector<Complex>& values, bool inverse) {
    const auto count = values.size();
    for (std::size_t index = 1, reversed = 0; index < count; ++index) {
        auto bit = count >> 1U;
        for (; (reversed & bit) != 0; bit >>= 1U) reversed ^= bit;
        reversed ^= bit;
        if (index < reversed) std::swap(values[index], values[reversed]);
    }
    for (std::size_t length = 2; length <= count; length <<= 1U) {
        const auto angle = (inverse ? 2.0F : -2.0F) * std::numbers::pi_v<float> /
                           static_cast<float>(length);
        const Complex step{std::cos(angle), std::sin(angle)};
        for (std::size_t base = 0; base < count; base += length) {
            Complex phase{1.0F, 0.0F};
            const auto half = length >> 1U;
            for (std::size_t offset = 0; offset < half; ++offset) {
                const auto even = values[base + offset];
                const auto odd = values[base + offset + half] * phase;
                values[base + offset] = even + odd;
                values[base + offset + half] = even - odd;
                phase *= step;
            }
        }
    }
    if (inverse) {
        for (auto& value : values) value /= static_cast<float>(count);
    }
}

void fft_2d(std::vector<Complex>& values, std::size_t height, std::size_t width, bool inverse) {
    std::vector<Complex> line(std::max(height, width));
    for (std::size_t y = 0; y < height; ++y) {
        std::copy_n(values.begin() + static_cast<std::ptrdiff_t>(y * width), width, line.begin());
        line.resize(width);
        fft(line, inverse);
        std::copy(line.begin(), line.end(), values.begin() + static_cast<std::ptrdiff_t>(y * width));
        line.resize(std::max(height, width));
    }
    line.resize(height);
    for (std::size_t x = 0; x < width; ++x) {
        for (std::size_t y = 0; y < height; ++y) line[y] = values[y * width + x];
        fft(line, inverse);
        for (std::size_t y = 0; y < height; ++y) values[y * width + x] = line[y];
    }
}

}  // namespace

std::vector<float> spectral_morph_frames(
    std::span<const float> tail,
    std::span<const float> head,
    std::size_t height,
    std::size_t width,
    std::size_t channels,
    float mix) {
    const auto count = height * width * channels;
    if (height == 0 || width == 0 || channels == 0 || tail.size() != count || head.size() != count) {
        throw std::invalid_argument("Spectral morph frames must have matching non-empty shapes");
    }
    mix = std::clamp(mix, 0.0F, 1.0F);
    if (mix == 0.0F) return {tail.begin(), tail.end()};
    if (mix == 1.0F) return {head.begin(), head.end()};

    const auto padded_height = next_power_of_two(height);
    const auto padded_width = next_power_of_two(width);
    const auto spectral_count = padded_height * padded_width;
    std::vector<float> output(count, 0.0F);
    std::vector<Complex> a(spectral_count), b(spectral_count);
    const auto has_alpha = channels >= 4;

    for (std::size_t channel = 0; channel < channels; ++channel) {
        if (has_alpha && channel == 3) continue;
        std::fill(a.begin(), a.end(), Complex{});
        std::fill(b.begin(), b.end(), Complex{});
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                const auto pixel = y * width + x;
                const auto index = pixel * channels + channel;
                const auto tail_alpha = has_alpha ? std::clamp(tail[pixel * channels + 3], 0.0F, 1.0F) : 1.0F;
                const auto head_alpha = has_alpha ? std::clamp(head[pixel * channels + 3], 0.0F, 1.0F) : 1.0F;
                a[y * padded_width + x] = channel < 3 && has_alpha && tail_alpha > 0.00001F
                    ? tail[index] / tail_alpha : tail[index];
                b[y * padded_width + x] = channel < 3 && has_alpha && head_alpha > 0.00001F
                    ? head[index] / head_alpha : head[index];
            }
        }
        fft_2d(a, padded_height, padded_width, false);
        fft_2d(b, padded_height, padded_width, false);
        for (std::size_t index = 0; index < spectral_count; ++index) {
            const auto magnitude = std::abs(a[index]) + (std::abs(b[index]) - std::abs(a[index])) * mix;
            const auto delta = std::remainder(std::arg(b[index]) - std::arg(a[index]), 2.0F * std::numbers::pi_v<float>);
            a[index] = std::polar(magnitude, std::arg(a[index]) + delta * mix);
        }
        fft_2d(a, padded_height, padded_width, true);
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                output[(y * width + x) * channels + channel] = a[y * padded_width + x].real();
            }
        }
    }

    for (std::size_t pixel = 0; pixel < height * width; ++pixel) {
        const auto base = pixel * channels;
        const auto alpha = has_alpha
            ? std::clamp(tail[base + 3] + (head[base + 3] - tail[base + 3]) * mix, 0.0F, 1.0F)
            : 1.0F;
        for (std::size_t channel = 0; channel < std::min<std::size_t>(3, channels); ++channel) {
            output[base + channel] = std::clamp(output[base + channel], 0.0F, 1.0F) * alpha;
        }
        if (has_alpha) output[base + 3] = alpha;
    }
    return output;
}

}  // namespace chronoforge
