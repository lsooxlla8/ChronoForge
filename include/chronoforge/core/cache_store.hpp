#pragma once

#include "chronoforge/core/resource_planner.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace chronoforge {

struct CachedChunk {
    ChunkExtent extent;
    std::vector<float> values;
};

// Content keys are graph-signature hashes. Chunks are written through a
// temporary file then atomically renamed, so cancelled work cannot poison cache.
class DiskCacheStore {
public:
    explicit DiskCacheStore(std::filesystem::path root);

    void write(const std::string& content_key, const ChunkExtent& extent, std::span<const float> values) const;
    [[nodiscard]] std::optional<CachedChunk> read(const std::string& content_key, const ChunkExtent& extent) const;
    [[nodiscard]] std::filesystem::path root() const { return root_; }

private:
    [[nodiscard]] std::filesystem::path path_for(const std::string& content_key, const ChunkExtent& extent) const;
    std::filesystem::path root_;
};

}  // namespace chronoforge
