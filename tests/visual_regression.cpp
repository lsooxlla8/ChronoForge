#include "chronoforge/core/effects.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kWidth = 96;
constexpr std::size_t kHeight = 64;
constexpr std::size_t kFrames = 12;

chronoforge::VideoTensor fixture(std::size_t frames, std::size_t height, std::size_t width, bool driver) {
    chronoforge::VideoTensor tensor(
        {frames, height, width, 4}, 0.0F,
        {24, 1, chronoforge::ColorTransfer::Linear, chronoforge::AlphaRepresentation::Premultiplied});
    for (std::size_t t = 0; t < frames; ++t) {
        const auto phase = static_cast<float>(t) / static_cast<float>(std::max<std::size_t>(1, frames - 1));
        const auto moving_x = driver ? (1.0F - phase) : phase;
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                const auto nx = static_cast<float>(x) / static_cast<float>(width - 1);
                const auto ny = static_cast<float>(y) / static_cast<float>(height - 1);
                const auto dx = nx - moving_x;
                const auto dy = ny - (driver ? 0.68F : 0.36F);
                const auto circle = dx * dx + dy * dy < (driver ? 0.018F : 0.026F);
                const auto head_dx = nx - 0.52F;
                const auto head_dy = ny - 0.46F;
                const auto silhouette = head_dx * head_dx / 0.035F + head_dy * head_dy / 0.13F < 1.0F;
                const auto sharp_lines = (x % 17 == 0) || (y % 13 == 0);
                auto noise = static_cast<std::uint64_t>((t + 1) * 0x9E3779B1U) ^
                             static_cast<std::uint64_t>((y + 1) * 0x85EBCA77U) ^
                             static_cast<std::uint64_t>((x + 1) * 0xC2B2AE3DU);
                noise ^= noise >> 15U;
                const auto grain = static_cast<float>(noise & 255U) / 255.0F;
                float r = driver ? 0.12F + 0.72F * ny : 0.08F + 0.82F * nx;
                float g = driver ? 0.10F + 0.78F * (1.0F - nx) : 0.12F + 0.72F * ny;
                float b = driver ? 0.18F + 0.74F * phase : 0.16F + 0.68F * (1.0F - nx);
                if (silhouette) { r *= 0.24F; g *= 0.29F; b *= 0.36F; }
                if (circle) { r = driver ? 0.95F : 0.16F; g = driver ? 0.20F : 0.92F; b = 0.22F; }
                if (sharp_lines) { r = grain; g = 1.0F - grain; b = grain > 0.5F ? 1.0F : 0.0F; }
                const auto edge_fade = std::clamp(std::min({nx, ny, 1.0F - nx, 1.0F - ny}) * 9.0F, 0.0F, 1.0F);
                const auto alpha = driver ? 1.0F : (0.18F + 0.82F * edge_fade);
                tensor.at(t, y, x, 0) = std::clamp(r, 0.0F, 1.0F) * alpha;
                tensor.at(t, y, x, 1) = std::clamp(g, 0.0F, 1.0F) * alpha;
                tensor.at(t, y, x, 2) = std::clamp(b, 0.0F, 1.0F) * alpha;
                tensor.at(t, y, x, 3) = alpha;
            }
        }
    }
    return tensor;
}

void composite_cell(
    std::vector<std::uint8_t>& sheet,
    std::size_t sheet_width,
    std::size_t cell_x,
    std::size_t cell_y,
    const chronoforge::VideoTensor& tensor) {
    const auto frame = std::min(tensor.shape().t - 1, tensor.shape().t / 2);
    for (std::size_t y = 0; y < kHeight; ++y) {
        const auto sy = tensor.shape().h <= 1 ? 0 : y * (tensor.shape().h - 1) / (kHeight - 1);
        for (std::size_t x = 0; x < kWidth; ++x) {
            const auto sx = tensor.shape().w <= 1 ? 0 : x * (tensor.shape().w - 1) / (kWidth - 1);
            const auto alpha = tensor.shape().c >= 4 ? std::clamp(tensor.at(frame, sy, sx, 3), 0.0F, 1.0F) : 1.0F;
            const auto checker = ((x / 8 + y / 8) & 1U) == 0 ? 0.22F : 0.42F;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto source_channel = std::min(channel, tensor.shape().c - 1);
                const auto premultiplied = std::clamp(tensor.at(frame, sy, sx, source_channel), 0.0F, alpha);
                const auto value = premultiplied + checker * (1.0F - alpha);
                const auto destination = (((cell_y * kHeight + y) * sheet_width) + cell_x * kWidth + x) * 3 + channel;
                sheet[destination] = static_cast<std::uint8_t>(std::round(std::clamp(value, 0.0F, 1.0F) * 255.0F));
            }
        }
    }
}

