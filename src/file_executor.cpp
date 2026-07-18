#include "chronoforge/core/file_executor.hpp"

#include "chronoforge/core/effects.hpp"
#include "chronoforge/core/mapped_tensor.hpp"
#include "chronoforge/core/spectral.hpp"
#include "chronoforge/core/spectral_morph.hpp"

#include <algorithm>
#include <array>
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

void require_amount(float amount) {
    if (!std::isfinite(amount) || amount < 0.0F || amount > 1.0F) {
        throw std::invalid_argument("Effect amount must be between zero and one");
    }
}

void blend_amount(const MappedTensor& input, MappedTensor& output, float amount, AmountBlendMode mode) {
    if (input.shape() != output.shape()) {
        throw std::invalid_argument("Partial Amount requires an effect that preserves tensor shape");
    }
    const auto& shape = input.shape();
    const auto frame_values = shape.h * shape.w * shape.c;
    const auto* source = input.data();
    auto* destination = output.mutable_data();
    if (mode == AmountBlendMode::Displace) {
        parallel_for(shape.t, [](double) {}, [&](std::size_t t) {
            for (std::size_t y = 0; y < shape.h; ++y) {
                for (std::size_t x = 0; x < shape.w; ++x) {
                    const auto field_r = read(output, t, y, x, 0);
                    const auto field_g = read(output, t, y, x, std::min<std::size_t>(1, shape.c - 1));
                    const auto field_b = read(output, t, y, x, std::min<std::size_t>(2, shape.c - 1));
                    const auto st = static_cast<std::size_t>(std::clamp(
                        std::llround(static_cast<double>(t) + (field_b - 0.5F) * amount * 0.15F * static_cast<float>(shape.t)),
                        0LL, static_cast<long long>(shape.t - 1)));
                    const auto sy = static_cast<std::size_t>(std::clamp(
                        std::llround(static_cast<double>(y) + (field_g - 0.5F) * amount * 0.15F * static_cast<float>(shape.h)),
                        0LL, static_cast<long long>(shape.h - 1)));
                    const auto sx = static_cast<std::size_t>(std::clamp(
                        std::llround(static_cast<double>(x) + (field_r - 0.5F) * amount * 0.15F * static_cast<float>(shape.w)),
                        0LL, static_cast<long long>(shape.w - 1)));
                    for (std::size_t c = 0; c < shape.c; ++c) {
                        destination[linear(t, y, x, c, shape)] = read(input, st, sy, sx, c);
                    }
                }
            }
        });
        return;
    }
    const auto composite = [mode](float base, float layer) {
        switch (mode) {
            case AmountBlendMode::Normal: return layer;
            case AmountBlendMode::Add: return base + layer;
            case AmountBlendMode::Screen: return 1.0F - (1.0F - base) * (1.0F - layer);
            case AmountBlendMode::Multiply: return base * layer;
            case AmountBlendMode::Difference: return std::abs(base - layer);
            case AmountBlendMode::XorGlitch: {
                constexpr auto maximum = 4095U;
                const auto a = static_cast<std::uint32_t>(std::llround(std::clamp(base, 0.0F, 1.0F) * maximum));
                const auto b = static_cast<std::uint32_t>(std::llround(std::clamp(layer, 0.0F, 1.0F) * maximum));
                return static_cast<float>((a ^ b) & maximum) / static_cast<float>(maximum);
            }
            case AmountBlendMode::Displace: return base;
        }
        return layer;
    };
    parallel_for(shape.t, [](double) {}, [&](std::size_t t) {
        const auto start = t * frame_values;
        const auto end = start + frame_values;
        for (auto index = start; index < end; ++index) {
            destination[index] = source[index] + (composite(source[index], destination[index]) - source[index]) * amount;
        }
    });
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
            require_option(effect.options[0], 4, "seamless loop mode");
            require_option(effect.options[1], 1, "loop transition placement");
            require_option(effect.options[2], 2, "spectral phase mode");
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

void rgb_time_slip(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 2, "RGB split axis");
    require_option(effect.options[1], 2, "RGB time slip edge behavior");
    const auto axis = static_cast<SplitAxis>(effect.options[0]);
    const auto edge = static_cast<EdgeBehavior>(effect.options[1]);
    const auto& shape = input.shape();
    if (shape.c < 3) throw std::invalid_argument("RGB Time Slip requires at least three channels");
    const std::array<float, 3> time_offsets{effect.values[0], effect.values[1], effect.values[2]};
    const std::array<float, 3> split_scales{1.0F, 0.0F, -1.0F};
    const auto center_x = 0.5F * static_cast<float>(shape.w - 1);
    const auto center_y = 0.5F * static_cast<float>(shape.h - 1);
    auto* destination = output.mutable_data();
    parallel_for(shape.t, progress, [&](std::size_t t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                const auto output_alpha = shape.c >= 4 ? read(input, t, y, x, 3) : 1.0F;
                for (std::size_t c = 0; c < 3; ++c) {
                    auto source_x = static_cast<float>(x);
                    auto source_y = static_cast<float>(y);
                    const auto split = effect.values[3] * split_scales[c];
                    if (axis == SplitAxis::Horizontal) {
                        source_x -= split;
                    } else if (axis == SplitAxis::Vertical) {
                        source_y -= split;
                    } else {
                        const auto dx = static_cast<float>(x) - center_x;
                        const auto dy = static_cast<float>(y) - center_y;
                        const auto length = std::sqrt(dx * dx + dy * dy);
                        if (length > 0.00001F) {
                            source_x -= split * dx / length;
                            source_y -= split * dy / length;
                        }
                    }
                    const auto source_t = static_cast<float>(t) - time_offsets[c];
                    auto value = sample_mapped(input, source_t, source_y, source_x, c, edge);
                    if (shape.c >= 4) {
                        const auto source_alpha = sample_mapped(input, source_t, source_y, source_x, 3, edge);
                        value = source_alpha > 0.00001F ? value / source_alpha * output_alpha : 0.0F;
                    }
                    destination[linear(t, y, x, c, shape)] = value;
                }
                if (shape.c >= 4) destination[linear(t, y, x, 3, shape)] = output_alpha;
                for (std::size_t c = 4; c < shape.c; ++c) {
                    destination[linear(t, y, x, c, shape)] = read(input, t, y, x, c);
                }
            }
        }
    });
}

