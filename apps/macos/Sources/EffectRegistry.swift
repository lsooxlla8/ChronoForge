import Foundation

enum EffectCategory: String, CaseIterable, Identifiable, Sendable {
    case timeAndMotion
    case spaceAndGeometry
    case signalAndAnalog
    case memoryAndCompression
    case dataAndChannels
    case multiSource
    case outputAndUtility

    var id: String { rawValue }
    var title: String {
        switch self {
        case .timeAndMotion: "Time & Motion"
        case .spaceAndGeometry: "Space & Geometry"
        case .signalAndAnalog: "Signal & Analog"
        case .memoryAndCompression: "Memory & Compression"
        case .dataAndChannels: "Data & Channels"
        case .multiSource: "Multi-Source · A + B"
        case .outputAndUtility: "Output & Utility"
        }
    }
}

enum EffectTint: String, Sendable {
    case orange, yellow, cyan, purple, indigo, gray, mint, blue, green, pink, red, teal
}

enum EffectInputArity: Sendable { case one, two }
enum EffectCostClass: Sendable { case local, temporal, global }

enum EffectShapeBehavior: Sendable {
    case preserving
    case preservingForOptions(index: Int, values: Set<Int32>)
    case changing

    func supportsAmount(for node: EffectNode) -> Bool {
        switch self {
        case .preserving: true
        case .preservingForOptions(let index, let values):
            node.options.indices.contains(index) && values.contains(node.options[index])
        case .changing: false
        }
    }
}

struct RandomizationProfile: Sendable {
    let identifier: String
}

struct EffectDefinition: Sendable {
    let kind: EffectKind
    let title: String
    let symbol: String
    let tint: EffectTint
    let category: EffectCategory
    let inputArity: EffectInputArity
    let costClass: EffectCostClass
    let shapeBehavior: EffectShapeBehavior
    let usesRandomSeed: Bool
    let valueCount: Int
    let optionCount: Int
    let isAddable: Bool
    let defaultNode: @Sendable () -> EffectNode
    let randomization: RandomizationProfile?
}

enum EffectRegistry {
    static let definitions: [EffectDefinition] = [
        definition(.spaceTimeTranspose, "Space-Time Transform", "rotate.3d", .orange, .spaceAndGeometry,
                   shape: .preservingForOptions(index: 1, values: [1]), values: [], options: [0, 1]),
        definition(.lumaTimeShift, "Self Time Displacement", "sun.max.trianglebadge.exclamationmark", .yellow,
                   .timeAndMotion, cost: .temporal, values: [20], options: [0, 0]),
        definition(.radialChronoFunnel, "Polar Time Warp", "hurricane", .cyan, .spaceAndGeometry,
                   cost: .temporal, values: [0.5, 0.5, 0.08, 0.75], options: [1, 0]),
        definition(.temporalPixelSort, "Pixel Sort (Time)", "arrow.up.arrow.down.square", .purple,
                   .timeAndMotion, cost: .temporal, values: [0], options: [0, 0]),
        definition(.tensor3DRotation, "Space-Time Transform", "rotate.3d", .orange, .spaceAndGeometry,
                   values: [0, 15, 0], options: [3], isAddable: false),
        definition(.spectralFFTSwap, "3D FFT Transform", "waveform.path.ecg.rectangle", .indigo,
                   .dataAndChannels, cost: .global, shape: .preservingForOptions(index: 2, values: [1]),
                   values: [0], options: [0, 1, 1, 0]),
        definition(.selectivePrefilter, "Output Prefilter", "camera.filters", .gray, .outputAndUtility,
                   values: [], options: [0, 0], isAddable: false),
        definition(.dimensionalSplicer, "Space-Time Map", "arrow.triangle.branch", .mint, .multiSource,
                   inputArity: .two, cost: .global, shape: .changing, values: [], options: [0, 1, 2, 1]),
        definition(.tensorDisplacement, "Space-Time Displacement", "move.3d", .blue, .multiSource,
                   inputArity: .two, cost: .temporal,
                   shape: .preservingForOptions(index: 1, values: [0, 1]),
                   values: [12, 24, 24], options: [0, 1, 0]),
        definition(.opticalFlowTimeWarp, "Optical Flow Time Warp", "wind", .green, .timeAndMotion,
                   cost: .global, values: [0.02, 4, 0, 180], options: [0]),
        definition(.chronoFeedback, "Time Feedback", "arrow.triangle.2.circlepath.circle", .pink,
                   .memoryAndCompression, cost: .temporal, values: [2, 0.35, 2, 0.15], options: [1]),
        definition(.structuralDatamosh, "Axis Datamosh", "waveform.path.badge.minus", .red,
                   .memoryAndCompression, cost: .temporal, values: [0.2, 8, 0.05], options: [0, 0]),
        definition(.seamlessLoop, "Seamless Loop", "repeat.circle", .teal, .outputAndUtility,
                   cost: .temporal, shape: .changing, values: [15, 0.12], options: [0]),
    ]

    private static let byKind = Dictionary(uniqueKeysWithValues: definitions.map { ($0.kind, $0) })

    static func definition(for kind: EffectKind) -> EffectDefinition {
        guard let definition = byKind[kind] else { preconditionFailure("Missing EffectDefinition for \(kind)") }
        return definition
    }

    static func definitions(in category: EffectCategory) -> [EffectDefinition] {
        definitions.filter { $0.category == category && $0.isAddable }
    }

    private static func definition(
        _ kind: EffectKind,
        _ title: String,
        _ symbol: String,
        _ tint: EffectTint,
        _ category: EffectCategory,
        inputArity: EffectInputArity = .one,
        cost: EffectCostClass = .local,
        shape: EffectShapeBehavior = .preserving,
        usesRandomSeed: Bool = false,
        values: [Float],
        options: [Int32],
        isAddable: Bool = true,
        randomization: RandomizationProfile? = nil
    ) -> EffectDefinition {
        EffectDefinition(
            kind: kind, title: title, symbol: symbol, tint: tint, category: category,
            inputArity: inputArity, costClass: cost, shapeBehavior: shape,
            usesRandomSeed: usesRandomSeed, valueCount: values.count, optionCount: options.count,
            isAddable: isAddable,
            defaultNode: { EffectNode(kind: kind, values: values, options: options) },
            randomization: randomization
        )
    }
}