void write_ppm(const std::filesystem::path& path, const std::vector<std::uint8_t>& pixels, std::size_t width, std::size_t height) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("Cannot create visual regression contact sheet");
    stream << "P6\n" << width << ' ' << height << "\n255\n";
    stream.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    if (!stream) throw std::runtime_error("Cannot finish visual regression contact sheet");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto output_directory = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("visual-regression");
        std::filesystem::create_directories(output_directory);
        const auto source = fixture(kFrames, kHeight, kWidth, false);
        const auto driver = fixture(9, 72, 80, true);
        using Pair = std::pair<chronoforge::VideoTensor, chronoforge::VideoTensor>;
        const std::vector<Pair> rows{
            {chronoforge::rgb_time_slip(source, {-3, 0, 4, 9, chronoforge::SplitAxis::Horizontal, chronoforge::EdgeBehavior::Wrap}),
             chronoforge::rgb_time_slip(source, {-6, 2, 7, -13, chronoforge::SplitAxis::Radial, chronoforge::EdgeBehavior::Mirror})},
            {chronoforge::horizontal_sync_loss(source, {0.22F, 6, 0.35F, 0.42F, chronoforge::SyncLossDriver::DeterministicNoise, chronoforge::EdgeBehavior::Wrap, 11}),
             chronoforge::horizontal_sync_loss(source, {0.36F, 3, -0.8F, 0.62F, chronoforge::SyncLossDriver::DeterministicNoise, chronoforge::EdgeBehavior::Mirror, 12})},
            {chronoforge::chroma_carrier_drift(source, {8, 2, 2, 7, chronoforge::ChromaDriftMode::SplitCbCr, chronoforge::EdgeBehavior::Wrap}),
             chronoforge::chroma_carrier_drift(source, {-14, 5, -3, 14, chronoforge::ChromaDriftMode::Alternating, chronoforge::EdgeBehavior::Mirror})},
            {chronoforge::stride_error(source, {0.06F, 0.04F, 0.01F, chronoforge::StrideChannelMode::RgbTogether, chronoforge::AddressEdge::Wrap}),
             chronoforge::stride_error(source, {-0.11F, 0.13F, -0.025F, chronoforge::StrideChannelMode::AlphaIncluded, chronoforge::AddressEdge::Mirror})},
            {chronoforge::block_address_corruption(source, {12, 0.38F, 3, 2, chronoforge::BlockCorruptionMapping::Swap, chronoforge::EdgeBehavior::Wrap, 21}),
             chronoforge::block_address_corruption(source, {7, 0.72F, 5, 3, chronoforge::BlockCorruptionMapping::Cascade, chronoforge::EdgeBehavior::Mirror, 22})},
            {chronoforge::bitplane_forge(source, {8, 0x00F0, 2, chronoforge::BitplaneOperation::Rotate, chronoforge::BitplaneChannel::RgbTogether, 31}),
             chronoforge::bitplane_forge(source, {10, 0x03FF, -3, chronoforge::BitplaneOperation::Xor, chronoforge::BitplaneChannel::Luma, 32})},
            {chronoforge::signal_weave(source, driver, {chronoforge::SignalWeavePattern::Bands, 8, 0.3F, 0.12F, 1, chronoforge::TensorBroadcast::Stretch, 41}),
             chronoforge::signal_weave(source, driver, {chronoforge::SignalWeavePattern::Checker, 6, -0.55F, 0.48F, -2, chronoforge::TensorBroadcast::Clamp, 42})},
            {chronoforge::block_graft(source, driver, {12, 0.38F, 3, 1, chronoforge::BlockGraftTrigger::Random, chronoforge::TensorBroadcast::Stretch, 51}),
             chronoforge::block_graft(source, driver, {8, 0.22F, 2, -1, chronoforge::BlockGraftTrigger::Difference, chronoforge::TensorBroadcast::Clamp, 52})},
            {chronoforge::channel_transplant(source, driver, {{true, false, true}, 1, 3, -2, chronoforge::ChannelTransplantColourModel::Rgb, chronoforge::TensorBroadcast::Stretch}),
             chronoforge::channel_transplant(source, driver, {{false, true, true}, -2, -4, 3, chronoforge::ChannelTransplantColourModel::YCbCr, chronoforge::TensorBroadcast::Clamp})},
        };
        const auto sheet_width = kWidth * 3;
        const auto sheet_height = kHeight * rows.size();
        std::vector<std::uint8_t> sheet(sheet_width * sheet_height * 3, 0);
        for (std::size_t row = 0; row < rows.size(); ++row) {
            composite_cell(sheet, sheet_width, 0, row, source);
            composite_cell(sheet, sheet_width, 1, row, rows[row].first);
            composite_cell(sheet, sheet_width, 2, row, rows[row].second);
        }
        const auto sheet_path = output_directory / "wave-a-contact-sheet.ppm";
        write_ppm(sheet_path, sheet, sheet_width, sheet_height);
        std::ofstream manifest(output_directory / "README.txt", std::ios::trunc);
        manifest << "Columns: source A | standard parameters | alternate/seeded parameters\n"
                    "Rows: RGB Time Slip | Horizontal Sync Loss | Chroma Carrier Drift | Stride Error | "
                    "Block Address Corruption | Bitplane Forge | Signal Weave | Block Graft | Channel Transplant\n"
                    "Fixture: procedural RGBA gradient, moving geometry, portrait-like silhouette, noise, one-pixel lines; "
                    "two-source rows use a different B size, duration, motion and colour.\n";
        if (!manifest) throw std::runtime_error("Cannot create visual regression manifest");
        std::cout << sheet_path << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Visual regression generation failed: " << error.what() << '\n';
        return 1;
    }
}