void horizontal_sync_loss(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 2, "sync loss driver");
    require_option(effect.options[1], 2, "sync loss edge behavior");
    require_option(effect.options[2], 1, "sync loss axis");
    const auto driver_kind = static_cast<SyncLossDriver>(effect.options[0]);
    const auto edge = static_cast<EdgeBehavior>(effect.options[1]);
    const auto horizontal = static_cast<SyncLossAxis>(effect.options[2]) == SyncLossAxis::Horizontal;
    const auto& shape = input.shape();
    const auto band_extent = horizontal ? shape.h : shape.w;
    const auto shift_extent = horizontal ? shape.w : shape.h;
    const auto band_size = 1 + static_cast<std::size_t>(std::round(
        std::clamp(effect.values[1], 0.0F, 1.0F) * static_cast<float>(band_extent - 1)));
    const auto density = std::clamp(effect.values[3], 0.0F, 1.0F);
    const auto maximum_shift = std::clamp(effect.values[0], 0.0F, 1.0F) * static_cast<float>(shift_extent);
    const auto mix_hash = [&](std::size_t t, std::int64_t band) {
        auto hash = (static_cast<std::uint64_t>(t) + 1) * 0x9E3779B185EBCA87ULL;
        hash ^= static_cast<std::uint64_t>(band) * 0xC2B2AE3D27D4EB4FULL;
        hash ^= effect.random_seed * 0xD6E8FEB86659FD93ULL;
        hash ^= hash >> 30U;
        hash *= 0xBF58476D1CE4E5B9ULL;
        hash ^= hash >> 27U;
        return hash;
    };
    auto* destination = output.mutable_data();
    parallel_for(shape.t, progress, [&](std::size_t t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                const auto coordinate = horizontal ? y : x;
                const auto base_band = static_cast<std::int64_t>(coordinate / band_size);
                const auto band = base_band + static_cast<std::int64_t>(std::floor(static_cast<float>(t) * effect.values[2]));
                const auto center_coordinate = std::min((coordinate / band_size) * band_size + band_size / 2, band_extent - 1);
                const auto center_y = horizontal ? center_coordinate : shape.h / 2;
                const auto center_x = horizontal ? shape.w / 2 : center_coordinate;
                float driver = 0.0F;
                bool tears = density > 0.0F;
                if (driver_kind == SyncLossDriver::DeterministicNoise) {
                    const auto hash = mix_hash(t, band);
                    const auto trigger = static_cast<float>(hash & 0xFFFFFFU) / static_cast<float>(0x1000000U);
                    driver = 2.0F * static_cast<float>((hash >> 24U) & 0xFFFFFFU) / static_cast<float>(0xFFFFFFU) - 1.0F;
                    tears = tears && trigger < density;
                } else if (driver_kind == SyncLossDriver::Luma) {
                    driver = 2.0F * std::clamp(luma(input, t, center_y, center_x), 0.0F, 1.0F) - 1.0F;
                    tears = tears && std::abs(driver) >= 1.0F - density;
                } else {
                    const auto previous_y = horizontal && center_y > 0 ? center_y - 1 : center_y;
                    const auto previous_x = !horizontal && center_x > 0 ? center_x - 1 : center_x;
                    const auto difference = luma(input, t, center_y, center_x) - luma(input, t, previous_y, previous_x);
                    const auto strength = std::clamp(std::abs(difference) * 4.0F, 0.0F, 1.0F);
                    driver = difference < 0.0F ? -strength : strength;
                    tears = tears && strength >= 1.0F - density;
                }
                const auto shift = tears ? driver * maximum_shift : 0.0F;
                for (std::size_t c = 0; c < shape.c; ++c) {
                    destination[linear(t, y, x, c, shape)] = sample_mapped(
                        input, static_cast<float>(t), static_cast<float>(y) - (horizontal ? 0.0F : shift),
                        static_cast<float>(x) - (horizontal ? shift : 0.0F), c, edge);
                }
            }
        }
    });
}

void chroma_carrier_drift(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 2, "chroma drift mode");
    require_option(effect.options[1], 2, "chroma drift edge behavior");
    const auto mode = static_cast<ChromaDriftMode>(effect.options[0]);
    const auto edge = static_cast<EdgeBehavior>(effect.options[1]);
    const auto& shape = input.shape();
    if (shape.c < 3) throw std::invalid_argument("Chroma Carrier Drift requires RGB input");
    const auto bleed = std::clamp(effect.values[3], 0.0F, 100.0F);
    const auto straight_rgb = [&](float t, float y, float x) {
        std::array<float, 3> rgb{};
        const auto alpha = shape.c >= 4 ? sample_mapped(input, t, y, x, 3, edge) : 1.0F;
        for (std::size_t channel = 0; channel < rgb.size(); ++channel) {
            const auto value = sample_mapped(input, t, y, x, channel, edge);
            rgb[channel] = alpha > 0.00001F ? value / alpha : 0.0F;
        }
        return rgb;
    };
    const auto chroma = [&](float t, float y, float x, bool cb) {
        float total = 0.0F;
        constexpr std::array<float, 5> taps{-1.0F, -0.5F, 0.0F, 0.5F, 1.0F};
        for (const auto tap : taps) {
            const auto rgb = straight_rgb(t, y, x + tap * bleed);
            const auto r = rgb[0];
            const auto g = rgb[1];
            const auto b = rgb[2];
            const auto yy = 0.2126F * r + 0.7152F * g + 0.0722F * b;
            total += cb ? (b - yy) / 1.8556F : (r - yy) / 1.5748F;
        }
        return total / static_cast<float>(taps.size());
    };
    auto* destination = output.mutable_data();
    parallel_for(shape.t, progress, [&](std::size_t t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                const auto alpha = shape.c >= 4 ? read(input, t, y, x, 3) : 1.0F;
                const auto current_r = alpha > 0.00001F ? read(input, t, y, x, 0) / alpha : 0.0F;
                const auto current_g = alpha > 0.00001F ? read(input, t, y, x, 1) / alpha : 0.0F;
                const auto current_b = alpha > 0.00001F ? read(input, t, y, x, 2) / alpha : 0.0F;
                const auto yy = 0.2126F * current_r + 0.7152F * current_g + 0.0722F * current_b;
                float cb_sign = 1.0F, cr_sign = 1.0F;
                if (mode == ChromaDriftMode::SplitCbCr) cr_sign = -1.0F;
                if (mode == ChromaDriftMode::Alternating) {
                    cb_sign = t % 2 == 0 ? 1.0F : -1.0F;
                    cr_sign = -cb_sign;
                }
                const auto cb = chroma(
                    static_cast<float>(t) - cb_sign * effect.values[2],
                    static_cast<float>(y) - cb_sign * effect.values[1],
                    static_cast<float>(x) - cb_sign * effect.values[0], true);
                const auto cr = chroma(
                    static_cast<float>(t) - cr_sign * effect.values[2],
                    static_cast<float>(y) - cr_sign * effect.values[1],
                    static_cast<float>(x) - cr_sign * effect.values[0], false);
                const auto r = yy + 1.5748F * cr;
                const auto b = yy + 1.8556F * cb;
                const auto g = (yy - 0.2126F * r - 0.0722F * b) / 0.7152F;
                destination[linear(t, y, x, 0, shape)] = std::clamp(r, 0.0F, 1.0F) * alpha;
                destination[linear(t, y, x, 1, shape)] = std::clamp(g, 0.0F, 1.0F) * alpha;
                destination[linear(t, y, x, 2, shape)] = std::clamp(b, 0.0F, 1.0F) * alpha;
                if (shape.c >= 4) destination[linear(t, y, x, 3, shape)] = alpha;
                for (std::size_t c = 4; c < shape.c; ++c) {
                    destination[linear(t, y, x, c, shape)] = read(input, t, y, x, c);
                }
            }
        }
    });
}

