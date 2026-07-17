import Foundation

struct VideoTensorData: Sendable {
    var values: [Float]
    var frames: Int
    var height: Int
    var width: Int
    var channels: Int
    var framesPerSecond: Double
    var duration: Double

    var valueCount: Int { frames * height * width * channels }
    var byteCount: Int { valueCount * MemoryLayout<Float>.stride }

    func frameValues(at index: Int) -> ArraySlice<Float> {
        let safeIndex = min(max(index, 0), max(frames - 1, 0))
        let frameSize = height * width * channels
        let start = safeIndex * frameSize
        return values[start..<(start + frameSize)]
    }
}

struct DiskTensorData: Codable, Sendable {
    var fileURL: URL
    var frames: Int
    var height: Int
    var width: Int
    var channels: Int
    var framesPerSecond: Double
    var duration: Double
    var timestamps: [Double]?

    var valueCount: Int { frames * height * width * channels }
    var byteCount: Int64 { Int64(valueCount) * Int64(MemoryLayout<Float>.stride) }

    func isValidOnDisk() -> Bool {
        guard let size = try? fileURL.resourceValues(forKeys: [.fileSizeKey]).fileSize else { return false }
        return Int64(size) == byteCount
    }
}

enum EffectKind: Int32, CaseIterable, Codable, Identifiable, Sendable {
    case spaceTimeTranspose = 0
    case lumaTimeShift = 1
    case radialChronoFunnel = 2
    case temporalPixelSort = 3
    case tensor3DRotation = 4
    case spectralFFTSwap = 5
    case selectivePrefilter = 6
    case dimensionalSplicer = 7
    case tensorDisplacement = 8
    case opticalFlowTimeWarp = 9
    case chronoFeedback = 10
    case structuralDatamosh = 11
    case seamlessLoop = 12
    case rgbTimeSlip = 13
    case horizontalSyncLoss = 14
    case chromaCarrierDrift = 15
    case strideError = 16
    case blockAddressCorruption = 17
    case bitplaneForge = 18
    case signalWeave = 19
    case blockGraft = 20
    case channelTransplant = 21

    var id: Int32 { rawValue }

    var definition: EffectDefinition { EffectRegistry.definition(for: self) }
    var title: String { definition.title }
    var symbol: String { definition.symbol }
    var tintName: String { definition.tint.rawValue }

    static let singleInputKinds = EffectRegistry.definitions
        .filter { $0.isAddable && $0.inputArity == .one }
        .map(\.kind)
    static let twoInputKinds = EffectRegistry.definitions
        .filter { $0.isAddable && $0.inputArity == .two }
        .map(\.kind)
    static let addableKinds = EffectRegistry.definitions.filter(\.isAddable).map(\.kind)

    var requiresDriver: Bool { definition.inputArity == .two }
}

struct EffectNode: Identifiable, Codable, Equatable, Sendable {
    var id = UUID()
    var kind: EffectKind
    var enabled = true
    var inputNodeID: UUID?
    var driverMediaID: UUID? = nil
    var amount: Float = 1
    var amountBlendMode: AmountBlendMode = .normal
    var randomSeed: UInt64
    var values: [Float]
    var options: [Int32]

    init(
        id: UUID = UUID(),
        kind: EffectKind,
        enabled: Bool = true,
        inputNodeID: UUID? = nil,
        driverMediaID: UUID? = nil,
        amount: Float = 1,
        amountBlendMode: AmountBlendMode = .normal,
        randomSeed: UInt64 = .random(in: UInt64.min...UInt64.max),
        values: [Float],
        options: [Int32]
    ) {
        self.id = id
        self.kind = kind
        self.enabled = enabled
        self.inputNodeID = inputNodeID
        self.driverMediaID = driverMediaID
        self.amount = amount
        self.amountBlendMode = amountBlendMode
        self.randomSeed = randomSeed
        self.values = values
        self.options = options
    }

    static func make(_ kind: EffectKind, inputNodeID: UUID? = nil) -> EffectNode {
        var node = kind.definition.defaultNode()
        node.inputNodeID = inputNodeID
        return node
    }

    static func makePrefilter(spatial: PrefilterStrength, temporal: PrefilterStrength) -> EffectNode {
        .init(
            kind: .selectivePrefilter,
            values: [],
            options: [spatial.rawValue, temporal.rawValue]
        )
    }

