#pragma once

#include "chronoforge/core/video_tensor.hpp"

#include <filesystem>

namespace chronoforge {

class MappedTensor {
public:
    enum class Access { ReadOnly, ReadWrite };

    static MappedTensor open(const std::filesystem::path& path, TensorShape shape, Access access);
    static MappedTensor create(const std::filesystem::path& path, TensorShape shape);

    MappedTensor(MappedTensor&& other) noexcept;
    MappedTensor& operator=(MappedTensor&& other) noexcept;
    MappedTensor(const MappedTensor&) = delete;
    MappedTensor& operator=(const MappedTensor&) = delete;
    ~MappedTensor();

    [[nodiscard]] const TensorShape& shape() const noexcept { return shape_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] const float* data() const noexcept { return static_cast<const float*>(mapping_); }
    [[nodiscard]] float* mutable_data();

    [[nodiscard]] float at(std::size_t t, std::size_t y, std::size_t x, std::size_t channel) const;
    void set(std::size_t t, std::size_t y, std::size_t x, std::size_t channel, float value);
    void sync();

private:
    MappedTensor(std::filesystem::path path, TensorShape shape, Access access, int descriptor, void* mapping);
    [[nodiscard]] std::size_t index(std::size_t t, std::size_t y, std::size_t x, std::size_t channel) const;
    void close() noexcept;

    std::filesystem::path path_;
    TensorShape shape_{};
    Access access_{Access::ReadOnly};
    int descriptor_{-1};
    void* mapping_{nullptr};
};

}  // namespace chronoforge