void stride_error(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 2, "stride channel mode");
    require_option(effect.options[1], 1, "stride address edge");
    const auto mode = static_cast<StrideChannelMode>(effect.options[0]);
    const auto edge = static_cast<AddressEdge>(effect.options[1]);
    const auto& shape = input.shape();
    const auto frame_pixels = shape.h * shape.w;
    const auto resolve_address = [&](std::int64_t address) {
        const auto count = static_cast<std::int64_t>(frame_pixels);
        if (edge == AddressEdge::Wrap || count == 1) {
            address %= count;
            if (address < 0) address += count;
            return static_cast<std::size_t>(address);
        }
        const auto period = 2 * count - 2;
        address %= period;
        if (address < 0) address += period;
        if (address >= count) address = period - address;
        return static_cast<std::size_t>(address);
    };
    auto* destination = output.mutable_data();
    parallel_for(shape.t, progress, [&](std::size_t t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                const auto nominal = static_cast<std::int64_t>(y * shape.w + x);
                const auto wrong_stride = static_cast<double>(shape.w) * (1.0 + std::clamp(effect.values[0], -0.5F, 0.5F));
                const auto corrupted = static_cast<std::int64_t>(std::llround(
                    static_cast<double>(y) * wrong_stride + static_cast<double>(x) +
                    static_cast<double>(effect.values[1]) * static_cast<double>(frame_pixels) +
                    static_cast<double>(t) * static_cast<double>(effect.values[2]) * static_cast<double>(frame_pixels)));
                const auto delta = corrupted - nominal;
                const auto address_for = [&](std::size_t channel) {
                    const auto factor = mode == StrideChannelMode::RgbTogether ? 1 : static_cast<std::int64_t>(channel + 1);
                    return resolve_address(nominal + delta * factor);
                };
                const auto alpha_address = shape.c >= 4 && mode == StrideChannelMode::AlphaIncluded ? address_for(3) : static_cast<std::size_t>(nominal);
                const auto output_alpha = shape.c >= 4
                    ? read(input, t, alpha_address / shape.w, alpha_address % shape.w, 3) : 1.0F;
                for (std::size_t c = 0; c < std::min<std::size_t>(3, shape.c); ++c) {
                    const auto address = address_for(c);
                    auto value = read(input, t, address / shape.w, address % shape.w, c);
                    if (shape.c >= 4) {
                        const auto source_alpha = read(input, t, address / shape.w, address % shape.w, 3);
                        value = source_alpha > 0.00001F ? value / source_alpha * output_alpha : 0.0F;
                    }
                    destination[linear(t, y, x, c, shape)] = value;
                }
                if (shape.c >= 4) destination[linear(t, y, x, 3, shape)] = output_alpha;
                for (std::size_t c = 4; c < shape.c; ++c) {
                    destination[linear(t, y, x, c, shape)] = read(input, t, y, x, c);
                }
            }
        }
    });
}

void block_address_corruption(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 3, "block corruption mapping");
    require_option(effect.options[1], 2, "block corruption edge");
    const auto mapping = static_cast<BlockCorruptionMapping>(effect.options[0]);
    const auto edge = static_cast<EdgeBehavior>(effect.options[1]);
    const auto& shape = input.shape();
    const auto maximum_block = std::max(shape.w, shape.h);
    const auto block_size = 1 + static_cast<std::size_t>(std::round(
        std::clamp(effect.values[0], 0.0F, 1.0F) * static_cast<float>(maximum_block - 1)));
    const auto blocks_x = (shape.w + block_size - 1) / block_size;
    const auto blocks_y = (shape.h + block_size - 1) / block_size;
    const auto block_count = blocks_x * blocks_y;
    const auto hold = static_cast<std::size_t>(std::max(1.0F, std::round(effect.values[3])));
    const auto time_reach = static_cast<std::size_t>(std::max(0.0F, std::round(effect.values[2])));
    const auto corruption = std::clamp(effect.values[1], 0.0F, 1.0F);
    const auto hash_for = [&](std::size_t epoch, std::size_t block) {
        auto hash = (epoch + 1) * 0x9E3779B185EBCA87ULL;
        hash ^= (block + 1) * 0xC2B2AE3D27D4EB4FULL;
        hash ^= effect.random_seed * 0xD6E8FEB86659FD93ULL;
        hash ^= hash >> 30U; hash *= 0xBF58476D1CE4E5B9ULL; hash ^= hash >> 27U;
        return hash;
    };
    auto* destination = output.mutable_data();
    parallel_for(shape.t, progress, [&](std::size_t t) {
        const auto epoch = t / hold;
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                const auto block = (y / block_size) * blocks_x + x / block_size;
                const auto hash = hash_for(epoch, block);
                const auto trigger = static_cast<float>(hash & 0xFFFFFFU) / static_cast<float>(0x1000000U);
                const auto corrupted = trigger < corruption;
                auto source_block = block;
                if (corrupted && block_count > 1) {
                    if (mapping == BlockCorruptionMapping::Swap) source_block = (block + 1 + static_cast<std::size_t>((hash >> 24U) % (block_count - 1))) % block_count;
                    else if (mapping == BlockCorruptionMapping::Repeat) source_block = block == 0 ? block_count - 1 : block - 1;
                    else if (mapping == BlockCorruptionMapping::Offset) source_block = (block + static_cast<std::size_t>((hash >> 32U) % block_count)) % block_count;
                    else source_block = (block + epoch + 1 + static_cast<std::size_t>((hash >> 40U) & 3U)) % block_count;
                }
                const auto source_x = static_cast<std::ptrdiff_t>((source_block % blocks_x) * block_size + x % block_size);
                const auto source_y = static_cast<std::ptrdiff_t>((source_block / blocks_x) * block_size + y % block_size);
                const auto span = static_cast<std::int64_t>(time_reach);
                const auto time_offset = !corrupted || span == 0 ? 0 : static_cast<std::int64_t>((hash >> 48U) % static_cast<std::uint64_t>(2 * span + 1)) - span;
                const auto st = resolve_time(static_cast<std::ptrdiff_t>(t) + time_offset, shape.t, edge);
                const auto sy = resolve_time(source_y, shape.h, edge);
                const auto sx = resolve_time(source_x, shape.w, edge);
                for (std::size_t c = 0; c < shape.c; ++c) destination[linear(t, y, x, c, shape)] = read(input, st, sy, sx, c);
            }
        }
    });
}

