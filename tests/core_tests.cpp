#include "chronoforge/core/cache_store.hpp"
#include "chronoforge/core/effects.hpp"
#include "chronoforge/core/node_graph.hpp"
#include "chronoforge/core/resource_planner.hpp"
#include "chronoforge/core/spectral.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_near(float actual, float expected, const std::string& message) {
    require(std::abs(actual - expected) < 0.0001F, message + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

chronoforge::VideoTensor numbered(chronoforge::TensorShape shape) {
    chronoforge::VideoTensor result(shape);
    for (std::size_t t = 0; t < shape.t; ++t) {
        for (std::size_t y = 0; y < shape.h; ++y) {
            for (std::size_t x = 0; x < shape.w; ++x) {
                for (std::size_t c = 0; c < shape.c; ++c) {
                    result.at(t, y, x, c) = static_cast<float>(100 * t + 10 * y + x + c);
                }
            }
        }
    }
    return result;
}

void test_transpose() {
    const chronoforge::VideoTensorMetadata metadata{24, 1, chronoforge::ColorTransfer::Rec709, chronoforge::AlphaRepresentation::Straight};
    auto input = numbered({2, 3, 4, 1});
    input = chronoforge::VideoTensor(input.shape(), input.values(), metadata);
    const auto output = chronoforge::space_time_transpose(input, chronoforge::SpatialAxis::X);
    require(output.shape() == chronoforge::TensorShape{4, 3, 2, 1}, "X/T transpose shape");
    require_near(output.at(3, 2, 1, 0), input.at(1, 2, 3, 0), "X/T transpose value");
    require(output.metadata() == metadata, "Effects preserve tensor colour and playback metadata");
}

void test_time_shift_and_funnel() {
    chronoforge::VideoTensor input({4, 1, 2, 1});
    for (std::size_t t = 0; t < 4; ++t) {
        for (std::size_t x = 0; x < 2; ++x) {
            input.at(t, 0, x, 0) = static_cast<float>(t) / 3.0F;
        }
    }
    const auto shifted = chronoforge::luma_time_shift(input, {3.0F, chronoforge::ShiftSource::Luma, chronoforge::EdgeBehavior::Clamp});
    require_near(shifted.at(3, 0, 0, 0), input.at(0, 0, 0, 0), "Luma shift samples an earlier frame");

    const auto funnel = chronoforge::radial_chrono_funnel(input, {0.0F, 0.0F, 1.0F, chronoforge::EdgeBehavior::Wrap});
    require_near(funnel.at(3, 0, 1, 0), input.at(0, 0, 1, 0), "Radial funnel wraps time at an outer pixel");
}

void test_sort_and_rotation() {
    chronoforge::VideoTensor input({3, 1, 1, 1});
    input.at(0, 0, 0, 0) = 0.8F;
    input.at(1, 0, 0, 0) = 0.2F;
    input.at(2, 0, 0, 0) = 0.5F;
    const auto sorted = chronoforge::temporal_pixel_sort(input, {chronoforge::SortCriterion::Luma, chronoforge::SortDirection::Ascending, 0.4F});
    require_near(sorted.at(0, 0, 0, 0), 0.5F, "Sort moves eligible darkest sample first");
    require_near(sorted.at(1, 0, 0, 0), 0.2F, "Sort preserves threshold-excluded frame");
    require_near(sorted.at(2, 0, 0, 0), 0.8F, "Sort moves eligible brightest sample last");

    const auto rotation = chronoforge::tensor_3d_rotation(input, {});
    require(rotation.values() == input.values(), "Zero 3D rotation is identity");
}

void test_fft_swap() {
    const auto input = numbered({3, 5, 6, 1});
    const auto output = chronoforge::spectral_fft_swap(input, {chronoforge::SpectralSwapAxis::XTime, false, 1024 * 1024});
    const auto expected = chronoforge::space_time_transpose(input, chronoforge::SpatialAxis::X);
    require(output.shape() == expected.shape(), "FFT swap has transposed shape");
    for (std::size_t i = 0; i < output.values().size(); ++i) {
        require_near(output.values()[i], expected.values()[i], "FFT swap matches axis permutation for distinct extents");
    }
}

void test_cache_and_graph() {
    const auto root = std::filesystem::temp_directory_path() / "chronoforge-core-test-cache";
    std::filesystem::remove_all(root);
    chronoforge::DiskCacheStore cache(root);
    const chronoforge::ChunkExtent extent{0, 2, 3, 4, 5, 7};
    const std::vector<float> values{1.0F, 2.0F, 3.0F};
    cache.write("0123abcd", extent, values);
    const auto hit = cache.read("0123abcd", extent);
    require(hit.has_value() && hit->values == values, "Disk cache returns its atomic write");
    std::filesystem::remove_all(root);

    chronoforge::NodeGraph graph;
    graph.add_node({"input", "Input", chronoforge::NodeKind::Input});
    graph.add_node({"shift", "Shift", chronoforge::NodeKind::LumaTimeShift});
    graph.add_node({"output", "Output", chronoforge::NodeKind::Output});
    graph.connect("input", "shift");
    graph.connect("shift", "output");
    require(graph.topological_order().size() == 3, "Graph topologically orders a chain");
    bool cycle_rejected = false;
    try {
        graph.connect("output", "input");
    } catch (const std::invalid_argument&) {
        cycle_rejected = true;
    }
    require(cycle_rejected, "Graph rejects a dependency cycle");
}

void test_tiling() {
    const auto tiles = chronoforge::TilePlanner::temporal_tiles({4, 10, 10, 1}, {4 * 4 * 10, 1024}, {});
    require(!tiles.empty(), "Temporal tile planner creates tiles");
    require(tiles.front().t_begin == 0 && tiles.front().t_end == 4, "Temporal tiles keep full time vectors");
}

}  // namespace

int main() {
    try {
        test_transpose();
        test_time_shift_and_funnel();
        test_sort_and_rotation();
        test_fft_swap();
        test_cache_and_graph();
        test_tiling();
        std::cout << "All ChronoForge core tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
