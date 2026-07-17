#include "chronoforge/core/effects.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace chronoforge {
namespace {

template <typename Function>
void parallel_for(std::size_t count, Function&& function) {
    if (count == 0) {
        return;
    }
    const auto worker_count = std::min<std::size_t>(
        count, std::min<std::size_t>(std::max(1U, std::thread::hardware_concurrency()), 16));
    std::atomic<std::size_t> next{0};
    std::atomic<bool> stopped{false};
    std::mutex failure_mutex;
    std::exception_ptr failure;
    auto worker = [&] {
        while (!stopped.load(std::memory_order_relaxed)) {
            const auto index = next.fetch_add(1, std::memory_order_relaxed);
            if (index >= count) {
                return;
            }
            try {
                function(index);
            } catch (...) {
                std::scoped_lock lock(failure_mutex);
                if (failure == nullptr) {
                    failure = std::current_exception();
                }
                stopped.store(true, std::memory_order_relaxed);
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
}

[[nodiscard]] std::size_t resolve_time(std::ptrdiff_t time, std::size_t count, EdgeBehavior behavior) {
    if (count == 0) {
        throw std::invalid_argument("A temporal effect requires at least one frame");
    }

    switch (behavior) {
        case EdgeBehavior::Clamp:
            return static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(time, 0, static_cast<std::ptrdiff_t>(count - 1)));
        case EdgeBehavior::Wrap: {
            const auto size = static_cast<std::ptrdiff_t>(count);
            auto wrapped = time % size;
            if (wrapped < 0) {
                wrapped += size;
            }
            return static_cast<std::size_t>(wrapped);
        }
        case EdgeBehavior::Mirror: {
            if (count == 1) {
                return 0;
            }
            const auto period = static_cast<std::ptrdiff_t>(2 * count - 2);
            auto mirrored = time % period;
            if (mirrored < 0) {
                mirrored += period;
            }
            if (mirrored >= static_cast<std::ptrdiff_t>(count)) {
                mirrored = period - mirrored;
            }
            return static_cast<std::size_t>(mirrored);
        }
    }
    throw std::invalid_argument("Unknown edge behavior");
}

[[nodiscard]] float resolve_fractional(float coordinate, std::size_t count, EdgeBehavior behavior) {
    if (count <= 1) {
        return 0.0F;
    }
    const auto maximum = static_cast<float>(count - 1);
    switch (behavior) {
        case EdgeBehavior::Clamp:
            return std::clamp(coordinate, 0.0F, maximum);
        case EdgeBehavior::Wrap: {
            const auto size = static_cast<float>(count);
            coordinate = std::fmod(coordinate, size);
            if (coordinate < 0.0F) coordinate += size;
            // Adding a tiny negative remainder to a large float can round to
            // exactly `size`. Keep the fractional coordinate inside the last
            // valid cell so floor() can never produce an out-of-range index.
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

[[nodiscard]] float sample_tensor(
    const VideoTensor& input,
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
    const auto c00 = input.at(t0, y0, x0, channel) + (input.at(t0, y0, x1, channel) - input.at(t0, y0, x0, channel)) * fx;
    const auto c01 = input.at(t0, y1, x0, channel) + (input.at(t0, y1, x1, channel) - input.at(t0, y1, x0, channel)) * fx;
    const auto c10 = input.at(t1, y0, x0, channel) + (input.at(t1, y0, x1, channel) - input.at(t1, y0, x0, channel)) * fx;
    const auto c11 = input.at(t1, y1, x0, channel) + (input.at(t1, y1, x1, channel) - input.at(t1, y1, x0, channel)) * fx;
    return (c00 + (c01 - c00) * fy) + ((c10 + (c11 - c10) * fy) - (c00 + (c01 - c00) * fy)) * ft;
}

[[nodiscard]] float cubic_weight(float distance) {
    distance = std::abs(distance);
    if (distance <= 1.0F) {
        return 1.5F * distance * distance * distance - 2.5F * distance * distance + 1.0F;
    }
    if (distance < 2.0F) {
        return -0.5F * distance * distance * distance + 2.5F * distance * distance - 4.0F * distance + 2.0F;
    }
    return 0.0F;
}

[[nodiscard]] float sample_tensor_cubic(
    const VideoTensor& input,
    float t,
    float y,
    float x,
    std::size_t channel) {
    const auto& shape = input.shape();
    const auto base_t = static_cast<std::ptrdiff_t>(std::floor(t));
    const auto base_y = static_cast<std::ptrdiff_t>(std::floor(y));
    const auto base_x = static_cast<std::ptrdiff_t>(std::floor(x));
    float value = 0.0F;
    float total_weight = 0.0F;
    for (std::ptrdiff_t dt = -1; dt <= 2; ++dt) {
        const auto st = resolve_time(base_t + dt, shape.t, EdgeBehavior::Clamp);
        const auto wt = cubic_weight(t - static_cast<float>(base_t + dt));
        for (std::ptrdiff_t dy = -1; dy <= 2; ++dy) {
            const auto sy = resolve_time(base_y + dy, shape.h, EdgeBehavior::Clamp);
            const auto wy = cubic_weight(y - static_cast<float>(base_y + dy));
            for (std::ptrdiff_t dx = -1; dx <= 2; ++dx) {
                const auto sx = resolve_time(base_x + dx, shape.w, EdgeBehavior::Clamp);
                const auto weight = wt * wy * cubic_weight(x - static_cast<float>(base_x + dx));
                value += input.at(st, sy, sx, channel) * weight;
                total_weight += weight;
            }
        }
    }
    return std::abs(total_weight) <= std::numeric_limits<float>::epsilon() ? value : value / total_weight;
}

[[nodiscard]] float luma(const VideoTensor& input, std::size_t t, std::size_t y, std::size_t x) {
    const auto& shape = input.shape();
    const auto r = input.at(t, y, x, 0);
    if (shape.c == 1) {
        return r;
    }
    const auto g = input.at(t, y, x, std::min<std::size_t>(1, shape.c - 1));
    const auto b = input.at(t, y, x, std::min<std::size_t>(2, shape.c - 1));
    return 0.2126F * r + 0.7152F * g + 0.0722F * b;
}

[[nodiscard]] float hue(const VideoTensor& input, std::size_t t, std::size_t y, std::size_t x) {
    const auto& shape = input.shape();
    if (shape.c < 3) {
        return 0.0F;
    }
    const auto r = input.at(t, y, x, 0);
    const auto g = input.at(t, y, x, 1);
    const auto b = input.at(t, y, x, 2);
    const auto maximum = std::max({r, g, b});
    const auto minimum = std::min({r, g, b});
    const auto delta = maximum - minimum;
    if (delta == 0.0F) {
        return 0.0F;
    }
    float value{};
    if (maximum == r) {
        value = std::fmod((g - b) / delta, 6.0F);
    } else if (maximum == g) {
        value = ((b - r) / delta) + 2.0F;
    } else {
        value = ((r - g) / delta) + 4.0F;
    }
    value /= 6.0F;
    return value < 0.0F ? value + 1.0F : value;
}

[[nodiscard]] float saturation(const VideoTensor& input, std::size_t t, std::size_t y, std::size_t x) {
    if (input.shape().c < 3) {
        return 0.0F;
    }
    const auto r = input.at(t, y, x, 0);
    const auto g = input.at(t, y, x, 1);
    const auto b = input.at(t, y, x, 2);
    const auto maximum = std::max({r, g, b});
    const auto minimum = std::min({r, g, b});
    return maximum == 0.0F ? 0.0F : (maximum - minimum) / maximum;
}

[[nodiscard]] float key_for(
    const VideoTensor& input,
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
    throw std::invalid_argument("Unknown temporal sort criterion");
}

[[nodiscard]] float shift_value(
    const VideoTensor& input,
    std::size_t t,
    std::size_t y,
    std::size_t x,
    ShiftSource source) {
    switch (source) {
        case ShiftSource::Luma:
            return luma(input, t, y, x);
        case ShiftSource::Red:
            return input.at(t, y, x, 0);
        case ShiftSource::Green:
            return input.at(t, y, x, std::min<std::size_t>(1, input.shape().c - 1));
        case ShiftSource::Blue:
            return input.at(t, y, x, std::min<std::size_t>(2, input.shape().c - 1));
        case ShiftSource::Alpha:
            if (input.shape().c < 4) {
                throw std::invalid_argument("Alpha time shift requires an RGBA tensor");
            }
            return input.at(t, y, x, 3);
    }
    throw std::invalid_argument("Unknown time shift source");
}

struct Point3 {
    float t;
    float y;
    float x;
};

void rotate_plane(float& a, float& b, float degrees) {
    const auto radians = degrees * std::numbers::pi_v<float> / 180.0F;
    const auto old_a = a;
    a = std::cos(radians) * old_a - std::sin(radians) * b;
    b = std::sin(radians) * old_a + std::cos(radians) * b;
}

[[nodiscard]] float normalized_coordinate(std::size_t coordinate, std::size_t extent) {
    if (extent == 1) {
        return 0.0F;
    }
    return 2.0F * static_cast<float>(coordinate) / static_cast<float>(extent - 1) - 1.0F;
}

[[nodiscard]] float denormalized_coordinate(float coordinate, std::size_t extent) {
    if (extent == 1) {
        return 0.0F;
    }
    return (coordinate + 1.0F) * 0.5F * static_cast<float>(extent - 1);
}

[[nodiscard]] float wrap_coordinate(float value, std::size_t extent) {
    const auto size = static_cast<float>(extent);
    auto wrapped = std::fmod(value, size);
    if (wrapped < 0.0F) {
        wrapped += size;
    }
    return wrapped;
}

[[nodiscard]] float trilinear(
    const VideoTensor& input,
    float time,
    float y,
    float x,
    std::size_t channel,
    FillMode fill) {
    const auto& shape = input.shape();
    if (fill == FillMode::Repeat) {
        time = wrap_coordinate(time, shape.t);
        y = wrap_coordinate(y, shape.h);
        x = wrap_coordinate(x, shape.w);
    } else if (fill == FillMode::Fit) {
        time = std::clamp(time, 0.0F, static_cast<float>(shape.t - 1));
        y = std::clamp(y, 0.0F, static_cast<float>(shape.h - 1));
        x = std::clamp(x, 0.0F, static_cast<float>(shape.w - 1));
    } else if (time < 0.0F || y < 0.0F || x < 0.0F || time > static_cast<float>(shape.t - 1) ||
               y > static_cast<float>(shape.h - 1) || x > static_cast<float>(shape.w - 1)) {
        return (fill == FillMode::Black && channel == 3 && shape.c >= 4) ? 1.0F : 0.0F;
    }

    const auto t0 = static_cast<std::size_t>(std::floor(time));
    const auto y0 = static_cast<std::size_t>(std::floor(y));
    const auto x0 = static_cast<std::size_t>(std::floor(x));
    const auto t1 = std::min(t0 + 1, shape.t - 1);
    const auto y1 = std::min(y0 + 1, shape.h - 1);
    const auto x1 = std::min(x0 + 1, shape.w - 1);
    const auto ft = time - static_cast<float>(t0);
    const auto fy = y - static_cast<float>(y0);
    const auto fx = x - static_cast<float>(x0);

    const auto c000 = input.at(t0, y0, x0, channel);
    const auto c001 = input.at(t0, y0, x1, channel);
    const auto c010 = input.at(t0, y1, x0, channel);
    const auto c011 = input.at(t0, y1, x1, channel);
    const auto c100 = input.at(t1, y0, x0, channel);
    const auto c101 = input.at(t1, y0, x1, channel);
    const auto c110 = input.at(t1, y1, x0, channel);
    const auto c111 = input.at(t1, y1, x1, channel);

    const auto c00 = c000 + (c001 - c000) * fx;
    const auto c01 = c010 + (c011 - c010) * fx;
    const auto c10 = c100 + (c101 - c100) * fx;
    const auto c11 = c110 + (c111 - c110) * fx;
    const auto c0 = c00 + (c01 - c00) * fy;
    const auto c1 = c10 + (c11 - c10) * fy;
    return c0 + (c1 - c0) * ft;
}

void inverse_rotate(Point3& point, const TensorRotationParams& params) {
    rotate_plane(point.y, point.t, -params.yt_degrees);
    rotate_plane(point.x, point.t, -params.xt_degrees);
    rotate_plane(point.x, point.y, -params.xy_degrees);
}

[[nodiscard]] float rotation_fit_scale(const TensorRotationParams& params) {
    std::array<Point3, 3> basis{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    for (auto& point : basis) {
        inverse_rotate(point, params);
    }
    const auto time_extent = std::abs(basis[0].t) + std::abs(basis[1].t) + std::abs(basis[2].t);
    const auto y_extent = std::abs(basis[0].y) + std::abs(basis[1].y) + std::abs(basis[2].y);
    const auto x_extent = std::abs(basis[0].x) + std::abs(basis[1].x) + std::abs(basis[2].x);
    return 1.0F / std::max({1.0F, time_extent, y_extent, x_extent});
}

}  // namespace

VideoTensor space_time_transpose(const VideoTensor& input, SpatialAxis axis) {
    return space_time_transpose(input, {axis, TransposeResolution::NativeTensor});
}

VideoTensor space_time_transpose(const VideoTensor& input, const SpaceTimeTransposeParams& params) {
    const auto& shape = input.shape();
    const auto fit = params.resolution == TransposeResolution::FitSourceCanvas;
    const auto output_shape = params.axis == SpatialAxis::X
                                  ? TensorShape{shape.w, shape.h, fit ? shape.w : shape.t, shape.c}
                                  : TensorShape{shape.h, fit ? shape.h : shape.t, shape.w, shape.c};
    VideoTensor output(output_shape, 0.0F, input.metadata());

    parallel_for(output_shape.t, [&](std::size_t t) {
        for (std::size_t y = 0; y < output_shape.h; ++y) {
            for (std::size_t x = 0; x < output_shape.w; ++x) {
                for (std::size_t c = 0; c < output_shape.c; ++c) {
                    if (!fit) {
                        output.at(t, y, x, c) = params.axis == SpatialAxis::X ? input.at(x, y, t, c) : input.at(y, t, x, c);
                        continue;
                    }
                    const auto spatial_extent = params.axis == SpatialAxis::X ? output_shape.w : output_shape.h;
                    const auto spatial_coordinate = params.axis == SpatialAxis::X ? x : y;
                    const auto source_time = spatial_extent == 1
                                                 ? 0.0F
                                                 : static_cast<float>(spatial_coordinate) * static_cast<float>(shape.t - 1) /
                                                       static_cast<float>(spatial_extent - 1);
                    const auto time0 = static_cast<std::size_t>(std::floor(source_time));
                    const auto time1 = std::min(time0 + 1, shape.t - 1);
                    const auto fraction = source_time - static_cast<float>(time0);
                    const auto value0 = params.axis == SpatialAxis::X ? input.at(time0, y, t, c) : input.at(time0, t, x, c);
                    const auto value1 = params.axis == SpatialAxis::X ? input.at(time1, y, t, c) : input.at(time1, t, x, c);
                    output.at(t, y, x, c) = value0 + (value1 - value0) * fraction;
                }
            }
        }
    });
    return output;
}

VideoTensor luma_time_shift(const VideoTensor& input, const LumaTimeShiftParams& params) {
    const auto& shape = input.shape();
    VideoTensor output(shape, 0.0F, input.metadata());
    parallel_for(shape.t, [&](std::size_t t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                const auto shift = static_cast<std::ptrdiff_t>(std::floor(params.shift_multiplier * shift_value(input, t, y, x, params.source)));
                const auto source_t = resolve_time(static_cast<std::ptrdiff_t>(t) - shift, shape.t, params.edge_behavior);
                for (std::size_t c = 0; c < shape.c; ++c) {
                    output.at(t, y, x, c) = input.at(source_t, y, x, c);
                }
            }
        }
    });
    return output;
}

VideoTensor rgb_time_slip(const VideoTensor& input, const RGBTimeSlipParams& params) {
    const auto& shape = input.shape();
    if (shape.c < 3) {
        throw std::invalid_argument("RGB Time Slip requires at least three channels");
    }
    VideoTensor output(shape, 0.0F, input.metadata());
    const std::array<float, 3> time_offsets{params.red_offset, params.green_offset, params.blue_offset};
    const std::array<float, 3> split_scales{1.0F, 0.0F, -1.0F};
    const auto center_x = 0.5F * static_cast<float>(shape.w - 1);
    const auto center_y = 0.5F * static_cast<float>(shape.h - 1);
    parallel_for(shape.t, [&](std::size_t t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                const auto output_alpha = shape.c >= 4 ? input.at(t, y, x, 3) : 1.0F;
                for (std::size_t c = 0; c < 3; ++c) {
                    auto source_x = static_cast<float>(x);
                    auto source_y = static_cast<float>(y);
                    const auto split = params.spatial_split * split_scales[c];
                    if (params.split_axis == SplitAxis::Horizontal) {
                        source_x -= split;
                    } else if (params.split_axis == SplitAxis::Vertical) {
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
                    auto value = sample_tensor(input, source_t, source_y, source_x, c, params.edge_behavior);
                    if (shape.c >= 4) {
                        const auto source_alpha = sample_tensor(input, source_t, source_y, source_x, 3, params.edge_behavior);
                        value = source_alpha > 0.00001F ? value / source_alpha * output_alpha : 0.0F;
                    }
                    output.at(t, y, x, c) = value;
                }
                if (shape.c >= 4) output.at(t, y, x, 3) = output_alpha;
                for (std::size_t c = 4; c < shape.c; ++c) output.at(t, y, x, c) = input.at(t, y, x, c);
            }
        }
    });
    return output;
}

VideoTensor horizontal_sync_loss(const VideoTensor& input, const HorizontalSyncLossParams& params) {
    const auto& shape = input.shape();
    VideoTensor output(shape, 0.0F, input.metadata());
    const auto band_height = std::max<std::size_t>(1, params.band_height);
    const auto density = std::clamp(params.tear_density, 0.0F, 1.0F);
    const auto maximum_shift = std::clamp(params.shift_fraction, 0.0F, 1.0F) * static_cast<float>(shape.w);
    const auto mix_hash = [&](std::size_t t, std::int64_t band) {
        auto hash = (static_cast<std::uint64_t>(t) + 1) * 0x9E3779B185EBCA87ULL;
        hash ^= static_cast<std::uint64_t>(band) * 0xC2B2AE3D27D4EB4FULL;
        hash ^= params.random_seed * 0xD6E8FEB86659FD93ULL;
        hash ^= hash >> 30U;
        hash *= 0xBF58476D1CE4E5B9ULL;
        hash ^= hash >> 27U;
        return hash;
    };
    parallel_for(shape.t, [&](std::size_t t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            const auto base_band = static_cast<std::int64_t>(y / band_height);
            const auto band = base_band + static_cast<std::int64_t>(std::floor(static_cast<float>(t) * params.drift_speed));
            const auto center_y = std::min((y / band_height) * band_height + band_height / 2, shape.h - 1);
            const auto center_x = shape.w / 2;
            float driver = 0.0F;
            bool tears = density > 0.0F;
            if (params.driver == SyncLossDriver::DeterministicNoise) {
                const auto hash = mix_hash(t, band);
                const auto trigger = static_cast<float>(hash & 0xFFFFFFU) / static_cast<float>(0x1000000U);
                driver = 2.0F * static_cast<float>((hash >> 24U) & 0xFFFFFFU) / static_cast<float>(0xFFFFFFU) - 1.0F;
                tears = tears && trigger < density;
            } else if (params.driver == SyncLossDriver::Luma) {
                driver = 2.0F * std::clamp(luma(input, t, center_y, center_x), 0.0F, 1.0F) - 1.0F;
                tears = tears && std::abs(driver) >= 1.0F - density;
            } else {
                const auto previous_y = center_y == 0 ? 0 : center_y - 1;
                const auto difference = luma(input, t, center_y, center_x) - luma(input, t, previous_y, center_x);
                const auto strength = std::clamp(std::abs(difference) * 4.0F, 0.0F, 1.0F);
                driver = difference < 0.0F ? -strength : strength;
                tears = tears && strength >= 1.0F - density;
            }
            const auto shift = tears ? driver * maximum_shift : 0.0F;
            for (std::size_t x = 0; x < shape.w; ++x) {
                for (std::size_t c = 0; c < shape.c; ++c) {
                    output.at(t, y, x, c) = sample_tensor(
                        input, static_cast<float>(t), static_cast<float>(y), static_cast<float>(x) - shift,
                        c, params.edge_behavior);
                }
            }
        }
    });
    return output;
}

VideoTensor chroma_carrier_drift(const VideoTensor& input, const ChromaCarrierDriftParams& params) {
    const auto& shape = input.shape();
    if (shape.c < 3) throw std::invalid_argument("Chroma Carrier Drift requires RGB input");
    VideoTensor output(shape, 0.0F, input.metadata());
    const auto straight_rgb = [&](float t, float y, float x, std::size_t channel) {
        auto value = sample_tensor(input, t, y, x, channel, params.edge_behavior);
        if (shape.c >= 4) {
            const auto alpha = sample_tensor(input, t, y, x, 3, params.edge_behavior);
            value = alpha > 0.00001F ? value / alpha : 0.0F;
        }
        return value;
    };
    const auto chroma = [&](float t, float y, float x, bool cb) {
        float total = 0.0F;
        constexpr std::array<float, 5> taps{-1.0F, -0.5F, 0.0F, 0.5F, 1.0F};
        for (const auto tap : taps) {
            const auto sample_x = x + tap * std::clamp(params.bleed, 0.0F, 100.0F);
            const auto r = straight_rgb(t, y, sample_x, 0);
            const auto g = straight_rgb(t, y, sample_x, 1);
            const auto b = straight_rgb(t, y, sample_x, 2);
            const auto yy = 0.2126F * r + 0.7152F * g + 0.0722F * b;
            total += cb ? (b - yy) / 1.8556F : (r - yy) / 1.5748F;
        }
        return total / static_cast<float>(taps.size());
    };
    parallel_for(shape.t, [&](std::size_t t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                const auto alpha = shape.c >= 4 ? input.at(t, y, x, 3) : 1.0F;
                const auto current_r = alpha > 0.00001F ? input.at(t, y, x, 0) / alpha : 0.0F;
                const auto current_g = alpha > 0.00001F ? input.at(t, y, x, 1) / alpha : 0.0F;
                const auto current_b = alpha > 0.00001F ? input.at(t, y, x, 2) / alpha : 0.0F;
                const auto yy = 0.2126F * current_r + 0.7152F * current_g + 0.0722F * current_b;
                float cb_sign = 1.0F, cr_sign = 1.0F;
                if (params.mode == ChromaDriftMode::SplitCbCr) cr_sign = -1.0F;
                if (params.mode == ChromaDriftMode::Alternating) {
                    cb_sign = t % 2 == 0 ? 1.0F : -1.0F;
                    cr_sign = -cb_sign;
                }
                const auto cb = chroma(
                    static_cast<float>(t) - cb_sign * params.time_offset,
                    static_cast<float>(y) - cb_sign * params.y_offset,
                    static_cast<float>(x) - cb_sign * params.x_offset, true);
                const auto cr = chroma(
                    static_cast<float>(t) - cr_sign * params.time_offset,
                    static_cast<float>(y) - cr_sign * params.y_offset,
                    static_cast<float>(x) - cr_sign * params.x_offset, false);
                const auto r = yy + 1.5748F * cr;
                const auto b = yy + 1.8556F * cb;
                const auto g = (yy - 0.2126F * r - 0.0722F * b) / 0.7152F;
                output.at(t, y, x, 0) = std::clamp(r, 0.0F, 1.0F) * alpha;
                output.at(t, y, x, 1) = std::clamp(g, 0.0F, 1.0F) * alpha;
                output.at(t, y, x, 2) = std::clamp(b, 0.0F, 1.0F) * alpha;
                if (shape.c >= 4) output.at(t, y, x, 3) = alpha;
                for (std::size_t c = 4; c < shape.c; ++c) output.at(t, y, x, c) = input.at(t, y, x, c);
            }
        }
    });
    return output;
}