    var supportsAmount: Bool { kind.definition.shapeBehavior.supportsAmount(for: self) }

    private enum CodingKeys: String, CodingKey {
        case id, kind, enabled, inputNodeID, driverMediaID, amount, amountBlendMode, randomSeed, values, options
    }

    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        id = try values.decodeIfPresent(UUID.self, forKey: .id) ?? UUID()
        kind = try values.decode(EffectKind.self, forKey: .kind)
        enabled = try values.decodeIfPresent(Bool.self, forKey: .enabled) ?? true
        inputNodeID = try values.decodeIfPresent(UUID.self, forKey: .inputNodeID)
        driverMediaID = try values.decodeIfPresent(UUID.self, forKey: .driverMediaID)
        amount = try values.decodeIfPresent(Float.self, forKey: .amount) ?? 1
        amountBlendMode = try values.decodeIfPresent(AmountBlendMode.self, forKey: .amountBlendMode) ?? .normal
        randomSeed = try values.decodeIfPresent(UInt64.self, forKey: .randomSeed) ?? 0
        self.values = try values.decodeIfPresent([Float].self, forKey: .values) ?? []
        options = try values.decodeIfPresent([Int32].self, forKey: .options) ?? []
        let definition = kind.definition
        if self.values.count < definition.valueCount {
            self.values += Array(repeating: 0, count: definition.valueCount - self.values.count)
        }
        if options.count < definition.optionCount {
            options += Array(repeating: 0, count: definition.optionCount - options.count)
        }
    }

    var modeTitle: String {
        switch kind {
        case .spaceTimeTranspose: options[0] == 0 ? "Axis Swap · X–Time" : "Axis Swap · Y–Time"
        case .tensor3DRotation: "3D Rotation"
        case .lumaTimeShift: ["Luma", "Red", "Green", "Blue", "Alpha"][min(max(Int(options[0]), 0), 4)]
        case .radialChronoFunnel: ["Time Loom", "Kaleido Fold", "Event Horizon"][min(max(Int(options[1]), 0), 2)]
        case .temporalPixelSort: ["Luma", "Hue", "Saturation"][min(max(Int(options[0]), 0), 2)]
        case .spectralFFTSwap: options[3] == 0 ? "Frequency Swap" : "Frequency Rotation"
        case .selectivePrefilter: "Spatial + Temporal"
        case .dimensionalSplicer: "RGB map B → A coordinates"
        case .tensorDisplacement: "Target A + Map B"
        case .opticalFlowTimeWarp: "Motion-driven time"
        case .chronoFeedback: "Past + future echo"
        case .structuralDatamosh: ["Time", "Horizontal", "Vertical"][min(max(Int(options[0]), 0), 2)]
        case .seamlessLoop: ["Crossfade", "Luma Weave", "Ping-Pong"][min(max(Int(options[0]), 0), 2)]
        case .rgbTimeSlip: ["Horizontal", "Vertical", "Radial"][min(max(Int(options[0]), 0), 2)]
        case .horizontalSyncLoss: ["Horizontal", "Vertical"][min(max(Int(options[2]), 0), 1)]
        case .chromaCarrierDrift: ["Together", "Split Cb–Cr", "Alternating"][min(max(Int(options[0]), 0), 2)]
        case .strideError: ["RGB Together", "Separate Channels", "Alpha Included"][min(max(Int(options[0]), 0), 2)]
        case .blockAddressCorruption: ["Swap", "Repeat", "Offset", "Cascade"][min(max(Int(options[0]), 0), 3)]
        case .bitplaneForge: ["Shuffle", "Rotate", "Invert", "XOR"][min(max(Int(options[0]), 0), 3)]
        case .signalWeave: ["Lines", "Interlaced Fields", "Bands", "Checker"][min(max(Int(options[0]), 0), 3)]
        case .blockGraft: ["Random", "A Luma", "B Luma", "Difference", "A Edges"][min(max(Int(options[0]), 0), 4)]
        case .channelTransplant: options[3] == 0 ? "RGB Components" : "YCbCr Components"
        }
    }
}

enum AmountBlendMode: Int32, CaseIterable, Codable, Identifiable, Sendable {
    case normal
    case add
    case screen
    case multiply
    case difference
    case displace
    case xorGlitch

