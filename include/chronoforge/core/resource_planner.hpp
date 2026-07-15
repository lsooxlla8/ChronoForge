#pragma once

#include "chronoforge/core/video_tensor.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace chronoforge {

struct ChunkExtent {
    std::size_t t_begin{};
    std::size_t t_end{};
    std::size_t y_begin{};
    std::size_t y_end{};
    std::size_t x_begin{};
    std::size_t x_end{};
};

struct Halo {
    std::size_t t{};
    std::size_t y{};
    std::size_t x{};
};

struct ResourceBudget {
    std::size_t max_chunk_bytes{256ULL * 1024ULL * 1024ULL};
    std::size_t max_fft_working_set_bytes{512ULL * 1024ULL * 1024ULL};
};

class TilePlanner {
public:
    // Temporal effects operate on complete per-pixel time series, so their tiles
    // span T and are split only across the image plane.
    static std::vector<ChunkExtent> temporal_tiles(
        const TensorShape& shape,
        const ResourceBudget& budget,
        Halo halo = {});

    [[nodiscard]] static std::string explain_fft_limit(
        const TensorShape& shape,
        const ResourceBudget& budget);
};

}  // namespace chronoforge
