#include "chronoforge/core/file_executor.hpp"

#include "chronoforge/core/effects.hpp"
#include "chronoforge/core/mapped_tensor.hpp"
#include "chronoforge/core/spectral.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
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
            return effect.options[0] == 0 ? TensorShape{input.w, input.h, input.t, input.c}
                                          : TensorShape{input.h, input.t, input.w, input.c};
        case EffectOperation::SpectralFftSwap:
            require_option(effect.options[0], 2, "FFT axis");
            if (effect.options[0] == 0) {
                return {input.w, input.h, input.t, input.c};
            }
            if (effect.options[0] == 1) {
                return {input.h, input.t, input.w, input.c};
            }
            return {input.h, input.w, input.t, input.c};
        default:
            return input;
    }
}

void transpose(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    const auto& s = output.shape();
    auto* destination = output.mutable_data();
    parallel_for(s.t, progress, [&](std::size_t t) {
        for (std::size_t y = 0; y < s.h; ++y) {
            for (std::size_t x = 0; x < s.w; ++x) {
                for (std::size_t c = 0; c < s.c; ++c) {
                    destination[linear(t, y, x, c, s)] = effect.options[0] == 0 ? read(input, x, y, t, c)
                                                                                 : read(input, y, t, x, c);
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
    const auto edge = static_cast<EdgeBehavior>(effect.options[0]);
    const auto& s = input.shape();
    const auto center_x = effect.values[0] * static_cast<float>(s.w - 1);
    const auto center_y = effect.values[1] * static_cast<float>(s.h - 1);
    auto* destination = output.mutable_data();
    parallel_for(s.t, progress, [&](std::size_t t) {
        for (std::size_t y = 0; y < s.h; ++y) {
            for (std::size_t x = 0; x < s.w; ++x) {
                const auto dx = static_cast<float>(x) - center_x;
                const auto dy = static_cast<float>(y) - center_y;
                const auto amount = static_cast<std::ptrdiff_t>(std::floor(effect.values[2] * std::sqrt(dx * dx + dy * dy)));
                const auto source_t = resolve_time(static_cast<std::ptrdiff_t>(t) + amount, s.t, edge);
                for (std::size_t c = 0; c < s.c; ++c) {
                    destination[linear(t, y, x, c, s)] = read(input, source_t, y, x, c);
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

void rotation(const MappedTensor& input, MappedTensor& output, const EffectSpec& effect, const LocalProgress& progress) {
    require_option(effect.options[0], 2, "fill mode");
    const auto fill = static_cast<FillMode>(effect.options[0]);
    const auto& s = input.shape();
    auto* destination = output.mutable_data();
    parallel_for(s.t, progress, [&](std::size_t t) {
        for (std::size_t y = 0; y < s.h; ++y) {
            for (std::size_t x = 0; x < s.w; ++x) {
                float pt = normalized(t, s.t);
                float py = normalized(y, s.h);
                float px = normalized(x, s.w);
                rotate_plane(py, pt, -effect.values[2]);
                rotate_plane(px, pt, -effect.values[1]);
                rotate_plane(px, py, -effect.values[0]);
                for (std::size_t c = 0; c < s.c; ++c) {
                    destination[linear(t, y, x, c, s)] = trilinear(
                        input, denormalized(pt, s.t), denormalized(py, s.h), denormalized(px, s.w), c, fill);
                }
            }
        }
    });
}

void spectral(
    const MappedTensor& input,
    MappedTensor& output,
    const EffectSpec& effect,
    std::size_t budget,
    const VideoTensorMetadata& metadata,
    const LocalProgress& progress) {
    require_option(effect.options[0], 2, "FFT axis");
    const auto next_power_of_two = [](std::size_t value) {
        std::size_t result = 1;
        while (result < value) {
            if (result > std::numeric_limits<std::size_t>::max() / 2) {
                throw std::overflow_error("FFT padded dimension is too large");
            }
            result <<= 1U;
        }
        return result;
    };
    const TensorShape padded{
        next_power_of_two(input.shape().t),
        next_power_of_two(input.shape().h),
        next_power_of_two(input.shape().w),
        input.shape().c,
    };
    const auto padded_elements = padded.element_count();
    const auto raw_bytes = input.shape().byte_count();
    if (padded_elements > std::numeric_limits<std::size_t>::max() / 16 ||
        raw_bytes > budget || padded_elements * 16 > budget - raw_bytes) {
        throw std::runtime_error("Full-resolution 3D FFT exceeds the configured RAM budget; use Proxy or reduce the tensor first");
    }
    const auto count = input.shape().element_count();
    std::vector<float> values(input.data(), input.data() + count);
    VideoTensor memory(input.shape(), std::move(values), metadata);
    auto result = spectral_fft_swap(
        memory,
        {static_cast<SpectralSwapAxis>(effect.options[0]), effect.options[1] != 0, budget - raw_bytes});
    std::memcpy(output.mutable_data(), result.values().data(), result.shape().byte_count());
    progress(1.0);
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
    }
    throw std::invalid_argument("Invalid effect operation");
}

[[nodiscard]] std::string stage_name(EffectOperation operation) {
    switch (operation) {
        case EffectOperation::SpaceTimeTranspose:
            return "Space-Time Transpose";
        case EffectOperation::LumaTimeShift:
            return "Luma-Time Shift";
        case EffectOperation::RadialChronoFunnel:
            return "Radial Chrono-Funnel";
        case EffectOperation::TemporalPixelSort:
            return "Temporal Pixel Sort";
        case EffectOperation::Tensor3dRotation:
            return "Tensor 3D Rotation";
        case EffectOperation::SpectralFftSwap:
            return "Spectral FFT Swap";
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

}  // namespace chronoforge
