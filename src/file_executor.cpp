#include "chronoforge/core/file_executor.hpp"

#include "chronoforge/core/effects.hpp"
#include "chronoforge/core/mapped_tensor.hpp"
#include "chronoforge/core/spectral.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace chronoforge {
namespace {

using LocalProgress = std::function<void(double)>;

template <typename Function>
void parallel_for(std::size_t count, const LocalProgress& progress, Function&& function) {
    if (count == 0) {
        return;
    }
    const auto hardware = std::max(1U, std::thread::hardware_concurrency());
    const auto worker_count = std::min<std::size_t>(count, std::min<std::size_t>(hardware, 16));
    const auto report_stride = std::max<std::size_t>(1, count / 100);
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> completed{0};
    std::atomic<bool> stopped{false};
    std::mutex state_mutex;
    std::size_t last_reported = 0;
    std::exception_ptr failure;

    auto worker = [&] {
        while (!stopped.load(std::memory_order_relaxed)) {
            const auto index = next.fetch_add(1, std::memory_order_relaxed);
            if (index >= count) {
                return;
            }
            try {
                function(index);
                const auto done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
                if (done == count || done % report_stride == 0) {
                    std::scoped_lock lock(state_mutex);
                    if (done > last_reported && !stopped.load(std::memory_order_relaxed)) {
                        progress(static_cast<double>(done) / static_cast<double>(count));
                        last_reported = done;
                    }
                }
            } catch (...) {
                std::scoped_lock lock(state_mutex);
                if (failure == nullptr) {
                    failure = std::current_exception();
                }
                stopped.store(true, std::memory_order_relaxed);
                return;
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers.emplace_back(worker);
    }
    for (auto& thread : workers) {
        thread.join();
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    if (last_reported < count) {
        progress(1.0);
    }
}

[[nodiscard]] std::size_t linear(std::size_t t, std::size_t y, std::size_t x, std::size_t c, const TensorShape& s) {
    return (((t * s.h) + y) * s.w + x) * s.c + c;
}

[[nodiscard]] float read(const MappedTensor& tensor, std::size_t t, std::size_t y, std::size_t x, std::size_t c) {
    return tensor.data()[linear(t, y, x, c, tensor.shape())];
}

[[nodiscard]] float luma(const MappedTensor& input, std::size_t t, std::size_t y, std::size_t x) {
    const auto& s = input.shape();
    const auto r = read(input, t, y, x, 0);
    if (s.c == 1) {
        return r;
    }
    const auto g = read(input, t, y, x, std::min<std::size_t>(1, s.c - 1));
    const auto b = read(input, t, y, x, std::min<std::size_t>(2, s.c - 1));
    return 0.2126F * r + 0.7152F * g + 0.0722F * b;
}

[[nodiscard]] float hue(const MappedTensor& input, std::size_t t, std::size_t y, std::size_t x) {
    if (input.shape().c < 3) {
        return 0.0F;
    }
    const auto r = read(input, t, y, x, 0);
    const auto g = read(input, t, y, x, 1);
    const auto b = read(input, t, y, x, 2);
    const auto maximum = std::max({r, g, b});
    const auto minimum = std::min({r, g, b});
    const auto delta = maximum - minimum;
    if (delta == 0.0F) {
        return 0.0F;
    }
    float value = maximum == r ? std::fmod((g - b) / delta, 6.0F)
                               : (maximum == g ? ((b - r) / delta) + 2.0F : ((r - g) / delta) + 4.0F);
    value /= 6.0F;
    return value < 0.0F ? value + 1.0F : value;
}

[[nodiscard]] float saturation(const MappedTensor& input, std::size_t t, std::size_t y, std::size_t x) {
    if (input.shape().c < 3) {
        return 0.0F;
    }
    const auto r = read(input, t, y, x, 0);
    const auto g = read(input, t, y, x, 1);
    const auto b = read(input, t, y, x, 2);
    const auto maximum = std::max({r, g, b});
    const auto minimum = std::min({r, g, b});
    return maximum == 0.0F ? 0.0F : (maximum - minimum) / maximum;
}

[[nodiscard]] std::size_t resolve_time(std::ptrdiff_t time, std::size_t count, EdgeBehavior behavior) {
    switch (behavior) {
        case EdgeBehavior::Clamp:
            return static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(time, 0, static_cast<std::ptrdiff_t>(count - 1)));
        case EdgeBehavior::Wrap: {
            const auto size = static_cast<std::ptrdiff_t>(count);
            auto value = time % size;
            return static_cast<std::size_t>(value < 0 ? value + size : value);
        }
        case EdgeBehavior::Mirror: {
            if (count == 1) {
                return 0;
            }
            const auto period = static_cast<std::ptrdiff_t>(2 * count - 2);
            auto value = time % period;
            if (value < 0) {
                value += period;
            }
            return static_cast<std::size_t>(value >= static_cast<std::ptrdiff_t>(count) ? period - value : value);
        }
    }
    throw std::invalid_argument("Invalid edge behavior");
}

[[nodiscard]] float resolve_fractional(float coordinate, std::size_t count, EdgeBehavior behavior) {
    if (count <= 1) return 0.0F;
    const auto maximum = static_cast<float>(count - 1);
    switch (behavior) {
        case EdgeBehavior::Clamp: return std::clamp(coordinate, 0.0F, maximum);
        case EdgeBehavior::Wrap: {
            const auto size = static_cast<float>(count);
            coordinate = std::fmod(coordinate, size);
            if (coordinate < 0.0F) coordinate += size;
            return std::min(coordinate, std::nextafter(size, 0.0F));
        }
        case EdgeBehavior::Mirror: {
            const auto period = 2.0F * maximum;
            coordinate = std::fmod(coordinate, period);
            if (coordinate < 0.0F) coordinate += period;
            return coordinate > maximum ? period - coordinate : coordinate;
        }
    }
    return 0.0F;
}

[[nodiscard]] float sample_mapped(
    const MappedTensor& input,
    float t,
    float y,
    float x,
    std::size_t channel,
    EdgeBehavior edge) {
    const auto& shape = input.shape();
    t = resolve_fractional(t, shape.t, edge);
    y = resolve_fractional(y, shape.h, edge);
    x = resolve_fractional(x, shape.w, edge);
    const auto t0 = static_cast<std::size_t>(std::floor(t));
    const auto y0 = static_cast<std::size_t>(std::floor(y));
    const auto x0 = static_cast<std::size_t>(std::floor(x));
    const auto t1 = resolve_time(static_cast<std::ptrdiff_t>(t0 + 1), shape.t, edge);
    const auto y1 = resolve_time(static_cast<std::ptrdiff_t>(y0 + 1), shape.h, edge);
    const auto x1 = resolve_time(static_cast<std::ptrdiff_t>(x0 + 1), shape.w, edge);
    const auto ft = t - static_cast<float>(t0);
    const auto fy = y - static_cast<float>(y0);
    const auto fx = x - static_cast<float>(x0);
    const auto c00 = read(input, t0, y0, x0, channel) + (read(input, t0, y0, x1, channel) - read(input, t0, y0, x0, channel)) * fx;
    const auto c01 = read(input, t0, y1, x0, channel) + (read(input, t0, y1, x1, channel) - read(input, t0, y1, x0, channel)) * fx;
    const auto c10 = read(input, t1, y0, x0, channel) + (read(input, t1, y0, x1, channel) - read(input, t1, y0, x0, channel)) * fx;
    const auto c11 = read(input, t1, y1, x0, channel) + (read(input, t1, y1, x1, channel) - read(input, t1, y1, x0, channel)) * fx;
    return (c00 + (c01 - c00) * fy) + ((c10 + (c11 - c10) * fy) - (c00 + (c01 - c00) * fy)) * ft;
}

[[nodiscard]] float cubic_weight(float distance) {
    distance = std::abs(distance);
    if (distance <= 1.0F) return 1.5F * distance * distance * distance - 2.5F * distance * distance + 1.0F;
    if (distance < 2.0F) return -0.5F * distance * distance * distance + 2.5F * distance * distance - 4.0F * distance + 2.0F;
    return 0.0F;
}

[[nodiscard]] float sample_mapped_cubic(
    const MappedTensor& input,
    float t,
    float y,
    float x,
    std::size_t channel) {
    const auto& shape = input.shape();
    const auto base_t = static_cast<std::ptrdiff_t>(std::floor(t));
    const auto base_y = static_cast<std::ptrdiff_t>(std::floor(y));
    const auto base_x = static_cast<std::ptrdiff_t>(std::floor(x));
    float value = 0.0F;
    float total = 0.0F;
    for (std::ptrdiff_t dt = -1; dt <= 2; ++dt) {
        const auto st = resolve_time(base_t + dt, shape.t, EdgeBehavior::Clamp);
        const auto wt = cubic_weight(t - static_cast<float>(base_t + dt));
        for (std::ptrdiff_t dy = -1; dy <= 2; ++dy) {
            const auto sy = resolve_time(base_y + dy, shape.h, EdgeBehavior::Clamp);
            const auto wy = cubic_weight(y - static_cast<float>(base_y + dy));
            for (std::ptrdiff_t dx = -1; dx <= 2; ++dx) {
                const auto sx = resolve_time(base_x + dx, shape.w, EdgeBehavior::Clamp);
                const auto weight = wt * wy * cubic_weight(x - static_cast<float>(base_x + dx));
                value += read(input, st, sy, sx, channel) * weight;
                total += weight;
            }
        }
    }
    return std::abs(total) <= std::numeric_limits<float>::epsilon() ? value : value / total;
}

[[nodiscard]] float sort_key(
    const MappedTensor& input,
    std::size_t t,
    std::size_t y,
    std::size_t x,
    SortCriterion criterion) {
    switch (criterion) {
        case SortCriterion::Luma:
            return luma(input, t, y, x);
        case SortCriterion::Hue:
            return hue(input, t, y, x);
        case SortCriterion::Saturation:
            return saturation(input, t, y, x);
    }
    throw std::invalid_argument("Invalid sort criterion");
}

[[nodiscard]] float shift_key(
    const MappedTensor& input,
    std::size_t t,
    std::size_t y,
    std::size_t x,
    ShiftSource source) {
    switch (source) {
        case ShiftSource::Luma:
            return luma(input, t, y, x);
        case ShiftSource::Red:
            return read(input, t, y, x, 0);
        case ShiftSource::Green:
            return read(input, t, y, x, std::min<std::size_t>(1, input.shape().c - 1));
        case ShiftSource::Blue:
            return read(input, t, y, x, std::min<std::size_t>(2, input.shape().c - 1));
        case ShiftSource::Alpha:
            if (input.shape().c < 4) {
                throw std::invalid_argument("Alpha time shift requires RGBA input");
            }
            return read(input, t, y, x, 3);
    }
    throw std::invalid_argument("Invalid shift source");
}

void require_option(std::int32_t value, std::int32_t maximum, const char* name) {
    if (value < 0 || value > maximum) {
        throw std::invalid_argument(std::string("Invalid ") + name + " option");
    }
}

void report(const FileRenderProgress& progress, double fraction, std::string_view stage) {
    if (progress && !progress(std::clamp(fraction, 0.0, 1.0), stage)) {
        throw std::runtime_error("Render cancelled");
    }
}

[[nodiscard]] TensorShape output_shape(const TensorShape& input, const EffectSpec& effect) {
    switch (effect.kind) {
        case EffectOperation::SpaceTimeTranspose:
            require_option(effect.options[0], 1, "transpose axis");
            require_option(effect.options[1], 1, "transpose resolution");
            if (effect.options[0] == 0) {
                return {input.w, input.h, effect.options[1] == 1 ? input.w : input.t, input.c};
            }
            return {input.h, effect.options[1] == 1 ? input.h : input.t, input.w, input.c};
        case EffectOperation::SpectralFftSwap:
            require_option(effect.options[0], 2, "FFT axis");
            require_option(effect.options[2], 1, "FFT resolution");
            require_option(effect.options[3], 1, "FFT transform");
            if (effect.options[3] == 1) {
                return input;
            }
            if (effect.options[2] == 1) {
                return input;
            }
            if (effect.options[0] == 0) {
                return {input.w, input.h, input.t, input.c};
            }
            if (effect.options[0] == 1) {
                return {input.h, input.t, input.w, input.c};
            }
            return {input.h, input.w, input.t, input.c};
        case EffectOperation::SeamlessLoop: {
            require_option(effect.options[0], 2, "seamless loop mode");
            if (effect.options[0] == static_cast<std::int32_t>(SeamlessLoopMode::PingPong)) {
                return {input.t <= 1 ? 1 : input.t * 2 - 2, input.h, input.w, input.c};
            }
            if (input.t <= 1) return input;
            const auto requested = static_cast<std::size_t>(std::max(1.0F, std::round(effect.values[0])));
            const auto transition = std::min(requested, std::max<std::size_t>(1, input.t / 2));
            return {input.t - transition, input.h, input.w, input.c};
        }
        default:
            return input;
    }
}

[[nodiscard]] std::array<std::int32_t, 3> normalized_space_time_axes(
    std::array<std::int32_t, 3> selections) {
    std::array<bool, 3> used{};
    for (auto& selection : selections) {
        selection = std::clamp(selection, 0, 5);
        auto semantic = static_cast<std::size_t>(selection % 3);
        if (used[semantic]) {
            const auto replacement = std::find(used.begin(), used.end(), false);
            semantic = static_cast<std::size_t>(std::distance(used.begin(), replacement));
            selection = (selection >= 3 ? 3 : 0) + static_cast<std::int32_t>(semantic);
        }
        used[semantic] = true;
    }
    return selections;
}

void transpose(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    const auto& s = output.shape();
    const auto& source_shape = input.shape();
    const auto fit = effect.options[1] == 1;
    auto* destination = output.mutable_data();
    parallel_for(s.t, progress, [&](std::size_t t) {
        for (std::size_t y = 0; y < s.h; ++y) {
            for (std::size_t x = 0; x < s.w; ++x) {
                for (std::size_t c = 0; c < s.c; ++c) {
                    if (!fit) {
                        destination[linear(t, y, x, c, s)] = effect.options[0] == 0 ? read(input, x, y, t, c)
                                                                                     : read(input, y, t, x, c);
                        continue;
                    }
                    const auto extent = effect.options[0] == 0 ? s.w : s.h;
                    const auto coordinate = effect.options[0] == 0 ? x : y;
                    const auto source_time = extent == 1
                                                 ? 0.0F
                                                 : static_cast<float>(coordinate) * static_cast<float>(source_shape.t - 1) /
                                                       static_cast<float>(extent - 1);
                    const auto time0 = static_cast<std::size_t>(std::floor(source_time));
                    const auto time1 = std::min(time0 + 1, source_shape.t - 1);
                    const auto fraction = source_time - static_cast<float>(time0);
                    const auto value0 = effect.options[0] == 0 ? read(input, time0, y, t, c) : read(input, time0, t, x, c);
                    const auto value1 = effect.options[0] == 0 ? read(input, time1, y, t, c) : read(input, time1, t, x, c);
                    destination[linear(t, y, x, c, s)] = value0 + (value1 - value0) * fraction;
                }
            }
        }
    });
}

void time_shift(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 4, "shift source");
    require_option(effect.options[1], 2, "edge behavior");
    const auto source = static_cast<ShiftSource>(effect.options[0]);
    const auto edge = static_cast<EdgeBehavior>(effect.options[1]);
    const auto& s = input.shape();
    auto* destination = output.mutable_data();
    parallel_for(s.t, progress, [&](std::size_t t) {
        for (std::size_t y = 0; y < s.h; ++y) {
            for (std::size_t x = 0; x < s.w; ++x) {
                const auto amount = static_cast<std::ptrdiff_t>(std::floor(effect.values[0] * shift_key(input, t, y, x, source)));
                const auto source_t = resolve_time(static_cast<std::ptrdiff_t>(t) - amount, s.t, edge);
                for (std::size_t c = 0; c < s.c; ++c) {
                    destination[linear(t, y, x, c, s)] = read(input, source_t, y, x, c);
                }
            }
        }
    });
}

void radial(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 2, "edge behavior");
    require_option(effect.options[1], 2, "radial topology");
    const auto edge = static_cast<EdgeBehavior>(effect.options[0]);
    const auto topology = static_cast<RadialTopology>(effect.options[1]);
    const auto& s = input.shape();
    const auto center_x = effect.values[0] * static_cast<float>(s.w - 1);
    const auto center_y = effect.values[1] * static_cast<float>(s.h - 1);
    const auto width = static_cast<float>(std::max<std::size_t>(1, s.w - 1));
    const auto height = static_cast<float>(std::max<std::size_t>(1, s.h - 1));
    auto* destination = output.mutable_data();
    parallel_for(s.t, progress, [&](std::size_t t) {
        const auto phase = static_cast<float>(t) / static_cast<float>(std::max<std::size_t>(1, s.t));
        for (std::size_t y = 0; y < s.h; ++y) {
            for (std::size_t x = 0; x < s.w; ++x) {
                const auto dx = (static_cast<float>(x) - center_x) / width;
                const auto dy = (static_cast<float>(y) - center_y) / height;
                const auto radius = std::sqrt(dx * dx + dy * dy);
                const auto turns = std::atan2(dy, dx) / (2.0F * std::numbers::pi_v<float>);
                const auto tau = 2.0F * std::numbers::pi_v<float>;
                const auto warp_gain = std::clamp(std::abs(effect.values[2]) * 5.0F, 0.0F, 1.25F);
                float weave{};
                float source_radius = radius;
                float source_turns = turns;
                switch (topology) {
                    case RadialTopology::TimeLoom: {
                        const auto strand_a = std::sin(tau * (3.0F * turns + 2.0F * radius - 2.0F * phase));
                        const auto strand_b = std::cos(tau * (5.0F * radius - turns + 3.0F * phase));
                        source_turns += effect.values[3] * (0.055F + 0.10F * radius) * strand_a + 0.045F * warp_gain * strand_b;
                        source_radius = std::max(0.0F, radius * (1.0F + 0.22F * warp_gain * strand_b) + 0.035F * warp_gain * strand_a);
                        weave = radius + effect.values[3] * turns + 0.34F * strand_a + 0.18F * strand_b;
                        break;
                    }
                    case RadialTopology::KaleidoFold: {
                        const auto segment = std::fmod(turns * 7.0F + 1000.0F, 1.0F);
                        const auto folded = std::abs(segment - 0.5F) * 2.0F;
                        source_turns = (std::floor(turns * 7.0F) + folded + 0.18F * std::sin(tau * (phase + radius))) / 7.0F;
                        source_radius = std::max(0.0F, radius + 0.08F * warp_gain * std::sin(tau * (14.0F * turns - 2.0F * phase)));
                        weave = 1.4F * folded + radius + effect.values[3] * std::sin(tau * (7.0F * turns + phase));
                        break;
                    }
                    case RadialTopology::EventHorizon: {
                        const auto gravity = std::exp(-4.0F * radius);
                        source_turns += (0.12F + 0.18F * effect.values[3]) * gravity / (radius + 0.045F) + 0.08F * phase;
                        source_radius = std::max(0.0F, radius + warp_gain * gravity * (0.11F * std::sin(tau * (phase * 3.0F - radius * 6.0F)) - 0.07F));
                        weave = std::clamp(0.18F / (radius + 0.045F), 0.0F, 3.5F) +
                                effect.values[3] * std::sin(tau * (6.0F * turns + 2.0F * phase)) * gravity;
                        break;
                    }
                }
                const auto source_angle = tau * source_turns;
                const auto source_x = center_x + std::cos(source_angle) * source_radius * width;
                const auto source_y = center_y + std::sin(source_angle) * source_radius * height;
                const auto source_t = static_cast<float>(t) + effect.values[2] * static_cast<float>(s.t) * weave;
                for (std::size_t c = 0; c < s.c; ++c) {
                    destination[linear(t, y, x, c, s)] = sample_mapped(input, source_t, source_y, source_x, c, edge);
                }
            }
        }
    });
}

void pixel_sort(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 2, "sort criterion");
    require_option(effect.options[1], 1, "sort direction");
    const auto criterion = static_cast<SortCriterion>(effect.options[0]);
    const auto descending = effect.options[1] == 1;
    const auto& s = input.shape();
    std::memcpy(output.mutable_data(), input.data(), s.byte_count());
    parallel_for(s.h, progress, [&](std::size_t y) {
        for (std::size_t x = 0; x < s.w; ++x) {
            std::vector<std::size_t> positions;
            positions.reserve(s.t);
            for (std::size_t t = 0; t < s.t; ++t) {
                if (sort_key(input, t, y, x, criterion) >= effect.values[0]) {
                    positions.push_back(t);
                }
            }
            auto sorted = positions;
            std::stable_sort(sorted.begin(), sorted.end(), [&](auto left, auto right) {
                const auto a = sort_key(input, left, y, x, criterion);
                const auto b = sort_key(input, right, y, x, criterion);
                return descending ? a > b : a < b;
            });
            for (std::size_t index = 0; index < positions.size(); ++index) {
                for (std::size_t c = 0; c < s.c; ++c) {
                    output.mutable_data()[linear(positions[index], y, x, c, s)] = read(input, sorted[index], y, x, c);
                }
            }
        }
    });
}

