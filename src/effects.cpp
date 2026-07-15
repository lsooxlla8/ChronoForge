#include "chronoforge/core/effects.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <exception>
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

VideoTensor radial_chrono_funnel(const VideoTensor& input, const RadialChronoFunnelParams& params) {
    const auto& shape = input.shape();
    VideoTensor output(shape, 0.0F, input.metadata());
    const auto center_x = params.center_x * static_cast<float>(shape.w - 1);
    const auto center_y = params.center_y * static_cast<float>(shape.h - 1);

    parallel_for(shape.t, [&](std::size_t t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                const auto dx = static_cast<float>(x) - center_x;
                const auto dy = static_cast<float>(y) - center_y;
                const auto shift = static_cast<std::ptrdiff_t>(std::floor(params.intensity * std::sqrt(dx * dx + dy * dy)));
                const auto source_t = resolve_time(static_cast<std::ptrdiff_t>(t) + shift, shape.t, params.edge_behavior);
                for (std::size_t c = 0; c < shape.c; ++c) {
                    output.at(t, y, x, c) = input.at(source_t, y, x, c);
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
    parallel_for(shape.t, [&](std::size_t t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                Point3 point{normalized_coordinate(t, shape.t), normalized_coordinate(y, shape.h), normalized_coordinate(x, shape.w)};
                // Backward sampling uses the inverse of the forward XY -> XT -> YT rotation.
                rotate_plane(point.y, point.t, -params.yt_degrees);
                rotate_plane(point.x, point.t, -params.xt_degrees);
                rotate_plane(point.x, point.y, -params.xy_degrees);
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

}  // namespace chronoforge