    var id: Int32 { rawValue }
    var title: String {
        switch self {
        case .normal: "Normal"
        case .add: "Add"
        case .screen: "Screen"
        case .multiply: "Multiply"
        case .difference: "Difference"
        case .displace: "Displace"
        case .xorGlitch: "XOR Glitch"
        }
    }
}

enum RenderQueueStatus: Equatable, Sendable {
    case waiting
    case running
    case completed
    case failed(String)
    case cancelled

    var title: String {
        switch self {
        case .waiting: "Waiting"
        case .running: "Rendering"
        case .completed: "Complete"
        case .failed: "Failed"
        case .cancelled: "Cancelled"
        }
    }

    var symbol: String {
        switch self {
        case .waiting: "clock"
        case .running: "gearshape.2"
        case .completed: "checkmark.circle.fill"
        case .failed: "exclamationmark.triangle.fill"
        case .cancelled: "xmark.circle"
        }
    }
}

struct RenderQueueItem: Identifiable, Sendable {
    let id: UUID
    let source: DecodedProxy
    let effects: [EffectNode]
    let mediaPool: [DecodedProxy]
    let audioMode: AudioMode
    let outputFramesPerSecond: Double?
    let destinationURL: URL
    var status: RenderQueueStatus

    init(
        source: DecodedProxy,
        effects: [EffectNode],
        mediaPool: [DecodedProxy],
        audioMode: AudioMode,
        outputFramesPerSecond: Double? = nil,
        destinationURL: URL
    ) {
        id = UUID()
        self.source = source
        self.effects = effects
        self.mediaPool = mediaPool
        self.audioMode = audioMode
        self.outputFramesPerSecond = outputFramesPerSecond
        self.destinationURL = destinationURL
        status = .waiting
    }
}

enum RenderOutputFormat: String, Sendable {
    case mp4
    case pngSequence

    var title: String {
        switch self {
        case .mp4: "MP4 Video"
        case .pngSequence: "PNG Sequence"
        }
    }
}

enum RenderQuality: String, CaseIterable, Identifiable, Sendable {
    case proxy
    case full

    var id: String { rawValue }
    var title: String { self == .proxy ? "Proxy" : "Full quality" }
}

enum ProxyQuality: String, CaseIterable, Identifiable, Codable, Sendable {
    case standard
    case high

    var id: String { rawValue }
    var title: String { self == .standard ? "Standard" : "High" }
    var detail: String { self == .standard ? "Up to 320 × 180 · 10 fps" : "Up to 480 × 270 · 15 fps" }
}

enum PrefilterStrength: Int32, CaseIterable, Identifiable, Codable, Sendable {
    case off
    case light
    case strong

    var id: Int32 { rawValue }
    var title: String {
        switch self {
        case .off: "Off"
        case .light: "Light"
        case .strong: "Strong"
        }
    }
}

enum AudioMode: String, CaseIterable, Identifiable, Codable, Sendable {
    case none
    case preserveOriginal

    var id: String { rawValue }
    var title: String { self == .none ? "No audio" : "Preserve original" }
}

enum PlaybackFPSPreset: String, CaseIterable, Identifiable, Codable, Sendable {
    case result
    case fps12
    case fps15
    case fps23976
    case fps24
    case fps25
    case fps2997
    case fps30
    case fps50
    case fps5994
    case fps60
    case custom

    var id: String { rawValue }
    var title: String {
        switch self {
        case .result: "Result"
        case .fps12: "12"
        case .fps15: "15"
        case .fps23976: "23.976"
        case .fps24: "24"
        case .fps25: "25"
        case .fps2997: "29.97"
        case .fps30: "30"
        case .fps50: "50"
        case .fps5994: "59.94"
        case .fps60: "60"
        case .custom: "Custom"
        }
    }
    var framesPerSecond: Double? {
        switch self {
        case .result, .custom: nil
        case .fps12: 12
        case .fps15: 15
        case .fps23976: 23.976
        case .fps24: 24
        case .fps25: 25
        case .fps2997: 29.97
        case .fps30: 30
        case .fps50: 50
        case .fps5994: 59.94
        case .fps60: 60
        }
    }
}

enum ViewerBackground: String, CaseIterable, Identifiable {
    case black
    case checkerboard

    var id: String { rawValue }
    var title: String { self == .black ? "Black" : "Checkerboard" }
}