void rotate_plane(float& a, float& b, float degrees) {
    const auto radians = degrees * std::numbers::pi_v<float> / 180.0F;
    const auto old = a;
    a = std::cos(radians) * old - std::sin(radians) * b;
    b = std::sin(radians) * old + std::cos(radians) * b;
}

[[nodiscard]] float normalized(std::size_t value, std::size_t extent) {
    return extent == 1 ? 0.0F : 2.0F * static_cast<float>(value) / static_cast<float>(extent - 1) - 1.0F;
}

[[nodiscard]] float denormalized(float value, std::size_t extent) {
    return extent == 1 ? 0.0F : (value + 1.0F) * 0.5F * static_cast<float>(extent - 1);
}

[[nodiscard]] float wrap_coordinate(float value, std::size_t extent) {
    auto result = std::fmod(value, static_cast<float>(extent));
    return result < 0.0F ? result + static_cast<float>(extent) : result;
}

[[nodiscard]] float trilinear(const MappedTensor& input, float t, float y, float x, std::size_t c, FillMode fill) {
    const auto& s = input.shape();
    if (fill == FillMode::Repeat) {
        t = wrap_coordinate(t, s.t);
        y = wrap_coordinate(y, s.h);
        x = wrap_coordinate(x, s.w);
    } else if (fill == FillMode::Fit) {
        t = std::clamp(t, 0.0F, static_cast<float>(s.t - 1));
        y = std::clamp(y, 0.0F, static_cast<float>(s.h - 1));
        x = std::clamp(x, 0.0F, static_cast<float>(s.w - 1));
    } else if (t < 0 || y < 0 || x < 0 || t > static_cast<float>(s.t - 1) || y > static_cast<float>(s.h - 1) ||
               x > static_cast<float>(s.w - 1)) {
        return fill == FillMode::Black && c == 3 && s.c >= 4 ? 1.0F : 0.0F;
    }
    const auto t0 = static_cast<std::size_t>(std::floor(t));
    const auto y0 = static_cast<std::size_t>(std::floor(y));
    const auto x0 = static_cast<std::size_t>(std::floor(x));
    const auto t1 = std::min(t0 + 1, s.t - 1);
    const auto y1 = std::min(y0 + 1, s.h - 1);
    const auto x1 = std::min(x0 + 1, s.w - 1);
    const auto ft = t - static_cast<float>(t0);
    const auto fy = y - static_cast<float>(y0);
    const auto fx = x - static_cast<float>(x0);
    const auto c00 = read(input, t0, y0, x0, c) + (read(input, t0, y0, x1, c) - read(input, t0, y0, x0, c)) * fx;
    const auto c01 = read(input, t0, y1, x0, c) + (read(input, t0, y1, x1, c) - read(input, t0, y1, x0, c)) * fx;
    const auto c10 = read(input, t1, y0, x0, c) + (read(input, t1, y0, x1, c) - read(input, t1, y0, x0, c)) * fx;
    const auto c11 = read(input, t1, y1, x0, c) + (read(input, t1, y1, x1, c) - read(input, t1, y1, x0, c)) * fx;
    const auto lower = c00 + (c01 - c00) * fy;
    const auto upper = c10 + (c11 - c10) * fy;
    return lower + (upper - lower) * ft;
}