VideoTensor stride_error(const VideoTensor& input, const StrideErrorParams& params) {
    const auto& shape = input.shape();
    const auto frame_pixels = shape.h * shape.w;
    VideoTensor output(shape, 0.0F, input.metadata());
    const auto resolve_address = [&](std::int64_t address) {
        const auto count = static_cast<std::int64_t>(frame_pixels);
        if (params.address_edge == AddressEdge::Wrap || count == 1) {
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
    parallel_for(shape.t, [&](std::size_t t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                const auto nominal = static_cast<std::int64_t>(y * shape.w + x);
                const auto wrong_stride = static_cast<double>(shape.w) * (1.0 + std::clamp(params.stride_delta, -0.5F, 0.5F));
                const auto corrupted = static_cast<std::int64_t>(std::llround(
                    static_cast<double>(y) * wrong_stride + static_cast<double>(x) +
                    static_cast<double>(params.base_offset) * static_cast<double>(frame_pixels) +
                    static_cast<double>(t) * static_cast<double>(params.temporal_drift) * static_cast<double>(frame_pixels)));
                const auto delta = corrupted - nominal;
                const auto address_for = [&](std::size_t channel) {
                    const auto factor = params.channel_mode == StrideChannelMode::RgbTogether ? 1 : static_cast<std::int64_t>(channel + 1);
                    return resolve_address(nominal + delta * factor);
                };
                const auto output_alpha = shape.c >= 4
                    ? (params.channel_mode == StrideChannelMode::AlphaIncluded
                        ? input.at(t, address_for(3) / shape.w, address_for(3) % shape.w, 3)
                        : input.at(t, y, x, 3))
                    : 1.0F;
                for (std::size_t c = 0; c < std::min<std::size_t>(3, shape.c); ++c) {
                    const auto address = address_for(c);
                    auto value = input.at(t, address / shape.w, address % shape.w, c);
                    if (shape.c >= 4) {
                        const auto source_alpha = input.at(t, address / shape.w, address % shape.w, 3);
                        value = source_alpha > 0.00001F ? value / source_alpha * output_alpha : 0.0F;
                    }
                    output.at(t, y, x, c) = value;
                }
                if (shape.c >= 4) output.at(t, y, x, 3) = output_alpha;
                for (std::size_t c = 4; c < shape.c; ++c) output.at(t, y, x, c) = input.at(t, y, x, c);
            }
        }
    });
    return output;
}

