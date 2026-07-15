#include "chronoforge/core/resource_planner.hpp"

#include <iostream>

int main() {
    const chronoforge::TensorShape proxy{10, 180, 320, 4};
    const chronoforge::ResourceBudget budget{};

    std::cout << "ChronoForge core " << "0.1.0" << '\n';
    std::cout << "Canonical tensor order: T, H, W, C\n";
    std::cout << "Default proxy: " << proxy.t << " x " << proxy.h << " x " << proxy.w << " x " << proxy.c << '\n';
    std::cout << chronoforge::TilePlanner::explain_fft_limit(proxy, budget) << '\n';
}