struct Point3 {
    float t;
    float y;
    float x;
};

void inverse_rotate(Point3& point, const EffectSpec& effect) {
    rotate_plane(point.y, point.t, -effect.values[2]);
    rotate_plane(point.x, point.t, -effect.values[1]);
    rotate_plane(point.x, point.y, -effect.values[0]);
}

[[nodiscard]] float rotation_fit_scale(const EffectSpec& effect) {
    std::array<Point3, 3> basis{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    for (auto& point : basis) {
        inverse_rotate(point, effect);
    }
    const auto t = std::abs(basis[0].t) + std::abs(basis[1].t) + std::abs(basis[2].t);
    const auto y = std::abs(basis[0].y) + std::abs(basis[1].y) + std::abs(basis[2].y);
    const auto x = std::abs(basis[0].x) + std::abs(basis[1].x) + std::abs(basis[2].x);
    return 1.0F / std::max({1.0F, t, y, x});
}

void rotation(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 3, "fill mode");
    const auto fill = static_cast<FillMode>(effect.options[0]);
    const auto& s = input.shape();
    const auto fit_scale = fill == FillMode::Fit ? rotation_fit_scale(effect) : 1.0F;
    auto* destination = output.mutable_data();
    parallel_for(s.t, progress, [&](std::size_t t) {
        for (std::size_t y = 0; y < s.h; ++y) {
            for (std::size_t x = 0; x < s.w; ++x) {
                float pt = normalized(t, s.t);
                float py = normalized(y, s.h);
                float px = normalized(x, s.w);
                Point3 point{pt * fit_scale, py * fit_scale, px * fit_scale};
                inverse_rotate(point, effect);
                for (std::size_t c = 0; c < s.c; ++c) {
                    destination[linear(t, y, x, c, s)] = trilinear(
                        input, denormalized(point.t, s.t), denormalized(point.y, s.h), denormalized(point.x, s.w), c, fill);
                }
            }
        }
    });
}

using Complex = std::complex<float>;