VideoTensor radial_chrono_funnel(const VideoTensor& input, const RadialChronoFunnelParams& params) {
    const auto& shape = input.shape();
    VideoTensor output(shape, 0.0F, input.metadata());
    const auto center_x = params.center_x * static_cast<float>(shape.w - 1);
    const auto center_y = params.center_y * static_cast<float>(shape.h - 1);
    const auto width = static_cast<float>(std::max<std::size_t>(1, shape.w - 1));
    const auto height = static_cast<float>(std::max<std::size_t>(1, shape.h - 1));

    parallel_for(shape.t, [&](std::size_t t) {
        const auto phase = static_cast<float>(t) / static_cast<float>(std::max<std::size_t>(1, shape.t));
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                const auto dx = (static_cast<float>(x) - center_x) / width;
                const auto dy = (static_cast<float>(y) - center_y) / height;
                const auto radius = std::sqrt(dx * dx + dy * dy);
                const auto turns = std::atan2(dy, dx) / (2.0F * std::numbers::pi_v<float>);
                const auto tau = 2.0F * std::numbers::pi_v<float>;
                const auto warp_gain = std::clamp(std::abs(params.intensity) * 5.0F, 0.0F, 1.25F);
                float weave{};
                float source_radius = radius;
                float source_turns = turns;
                switch (params.topology) {
                    case RadialTopology::TimeLoom: {
                        const auto strand_a = std::sin(tau * (3.0F * turns + 2.0F * radius - 2.0F * phase));
                        const auto strand_b = std::cos(tau * (5.0F * radius - turns + 3.0F * phase));
                        source_turns += params.twist * (0.055F + 0.10F * radius) * strand_a + 0.045F * warp_gain * strand_b;
                        source_radius = std::max(0.0F, radius * (1.0F + 0.22F * warp_gain * strand_b) + 0.035F * warp_gain * strand_a);
                        weave = radius + params.twist * turns + 0.34F * strand_a + 0.18F * strand_b;
                        break;
                    }
                    case RadialTopology::KaleidoFold: {
                        const auto segment = std::fmod(turns * 7.0F + 1000.0F, 1.0F);
                        const auto folded = std::abs(segment - 0.5F) * 2.0F;
                        source_turns = (std::floor(turns * 7.0F) + folded + 0.18F * std::sin(tau * (phase + radius))) / 7.0F;
                        source_radius = std::max(0.0F, radius + 0.08F * warp_gain * std::sin(tau * (14.0F * turns - 2.0F * phase)));
                        weave = 1.4F * folded + radius + params.twist * std::sin(tau * (7.0F * turns + phase));
                        break;
                    }
                    case RadialTopology::EventHorizon: {
                        const auto gravity = std::exp(-4.0F * radius);
                        source_turns += (0.12F + 0.18F * params.twist) * gravity / (radius + 0.045F) + 0.08F * phase;
                        source_radius = std::max(0.0F, radius + warp_gain * gravity * (0.11F * std::sin(tau * (phase * 3.0F - radius * 6.0F)) - 0.07F));
                        weave = std::clamp(0.18F / (radius + 0.045F), 0.0F, 3.5F) +
                                params.twist * std::sin(tau * (6.0F * turns + 2.0F * phase)) * gravity;
                        break;
                    }
                }
                const auto source_angle = tau * source_turns;
                const auto source_x = center_x + std::cos(source_angle) * source_radius * width;
                const auto source_y = center_y + std::sin(source_angle) * source_radius * height;
                const auto source_t = static_cast<float>(t) + params.intensity * static_cast<float>(shape.t) * weave;
                for (std::size_t c = 0; c < shape.c; ++c) {
                    output.at(t, y, x, c) = sample_tensor(input, source_t, source_y, source_x, c, params.edge_behavior);
                }
            }
        }
    });
    return output;
}

