#include "chronoforge/core/resource_planner.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace chronoforge {

std::vector<ChunkExtent> TilePlanner::temporal_tiles(const TensorShape& shape, const ResourceBudget& budget, Halo halo) {
    if (budget.max_chunk_bytes == 0) {
        throw std::invalid_argument("Chunk budget must be greater than zero");
    }
    const auto bytes_per_pixel_time_series = shape.t * shape.c * sizeof(float);
    if (bytes_per_pixel_time_series > budget.max_chunk_bytes) {
        throw std::runtime_error("One pixel's temporal series exceeds the chunk budget");
    }
    const auto pixel_capacity = std::max<std::size_t>(1, budget.max_chunk_bytes / bytes_per_pixel_time_series);
    const auto tile_width = std::min(shape.w, pixel_capacity);
    const auto tile_height = std::max<std::size_t>(1, pixel_capacity / tile_width);

    std::vector<ChunkExtent> tiles;
    for (std::size_t y = 0; y < shape.h; y += tile_height) {
        for (std::size_t x = 0; x < shape.w; x += tile_width) {
            tiles.push_back({
                0,
                shape.t,
                y > halo.y ? y - halo.y : 0,
                std::min(shape.h, y + tile_height + halo.y),
                x > halo.x ? x - halo.x : 0,
                std::min(shape.w, x + tile_width + halo.x),
            });
        }
    }
    return tiles;
}

std::string TilePlanner::explain_fft_limit(const TensorShape& shape, const ResourceBudget& budget) {
    const auto elements = shape.element_count();
    constexpr auto complex_buffers = 2ULL;
    constexpr auto bytes_per_complex = 8ULL;
    const auto estimated = elements > std::numeric_limits<std::size_t>::max() / (complex_buffers * bytes_per_complex)
                               ? std::numeric_limits<std::size_t>::max()
                               : elements * complex_buffers * bytes_per_complex;
    std::ostringstream message;
    message << "3D FFT estimated working set: " << estimated / (1024 * 1024) << " MiB; limit: "
            << budget.max_fft_working_set_bytes / (1024 * 1024) << " MiB. "
            << (estimated <= budget.max_fft_working_set_bytes ? "Allowed for CPU proxy." : "Blocked before allocation.");
    return message.str();
}

}  // namespace chronoforge