[[nodiscard]] std::size_t next_power_of_two(std::size_t value) {
    if (value == 0) {
        throw std::invalid_argument("FFT line cannot be empty");
    }
    std::size_t result = 1;
    while (result < value) {
        if (result > std::numeric_limits<std::size_t>::max() / 2) {
            throw std::overflow_error("FFT line is too large");
        }
        result <<= 1U;
    }
    return result;
}

void fft_power_of_two(std::vector<Complex>& values, bool inverse) {
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
        const Complex step{std::cos(angle), std::sin(angle)};
        for (std::size_t base = 0; base < count; base += length) {
            Complex phase{1, 0};
            for (std::size_t offset = 0; offset < length / 2; ++offset) {
                const auto even = values[base + offset];
                const auto odd = values[base + offset + length / 2] * phase;
                values[base + offset] = even + odd;
                values[base + offset + length / 2] = even - odd;
                phase *= step;
            }
        }
    }
    if (inverse) {
        for (auto& value : values) {
            value /= static_cast<float>(count);
        }
    }
}

void fft_any_length(std::vector<Complex>& values, bool inverse) {
    const auto count = values.size();
    if ((count & (count - 1)) == 0) {
        fft_power_of_two(values, inverse);
        return;
    }
    if (count > std::numeric_limits<std::size_t>::max() / 2) {
        throw std::overflow_error("FFT line is too large");
    }
    const auto convolution_size = next_power_of_two(2 * count - 1);
    std::vector<Complex> left(convolution_size, {0, 0});
    std::vector<Complex> right(convolution_size, {0, 0});
    const auto sign = inverse ? 1.0F : -1.0F;
    for (std::size_t index = 0; index < count; ++index) {
        const auto reduced = std::fmod(static_cast<double>(index) * static_cast<double>(index), static_cast<double>(2 * count));
        const auto angle = static_cast<float>(std::numbers::pi * reduced / static_cast<double>(count));
        const Complex chirp{std::cos(sign * angle), std::sin(sign * angle)};
        const Complex inverse_chirp{std::cos(-sign * angle), std::sin(-sign * angle)};
        left[index] = values[index] * chirp;
        right[index] = inverse_chirp;
        if (index != 0) {
            right[convolution_size - index] = inverse_chirp;
        }
    }
    fft_power_of_two(left, false);
    fft_power_of_two(right, false);
    for (std::size_t index = 0; index < convolution_size; ++index) {
        left[index] *= right[index];
    }
    fft_power_of_two(left, true);
    for (std::size_t index = 0; index < count; ++index) {
        const auto reduced = std::fmod(static_cast<double>(index) * static_cast<double>(index), static_cast<double>(2 * count));
        const auto angle = static_cast<float>(std::numbers::pi * reduced / static_cast<double>(count));
        const Complex chirp{std::cos(sign * angle), std::sin(sign * angle)};
        values[index] = left[index] * chirp;
        if (inverse) {
            values[index] /= static_cast<float>(count);
        }
    }
}

enum class DiskFftAxis { Time, Height, Width };

[[nodiscard]] Complex read_complex(
    const MappedTensor& tensor,
    const TensorShape& logical,
    std::size_t t,
    std::size_t y,
    std::size_t x,
    std::size_t channel) {
    static_cast<void>(logical);
    const auto base = linear(t, y, x, channel * 2, tensor.shape());
    return {tensor.data()[base], tensor.data()[base + 1]};
}

void write_complex(
    MappedTensor& tensor,
    const TensorShape& logical,
    std::size_t t,
    std::size_t y,
    std::size_t x,
    std::size_t channel,
    Complex value) {
    static_cast<void>(logical);
    const auto base = linear(t, y, x, channel * 2, tensor.shape());
    tensor.mutable_data()[base] = value.real();
    tensor.mutable_data()[base + 1] = value.imag();
}

void transform_disk_axis(
    MappedTensor& tensor,
    const TensorShape& logical,
    DiskFftAxis axis,
    bool inverse,
    std::size_t budget,
    const LocalProgress& progress) {
    const auto extent = axis == DiskFftAxis::Time ? logical.t : (axis == DiskFftAxis::Height ? logical.h : logical.w);
    const auto line_count = axis == DiskFftAxis::Time
                                ? logical.h * logical.w * logical.c
                                : (axis == DiskFftAxis::Height ? logical.t * logical.w * logical.c
                                                               : logical.t * logical.h * logical.c);
    const auto convolution = (extent & (extent - 1)) == 0 ? extent : next_power_of_two(2 * extent - 1);
    const auto per_worker = (extent + 2 * convolution) * sizeof(Complex);
    if (per_worker > budget) {
        throw std::runtime_error("A single FFT line exceeds the configured working memory budget");
    }
    auto transform_line = [&](std::size_t line_index) {
        const auto channel = line_index % logical.c;
        const auto position = line_index / logical.c;
        std::size_t fixed_a{};
        std::size_t fixed_b{};
        if (axis == DiskFftAxis::Time) {
            fixed_a = position / logical.w;
            fixed_b = position % logical.w;
        } else if (axis == DiskFftAxis::Height) {
            fixed_a = position / logical.w;
            fixed_b = position % logical.w;
        } else {
            fixed_a = position / logical.h;
            fixed_b = position % logical.h;
        }
        std::vector<Complex> line_values(extent);
        for (std::size_t index = 0; index < extent; ++index) {
            const auto t = axis == DiskFftAxis::Time ? index : fixed_a;
            const auto y = axis == DiskFftAxis::Height ? index : (axis == DiskFftAxis::Time ? fixed_a : fixed_b);
            const auto x = axis == DiskFftAxis::Width ? index : fixed_b;
            line_values[index] = read_complex(tensor, logical, t, y, x, channel);
        }
        fft_any_length(line_values, inverse);
        for (std::size_t index = 0; index < extent; ++index) {
            const auto t = axis == DiskFftAxis::Time ? index : fixed_a;
            const auto y = axis == DiskFftAxis::Height ? index : (axis == DiskFftAxis::Time ? fixed_a : fixed_b);
            const auto x = axis == DiskFftAxis::Width ? index : fixed_b;
            write_complex(tensor, logical, t, y, x, channel, line_values[index]);
        }
    };
    const auto parallel_workers = std::min<std::size_t>(line_count, std::min<std::size_t>(std::max(1U, std::thread::hardware_concurrency()), 16));
    if (per_worker <= budget / std::max<std::size_t>(1, parallel_workers)) {
        parallel_for(line_count, progress, transform_line);
        return;
    }
    const auto stride = std::max<std::size_t>(1, line_count / 100);
    for (std::size_t line = 0; line < line_count; ++line) {
        transform_line(line);
        if ((line + 1) % stride == 0 || line + 1 == line_count) {
            progress(static_cast<double>(line + 1) / static_cast<double>(line_count));
        }
    }
}

[[nodiscard]] TensorShape native_spectral_shape(const TensorShape& input, SpectralSwapAxis axis) {
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

void transpose_disk_spectrum(
    const MappedTensor& input,
    const TensorShape& input_shape,
    MappedTensor& output,
    const TensorShape& output_shape,
    SpectralSwapAxis axis,
    const LocalProgress& progress) {
    parallel_for(output_shape.t, progress, [&](std::size_t t) {
        for (std::size_t y = 0; y < output_shape.h; ++y) {
            for (std::size_t x = 0; x < output_shape.w; ++x) {
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
                for (std::size_t c = 0; c < output_shape.c; ++c) {
                    write_complex(output, output_shape, t, y, x, c, read_complex(input, input_shape, source_t, source_y, source_x, c));
                }
            }
        }
    });
}

[[nodiscard]] float wrap_float(float value, std::size_t extent) {
    value = std::fmod(value, static_cast<float>(extent));
    return value < 0.0F ? value + static_cast<float>(extent) : value;
}

[[nodiscard]] Complex sample_complex_periodic(
    const MappedTensor& input,
    const TensorShape& shape,
    float t,
    float y,
    float x,
    std::size_t channel) {
    t = wrap_float(t, shape.t);
    y = wrap_float(y, shape.h);
    x = wrap_float(x, shape.w);
    const auto t0 = static_cast<std::size_t>(std::floor(t));
    const auto y0 = static_cast<std::size_t>(std::floor(y));
    const auto x0 = static_cast<std::size_t>(std::floor(x));
    const auto t1 = (t0 + 1) % shape.t;
    const auto y1 = (y0 + 1) % shape.h;
    const auto x1 = (x0 + 1) % shape.w;
    const auto ft = t - static_cast<float>(t0);
    const auto fy = y - static_cast<float>(y0);
    const auto fx = x - static_cast<float>(x0);
    const auto value = [&](std::size_t st, std::size_t sy, std::size_t sx) {
        return read_complex(input, shape, st, sy, sx, channel);
    };
    const auto c00 = value(t0, y0, x0) + (value(t0, y0, x1) - value(t0, y0, x0)) * fx;
    const auto c01 = value(t0, y1, x0) + (value(t0, y1, x1) - value(t0, y1, x0)) * fx;
    const auto c10 = value(t1, y0, x0) + (value(t1, y0, x1) - value(t1, y0, x0)) * fx;
    const auto c11 = value(t1, y1, x0) + (value(t1, y1, x1) - value(t1, y1, x0)) * fx;
    return (c00 + (c01 - c00) * fy) + ((c10 + (c11 - c10) * fy) - (c00 + (c01 - c00) * fy)) * ft;
}

void rotate_disk_spectrum(
    const MappedTensor& input,
    const TensorShape& shape,
    MappedTensor& output,
    SpectralSwapAxis plane,
    float angle_degrees,
    const LocalProgress& progress) {
    const auto radians = -angle_degrees * std::numbers::pi_v<float> / 180.0F;
    const auto cosine = std::cos(radians);
    const auto sine = std::sin(radians);
    const auto frequency = [](std::size_t index, std::size_t extent) {
        const auto signed_index = index <= extent / 2
                                      ? static_cast<float>(index)
                                      : static_cast<float>(static_cast<std::ptrdiff_t>(index) - static_cast<std::ptrdiff_t>(extent));
        return signed_index / static_cast<float>(extent);
    };
    const auto coordinate = [](float normalized, std::size_t extent) {
        return wrap_float(normalized * static_cast<float>(extent), extent);
    };
    parallel_for(shape.t, progress, [&](std::size_t t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                auto source_t = frequency(t, shape.t);
                auto source_y = frequency(y, shape.h);
                auto source_x = frequency(x, shape.w);
                auto rotate_pair = [&](float& first, float& second) {
                    const auto original_first = first;
                    first = cosine * first - sine * second;
                    second = sine * original_first + cosine * second;
                };
                switch (plane) {
                    case SpectralSwapAxis::XTime: rotate_pair(source_x, source_t); break;
                    case SpectralSwapAxis::YTime: rotate_pair(source_y, source_t); break;
                    case SpectralSwapAxis::AllAxes: rotate_pair(source_x, source_y); break;
                }
                for (std::size_t c = 0; c < shape.c; ++c) {
                    write_complex(output, shape, t, y, x, c, sample_complex_periodic(
                        input, shape, coordinate(source_t, shape.t), coordinate(source_y, shape.h), coordinate(source_x, shape.w), c));
                }
            }
        }
    });
}