VideoTensor temporal_pixel_sort(const VideoTensor& input, const TemporalPixelSortParams& params) {
    const auto& shape = input.shape();
    VideoTensor output = input;
    parallel_for(shape.h, [&](std::size_t y) {
        for (std::size_t x = 0; x < shape.w; ++x) {
            std::vector<std::size_t> eligible;
            eligible.reserve(shape.t);
            for (std::size_t t = 0; t < shape.t; ++t) {
                if (key_for(input, t, y, x, params.criterion) >= params.threshold) {
                    eligible.push_back(t);
                }
            }
            auto sorted = eligible;
            std::stable_sort(sorted.begin(), sorted.end(), [&](std::size_t left, std::size_t right) {
                const auto left_key = key_for(input, left, y, x, params.criterion);
                const auto right_key = key_for(input, right, y, x, params.criterion);
                return params.direction == SortDirection::Ascending ? left_key < right_key : left_key > right_key;
            });
            for (std::size_t index = 0; index < eligible.size(); ++index) {
                for (std::size_t c = 0; c < shape.c; ++c) {
                    output.at(eligible[index], y, x, c) = input.at(sorted[index], y, x, c);
                }
            }
        }
    });
    return output;
}

VideoTensor tensor_3d_rotation(const VideoTensor& input, const TensorRotationParams& params) {
    const auto& shape = input.shape();
    VideoTensor output(shape, 0.0F, input.metadata());
    const auto fit_scale = params.fill_mode == FillMode::Fit ? rotation_fit_scale(params) : 1.0F;
    parallel_for(shape.t, [&](std::size_t t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                Point3 point{normalized_coordinate(t, shape.t), normalized_coordinate(y, shape.h), normalized_coordinate(x, shape.w)};
                point.t *= fit_scale;
                point.y *= fit_scale;
                point.x *= fit_scale;
                // Backward sampling uses the inverse of the forward XY -> XT -> YT rotation.
                inverse_rotate(point, params);
                const auto source_t = denormalized_coordinate(point.t, shape.t);
                const auto source_y = denormalized_coordinate(point.y, shape.h);
                const auto source_x = denormalized_coordinate(point.x, shape.w);
                for (std::size_t c = 0; c < shape.c; ++c) {
                    output.at(t, y, x, c) = trilinear(input, source_t, source_y, source_x, c, params.fill_mode);
                }
            }
        }
    });
    return output;
}