void bitplane_forge(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 3, "bitplane operation");
    require_option(effect.options[1], 5, "bitplane channel");
    const auto operation = static_cast<BitplaneOperation>(effect.options[0]);
    const auto channel = static_cast<BitplaneChannel>(effect.options[1]);
    const auto bits = static_cast<std::size_t>(std::clamp(std::round(effect.values[0]), 2.0F, 16.0F));
    const auto maximum = bits == 16 ? 0xFFFFU : (1U << bits) - 1U;
    const auto mask = static_cast<std::uint32_t>(std::clamp(std::round(effect.values[1]), 0.0F, 65535.0F)) & maximum;
    const auto raw_shift = static_cast<int>(std::clamp(std::round(effect.values[2]), -15.0F, 15.0F));
    const auto normalized_shift = static_cast<unsigned>((raw_shift % static_cast<int>(bits) + static_cast<int>(bits)) % static_cast<int>(bits));
    const auto mix = [](std::uint64_t value) {
        value ^= value >> 30U; value *= 0xBF58476D1CE4E5B9ULL;
        value ^= value >> 27U; value *= 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    };
    const auto greatest_common_divisor = [](std::size_t left, std::size_t right) {
        while (right != 0) { const auto next = left % right; left = right; right = next; }
        return left;
    };
    std::array<std::array<std::size_t, 16>, 5> permutations{};
    for (std::size_t stream = 0; stream < permutations.size(); ++stream) {
        const auto stream_hash = mix(effect.random_seed ^ ((stream + 1) * 0x9E3779B185EBCA87ULL));
        auto multiplier = static_cast<std::size_t>((stream_hash | 1U) % bits);
        if (multiplier == 0) multiplier = 1;
        while (greatest_common_divisor(multiplier, bits) != 1) multiplier = (multiplier + 2) % bits;
        const auto offset = static_cast<std::size_t>((stream_hash >> 32U) % bits);
        for (std::size_t bit = 0; bit < bits; ++bit) permutations[stream][bit] = (bit * multiplier + offset) % bits;
    }
    const auto forge = [&](float value, std::size_t t, std::size_t y, std::size_t x, std::size_t stream) {
        const auto quantized = static_cast<std::uint32_t>(std::llround(
            static_cast<double>(std::clamp(value, 0.0F, 1.0F)) * static_cast<double>(maximum)));
        std::uint32_t transformed = quantized;
        if (operation == BitplaneOperation::Shuffle) {
            transformed = 0;
            for (std::size_t bit = 0; bit < bits; ++bit) {
                if ((quantized & (1U << bit)) != 0) transformed |= 1U << permutations[stream][bit];
            }
            transformed = (quantized & ~mask) | (transformed & mask);
        } else if (operation == BitplaneOperation::Rotate) {
            if (normalized_shift != 0) transformed = ((quantized << normalized_shift) | (quantized >> (bits - normalized_shift))) & maximum;
            transformed = (quantized & ~mask) | (transformed & mask);
        } else if (operation == BitplaneOperation::Invert) {
            transformed = quantized ^ mask;
        } else {
            auto hash = effect.random_seed ^ ((t + 1) * 0xD6E8FEB86659FD93ULL);
            hash ^= (y + 1) * 0xA24BAED4963EE407ULL;
            hash ^= (x + 1) * 0x9FB21C651E98DF25ULL;
            hash ^= (stream + 1) * 0xC2B2AE3D27D4EB4FULL;
            transformed = quantized ^ (static_cast<std::uint32_t>(mix(hash)) & mask);
        }
        return static_cast<float>(transformed & maximum) / static_cast<float>(maximum);
    };
    const auto& shape = input.shape();
    auto* destination = output.mutable_data();
    parallel_for(shape.t, progress, [&](std::size_t t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                if (shape.c == 1) {
                    const auto value = channel == BitplaneChannel::Alpha ? read(input, t, y, x, 0) : forge(read(input, t, y, x, 0), t, y, x, 0);
                    destination[linear(t, y, x, 0, shape)] = value;
                    continue;
                }
                const auto current_alpha = shape.c >= 4 ? std::clamp(read(input, t, y, x, 3), 0.0F, 1.0F) : 1.0F;
                std::array<float, 3> straight{};
                for (std::size_t c = 0; c < std::min<std::size_t>(3, shape.c); ++c) {
                    straight[c] = shape.c >= 4 && current_alpha > 0.00001F
                        ? read(input, t, y, x, c) / current_alpha : (shape.c >= 4 ? 0.0F : read(input, t, y, x, c));
                }
                auto output_alpha = current_alpha;
                if (channel == BitplaneChannel::Luma) {
                    const auto luminance = shape.c >= 3
                        ? 0.2126F * straight[0] + 0.7152F * straight[1] + 0.0722F * straight[2] : straight[0];
                    const auto delta = forge(luminance, t, y, x, 0) - luminance;
                    for (std::size_t c = 0; c < std::min<std::size_t>(3, shape.c); ++c) straight[c] = std::clamp(straight[c] + delta, 0.0F, 1.0F);
                } else if (channel == BitplaneChannel::RgbTogether) {
                    for (std::size_t c = 0; c < std::min<std::size_t>(3, shape.c); ++c) straight[c] = forge(straight[c], t, y, x, 0);
                } else if (channel >= BitplaneChannel::Red && channel <= BitplaneChannel::Blue) {
                    const auto c = static_cast<std::size_t>(channel) - static_cast<std::size_t>(BitplaneChannel::Red);
                    if (c < std::min<std::size_t>(3, shape.c)) straight[c] = forge(straight[c], t, y, x, c + 1);
                } else if (channel == BitplaneChannel::Alpha && shape.c >= 4) {
                    output_alpha = forge(current_alpha, t, y, x, 4);
                }
                for (std::size_t c = 0; c < std::min<std::size_t>(3, shape.c); ++c) destination[linear(t, y, x, c, shape)] = straight[c] * output_alpha;
                if (shape.c >= 4) destination[linear(t, y, x, 3, shape)] = output_alpha;
                for (std::size_t c = 4; c < shape.c; ++c) destination[linear(t, y, x, c, shape)] = read(input, t, y, x, c);
            }
        }
    });
}