[[nodiscard]] float scaled_coordinate(std::size_t value, std::size_t destination_extent, std::size_t source_extent) {
    return destination_extent <= 1 || source_extent <= 1
               ? 0.0F
               : static_cast<float>(value) * static_cast<float>(source_extent - 1) /
                     static_cast<float>(destination_extent - 1);
}

[[nodiscard]] float sample_complex_real(
    const MappedTensor& input,
    const TensorShape& shape,
    float t,
    float y,
    float x,
    std::size_t channel) {
    const auto t0 = static_cast<std::size_t>(std::floor(t));
    const auto y0 = static_cast<std::size_t>(std::floor(y));
    const auto x0 = static_cast<std::size_t>(std::floor(x));
    const auto t1 = std::min(t0 + 1, shape.t - 1);
    const auto y1 = std::min(y0 + 1, shape.h - 1);
    const auto x1 = std::min(x0 + 1, shape.w - 1);
    const auto ft = t - static_cast<float>(t0);
    const auto fy = y - static_cast<float>(y0);
    const auto fx = x - static_cast<float>(x0);
    const auto value = [&](std::size_t st, std::size_t sy, std::size_t sx) {
        return read_complex(input, shape, st, sy, sx, channel).real();
    };
    const auto c00 = value(t0, y0, x0) + (value(t0, y0, x1) - value(t0, y0, x0)) * fx;
    const auto c01 = value(t0, y1, x0) + (value(t0, y1, x1) - value(t0, y1, x0)) * fx;
    const auto c10 = value(t1, y0, x0) + (value(t1, y0, x1) - value(t1, y0, x0)) * fx;
    const auto c11 = value(t1, y1, x0) + (value(t1, y1, x1) - value(t1, y1, x0)) * fx;
    return (c00 + (c01 - c00) * fy) + ((c10 + (c11 - c10) * fy) - (c00 + (c01 - c00) * fy)) * ft;
}

void spectral(
    const MappedTensor& input,
    MappedTensor& output,
    const EffectSpec& effect,
    std::size_t budget,
    const VideoTensorMetadata& metadata,
    const LocalProgress& progress) {
    static_cast<void>(metadata);
    require_option(effect.options[0], 2, "FFT axis");
    require_option(effect.options[2], 1, "FFT resolution");
    require_option(effect.options[3], 1, "FFT transform");
    const auto axis = static_cast<SpectralSwapAxis>(effect.options[0]);
    const auto transform = static_cast<SpectralTransform>(effect.options[3]);
    const auto native_shape = transform == SpectralTransform::Rotate ? input.shape() : native_spectral_shape(input.shape(), axis);
    const TensorShape input_complex_shape{input.shape().t, input.shape().h, input.shape().w, input.shape().c * 2};
    const TensorShape output_complex_shape{native_shape.t, native_shape.h, native_shape.w, native_shape.c * 2};
    const auto first_path = output.path().string() + ".fft-forward";
    const auto second_path = output.path().string() + ".fft-inverse";
    if (input_complex_shape.byte_count() > std::numeric_limits<std::size_t>::max() - output_complex_shape.byte_count()) {
        throw std::overflow_error("FFT temporary disk requirement is too large");
    }
    const auto required_disk = input_complex_shape.byte_count() + output_complex_shape.byte_count();
    const auto disk = std::filesystem::space(output.path().parent_path());
    constexpr std::uintmax_t reserve = 512ULL * 1024ULL * 1024ULL;
    if (disk.available < required_disk || disk.available - required_disk < reserve) {
        throw std::runtime_error("Full-resolution FFT needs additional SSD space for two temporary frequency tensors");
    }
    try {
        {
            auto forward = MappedTensor::create(first_path, input_complex_shape);
            parallel_for(input.shape().t, [&](double fraction) { progress(fraction * 0.05); }, [&](std::size_t t) {
                for (std::size_t y = 0; y < input.shape().h; ++y) {
                    for (std::size_t x = 0; x < input.shape().w; ++x) {
                        for (std::size_t c = 0; c < input.shape().c; ++c) {
                            write_complex(forward, input.shape(), t, y, x, c, {read(input, t, y, x, c), 0});
                        }
                    }
                }
            });
            transform_disk_axis(forward, input.shape(), DiskFftAxis::Width, false, budget, [&](double value) { progress(0.05 + value * 0.10); });
            transform_disk_axis(forward, input.shape(), DiskFftAxis::Height, false, budget, [&](double value) { progress(0.15 + value * 0.10); });
            transform_disk_axis(forward, input.shape(), DiskFftAxis::Time, false, budget, [&](double value) { progress(0.25 + value * 0.10); });
            forward.sync();
            {
                auto inverse = MappedTensor::create(second_path, output_complex_shape);
                if (transform == SpectralTransform::Rotate) {
                    rotate_disk_spectrum(
                        forward, input.shape(), inverse, axis, effect.values[0],
                        [&](double value) { progress(0.35 + value * 0.15); });
                } else {
                    transpose_disk_spectrum(
                        forward, input.shape(), inverse, native_shape, axis,
                        [&](double value) { progress(0.35 + value * 0.15); });
                }
                inverse.sync();
                transform_disk_axis(inverse, native_shape, DiskFftAxis::Width, true, budget, [&](double value) { progress(0.50 + value * 0.10); });
                transform_disk_axis(inverse, native_shape, DiskFftAxis::Height, true, budget, [&](double value) { progress(0.60 + value * 0.10); });
                transform_disk_axis(inverse, native_shape, DiskFftAxis::Time, true, budget, [&](double value) { progress(0.70 + value * 0.10); });
                parallel_for(output.shape().t, [&](double value) { progress(0.80 + value * 0.15); }, [&](std::size_t t) {
                    const auto source_t = scaled_coordinate(t, output.shape().t, native_shape.t);
                    for (std::size_t y = 0; y < output.shape().h; ++y) {
                        const auto source_y = scaled_coordinate(y, output.shape().h, native_shape.h);
                        for (std::size_t x = 0; x < output.shape().w; ++x) {
                            const auto source_x = scaled_coordinate(x, output.shape().w, native_shape.w);
                            for (std::size_t c = 0; c < output.shape().c; ++c) {
                                output.mutable_data()[linear(t, y, x, c, output.shape())] =
                                    sample_complex_real(inverse, native_shape, source_t, source_y, source_x, c);
                            }
                        }
                    }
                });
            }
        }
        std::filesystem::remove(first_path);
        std::filesystem::remove(second_path);
        if (effect.options[1] != 0) {
            auto* values = output.mutable_data();
            const auto count = output.shape().element_count();
            const auto [minimum, maximum] = std::minmax_element(values, values + count);
            const auto minimum_value = *minimum;
            const auto span = *maximum - minimum_value;
            if (std::abs(span) > std::numeric_limits<float>::epsilon()) {
                parallel_for(count, [&](double value) { progress(0.95 + value * 0.05); }, [&](std::size_t index) {
                    values[index] = (values[index] - minimum_value) / span;
                });
            }
        }
        progress(1.0);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(first_path, ignored);
        std::filesystem::remove(second_path, ignored);
        throw;
    }
}