VideoTensor selective_prefilter(const VideoTensor& input, const SelectivePrefilterParams& params) {
    if (params.spatial == PrefilterStrength::Off && params.temporal == PrefilterStrength::Off) {
        return input;
    }
    const auto amount = [](PrefilterStrength strength) {
        switch (strength) {
            case PrefilterStrength::Off: return 0.0F;
            case PrefilterStrength::Light: return 0.2F;
            case PrefilterStrength::Strong: return 0.4F;
        }
        throw std::invalid_argument("Invalid prefilter strength");
    };
    const auto spatial = amount(params.spatial);
    const auto temporal = amount(params.temporal);
    const auto& shape = input.shape();
    VideoTensor output(shape, 0.0F, input.metadata());
    const auto clamped = [](std::size_t value, int offset, std::size_t extent) {
        return static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(
            static_cast<std::ptrdiff_t>(value) + offset, 0, static_cast<std::ptrdiff_t>(extent - 1)));
    };
    parallel_for(shape.t, [&](std::size_t t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                for (std::size_t c = 0; c < shape.c; ++c) {
                    const auto center = input.at(t, y, x, c);
                    auto value = center;
                    if (spatial > 0.0F) {
                        const auto average = (input.at(t, clamped(y, -1, shape.h), x, c) +
                                              input.at(t, clamped(y, 1, shape.h), x, c) +
                                              input.at(t, y, clamped(x, -1, shape.w), c) +
                                              input.at(t, y, clamped(x, 1, shape.w), c)) *
                                             0.25F;
                        value += spatial * (average - center);
                    }
                    if (temporal > 0.0F) {
                        const auto average = (input.at(clamped(t, -1, shape.t), y, x, c) +
                                              input.at(clamped(t, 1, shape.t), y, x, c)) *
                                             0.5F;
                        value += temporal * (average - center);
                    }
                    output.at(t, y, x, c) = value;
                }
            }
        }
    });
    return output;
}

