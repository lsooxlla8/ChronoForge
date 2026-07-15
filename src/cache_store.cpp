#include "chronoforge/core/cache_store.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace chronoforge {
namespace {

constexpr std::uint32_t kMagic = 0x43465247;  // CFRG
constexpr std::uint32_t kVersion = 1;

struct ChunkHeader {
    std::uint32_t magic{kMagic};
    std::uint32_t version{kVersion};
    std::array<std::uint64_t, 6> extent{};
    std::uint64_t value_count{};
};

[[nodiscard]] bool is_safe_key(const std::string& key) {
    return !key.empty() && key.find("..") == std::string::npos && key.find('/') == std::string::npos &&
           key.find('\\') == std::string::npos;
}

[[nodiscard]] ChunkHeader make_header(const ChunkExtent& extent, std::size_t value_count) {
    return {
        kMagic,
        kVersion,
        {extent.t_begin, extent.t_end, extent.y_begin, extent.y_end, extent.x_begin, extent.x_end},
        value_count,
    };
}

[[nodiscard]] ChunkExtent extent_from(const ChunkHeader& header) {
    return {
        static_cast<std::size_t>(header.extent[0]),
        static_cast<std::size_t>(header.extent[1]),
        static_cast<std::size_t>(header.extent[2]),
        static_cast<std::size_t>(header.extent[3]),
        static_cast<std::size_t>(header.extent[4]),
        static_cast<std::size_t>(header.extent[5]),
    };
}

}  // namespace

DiskCacheStore::DiskCacheStore(std::filesystem::path root) : root_(std::move(root)) {
    std::filesystem::create_directories(root_);
}

std::filesystem::path DiskCacheStore::path_for(const std::string& content_key, const ChunkExtent& extent) const {
    if (!is_safe_key(content_key)) {
        throw std::invalid_argument("Cache content key must be a single safe path component");
    }
    const auto filename = std::to_string(extent.t_begin) + "-" + std::to_string(extent.t_end) + "_" +
                          std::to_string(extent.y_begin) + "-" + std::to_string(extent.y_end) + "_" +
                          std::to_string(extent.x_begin) + "-" + std::to_string(extent.x_end) + ".cfc";
    return root_ / content_key / filename;
}

void DiskCacheStore::write(const std::string& content_key, const ChunkExtent& extent, std::span<const float> values) const {
    const auto target = path_for(content_key, extent);
    std::filesystem::create_directories(target.parent_path());
    const auto temporary = target.string() + ".partial";
    if (values.size_bytes() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::overflow_error("ChronoForge cache chunk is too large to write");
    }

    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            throw std::runtime_error("Unable to open temporary ChronoForge cache chunk");
        }
        const auto header = make_header(extent, values.size());
        stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
        stream.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size_bytes()));
        if (!stream) {
            throw std::runtime_error("Unable to write ChronoForge cache chunk");
        }
    }
    std::filesystem::rename(temporary, target);
}

std::optional<CachedChunk> DiskCacheStore::read(const std::string& content_key, const ChunkExtent& extent) const {
    const auto target = path_for(content_key, extent);
    if (!std::filesystem::exists(target)) {
        return std::nullopt;
    }
    std::ifstream stream(target, std::ios::binary);
    ChunkHeader header;
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!stream || header.magic != kMagic || header.version != kVersion || extent_from(header).t_begin != extent.t_begin ||
        extent_from(header).t_end != extent.t_end || extent_from(header).y_begin != extent.y_begin ||
        extent_from(header).y_end != extent.y_end || extent_from(header).x_begin != extent.x_begin ||
        extent_from(header).x_end != extent.x_end) {
        return std::nullopt;
    }
    if (header.value_count > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    const auto value_count = static_cast<std::size_t>(header.value_count);
    if (value_count > std::numeric_limits<std::size_t>::max() / sizeof(float) ||
        value_count * sizeof(float) > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        return std::nullopt;
    }
    std::vector<float> values(value_count);
    stream.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(float)));
    if (!stream) {
        return std::nullopt;
    }
    return CachedChunk{extent_from(header), std::move(values)};
}

}  // namespace chronoforge