void prefilter(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 2, "spatial prefilter");
    require_option(effect.options[1], 2, "temporal prefilter");
    const auto amount = [](std::int32_t strength) {
        switch (strength) {
            case 0: return 0.0F;
            case 1: return 0.2F;
            case 2: return 0.4F;
            default: throw std::invalid_argument("Invalid prefilter strength");
        }
    };
    const auto spatial = amount(effect.options[0]);
    const auto temporal = amount(effect.options[1]);
    const auto& shape = input.shape();
    auto* destination = output.mutable_data();
    const auto clamped = [](std::size_t value, int offset, std::size_t extent) {
        return static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(
            static_cast<std::ptrdiff_t>(value) + offset, 0, static_cast<std::ptrdiff_t>(extent - 1)));
    };
    parallel_for(shape.t, progress, [&](std::size_t t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                for (std::size_t c = 0; c < shape.c; ++c) {
                    const auto center = read(input, t, y, x, c);
                    auto value = center;
                    if (spatial > 0.0F) {
                        const auto average = (read(input, t, clamped(y, -1, shape.h), x, c) +
                                              read(input, t, clamped(y, 1, shape.h), x, c) +
                                              read(input, t, y, clamped(x, -1, shape.w), c) +
                                              read(input, t, y, clamped(x, 1, shape.w), c)) *
                                             0.25F;
                        value += spatial * (average - center);
                    }
                    if (temporal > 0.0F) {
                        const auto average = (read(input, clamped(t, -1, shape.t), y, x, c) +
                                              read(input, clamped(t, 1, shape.t), y, x, c)) *
                                             0.5F;
                        value += temporal * (average - center);
                    }
                    destination[linear(t, y, x, c, shape)] = value;
                }
            }
        }
    });
}

void optical_flow_warp(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 2, "flow edge behavior");
    const auto edge = static_cast<EdgeBehavior>(effect.options[0]);
    const auto& shape = input.shape();
    const auto clamped = [](std::size_t value, int offset, std::size_t extent) {
        return static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(
            static_cast<std::ptrdiff_t>(value) + offset, 0, static_cast<std::ptrdiff_t>(extent - 1)));
    };
    auto* destination = output.mutable_data();
    parallel_for(shape.t, progress, [&](std::size_t t) {
        const auto next_t = std::min(t + 1, shape.t - 1);
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                float xx = 0.0F, yy = 0.0F, xy = 0.0F, xt = 0.0F, yt = 0.0F;
                for (int oy = -1; oy <= 1; ++oy) {
                    const auto sy = clamped(y, oy, shape.h);
                    for (int ox = -1; ox <= 1; ++ox) {
                        const auto sx = clamped(x, ox, shape.w);
                        const auto ix = 0.5F * (luma(input, t, sy, clamped(sx, 1, shape.w)) -
                                                luma(input, t, sy, clamped(sx, -1, shape.w)));
                        const auto iy = 0.5F * (luma(input, t, clamped(sy, 1, shape.h), sx) -
                                                luma(input, t, clamped(sy, -1, shape.h), sx));
                        const auto it = luma(input, next_t, sy, sx) - luma(input, t, sy, sx);
                        xx += ix * ix; yy += iy * iy; xy += ix * iy; xt += ix * it; yt += iy * it;
                    }
                }
                const auto determinant = (xx + 0.0001F) * (yy + 0.0001F) - xy * xy;
                const auto flow_x = determinant == 0.0F ? 0.0F : (-xt * (yy + 0.0001F) + xy * yt) / determinant;
                const auto flow_y = determinant == 0.0F ? 0.0F : (xy * xt - (xx + 0.0001F) * yt) / determinant;
                auto magnitude = std::sqrt(flow_x * flow_x + flow_y * flow_y);
                const auto angle = std::atan2(flow_y, flow_x) * 180.0F / std::numbers::pi_v<float>;
                auto difference = std::fmod(std::abs(angle - effect.values[2]), 360.0F);
                difference = std::min(difference, 360.0F - difference);
                if (difference > std::clamp(effect.values[3], 0.0F, 180.0F)) magnitude = 0.0F;
                magnitude = std::max(0.0F, magnitude - std::max(0.0F, effect.values[0]));
                const auto source_t = static_cast<float>(t) + effect.values[1] * magnitude;
                for (std::size_t c = 0; c < shape.c; ++c) {
                    destination[linear(t, y, x, c, shape)] = sample_mapped(
                        input, source_t, static_cast<float>(y), static_cast<float>(x), c, edge);
                }
            }
        }
    });
}

void feedback(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 5, "feedback blend mode");
    const auto mode = static_cast<FeedbackBlendMode>(effect.options[0]);
    const auto past_delay = static_cast<std::size_t>(std::max(0.0F, std::round(effect.values[0])));
    const auto future_delay = static_cast<std::size_t>(std::max(0.0F, std::round(effect.values[2])));
    const auto past_amount = std::clamp(effect.values[1], 0.0F, 1.0F);
    const auto future_amount = std::clamp(effect.values[3], 0.0F, 1.0F);
    const auto& shape = input.shape();
    auto* destination = output.mutable_data();
    const auto blend = [&](float base, float layer) {
        switch (mode) {
            case FeedbackBlendMode::Add: return base + layer;
            case FeedbackBlendMode::Screen: return 1.0F - (1.0F - base) * (1.0F - layer);
            case FeedbackBlendMode::Multiply: return base * layer;
            case FeedbackBlendMode::Lighten: return std::max(base, layer);
            case FeedbackBlendMode::Difference: return std::abs(base - layer);
            case FeedbackBlendMode::Displace: return base;
        }
        return base;
    };
    for (std::size_t t = 0; t < shape.t; ++t) {
        const auto has_past = past_delay > 0 && t >= past_delay;
        const auto past_t = has_past ? t - past_delay : 0;
        const auto future_t = std::min(t + future_delay, shape.t - 1);
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                if (mode == FeedbackBlendMode::Displace) {
                    const auto mapped_luma = [&](std::size_t sample_t, bool from_output) {
                        const auto channel = [&](std::size_t c) {
                            return from_output ? destination[linear(sample_t, y, x, c, shape)]
                                               : read(input, sample_t, y, x, c);
                        };
                        if (shape.c >= 3) return 0.2126F * channel(0) + 0.7152F * channel(1) + 0.0722F * channel(2);
                        return channel(0);
                    };
                    const auto past_luma = has_past ? mapped_luma(past_t, true) : 0.5F;
                    const auto future_luma = mapped_luma(future_t, false);
                    const auto source_x = static_cast<float>(x) +
                        (past_luma - 0.5F) * 0.5F * past_amount * static_cast<float>(shape.w);
                    const auto source_y = static_cast<float>(y) +
                        (future_luma - 0.5F) * 0.5F * future_amount * static_cast<float>(shape.h);
                    for (std::size_t c = 0; c < shape.c; ++c) {
                        destination[linear(t, y, x, c, shape)] = sample_mapped(
                            input, static_cast<float>(t), source_y, source_x, c, EdgeBehavior::Clamp);
                    }
                    continue;
                }
                for (std::size_t c = 0; c < shape.c; ++c) {
                    const auto current = read(input, t, y, x, c);
                    const auto past = has_past ? destination[linear(past_t, y, x, c, shape)] : current;
                    const auto future = read(input, future_t, y, x, c);
                    if (mode == FeedbackBlendMode::Add) {
                        destination[linear(t, y, x, c, shape)] =
                            current * std::max(0.0F, 1.0F - past_amount - future_amount) +
                            past * past_amount + future * future_amount;
                    } else {
                        auto value = current + (blend(current, past) - current) * past_amount;
                        value += (blend(value, future) - value) * future_amount;
                        destination[linear(t, y, x, c, shape)] = value;
                    }
                }
            }
        }
        progress(static_cast<double>(t + 1) / static_cast<double>(shape.t));
    }
}

