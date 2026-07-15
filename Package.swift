// swift-tools-version: 5.10
import PackageDescription

let package = Package(
    name: "ChronoForge",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "ChronoForgeMac", targets: ["ChronoForgeMac"]),
    ],
    targets: [
        .target(
            name: "ChronoForgeCoreBridge",
            path: ".",
            exclude: [
                ".git", "apps", "build", "docs", "tests",
                ".gitignore", "CMakeLists.txt", "README.md", "Package.swift",
            ],
            sources: [
                "src/cache_store.cpp",
                "src/effects.cpp",
                "src/node_graph.cpp",
                "src/resource_planner.cpp",
                "src/spectral.cpp",
                "bridge/ChronoForgeBridge.cpp",
            ],
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("include"),
            ]
        ),
        .executableTarget(
            name: "ChronoForgeMac",
            dependencies: ["ChronoForgeCoreBridge"],
            path: "apps/macos/Sources",
            swiftSettings: [
                .interoperabilityMode(.Cxx),
            ],
            linkerSettings: [
                .linkedFramework("AVFoundation"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("VideoToolbox"),
            ]
        ),
        .executableTarget(
            name: "ChronoForgeIntegration",
            dependencies: ["ChronoForgeCoreBridge"],
            path: "tests/integration",
            swiftSettings: [.interoperabilityMode(.Cxx)],
            linkerSettings: [
                .linkedFramework("AVFoundation"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
            ]
        ),
    ],
    cxxLanguageStandard: .cxx20
)
