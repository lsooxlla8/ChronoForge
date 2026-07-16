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

    var id: Int32 { rawValue }

    var title: String {
        switch self {
        case .spaceTimeTranspose, .tensor3DRotation: "Space-Time Transform"
        case .lumaTimeShift: "Self Time Displacement"
        case .radialChronoFunnel: "Polar Time Warp"
        case .temporalPixelSort: "Pixel Sort (Time)"
        case .spectralFFTSwap: "3D FFT Transform"
        case .selectivePrefilter: "Output Prefilter"
        case .dimensionalSplicer: "Space-Time Map"
        case .tensorDisplacement: "Space-Time Displacement"
        case .opticalFlowTimeWarp: "Optical Flow Time Warp"
        case .chronoFeedback: "Time Feedback"
        case .structuralDatamosh: "Axis Datamosh"
        case .seamlessLoop: "Seamless Loop"
        }
    }

    var symbol: String {
        switch self {
        case .spaceTimeTranspose, .tensor3DRotation: "rotate.3d"
        case .lumaTimeShift: "sun.max.trianglebadge.exclamationmark"
        case .radialChronoFunnel: "hurricane"
        case .temporalPixelSort: "arrow.up.arrow.down.square"
        case .spectralFFTSwap: "waveform.path.ecg.rectangle"
        case .selectivePrefilter: "camera.filters"
        case .dimensionalSplicer: "arrow.triangle.branch"
        case .tensorDisplacement: "move.3d"
        case .opticalFlowTimeWarp: "wind"
        case .chronoFeedback: "arrow.triangle.2.circlepath.circle"
        case .structuralDatamosh: "waveform.path.badge.minus"
        case .seamlessLoop: "repeat.circle"
        }
    }

    var tintName: String {
        switch self {
        case .spaceTimeTranspose, .tensor3DRotation: "orange"
        case .lumaTimeShift: "yellow"
        case .radialChronoFunnel: "cyan"
        case .temporalPixelSort: "purple"
        case .spectralFFTSwap: "indigo"
        case .selectivePrefilter: "gray"
        case .dimensionalSplicer: "mint"
        case .tensorDisplacement: "blue"
        case .opticalFlowTimeWarp: "green"
        case .chronoFeedback: "pink"
        case .structuralDatamosh: "red"
        case .seamlessLoop: "teal"
        }
    }

    static let singleInputKinds: [EffectKind] = [
        .spaceTimeTranspose, .lumaTimeShift, .radialChronoFunnel, .temporalPixelSort, .spectralFFTSwap,
        .opticalFlowTimeWarp, .chronoFeedback, .structuralDatamosh, .seamlessLoop,
    ]
    static let twoInputKinds: [EffectKind] = [.dimensionalSplicer, .tensorDisplacement]
    static let addableKinds: [EffectKind] = singleInputKinds + twoInputKinds

    var requiresDriver: Bool { self == .dimensionalSplicer || self == .tensorDisplacement }
}

struct EffectNode: Identifiable, Codable, Equatable, Sendable {
    var id = UUID()
    var kind: EffectKind
    var enabled = true
    var inputNodeID: UUID?
    var driverMediaID: UUID? = nil
    var values: [Float]
    var options: [Int32]

    static func make(_ kind: EffectKind, inputNodeID: UUID? = nil) -> EffectNode {
        switch kind {
        case .spaceTimeTranspose:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [0, 0, 0, 0], options: [0, 1, 0, 0])
        case .lumaTimeShift:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [20, 0, 0, 0], options: [0, 0, 0, 0])
        case .radialChronoFunnel:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [0.5, 0.5, 0.08, 0.75], options: [1, 0, 0, 0])
        case .temporalPixelSort:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [0, 0, 0, 0], options: [0, 0, 0, 0])
        case .tensor3DRotation:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [0, 15, 0, 0], options: [3, 0, 0, 0])
        case .spectralFFTSwap:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [0, 0, 0, 0], options: [0, 1, 1, 0])
        case .selectivePrefilter:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [0, 0, 0, 0], options: [0, 0, 0, 0])
        case .dimensionalSplicer:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [0, 0, 0, 0], options: [0, 1, 2, 1])
        case .tensorDisplacement:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [12, 24, 24, 0], options: [0, 1, 0, 0])
        case .opticalFlowTimeWarp:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [0.02, 4, 0, 180], options: [0, 0, 0, 0])
        case .chronoFeedback:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [2, 0.35, 2, 0.15], options: [1, 0, 0, 0])
        case .structuralDatamosh:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [0.2, 8, 0.05, 0], options: [0, 0, 0, 0])
        case .seamlessLoop:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [15, 0.12, 0, 0], options: [0, 0, 0, 0])
        }
    }

    static func makePrefilter(spatial: PrefilterStrength, temporal: PrefilterStrength) -> EffectNode {
        .init(
            kind: .selectivePrefilter,
            values: [0, 0, 0, 0],
            options: [spatial.rawValue, temporal.rawValue, 0, 0]
        )
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
    let destinationURL: URL
    var status: RenderQueueStatus

    init(
        source: DecodedProxy,
        effects: [EffectNode],
        mediaPool: [DecodedProxy],
        audioMode: AudioMode,
        destinationURL: URL
    ) {
        id = UUID()
        self.source = source
        self.effects = effects
        self.mediaPool = mediaPool
        self.audioMode = audioMode
        self.destinationURL = destinationURL
        status = .waiting
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