void seamless_loop(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 2, "seamless loop mode");
    const auto mode = static_cast<SeamlessLoopMode>(effect.options[0]);
    const auto& source_shape = input.shape();
    const auto& result_shape = output.shape();
    auto* destination = output.mutable_data();
    if (mode == SeamlessLoopMode::PingPong) {
        parallel_for(result_shape.t, progress, [&](std::size_t t) {
            const auto source_t = t < source_shape.t ? t : (2 * source_shape.t - 2 - t);
            for (std::size_t y = 0; y < source_shape.h; ++y) {
                for (std::size_t x = 0; x < source_shape.w; ++x) {
                    for (std::size_t c = 0; c < source_shape.c; ++c) {
                        destination[linear(t, y, x, c, result_shape)] = read(input, source_t, y, x, c);
                    }
                }
            }
        });
        return;
    }
    const auto transition = source_shape.t - result_shape.t;
    const auto softness = std::clamp(effect.values[1], 0.01F, 0.5F);
    const auto smoothstep = [](float edge0, float edge1, float value) {
        const auto normalized = std::clamp((value - edge0) / std::max(0.0001F, edge1 - edge0), 0.0F, 1.0F);
        return normalized * normalized * (3.0F - 2.0F * normalized);
    };
    parallel_for(result_shape.t, progress, [&](std::size_t t) {
        for (std::size_t y = 0; y < source_shape.h; ++y) {
            for (std::size_t x = 0; x < source_shape.w; ++x) {
                if (t >= transition) {
                    for (std::size_t c = 0; c < source_shape.c; ++c) {
                        destination[linear(t, y, x, c, result_shape)] = read(input, t, y, x, c);
                    }
                    continue;
                }
                const auto tail_t = t + result_shape.t;
                const auto fraction = transition <= 1 ? 0.5F : static_cast<float>(t) / static_cast<float>(transition - 1);
                auto mix = smoothstep(0.0F, 1.0F, fraction);
                if (mode == SeamlessLoopMode::LumaWeave && transition > 1) {
                    if (t == 0) {
                        mix = 0.0F;
                    } else if (t + 1 == transition) {
                        mix = 1.0F;
                    } else {
                        const auto threshold = std::clamp(
                            0.5F * (luma(input, t, y, x) + luma(input, tail_t, y, x)), 0.0F, 1.0F);
                        mix = smoothstep(threshold - softness, threshold + softness, fraction);
                    }
                }
                for (std::size_t c = 0; c < source_shape.c; ++c) {
                    const auto tail = read(input, tail_t, y, x, c);
                    destination[linear(t, y, x, c, result_shape)] =
                        tail + (read(input, t, y, x, c) - tail) * mix;
                }
            }
        }
    });
}

void datamosh(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 2, "freeze axis");
    require_option(effect.options[1], 2, "freeze trigger");
    const auto axis = static_cast<FreezeAxis>(effect.options[0]);
    const auto trigger = static_cast<FreezeTrigger>(effect.options[1]);
    const auto max_hold = static_cast<std::size_t>(std::max(0.0F, std::round(effect.values[1])));
    const auto& shape = input.shape();
    std::memcpy(output.mutable_data(), input.data(), shape.byte_count());
    const auto should_trigger = [&](std::size_t t, std::size_t y, std::size_t x,
                                    std::size_t pt, std::size_t py, std::size_t px) {
        if (trigger == FreezeTrigger::Edge) return std::abs(luma(input, t, y, x) - luma(input, pt, py, px)) > effect.values[0];
        if (trigger == FreezeTrigger::Luma) return luma(input, t, y, x) >= effect.values[0];
        std::uint64_t hash = (static_cast<std::uint64_t>(t) + 1) * 0x9E3779B185EBCA87ULL;
        hash ^= (static_cast<std::uint64_t>(y) + 1) * 0xC2B2AE3D27D4EB4FULL;
        hash ^= (static_cast<std::uint64_t>(x) + 1) * 0x165667B19E3779F9ULL;
        hash ^= hash >> 29U;
        return static_cast<double>(hash & 0xFFFFFFU) / static_cast<double>(0x1000000U) <
               std::clamp(static_cast<double>(effect.values[2]), 0.0, 1.0);
    };
    const auto process_line = [&](std::size_t length, auto coordinate) {
        std::size_t remaining = 0;
        for (std::size_t index = 1; index < length; ++index) {
            const auto [t, y, x] = coordinate(index);
            const auto [pt, py, px] = coordinate(index - 1);
            if (remaining == 0 && should_trigger(t, y, x, pt, py, px)) remaining = max_hold;
            if (remaining > 0) {
                for (std::size_t c = 0; c < shape.c; ++c) {
                    output.mutable_data()[linear(t, y, x, c, shape)] = output.data()[linear(pt, py, px, c, shape)];
                }
                --remaining;
            }
        }
    };
    if (axis == FreezeAxis::Time) {
        parallel_for(shape.h * shape.w, progress, [&](std::size_t line) {
            const auto y = line / shape.w, x = line % shape.w;
            process_line(shape.t, [&](std::size_t i) { return std::array<std::size_t, 3>{i, y, x}; });
        });
    } else if (axis == FreezeAxis::Horizontal) {
        parallel_for(shape.t * shape.h, progress, [&](std::size_t line) {
            const auto t = line / shape.h, y = line % shape.h;
            process_line(shape.w, [&](std::size_t i) { return std::array<std::size_t, 3>{t, y, i}; });
        });
    } else {
        parallel_for(shape.t * shape.w, progress, [&](std::size_t line) {
            const auto t = line / shape.w, x = line % shape.w;
            process_line(shape.h, [&](std::size_t i) { return std::array<std::size_t, 3>{t, i, x}; });
        });
    }
}

void apply(
    const MappedTensor& input,
    MappedTensor& output,
    const EffectSpec& effect,
    std::size_t budget,
    const VideoTensorMetadata& metadata,
    const LocalProgress& progress) {
    switch (effect.kind) {
        case EffectOperation::SpaceTimeTranspose:
            transpose(input, output, effect, progress);
            return;
        case EffectOperation::LumaTimeShift:
            time_shift(input, output, effect, progress);
            return;
        case EffectOperation::RadialChronoFunnel:
            radial(input, output, effect, progress);
            return;
        case EffectOperation::TemporalPixelSort:
            pixel_sort(input, output, effect, progress);
            return;
        case EffectOperation::Tensor3dRotation:
            rotation(input, output, effect, progress);
            return;
        case EffectOperation::SpectralFftSwap:
            spectral(input, output, effect, budget, metadata, progress);
            return;
        case EffectOperation::SelectivePrefilter:
            prefilter(input, output, effect, progress);
            return;
        case EffectOperation::OpticalFlowTimeWarp:
            optical_flow_warp(input, output, effect, progress);
            return;
        case EffectOperation::ChronoFeedback:
            feedback(input, output, effect, progress);
            return;
        case EffectOperation::StructuralDatamosh:
            datamosh(input, output, effect, progress);
            return;
        case EffectOperation::SeamlessLoop:
            seamless_loop(input, output, effect, progress);
            return;
        case EffectOperation::DimensionalSplicer:
        case EffectOperation::TensorDisplacement:
            throw std::invalid_argument("This effect requires a driver tensor");
    }
    throw std::invalid_argument("Invalid effect operation");
}

[[nodiscard]] std::string stage_name(EffectOperation operation) {
    switch (operation) {
        case EffectOperation::SpaceTimeTranspose:
            return "Space-Time Transform";
        case EffectOperation::LumaTimeShift:
            return "Self Time Displacement";
        case EffectOperation::RadialChronoFunnel:
            return "Polar Time Warp";
        case EffectOperation::TemporalPixelSort:
            return "Pixel Sort (Time)";
        case EffectOperation::Tensor3dRotation:
            return "Space-Time Transform";
        case EffectOperation::SpectralFftSwap:
            return "3D FFT Transform";
        case EffectOperation::SelectivePrefilter:
            return "Output Prefilter";
        case EffectOperation::DimensionalSplicer:
            return "Space-Time Map";
        case EffectOperation::TensorDisplacement:
            return "Space-Time Displacement";
        case EffectOperation::OpticalFlowTimeWarp:
            return "Optical Flow Time Warp";
        case EffectOperation::ChronoFeedback:
            return "Time Feedback";
        case EffectOperation::StructuralDatamosh:
            return "Axis Datamosh";
        case EffectOperation::SeamlessLoop:
            return "Seamless Loop";
    }
    return "Unknown";
}

void ensure_disk_space(const std::filesystem::path& directory, std::size_t bytes) {
    const auto info = std::filesystem::space(directory);
    constexpr std::uintmax_t reserve = 512ULL * 1024ULL * 1024ULL;
    if (info.available < bytes || info.available - bytes < reserve) {
        throw std::runtime_error("Not enough free disk space for the next tensor cache file");
    }
}

}  // namespace

