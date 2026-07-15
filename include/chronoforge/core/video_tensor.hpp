#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace chronoforge {

enum class ColorTransfer { Linear, SRgb, Rec709, Pq, Hlg };
enum class AlphaRepresentation { None, Straight, Premultiplied };

// Decode converts source media to a declared working space before effects run.
// Frame rate belongs to playback/export policy and is deliberately preserved
// across dimension-changing effects unless the user changes it at Output.
struct VideoTensorMetadata {
    std::uint32_t frame_rate_numerator{30};
    std::uint32_t frame_rate_denominator{1};
    ColorTransfer transfer{ColorTransfer::Linear};
    AlphaRepresentation alpha{AlphaRepresentation::None};

    [[nodiscard]] bool operator==(const VideoTensorMetadata&) const = default;
};

// Canonical storage order is always T, H, W, C. Effects may change the first
// three extents, but never need to reinterpret the underlying channel stride.
struct TensorShape {
    std::size_t t{};
    std::size_t h{};
    std::size_t w{};
    std::size_t c{};

    [[nodiscard]] bool valid() const noexcept { return t > 0 && h > 0 && w > 0 && c > 0; }

    [[nodiscard]] std::size_t element_count() const {
        if (!valid()) {
            throw std::invalid_argument("TensorShape dimensions must be non-zero");
        }
        std::size_t value = t;
        for (const auto dimension : {h, w, c}) {
            if (value > std::numeric_limits<std::size_t>::max() / dimension) {
                throw std::overflow_error("TensorShape element count overflows size_t");
            }
            value *= dimension;
        }
        return value;
    }

    [[nodiscard]] std::size_t byte_count() const {
        const auto elements = element_count();
        if (elements > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
            throw std::overflow_error("TensorShape byte count overflows size_t");
        }
        return elements * sizeof(float);
    }

    [[nodiscard]] bool operator==(const TensorShape&) const = default;
};

class VideoTensor {
public:
    explicit VideoTensor(TensorShape shape, float initial_value = 0.0F, VideoTensorMetadata metadata = {})
        : shape_(shape), metadata_(metadata), values_(shape.element_count(), initial_value) {}

    VideoTensor(TensorShape shape, std::vector<float> values, VideoTensorMetadata metadata = {})
        : shape_(shape), metadata_(metadata), values_(std::move(values)) {
        if (values_.size() != shape_.element_count()) {
            throw std::invalid_argument("VideoTensor data size does not match shape");
        }
    }

    [[nodiscard]] const TensorShape& shape() const noexcept { return shape_; }
    [[nodiscard]] const VideoTensorMetadata& metadata() const noexcept { return metadata_; }
    [[nodiscard]] std::vector<float>& values() noexcept { return values_; }
    [[nodiscard]] const std::vector<float>& values() const noexcept { return values_; }

    [[nodiscard]] float& at(std::size_t t, std::size_t y, std::size_t x, std::size_t channel) {
        return values_.at(index(t, y, x, channel));
    }

    [[nodiscard]] const float& at(std::size_t t, std::size_t y, std::size_t x, std::size_t channel) const {
        return values_.at(index(t, y, x, channel));
    }

    [[nodiscard]] std::size_t index(std::size_t t, std::size_t y, std::size_t x, std::size_t channel) const {
        if (t >= shape_.t || y >= shape_.h || x >= shape_.w || channel >= shape_.c) {
            throw std::out_of_range("VideoTensor coordinate is outside the tensor");
        }
        return (((t * shape_.h) + y) * shape_.w + x) * shape_.c + channel;
    }

private:
    TensorShape shape_;
    VideoTensorMetadata metadata_;
    std::vector<float> values_;
};

}  // namespace chronoforge
