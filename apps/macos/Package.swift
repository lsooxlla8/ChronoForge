// swift-tools-version: 5.10
import PackageDescription

let package = Package(
    name: "ChronoForgeMac",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "ChronoForgeMac", targets: ["ChronoForgeMac"]),
    ],
    targets: [
        .executableTarget(name: "ChronoForgeMac", path: "Sources"),
    ]
)
