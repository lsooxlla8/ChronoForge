#include "chronoforge/core/effects.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {

chronoforge::VideoTensor fixture(std::size_t frames, bool driver) {
    constexpr std::size_t height = 180;
    constexpr std::size_t width = 320;
    chronoforge::VideoTensor tensor(
        {frames, height, width, 4}, 0.0F,
        {10, 1, chronoforge::ColorTransfer::Linear, chronoforge::AlphaRepresentation::Premultiplied});
    for (std::size_t t = 0; t < frames; ++t) {
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                const auto nx = static_cast<float>(x) / static_cast<float>(width - 1);
                const auto ny = static_cast<float>(y) / static_cast<float>(height - 1);
                const auto phase = static_cast<float>(t) / static_cast<float>(std::max<std::size_t>(1, frames - 1));
                const auto alpha = driver ? 1.0F : 0.35F + 0.65F * std::min(1.0F, std::min({nx, ny, 1.0F - nx, 1.0F - ny}) * 10.0F);
                tensor.at(t, y, x, 0) = (driver ? ny : nx) * alpha;
                tensor.at(t, y, x, 1) = (driver ? 1.0F - nx : ny) * alpha;
                tensor.at(t, y, x, 2) = (0.2F + 0.8F * phase) * alpha;
                tensor.at(t, y, x, 3) = alpha;
            }
        }
    }
    return tensor;
}

template <typename Function>
bool measure(std::string_view name, double budget_milliseconds, Function&& function) {
    const auto started = std::chrono::steady_clock::now();
    const auto output = function();
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    volatile float checksum = output.values().front() + output.values()[output.values().size() / 2] + output.values().back();
    (void)checksum;
    const auto within_budget = elapsed <= budget_milliseconds;
    std::cout << std::left << std::setw(28) << name << std::right << std::fixed << std::setprecision(1)
              << std::setw(9) << elapsed << " ms  budget " << std::setw(6) << budget_milliseconds
              << " ms  " << (within_budget ? "PASS" : "OVER") << '\n';
    return within_budget;
}

}  // namespace

int main() {
    const auto source = fixture(100, false);
    const auto driver = fixture(100, true);
    bool passed = true;
    passed &= measure("RGB Time Slip", 4000, [&] {
        return chronoforge::rgb_time_slip(source, {-5, 0, 6, 10, chronoforge::SplitAxis::Radial, chronoforge::EdgeBehavior::Wrap});
    });
    passed &= measure("Sync Loss", 1000, [&] {
        return chronoforge::horizontal_sync_loss(source, {0.25F, 0.045F, 0.4F, 0.4F, chronoforge::SyncLossDriver::DeterministicNoise, chronoforge::EdgeBehavior::Wrap, 11});
    });
    passed &= measure("Chroma Carrier Drift", 4000, [&] {
        return chronoforge::chroma_carrier_drift(source, {12, 4, 2, 8, chronoforge::ChromaDriftMode::SplitCbCr, chronoforge::EdgeBehavior::Wrap});
    });
    passed &= measure("Seamless Loop Spectral", 4000, [&] {
        return chronoforge::seamless_loop(
            source,
            {10, 0.12F, chronoforge::SeamlessLoopMode::SpectralMorph, 1.0F, 0.35F});
    });
    passed &= measure("Stride Error", 1000, [&] {
        return chronoforge::stride_error(source, {0.08F, 0.06F, 0.01F, chronoforge::StrideChannelMode::SeparateChannels, chronoforge::AddressEdge::Mirror});
    });
    passed &= measure("Block Address Corruption", 4000, [&] {
        return chronoforge::block_address_corruption(source, {0.05F, 0.4F, 6, 3, chronoforge::BlockCorruptionMapping::Cascade, chronoforge::EdgeBehavior::Mirror, 21});
    });
    passed &= measure("Bitplane Forge", 1000, [&] {
        return chronoforge::bitplane_forge(source, {10, 0x03FF, 2, chronoforge::BitplaneOperation::Xor, chronoforge::BitplaneChannel::RgbTogether, 31});
    });
    passed &= measure("Signal Weave", 4000, [&] {
        return chronoforge::signal_weave(source, driver, {chronoforge::SignalWeavePattern::Checker, 0.025F, 0.3F, 0.2F, 2, chronoforge::TensorBroadcast::Stretch, 41});
    });
    passed &= measure("Block Graft", 4000, [&] {
        return chronoforge::block_graft(source, driver, {0.05F, 0.4F, 3, 1, chronoforge::BlockGraftTrigger::Random, chronoforge::TensorBroadcast::Stretch, 51});
    });
    passed &= measure("Channel Transplant", 4000, [&] {
        return chronoforge::channel_transplant(source, driver, {{true, false, true}, 1, 4, -3, chronoforge::ChannelTransplantColourModel::YCbCr, chronoforge::TensorBroadcast::Stretch});
    });
    return passed ? 0 : 1;
}