void radial(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 2, "edge behavior");
    require_option(effect.options[1], 2, "radial topology");
    require_option(effect.options[2], 1, "radial seam mode");
    const auto edge = static_cast<EdgeBehavior>(effect.options[0]);
    const auto topology = static_cast<RadialTopology>(effect.options[1]);
    const auto seam_mode = static_cast<RadialSeamMode>(effect.options[2]);
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
                const auto tau = 2.0F * std::numbers::pi_v<float>;
                const auto raw_turns = std::atan2(dy, dx) / tau;
                const auto rotation_turns = effect.values[4] / 360.0F;
                const auto turns = std::remainder(raw_turns - rotation_turns, 1.0F);
                const auto angular_time = seam_mode == RadialSeamMode::Periodic
                    ? 0.5F * std::cos(tau * turns)
                    : turns;
                const auto warp_gain = std::clamp(std::abs(effect.values[2]) * 5.0F, 0.0F, 1.25F);
                float weave{};
                float source_radius = radius;
                float source_turns = raw_turns;
                switch (topology) {
                    case RadialTopology::TimeLoom: {
                        const auto strand_a = std::sin(tau * (3.0F * turns + 2.0F * radius - 2.0F * phase));
                        const auto strand_b = std::cos(tau * (5.0F * radius - turns + 3.0F * phase));
                        source_turns += effect.values[3] * (0.055F + 0.10F * radius) * strand_a + 0.045F * warp_gain * strand_b;
                        source_radius = std::max(0.0F, radius * (1.0F + 0.22F * warp_gain * strand_b) + 0.035F * warp_gain * strand_a);
                        weave = radius + effect.values[3] * angular_time + 0.34F * strand_a + 0.18F * strand_b;
                        break;
                    }
                    case RadialTopology::KaleidoFold: {
                        const auto segment = std::fmod(turns * 7.0F + 1000.0F, 1.0F);
                        const auto folded = std::abs(segment - 0.5F) * 2.0F;
                        source_turns = rotation_turns +
                            (std::floor(turns * 7.0F) + folded + 0.18F * std::sin(tau * (phase + radius))) / 7.0F;
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
    require_option(effect.options[1], 3, "sort order");
    const auto criterion = static_cast<SortCriterion>(effect.options[0]);
    const auto order = static_cast<SortDirection>(effect.options[1]);
    const auto& s = input.shape();
    const auto key_for_sort = [&](std::size_t t, std::size_t y, std::size_t x) {
        auto key = sort_key(input, t, y, x, criterion);
        if (criterion == SortCriterion::Hue) {
            key = std::fmod(key + effect.values[1] / 360.0F, 1.0F);
            if (key < 0.0F) key += 1.0F;
        }
        return key;
    };
    std::memcpy(output.mutable_data(), input.data(), s.byte_count());
    parallel_for(s.h, progress, [&](std::size_t y) {
        for (std::size_t x = 0; x < s.w; ++x) {
            std::vector<std::size_t> positions;
            positions.reserve(s.t);
            for (std::size_t t = 0; t < s.t; ++t) {
                if (key_for_sort(t, y, x) >= effect.values[0]) {
                    positions.push_back(t);
                }
            }
            auto sorted = positions;
            std::stable_sort(sorted.begin(), sorted.end(), [&](auto left, auto right) {
                return key_for_sort(left, y, x) < key_for_sort(right, y, x);
            });
            if (order == SortDirection::Descending) {
                std::reverse(sorted.begin(), sorted.end());
            } else if (order == SortDirection::Zigzag && sorted.size() > 1) {
                std::vector<std::size_t> reordered;
                reordered.reserve(sorted.size());
                for (std::size_t low = 0, high = sorted.size() - 1; low <= high;) {
                    reordered.push_back(sorted[low++]);
                    if (low <= high) reordered.push_back(sorted[high--]);
                }
                sorted = std::move(reordered);
            } else if (order == SortDirection::CenterOut && sorted.size() > 1) {
                std::vector<std::size_t> reordered;
                reordered.reserve(sorted.size());
                const auto middle = (sorted.size() - 1) / 2;
                reordered.push_back(sorted[middle]);
                for (std::size_t distance = 1; reordered.size() < sorted.size(); ++distance) {
                    if (middle + distance < sorted.size()) reordered.push_back(sorted[middle + distance]);
                    if (distance <= middle) reordered.push_back(sorted[middle - distance]);
                }
                sorted = std::move(reordered);
            }
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
    require_option(effect.options[0], 4, "seamless loop mode");
    require_option(effect.options[1], 1, "loop transition placement");
    require_option(effect.options[2], 2, "spectral phase mode");
    const auto mode = static_cast<SeamlessLoopMode>(effect.options[0]);
    const auto placement = static_cast<LoopTransitionPlacement>(effect.options[1]);
    const auto phase_mode = static_cast<SpectralPhaseMode>(effect.options[2]);
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
    const auto move_transition_to_end = [&] {
        if (placement != LoopTransitionPlacement::End || transition >= result_shape.t) return;
        const auto frame_values = result_shape.h * result_shape.w * result_shape.c;
        std::rotate(
            destination,
            destination + transition * frame_values,
            destination + result_shape.t * frame_values);
    };
    if (mode == SeamlessLoopMode::SpectralMorph) {
        const auto frame_values = source_shape.h * source_shape.w * source_shape.c;
        parallel_for(result_shape.t, progress, [&](std::size_t t) {
            auto* frame_destination = destination + t * frame_values;
            if (t >= transition) {
                std::copy_n(input.data() + t * frame_values, frame_values, frame_destination);
                return;
            }
            const auto tail_t = t + result_shape.t;
            const auto fraction = transition <= 1 ? 0.5F : static_cast<float>(t) / static_cast<float>(transition - 1);
            if (t == 0 || t + 1 == transition) {
                const auto source_t = t == 0 ? tail_t : t;
                std::copy_n(input.data() + source_t * frame_values, frame_values, frame_destination);
                return;
            }
            const auto morphed = spectral_morph_frames(
                std::span<const float>(input.data() + tail_t * frame_values, frame_values),
                std::span<const float>(input.data() + t * frame_values, frame_values),
                source_shape.h, source_shape.w, source_shape.c, smoothstep(0.0F, 1.0F, fraction),
                effect.values[2], effect.values[3], static_cast<SpectralMorphPhaseMode>(phase_mode));
            std::copy(morphed.begin(), morphed.end(), frame_destination);
        });
        move_transition_to_end();
        return;
    }

    std::vector<std::size_t> best_difference_frame;
    if (mode == SeamlessLoopMode::DifferenceWeave) {
        best_difference_frame.resize(source_shape.h * source_shape.w);
        parallel_for(source_shape.h, [](double) {}, [&](std::size_t y) {
            for (std::size_t x = 0; x < source_shape.w; ++x) {
                auto best = std::numeric_limits<float>::max();
                std::size_t best_t = 0;
                for (std::size_t candidate = 0; candidate < transition; ++candidate) {
                    const auto tail_t = candidate + result_shape.t;
                    float difference{};
                    for (std::size_t c = 0; c < std::min<std::size_t>(3, source_shape.c); ++c) {
                        difference += std::abs(read(input, candidate, y, x, c) - read(input, tail_t, y, x, c));
                    }
                    if (difference < best) { best = difference; best_t = candidate; }
                }
                best_difference_frame[y * source_shape.w + x] = best_t;
            }
        });
    }
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
                } else if (mode == SeamlessLoopMode::DifferenceWeave && transition > 1) {
                    if (t == 0) mix = 0.0F;
                    else if (t + 1 == transition) mix = 1.0F;
                    else {
                        const auto center = static_cast<float>(best_difference_frame[y * source_shape.w + x]) /
                                            static_cast<float>(transition - 1);
                        mix = smoothstep(center - softness, center + softness, fraction);
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
    move_transition_to_end();
}

void datamosh(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 2, "freeze axis");
    require_option(effect.options[1], 2, "freeze trigger");
    const auto axis = static_cast<FreezeAxis>(effect.options[0]);
    const auto trigger = static_cast<FreezeTrigger>(effect.options[1]);
    require_option(effect.options[2], 1, "freeze trigger polarity");
    const auto invert_trigger = effect.options[2] != 0;
    const auto max_hold = static_cast<std::size_t>(std::max(0.0F, std::round(effect.values[1])));
    const auto& shape = input.shape();
    std::memcpy(output.mutable_data(), input.data(), shape.byte_count());
    const auto should_trigger = [&](std::size_t t, std::size_t y, std::size_t x,
                                    std::size_t pt, std::size_t py, std::size_t px) {
        if (trigger == FreezeTrigger::Edge) return std::abs(luma(input, t, y, x) - luma(input, pt, py, px)) > effect.values[0];
        if (trigger == FreezeTrigger::Luma) {
            return invert_trigger ? luma(input, t, y, x) <= effect.values[0]
                                  : luma(input, t, y, x) >= effect.values[0];
        }
        std::uint64_t hash = (static_cast<std::uint64_t>(t) + 1) * 0x9E3779B185EBCA87ULL;
        hash ^= (static_cast<std::uint64_t>(y) + 1) * 0xC2B2AE3D27D4EB4FULL;
        hash ^= (static_cast<std::uint64_t>(x) + 1) * 0x165667B19E3779F9ULL;
        hash ^= effect.random_seed * 0xD6E8FEB86659FD93ULL;
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
        case EffectOperation::RgbTimeSlip:
            rgb_time_slip(input, output, effect, progress);
            return;
        case EffectOperation::HorizontalSyncLoss:
            horizontal_sync_loss(input, output, effect, progress);
            return;
        case EffectOperation::ChromaCarrierDrift:
            chroma_carrier_drift(input, output, effect, progress);
            return;
        case EffectOperation::StrideError:
            stride_error(input, output, effect, progress);
            return;
        case EffectOperation::BlockAddressCorruption:
            block_address_corruption(input, output, effect, progress);
            return;
        case EffectOperation::BitplaneForge:
            bitplane_forge(input, output, effect, progress);
            return;
        case EffectOperation::DimensionalSplicer:
        case EffectOperation::TensorDisplacement:
        case EffectOperation::SignalWeave:
        case EffectOperation::BlockGraft:
        case EffectOperation::ChannelTransplant:
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
        case EffectOperation::RgbTimeSlip:
            return "RGB Time Slip";
        case EffectOperation::HorizontalSyncLoss:
            return "Sync Loss";
        case EffectOperation::ChromaCarrierDrift:
            return "Chroma Carrier Drift";
        case EffectOperation::StrideError:
            return "Stride Error";
        case EffectOperation::BlockAddressCorruption:
            return "Block Address Corruption";
        case EffectOperation::BitplaneForge:
            return "Bitplane Forge";
        case EffectOperation::SignalWeave:
            return "Signal Weave";
        case EffectOperation::BlockGraft:
            return "Block Graft";
        case EffectOperation::ChannelTransplant:
            return "Channel Transplant";
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
            require_amount(effect.amount);
            require_option(static_cast<std::int32_t>(effect.amount_blend_mode), 6, "Amount blend mode");
            const auto needs_amount_blend = effect.amount < 1.0F || effect.amount_blend_mode != AmountBlendMode::Normal;
            const auto next_shape = output_shape(current_shape, effect);
            if (needs_amount_blend && next_shape != current_shape) {
                throw std::invalid_argument("Partial Amount requires an effect that preserves tensor shape");
            }
            if (effect.amount == 0.0F) {
                report(progress, static_cast<double>(index + 1) / static_cast<double>(effects.size() + 1), stage_name(effect.kind));
                continue;
            }
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
                if (needs_amount_blend) {
                    blend_amount(input, output, effect.amount, effect.amount_blend_mode);
                }
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
    require_amount(effect.amount);
    require_option(static_cast<std::int32_t>(effect.amount_blend_mode), 6, "Amount blend mode");
    const auto needs_amount_blend = effect.amount < 1.0F || effect.amount_blend_mode != AmountBlendMode::Normal;
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
    } else if (effect.kind == EffectOperation::SignalWeave) {
        require_option(effect.options[0], 3, "signal weave pattern");
        require_option(effect.options[1], 2, "signal weave size matching");
        if (effect.options[1] == static_cast<std::int32_t>(TensorBroadcast::Crop)) {
            result_shape.t = std::min(source_shape.t, driver_shape.t);
            result_shape.h = std::min(source_shape.h, driver_shape.h);
            result_shape.w = std::min(source_shape.w, driver_shape.w);
        }
    } else if (effect.kind == EffectOperation::BlockGraft) {
        require_option(effect.options[0], 4, "block graft trigger");
        require_option(effect.options[1], 2, "block graft size matching");
        if (effect.options[1] == static_cast<std::int32_t>(TensorBroadcast::Crop)) {
            result_shape.t = std::min(source_shape.t, driver_shape.t);
            result_shape.h = std::min(source_shape.h, driver_shape.h);
            result_shape.w = std::min(source_shape.w, driver_shape.w);
        }
    } else if (effect.kind == EffectOperation::ChannelTransplant) {
        require_option(effect.options[0], 1, "channel transplant component source");
        require_option(effect.options[1], 1, "channel transplant component source");
        require_option(effect.options[2], 1, "channel transplant component source");
        require_option(effect.options[3], 1, "channel transplant colour model");
        require_option(effect.options[4], 2, "channel transplant size matching");
        if (effect.options[4] == static_cast<std::int32_t>(TensorBroadcast::Crop)) {
            result_shape.t = std::min(source_shape.t, driver_shape.t);
            result_shape.h = std::min(source_shape.h, driver_shape.h);
            result_shape.w = std::min(source_shape.w, driver_shape.w);
        }
    } else {
        throw std::invalid_argument("The selected effect does not accept a driver tensor");
    }

    if (needs_amount_blend && result_shape != source_shape) {
        throw std::invalid_argument("Partial Amount requires an effect that preserves tensor shape");
    }
    if (effect.amount == 0.0F) {
        std::filesystem::create_directories(output_path.parent_path());
        std::filesystem::copy_file(source_path, output_path, std::filesystem::copy_options::overwrite_existing);
        report(progress, 1.0, stage_name(effect.kind));
        return {source_shape, metadata};
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
    } else if (effect.kind == EffectOperation::TensorDisplacement) {
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
    } else if (effect.kind == EffectOperation::SignalWeave) {
        const auto pattern = static_cast<SignalWeavePattern>(effect.options[0]);
        const auto size_matching = static_cast<TensorBroadcast>(effect.options[1]);
        const auto band_extent = pattern == SignalWeavePattern::Checker
            ? std::max(result_shape.w, result_shape.h) : result_shape.h;
        const auto band_size = 1 + static_cast<std::size_t>(std::round(
            std::clamp(effect.values[0], 0.0F, 1.0F) * static_cast<float>(band_extent - 1)));
        const auto irregularity = std::clamp(effect.values[2], 0.0F, 1.0F);
        const auto time_offset = static_cast<int>(std::clamp(std::round(effect.values[3]), -240.0F, 240.0F));
        const auto mix = [](std::uint64_t value) {
            value ^= value >> 30U; value *= 0xBF58476D1CE4E5B9ULL;
            value ^= value >> 27U; value *= 0x94D049BB133111EBULL;
            return value ^ (value >> 31U);
        };
        const auto driver_coordinate = [&](std::size_t coordinate, std::size_t output_extent, std::size_t driver_extent) {
            if (size_matching == TensorBroadcast::Stretch) {
                return output_extent <= 1 || driver_extent <= 1 ? std::size_t{0} : static_cast<std::size_t>(std::round(
                    static_cast<double>(coordinate) * static_cast<double>(driver_extent - 1) /
                    static_cast<double>(output_extent - 1)));
            }
            return std::min(coordinate, driver_extent - 1);
        };
        parallel_for(result_shape.t, [&](double fraction) { report(progress, fraction, "Signal Weave"); }, [&](std::size_t t) {
            const auto phase = effect.values[1] * static_cast<float>(t);
            const auto base_driver_t = driver_coordinate(t, result_shape.t, driver_shape.t);
            const auto driver_t = resolve_time(static_cast<std::ptrdiff_t>(base_driver_t) + time_offset, driver_shape.t, EdgeBehavior::Clamp);
            for (std::size_t y = 0; y < result_shape.h; ++y) {
                const auto driver_y = driver_coordinate(y, result_shape.h, driver_shape.h);
                for (std::size_t x = 0; x < result_shape.w; ++x) {
                    const auto driver_x = driver_coordinate(x, result_shape.w, driver_shape.w);
                    std::int64_t pattern_index{};
                    if (pattern == SignalWeavePattern::Lines) pattern_index = static_cast<std::int64_t>(y) + static_cast<std::int64_t>(std::floor(phase));
                    else if (pattern == SignalWeavePattern::InterlacedFields) pattern_index = static_cast<std::int64_t>(y + t) + static_cast<std::int64_t>(std::floor(phase));
                    else if (pattern == SignalWeavePattern::Bands) pattern_index = static_cast<std::int64_t>(std::floor(static_cast<float>(y) / static_cast<float>(band_size) + phase));
                    else pattern_index = static_cast<std::int64_t>(x / band_size + y / band_size) + static_cast<std::int64_t>(std::floor(phase));
                    const auto regular_driver = (pattern_index & 1) != 0;
                    auto hash = effect.random_seed ^ ((t + 1) * 0xD6E8FEB86659FD93ULL);
                    hash ^= (y / band_size + 1) * 0xA24BAED4963EE407ULL;
                    hash ^= (x / band_size + 1) * 0x9FB21C651E98DF25ULL;
                    const auto noise = static_cast<float>(mix(hash) & 0xFFFFFFU) / static_cast<float>(0x1000000U);
                    const auto driver_probability = regular_driver ? 1.0F - irregularity * 0.5F : irregularity * 0.5F;
                    const auto use_driver = noise < driver_probability;
                    for (std::size_t c = 0; c < result_shape.c; ++c) {
                        destination[linear(t, y, x, c, result_shape)] = use_driver
                            ? read(driver, driver_t, driver_y, driver_x, std::min(c, driver_shape.c - 1))
                            : read(source, t, y, x, c);
                    }
                }
            }
        });
    } else if (effect.kind == EffectOperation::BlockGraft) {
        const auto trigger = static_cast<BlockGraftTrigger>(effect.options[0]);
        const auto size_matching = static_cast<TensorBroadcast>(effect.options[1]);
        const auto maximum_block = std::max(result_shape.w, result_shape.h);
        const auto block_size = 1 + static_cast<std::size_t>(std::round(
            std::clamp(effect.values[0], 0.0F, 1.0F) * static_cast<float>(maximum_block - 1)));
        const auto threshold = std::clamp(effect.values[1], 0.0F, 1.0F);
        const auto hold = static_cast<std::size_t>(std::max(1.0F, std::round(effect.values[2])));
        const auto time_offset = static_cast<int>(std::clamp(std::round(effect.values[3]), -240.0F, 240.0F));
        const auto mix = [](std::uint64_t value) {
            value ^= value >> 30U; value *= 0xBF58476D1CE4E5B9ULL;
            value ^= value >> 27U; value *= 0x94D049BB133111EBULL;
            return value ^ (value >> 31U);
        };
        const auto driver_coordinate = [&](std::size_t coordinate, std::size_t output_extent, std::size_t driver_extent) {
            if (size_matching == TensorBroadcast::Stretch) {
                return output_extent <= 1 || driver_extent <= 1 ? std::size_t{0} : static_cast<std::size_t>(std::round(
                    static_cast<double>(coordinate) * static_cast<double>(driver_extent - 1) /
                    static_cast<double>(output_extent - 1)));
            }
            return std::min(coordinate, driver_extent - 1);
        };
        parallel_for(result_shape.t, [&](double fraction) { report(progress, fraction, "Block Graft"); }, [&](std::size_t t) {
            const auto epoch = t / hold;
            const auto anchor_t = std::min(epoch * hold, result_shape.t - 1);
            const auto current_driver_t = resolve_time(
                static_cast<std::ptrdiff_t>(driver_coordinate(t, result_shape.t, driver_shape.t)) + time_offset,
                driver_shape.t, EdgeBehavior::Clamp);
            const auto anchor_driver_t = resolve_time(
                static_cast<std::ptrdiff_t>(driver_coordinate(anchor_t, result_shape.t, driver_shape.t)) + time_offset,
                driver_shape.t, EdgeBehavior::Clamp);
            for (std::size_t y = 0; y < result_shape.h; ++y) {
                const auto driver_y = driver_coordinate(y, result_shape.h, driver_shape.h);
                for (std::size_t x = 0; x < result_shape.w; ++x) {
                    const auto driver_x = driver_coordinate(x, result_shape.w, driver_shape.w);
                    const auto block_x = (x / block_size) * block_size;
                    const auto block_y = (y / block_size) * block_size;
                    const auto driver_block_x = driver_coordinate(block_x, result_shape.w, driver_shape.w);
                    const auto driver_block_y = driver_coordinate(block_y, result_shape.h, driver_shape.h);
                    bool graft{};
                    if (trigger == BlockGraftTrigger::Random) {
                        auto hash = effect.random_seed ^ ((epoch + 1) * 0xD6E8FEB86659FD93ULL);
                        hash ^= (block_y / block_size + 1) * 0xA24BAED4963EE407ULL;
                        hash ^= (block_x / block_size + 1) * 0x9FB21C651E98DF25ULL;
                        graft = static_cast<float>(mix(hash) & 0xFFFFFFU) / static_cast<float>(0x1000000U) < threshold;
                    } else {
                        const auto a_luma = luma(source, anchor_t, block_y, block_x);
                        const auto b_luma = luma(driver, anchor_driver_t, driver_block_y, driver_block_x);
                        if (trigger == BlockGraftTrigger::ALuma) graft = a_luma >= threshold;
                        else if (trigger == BlockGraftTrigger::BLuma) graft = b_luma >= threshold;
                        else if (trigger == BlockGraftTrigger::Difference) graft = std::abs(a_luma - b_luma) >= threshold;
                        else {
                            const auto right = std::min(block_x + 1, source_shape.w - 1);
                            const auto down = std::min(block_y + 1, source_shape.h - 1);
                            const auto edge_value = std::max(
                                std::abs(a_luma - luma(source, anchor_t, block_y, right)),
                                std::abs(a_luma - luma(source, anchor_t, down, block_x)));
                            graft = edge_value >= threshold;
                        }
                    }
                    for (std::size_t c = 0; c < result_shape.c; ++c) {
                        destination[linear(t, y, x, c, result_shape)] = graft
                            ? read(driver, current_driver_t, driver_y, driver_x, std::min(c, driver_shape.c - 1))
                            : read(source, t, y, x, c);
                    }
                }
            }
        });
    } else {
        const std::array<bool, 3> from_b{effect.options[0] != 0, effect.options[1] != 0, effect.options[2] != 0};
        const auto colour_model = static_cast<ChannelTransplantColourModel>(effect.options[3]);
        const auto size_matching = static_cast<TensorBroadcast>(effect.options[4]);
        const auto time_offset = static_cast<int>(std::clamp(std::round(effect.values[0]), -240.0F, 240.0F));
        const auto x_offset = static_cast<int>(std::round(effect.values[1]));
        const auto y_offset = static_cast<int>(std::round(effect.values[2]));
        const auto driver_coordinate = [&](std::size_t coordinate, std::size_t output_extent, std::size_t driver_extent) {
            if (size_matching == TensorBroadcast::Stretch) {
                return output_extent <= 1 || driver_extent <= 1 ? std::size_t{0} : static_cast<std::size_t>(std::round(
                    static_cast<double>(coordinate) * static_cast<double>(driver_extent - 1) /
                    static_cast<double>(output_extent - 1)));
            }
            return std::min(coordinate, driver_extent - 1);
        };
        const auto straight_rgb = [](const MappedTensor& tensor, std::size_t t, std::size_t y, std::size_t x) {
            std::array<float, 3> rgb{};
            const auto alpha = tensor.shape().c >= 4 ? std::clamp(read(tensor, t, y, x, 3), 0.0F, 1.0F) : 1.0F;
            for (std::size_t c = 0; c < 3; ++c) {
                const auto channel = std::min(c, tensor.shape().c - 1);
                rgb[c] = tensor.shape().c >= 4 ? (alpha > 0.00001F ? read(tensor, t, y, x, channel) / alpha : 0.0F)
                                                : read(tensor, t, y, x, channel);
            }
            return rgb;
        };
        const auto components = [&](const std::array<float, 3>& rgb) {
            if (colour_model == ChannelTransplantColourModel::Rgb) return rgb;
            const auto yy = 0.2126F * rgb[0] + 0.7152F * rgb[1] + 0.0722F * rgb[2];
            return std::array<float, 3>{yy, (rgb[2] - yy) / 1.8556F, (rgb[0] - yy) / 1.5748F};
        };
        parallel_for(result_shape.t, [&](double fraction) { report(progress, fraction, "Channel Transplant"); }, [&](std::size_t t) {
            const auto bt = resolve_time(
                static_cast<std::ptrdiff_t>(driver_coordinate(t, result_shape.t, driver_shape.t)) + time_offset,
                driver_shape.t, EdgeBehavior::Clamp);
            for (std::size_t y = 0; y < result_shape.h; ++y) {
                const auto by = resolve_time(
                    static_cast<std::ptrdiff_t>(driver_coordinate(y, result_shape.h, driver_shape.h)) + y_offset,
                    driver_shape.h, EdgeBehavior::Clamp);
                for (std::size_t x = 0; x < result_shape.w; ++x) {
                    const auto bx = resolve_time(
                        static_cast<std::ptrdiff_t>(driver_coordinate(x, result_shape.w, driver_shape.w)) + x_offset,
                        driver_shape.w, EdgeBehavior::Clamp);
                    const auto a_rgb = straight_rgb(source, t, y, x);
                    const auto b_rgb = straight_rgb(driver, bt, by, bx);
                    auto selected = components(a_rgb);
                    const auto b_components = components(b_rgb);
                    for (std::size_t component = 0; component < 3; ++component) if (from_b[component]) selected[component] = b_components[component];
                    std::array<float, 3> rgb = selected;
                    if (colour_model == ChannelTransplantColourModel::YCbCr) {
                        rgb[0] = selected[0] + 1.5748F * selected[2];
                        rgb[2] = selected[0] + 1.8556F * selected[1];
                        rgb[1] = (selected[0] - 0.2126F * rgb[0] - 0.0722F * rgb[2]) / 0.7152F;
                    }
                    const auto alpha = source_shape.c >= 4 ? read(source, t, y, x, 3) : 1.0F;
                    for (std::size_t c = 0; c < std::min<std::size_t>(3, result_shape.c); ++c) {
                        destination[linear(t, y, x, c, result_shape)] = std::clamp(rgb[c], 0.0F, 1.0F) * alpha;
                    }
                    if (result_shape.c >= 4) destination[linear(t, y, x, 3, result_shape)] = alpha;
                    for (std::size_t c = 4; c < result_shape.c; ++c) destination[linear(t, y, x, c, result_shape)] = read(source, t, y, x, c);
                }
            }
        });
    }
    if (needs_amount_blend) {
        blend_amount(source, output, effect.amount, effect.amount_blend_mode);
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