FileRenderResult render_file_effect_chain(
    const std::filesystem::path& input_path,
    const std::filesystem::path& output_path,
    const std::filesystem::path& scratch_directory,
    TensorShape input_shape,
    VideoTensorMetadata metadata,
    std::span<const EffectSpec> effects,
    std::size_t max_working_set_bytes,
    const FileRenderProgress& progress) {
    if (max_working_set_bytes == 0) {
        throw std::invalid_argument("Working-set budget must be greater than zero");
    }
    std::filesystem::create_directories(scratch_directory);
    std::vector<std::filesystem::path> scratch_files;
    auto current_path = input_path;
    auto current_shape = input_shape;
    try {
        for (std::size_t index = 0; index < effects.size(); ++index) {
            const auto& effect = effects[index];
            const auto next_shape = output_shape(current_shape, effect);
            const auto next_path = scratch_directory / ("node-" + std::to_string(index) + ".raw");
            ensure_disk_space(scratch_directory, next_shape.byte_count());
            report(progress, static_cast<double>(index) / static_cast<double>(effects.size() + 1), stage_name(effect.kind));
            {
                auto input = MappedTensor::open(current_path, current_shape, MappedTensor::Access::ReadOnly);
                auto output = MappedTensor::create(next_path, next_shape);
                const auto denominator = static_cast<double>(effects.size() + 1);
                const auto stage = stage_name(effect.kind);
                const auto local_progress = [&](double fraction) {
                    report(progress, (static_cast<double>(index) + fraction) / denominator, stage);
                };
                apply(input, output, effect, max_working_set_bytes, metadata, local_progress);
                output.sync();
            }
            scratch_files.push_back(next_path);
            if (current_path != input_path) {
                std::filesystem::remove(current_path);
            }
            current_path = next_path;
            current_shape = next_shape;
        }

        report(progress, static_cast<double>(effects.size()) / static_cast<double>(effects.size() + 1), "Finalizing cache");
        const auto partial = output_path.string() + ".partial";
        std::filesystem::create_directories(output_path.parent_path());
        std::filesystem::copy_file(current_path, partial, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::rename(partial, output_path);
        if (current_path != input_path) {
            std::filesystem::remove(current_path);
        }
        report(progress, 1.0, "Complete");
        return {current_shape, metadata};
    } catch (...) {
        for (const auto& path : scratch_files) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
        std::error_code ignored;
        std::filesystem::remove(output_path.string() + ".partial", ignored);
        throw;
    }
}

FileRenderResult render_file_cross_tensor_effect(
    const std::filesystem::path& source_path,
    const std::filesystem::path& driver_path,
    const std::filesystem::path& output_path,
    TensorShape source_shape,
    TensorShape driver_shape,
    VideoTensorMetadata metadata,
    VideoTensorMetadata driver_metadata,
    const EffectSpec& effect,
    const FileRenderProgress& progress) {
    TensorShape result_shape = source_shape;
    if (effect.kind == EffectOperation::DimensionalSplicer) {
        require_option(effect.options[0], 5, "output X axis source");
        require_option(effect.options[1], 5, "output Y axis source");
        require_option(effect.options[2], 5, "output Time axis source");
        require_option(effect.options[3], 2, "splicer interpolation");
        const auto selections = normalized_space_time_axes({effect.options[0], effect.options[1], effect.options[2]});
        const auto extent = [&](std::int32_t selection) {
            const auto& shape = selection < 3 ? source_shape : driver_shape;
            if (selection % 3 == 0) return shape.w;
            if (selection % 3 == 1) return shape.h;
            return shape.t;
        };
        result_shape = {extent(selections[2]), extent(selections[1]), extent(selections[0]), source_shape.c};
    } else if (effect.kind == EffectOperation::TensorDisplacement) {
        require_option(effect.options[0], 4, "displacement source");
        require_option(effect.options[1], 2, "tensor broadcast");
        require_option(effect.options[2], 2, "displacement edge behavior");
        if (effect.options[1] == static_cast<std::int32_t>(TensorBroadcast::Crop)) {
            result_shape.t = std::min(source_shape.t, driver_shape.t);
            result_shape.h = std::min(source_shape.h, driver_shape.h);
            result_shape.w = std::min(source_shape.w, driver_shape.w);
        }
    } else {
        throw std::invalid_argument("The selected effect does not accept a driver tensor");
    }

    std::filesystem::create_directories(output_path.parent_path());
    ensure_disk_space(output_path.parent_path(), result_shape.byte_count());
    auto source = MappedTensor::open(source_path, source_shape, MappedTensor::Access::ReadOnly);
    auto driver = MappedTensor::open(driver_path, driver_shape, MappedTensor::Access::ReadOnly);
    auto output = MappedTensor::create(output_path, result_shape);
    auto* destination = output.mutable_data();

    if (effect.kind == EffectOperation::DimensionalSplicer) {
        const auto selections = normalized_space_time_axes({effect.options[0], effect.options[1], effect.options[2]});
        const auto scale = [](std::size_t coordinate, std::size_t output_extent, std::size_t input_extent) {
            return output_extent <= 1 || input_extent <= 1
                       ? 0.0F
                       : static_cast<float>(coordinate) * static_cast<float>(input_extent - 1) /
                             static_cast<float>(output_extent - 1);
        };
        parallel_for(result_shape.t, [&](double fraction) { report(progress, fraction, "Space-Time Map"); }, [&](std::size_t t) {
            for (std::size_t y = 0; y < result_shape.h; ++y) {
                for (std::size_t x = 0; x < result_shape.w; ++x) {
                    std::array<float, 3> source_coordinates{};
                    const std::array<std::size_t, 3> coordinates{x, y, t};
                    const std::array<std::size_t, 3> output_extents{result_shape.w, result_shape.h, result_shape.t};
                    const auto driver_t = scale(t, result_shape.t, driver_shape.t);
                    const auto driver_y = scale(y, result_shape.h, driver_shape.h);
                    const auto driver_x = scale(x, result_shape.w, driver_shape.w);
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        const auto semantic = static_cast<std::size_t>(selections[axis] % 3);
                        const auto input_extent = semantic == 0 ? source_shape.w : (semantic == 1 ? source_shape.h : source_shape.t);
                        if (selections[axis] < 3) {
                            source_coordinates[semantic] = scale(coordinates[axis], output_extents[axis], input_extent);
                        } else {
                            const auto map_channel = std::min(semantic, driver_shape.c - 1);
                            const auto map_value = std::clamp(sample_mapped(
                                driver, driver_t, driver_y, driver_x, map_channel, EdgeBehavior::Clamp), 0.0F, 1.0F);
                            source_coordinates[semantic] = map_value * static_cast<float>(input_extent - 1);
                        }
                    }
                    for (std::size_t c = 0; c < result_shape.c; ++c) {
                        float value{};
                        if (effect.options[3] == static_cast<std::int32_t>(TensorInterpolation::Nearest)) {
                            value = read(source,
                                static_cast<std::size_t>(std::round(source_coordinates[2])),
                                static_cast<std::size_t>(std::round(source_coordinates[1])),
                                static_cast<std::size_t>(std::round(source_coordinates[0])), c);
                        } else if (effect.options[3] == static_cast<std::int32_t>(TensorInterpolation::Cubic)) {
                            value = sample_mapped_cubic(source, source_coordinates[2], source_coordinates[1], source_coordinates[0], c);
                        } else {
                            value = sample_mapped(source, source_coordinates[2], source_coordinates[1], source_coordinates[0], c, EdgeBehavior::Clamp);
                        }
                        destination[linear(t, y, x, c, result_shape)] = value;
                    }
                }
            }
        });
    } else {
        const auto broadcast = static_cast<TensorBroadcast>(effect.options[1]);
        const auto edge = static_cast<EdgeBehavior>(effect.options[2]);
        const auto source_channel = static_cast<ShiftSource>(effect.options[0]);
        const auto driver_coordinate = [&](std::size_t coordinate, std::size_t output_extent, std::size_t driver_extent) {
            if (broadcast == TensorBroadcast::Stretch) {
                return output_extent <= 1 || driver_extent <= 1 ? std::size_t{0} : static_cast<std::size_t>(std::round(
                    static_cast<double>(coordinate) * static_cast<double>(driver_extent - 1) /
                    static_cast<double>(output_extent - 1)));
            }
            return std::min(coordinate, driver_extent - 1);
        };
        parallel_for(result_shape.t, [&](double fraction) { report(progress, fraction, "Space-Time Displacement"); }, [&](std::size_t t) {
            const auto dt = driver_coordinate(t, result_shape.t, driver_shape.t);
            for (std::size_t y = 0; y < result_shape.h; ++y) {
                const auto dy = driver_coordinate(y, result_shape.h, driver_shape.h);
                for (std::size_t x = 0; x < result_shape.w; ++x) {
                    const auto dx = driver_coordinate(x, result_shape.w, driver_shape.w);
                    const auto amount = shift_key(driver, dt, dy, dx, source_channel);
                    for (std::size_t c = 0; c < result_shape.c; ++c) {
                        destination[linear(t, y, x, c, result_shape)] = sample_mapped(
                            source,
                            static_cast<float>(t) + effect.values[0] * amount,
                            static_cast<float>(y) + effect.values[2] * amount,
                            static_cast<float>(x) + effect.values[1] * amount,
                            c, edge);
                    }
                }
            }
        });
    }
    output.sync();
    report(progress, 1.0, stage_name(effect.kind));
    const auto normalized_time = effect.kind == EffectOperation::DimensionalSplicer
                                     ? normalized_space_time_axes({effect.options[0], effect.options[1], effect.options[2]})[2]
                                     : -1;
    const auto result_metadata = normalized_time == 5 ? driver_metadata : metadata;
    return {result_shape, result_metadata};
}

}  // namespace chronoforge