VideoTensor dimensional_splicer(
    const VideoTensor& source,
    const VideoTensor& driver,
    const DimensionalSplicerParams& params) {
    std::array<TensorAxisSource, 3> selections{params.output_x, params.output_y, params.output_t};
    std::array<bool, 3> semantic_axes{};
    for (auto& selection : selections) {
        auto semantic = static_cast<std::size_t>(selection) % 3;
        if (semantic_axes[semantic]) {
            const auto replacement = std::find(semantic_axes.begin(), semantic_axes.end(), false);
            semantic = static_cast<std::size_t>(std::distance(semantic_axes.begin(), replacement));
            const auto source_offset = static_cast<int>(selection) >= 3 ? 3 : 0;
            selection = static_cast<TensorAxisSource>(source_offset + static_cast<int>(semantic));
        }
        semantic_axes[semantic] = true;
    }
    const auto extent = [&](TensorAxisSource selection) {
        const auto& shape = static_cast<int>(selection) < 3 ? source.shape() : driver.shape();
        switch (static_cast<int>(selection) % 3) {
            case 0: return shape.w;
            case 1: return shape.h;
            default: return shape.t;
        }
    };
    const TensorShape output_shape{extent(selections[2]), extent(selections[1]), extent(selections[0]), source.shape().c};
    const auto output_metadata = selections[2] == TensorAxisSource::BT ? driver.metadata() : source.metadata();
    VideoTensor output(output_shape, 0.0F, output_metadata);
    const auto scale = [](std::size_t coordinate, std::size_t output_extent, std::size_t source_extent) {
        return output_extent <= 1 || source_extent <= 1
                   ? 0.0F
                   : static_cast<float>(coordinate) * static_cast<float>(source_extent - 1) /
                         static_cast<float>(output_extent - 1);
    };
    parallel_for(output_shape.t, [&](std::size_t t) {
        for (std::size_t y = 0; y < output_shape.h; ++y) {
            for (std::size_t x = 0; x < output_shape.w; ++x) {
                std::array<float, 3> source_coordinates{};  // X, Y, T
                const std::array<std::size_t, 3> coordinates{x, y, t};
                const std::array<std::size_t, 3> output_extents{output_shape.w, output_shape.h, output_shape.t};
                const auto driver_t = scale(t, output_shape.t, driver.shape().t);
                const auto driver_y = scale(y, output_shape.h, driver.shape().h);
                const auto driver_x = scale(x, output_shape.w, driver.shape().w);
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const auto semantic = static_cast<std::size_t>(selections[axis]) % 3;
                    const auto source_extent = semantic == 0 ? source.shape().w : (semantic == 1 ? source.shape().h : source.shape().t);
                    if (static_cast<int>(selections[axis]) < 3) {
                        source_coordinates[semantic] = scale(coordinates[axis], output_extents[axis], source_extent);
                    } else {
                        const auto map_channel = std::min(semantic, driver.shape().c - 1);
                        const auto map_value = std::clamp(sample_tensor(
                            driver, driver_t, driver_y, driver_x, map_channel, EdgeBehavior::Clamp), 0.0F, 1.0F);
                        source_coordinates[semantic] = map_value * static_cast<float>(source_extent - 1);
                    }
                }
                for (std::size_t c = 0; c < output_shape.c; ++c) {
                    if (params.interpolation == TensorInterpolation::Nearest) {
                        output.at(t, y, x, c) = source.at(
                            static_cast<std::size_t>(std::round(source_coordinates[2])),
                            static_cast<std::size_t>(std::round(source_coordinates[1])),
                            static_cast<std::size_t>(std::round(source_coordinates[0])), c);
                    } else if (params.interpolation == TensorInterpolation::Cubic) {
                        output.at(t, y, x, c) = sample_tensor_cubic(
                            source, source_coordinates[2], source_coordinates[1], source_coordinates[0], c);
                    } else {
                        output.at(t, y, x, c) = sample_tensor(
                            source, source_coordinates[2], source_coordinates[1], source_coordinates[0], c, EdgeBehavior::Clamp);
                    }
                }
            }
        }
    });
    return output;
}

