#include "chronoforge/core/mapped_tensor.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace chronoforge {
namespace {

[[nodiscard]] std::runtime_error system_error(const std::string& action, const std::filesystem::path& path) {
    return std::runtime_error(action + " '" + path.string() + "': " + std::strerror(errno));
}

}  // namespace

MappedTensor MappedTensor::open(const std::filesystem::path& path, TensorShape shape, Access access) {
    const auto expected_bytes = shape.byte_count();
    const auto flags = access == Access::ReadOnly ? O_RDONLY : O_RDWR;
    const auto descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        throw system_error("Unable to open tensor file", path);
    }
    struct stat status {};
    if (fstat(descriptor, &status) != 0 || status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) != expected_bytes) {
        const auto saved_errno = errno;
        ::close(descriptor);
        errno = saved_errno;
        throw std::runtime_error("Tensor file size does not match its declared shape: " + path.string());
    }
    const auto protection = access == Access::ReadOnly ? PROT_READ : (PROT_READ | PROT_WRITE);
    void* mapping = mmap(nullptr, expected_bytes, protection, MAP_SHARED, descriptor, 0);
    if (mapping == MAP_FAILED) {
        const auto error = system_error("Unable to map tensor file", path);
        ::close(descriptor);
        throw error;
    }
    return MappedTensor(path, shape, access, descriptor, mapping);
}

MappedTensor MappedTensor::create(const std::filesystem::path& path, TensorShape shape) {
    std::filesystem::create_directories(path.parent_path());
    const auto bytes = shape.byte_count();
    const auto descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0) {
        throw system_error("Unable to create tensor file", path);
    }
    if (bytes > static_cast<std::size_t>(std::numeric_limits<off_t>::max()) ||
        ftruncate(descriptor, static_cast<off_t>(bytes)) != 0) {
        const auto error = system_error("Unable to allocate tensor file", path);
        ::close(descriptor);
        throw error;
    }
    void* mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    if (mapping == MAP_FAILED) {
        const auto error = system_error("Unable to map tensor file", path);
        ::close(descriptor);
        throw error;
    }
    return MappedTensor(path, shape, Access::ReadWrite, descriptor, mapping);
}

MappedTensor::MappedTensor(std::filesystem::path path, TensorShape shape, Access access, int descriptor, void* mapping)
    : path_(std::move(path)), shape_(shape), access_(access), descriptor_(descriptor), mapping_(mapping) {}

MappedTensor::MappedTensor(MappedTensor&& other) noexcept
    : path_(std::move(other.path_)),
      shape_(other.shape_),
      access_(other.access_),
      descriptor_(other.descriptor_),
      mapping_(other.mapping_) {
    other.descriptor_ = -1;
    other.mapping_ = nullptr;
}

MappedTensor& MappedTensor::operator=(MappedTensor&& other) noexcept {
    if (this != &other) {
        close();
        path_ = std::move(other.path_);
        shape_ = other.shape_;
        access_ = other.access_;
        descriptor_ = other.descriptor_;
        mapping_ = other.mapping_;
        other.descriptor_ = -1;
        other.mapping_ = nullptr;
    }
    return *this;
}

MappedTensor::~MappedTensor() { close(); }

float* MappedTensor::mutable_data() {
    if (access_ == Access::ReadOnly) {
        throw std::logic_error("Cannot modify a read-only mapped tensor");
    }
    return static_cast<float*>(mapping_);
}

float MappedTensor::at(std::size_t t, std::size_t y, std::size_t x, std::size_t channel) const {
    return data()[index(t, y, x, channel)];
}

void MappedTensor::set(std::size_t t, std::size_t y, std::size_t x, std::size_t channel, float value) {
    mutable_data()[index(t, y, x, channel)] = value;
}

void MappedTensor::sync() {
    if (mapping_ != nullptr && access_ == Access::ReadWrite && msync(mapping_, shape_.byte_count(), MS_SYNC) != 0) {
        throw system_error("Unable to flush tensor file", path_);
    }
}

std::size_t MappedTensor::index(std::size_t t, std::size_t y, std::size_t x, std::size_t channel) const {
    if (t >= shape_.t || y >= shape_.h || x >= shape_.w || channel >= shape_.c) {
        throw std::out_of_range("Mapped tensor coordinate is outside its shape");
    }
    return (((t * shape_.h) + y) * shape_.w + x) * shape_.c + channel;
}

void MappedTensor::close() noexcept {
    if (mapping_ != nullptr) {
        munmap(mapping_, shape_.byte_count());
        mapping_ = nullptr;
    }
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
}

}  // namespace chronoforge