VideoTensor tensor_displacement(
    const VideoTensor& target,
    const VideoTensor& displacement,
    const TensorDisplacementParams& params) {
    TensorShape output_shape = target.shape();
    if (params.broadcast == TensorBroadcast::Crop) {
        output_shape.t = std::min(target.shape().t, displacement.shape().t);
        output_shape.h = std::min(target.shape().h, displacement.shape().h);
        output_shape.w = std::min(target.shape().w, displacement.shape().w);
    }
    VideoTensor output(output_shape, 0.0F, target.metadata());
    const auto driver_coordinate = [&](std::size_t coordinate, std::size_t output_extent, std::size_t driver_extent) {
        if (params.broadcast == TensorBroadcast::Stretch) {
            return output_extent <= 1 || driver_extent <= 1
                       ? std::size_t{0}
                       : static_cast<std::size_t>(std::round(
                             static_cast<double>(coordinate) * static_cast<double>(driver_extent - 1) /
                             static_cast<double>(output_extent - 1)));
        }
        return std::min(coordinate, driver_extent - 1);
    };
    parallel_for(output_shape.t, [&](std::size_t t) {
        const auto dt = driver_coordinate(t, output_shape.t, displacement.shape().t);
        for (std::size_t y = 0; y < output_shape.h; ++y) {
            const auto dy = driver_coordinate(y, output_shape.h, displacement.shape().h);
            for (std::size_t x = 0; x < output_shape.w; ++x) {
                const auto dx = driver_coordinate(x, output_shape.w, displacement.shape().w);
                const auto amount = shift_value(displacement, dt, dy, dx, params.source);
                const auto source_t = static_cast<float>(t) + params.time_multiplier * amount;
                const auto source_y = static_cast<float>(y) + params.y_multiplier * amount;
                const auto source_x = static_cast<float>(x) + params.x_multiplier * amount;
                for (std::size_t c = 0; c < output_shape.c; ++c) {
                    output.at(t, y, x, c) = sample_tensor(target, source_t, source_y, source_x, c, params.edge_behavior);
                }
            }
        }
    });
    return output;
}

VideoTensor optical_flow_time_warp(const VideoTensor& input, const OpticalFlowTimeWarpParams& params) {
    const auto& shape = input.shape();
    VideoTensor output(shape, 0.0F, input.metadata());
    const auto clamped = [](std::size_t value, int offset, std::size_t extent) {
        return static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(
            static_cast<std::ptrdiff_t>(value) + offset, 0, static_cast<std::ptrdiff_t>(extent - 1)));
    };
    parallel_for(shape.t, [&](std::size_t t) {
        const auto next_t = std::min(t + 1, shape.t - 1);
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                float xx = 0.0F;
                float yy = 0.0F;
                float xy = 0.0F;
                float xt = 0.0F;
                float yt = 0.0F;
                for (int oy = -1; oy <= 1; ++oy) {
                    const auto sy = clamped(y, oy, shape.h);
                    for (int ox = -1; ox <= 1; ++ox) {
                        const auto sx = clamped(x, ox, shape.w);
                        const auto ix = 0.5F * (luma(input, t, sy, clamped(sx, 1, shape.w)) -
                                                luma(input, t, sy, clamped(sx, -1, shape.w)));
                        const auto iy = 0.5F * (luma(input, t, clamped(sy, 1, shape.h), sx) -
                                                luma(input, t, clamped(sy, -1, shape.h), sx));
                        const auto it = luma(input, next_t, sy, sx) - luma(input, t, sy, sx);
                        xx += ix * ix;
                        yy += iy * iy;
                        xy += ix * iy;
                        xt += ix * it;
                        yt += iy * it;
                    }
                }
                const auto determinant = (xx + 0.0001F) * (yy + 0.0001F) - xy * xy;
                const auto flow_x = determinant == 0.0F ? 0.0F : (-xt * (yy + 0.0001F) + xy * yt) / determinant;
                const auto flow_y = determinant == 0.0F ? 0.0F : (xy * xt - (xx + 0.0001F) * yt) / determinant;
                auto magnitude = std::sqrt(flow_x * flow_x + flow_y * flow_y);
                const auto angle = std::atan2(flow_y, flow_x) * 180.0F / std::numbers::pi_v<float>;
                auto difference = std::fmod(std::abs(angle - params.direction_degrees), 360.0F);
                difference = std::min(difference, 360.0F - difference);
                if (difference > std::clamp(params.direction_tolerance, 0.0F, 180.0F)) {
                    magnitude = 0.0F;
                }
                magnitude = std::max(0.0F, magnitude - std::max(0.0F, params.sensitivity));
                const auto source_t = static_cast<float>(t) + params.intensity * magnitude;
                for (std::size_t c = 0; c < shape.c; ++c) {
                    output.at(t, y, x, c) = sample_tensor(input, source_t, static_cast<float>(y), static_cast<float>(x), c, params.edge_behavior);
                }
            }
        }
    });
    return output;
}

VideoTensor chrono_feedback(const VideoTensor& input, const ChronoFeedbackParams& params) {
    const auto& shape = input.shape();
    VideoTensor output(shape, 0.0F, input.metadata());
    const auto past_amount = std::clamp(params.past_amount, 0.0F, 1.0F);
    const auto future_amount = std::clamp(params.future_amount, 0.0F, 1.0F);
    const auto blend = [&](float base, float layer) {
        switch (params.blend_mode) {
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
        const auto has_past = params.past_delay > 0 && t >= params.past_delay;
        const auto past_t = has_past ? t - params.past_delay : 0;
        const auto future_t = std::min(t + params.future_delay, shape.t - 1);
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                if (params.blend_mode == FeedbackBlendMode::Displace) {
                    const auto pixel_luma = [&](const VideoTensor& tensor, std::size_t sample_t) {
                        return luma(tensor, sample_t, y, x);
                    };
                    const auto past_luma = has_past ? pixel_luma(output, past_t) : 0.5F;
                    const auto future_luma = pixel_luma(input, future_t);
                    const auto source_x = static_cast<float>(x) +
                        (past_luma - 0.5F) * 0.5F * past_amount * static_cast<float>(shape.w);
                    const auto source_y = static_cast<float>(y) +
                        (future_luma - 0.5F) * 0.5F * future_amount * static_cast<float>(shape.h);
                    for (std::size_t c = 0; c < shape.c; ++c) {
                        output.at(t, y, x, c) = sample_tensor(
                            input, static_cast<float>(t), source_y, source_x, c, EdgeBehavior::Clamp);
                    }
                    continue;
                }
                for (std::size_t c = 0; c < shape.c; ++c) {
                    const auto current = input.at(t, y, x, c);
                    const auto past = has_past ? output.at(past_t, y, x, c) : current;
                    const auto future = input.at(future_t, y, x, c);
                    if (params.blend_mode == FeedbackBlendMode::Add) {
                        const auto base_amount = std::max(0.0F, 1.0F - past_amount - future_amount);
                        output.at(t, y, x, c) = current * base_amount + past * past_amount + future * future_amount;
                    } else {
                        auto value = current + (blend(current, past) - current) * past_amount;
                        value += (blend(value, future) - value) * future_amount;
                        output.at(t, y, x, c) = value;
                    }
                }
            }
        }
    }
    return output;
}

VideoTensor seamless_loop(const VideoTensor& input, const SeamlessLoopParams& params) {
    const auto& shape = input.shape();
    if (shape.t <= 1) return input;
    if (params.mode == SeamlessLoopMode::PingPong) {
        const TensorShape output_shape{shape.t * 2 - 2, shape.h, shape.w, shape.c};
        VideoTensor output(output_shape, 0.0F, input.metadata());
        parallel_for(output_shape.t, [&](std::size_t t) {
            const auto source_t = t < shape.t ? t : (2 * shape.t - 2 - t);
            for (std::size_t y = 0; y < shape.h; ++y) {
                for (std::size_t x = 0; x < shape.w; ++x) {
                    for (std::size_t c = 0; c < shape.c; ++c) {
                        output.at(t, y, x, c) = input.at(source_t, y, x, c);
                    }
                }
            }
        });
        return output;
    }

    const auto maximum_transition = std::max<std::size_t>(1, shape.t / 2);
    const auto requested = std::max<std::size_t>(1, params.transition_frames);
    const auto transition = std::min(requested, maximum_transition);
    const TensorShape output_shape{shape.t - transition, shape.h, shape.w, shape.c};
    VideoTensor output(output_shape, 0.0F, input.metadata());
    const auto softness = std::clamp(params.weave_softness, 0.01F, 0.5F);
    const auto smoothstep = [](float edge0, float edge1, float value) {
        const auto normalized = std::clamp((value - edge0) / std::max(0.0001F, edge1 - edge0), 0.0F, 1.0F);
        return normalized * normalized * (3.0F - 2.0F * normalized);
    };
    parallel_for(output_shape.t, [&](std::size_t t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                if (t >= transition) {
                    for (std::size_t c = 0; c < shape.c; ++c) output.at(t, y, x, c) = input.at(t, y, x, c);
                    continue;
                }
                const auto tail_t = t + output_shape.t;
                const auto progress = transition <= 1 ? 0.5F : static_cast<float>(t) / static_cast<float>(transition - 1);
                auto mix = smoothstep(0.0F, 1.0F, progress);
                if (params.mode == SeamlessLoopMode::LumaWeave && transition > 1) {
                    if (t == 0) {
                        mix = 0.0F;
                    } else if (t + 1 == transition) {
                        mix = 1.0F;
                    } else {
                        const auto threshold = std::clamp(
                            0.5F * (luma(input, t, y, x) + luma(input, tail_t, y, x)), 0.0F, 1.0F);
                        mix = smoothstep(threshold - softness, threshold + softness, progress);
                    }
                }
                for (std::size_t c = 0; c < shape.c; ++c) {
                    const auto tail = input.at(tail_t, y, x, c);
                    output.at(t, y, x, c) = tail + (input.at(t, y, x, c) - tail) * mix;
                }
            }
        }
    });
    return output;
}

VideoTensor structural_datamosh(const VideoTensor& input, const StructuralDatamoshParams& params) {
    const auto& shape = input.shape();
    VideoTensor output = input;
    const auto should_trigger = [&](std::size_t t, std::size_t y, std::size_t x,
                                    std::size_t previous_t, std::size_t previous_y, std::size_t previous_x) {
        switch (params.trigger) {
            case FreezeTrigger::Edge:
                return std::abs(luma(input, t, y, x) - luma(input, previous_t, previous_y, previous_x)) > params.threshold;
            case FreezeTrigger::Luma:
                return luma(input, t, y, x) >= params.threshold;
            case FreezeTrigger::Random: {
                std::uint64_t hash = (static_cast<std::uint64_t>(t) + 1) * 0x9E3779B185EBCA87ULL;
                hash ^= (static_cast<std::uint64_t>(y) + 1) * 0xC2B2AE3D27D4EB4FULL;
                hash ^= (static_cast<std::uint64_t>(x) + 1) * 0x165667B19E3779F9ULL;
                hash ^= params.random_seed * 0xD6E8FEB86659FD93ULL;
                hash ^= hash >> 29U;
                return static_cast<double>(hash & 0xFFFFFFU) / static_cast<double>(0x1000000U) <
                       std::clamp(static_cast<double>(params.probability), 0.0, 1.0);
            }
        }
        return false;
    };
    const auto process_line = [&](std::size_t length, auto coordinate) {
        std::size_t remaining = 0;
        for (std::size_t index = 1; index < length; ++index) {
            const auto [t, y, x] = coordinate(index);
            const auto [pt, py, px] = coordinate(index - 1);
            if (remaining == 0 && should_trigger(t, y, x, pt, py, px)) {
                remaining = params.max_hold;
            }
            if (remaining > 0) {
                for (std::size_t c = 0; c < shape.c; ++c) {
                    output.at(t, y, x, c) = output.at(pt, py, px, c);
                }
                --remaining;
            }
        }
    };
    switch (params.axis) {
        case FreezeAxis::Time:
            parallel_for(shape.h * shape.w, [&](std::size_t line) {
                const auto y = line / shape.w;
                const auto x = line % shape.w;
                process_line(shape.t, [&](std::size_t index) { return std::array<std::size_t, 3>{index, y, x}; });
            });
            break;
        case FreezeAxis::Horizontal:
            parallel_for(shape.t * shape.h, [&](std::size_t line) {
                const auto t = line / shape.h;
                const auto y = line % shape.h;
                process_line(shape.w, [&](std::size_t index) { return std::array<std::size_t, 3>{t, y, index}; });
            });
            break;
        case FreezeAxis::Vertical:
            parallel_for(shape.t * shape.w, [&](std::size_t line) {
                const auto t = line / shape.w;
                const auto x = line % shape.w;
                process_line(shape.h, [&](std::size_t index) { return std::array<std::size_t, 3>{t, index, x}; });
            });
            break;
    }
    return output;
}

}  // namespace chronoforge
