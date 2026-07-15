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

    var id: Int32 { rawValue }

    var title: String {
        switch self {
        case .spaceTimeTranspose: "Space–Time Transpose"
        case .lumaTimeShift: "Luma–Time Shift"
        case .radialChronoFunnel: "Radial Chrono-Funnel"
        case .temporalPixelSort: "Temporal Pixel Sort"
        case .tensor3DRotation: "Tensor 3D Rotation"
        case .spectralFFTSwap: "Spectral FFT Swap"
        }
    }

    var symbol: String {
        switch self {
        case .spaceTimeTranspose: "arrow.triangle.swap"
        case .lumaTimeShift: "sun.max.trianglebadge.exclamationmark"
        case .radialChronoFunnel: "hurricane"
        case .temporalPixelSort: "arrow.up.arrow.down.square"
        case .tensor3DRotation: "rotate.3d"
        case .spectralFFTSwap: "waveform.path.ecg.rectangle"
        }
    }

    var tintName: String {
        switch self {
        case .spaceTimeTranspose: "orange"
        case .lumaTimeShift: "yellow"
        case .radialChronoFunnel: "cyan"
        case .temporalPixelSort: "purple"
        case .tensor3DRotation: "pink"
        case .spectralFFTSwap: "indigo"
        }
    }
}

struct EffectNode: Identifiable, Codable, Equatable, Sendable {
    var id = UUID()
    var kind: EffectKind
    var enabled = true
    var inputNodeID: UUID?
    var values: [Float]
    var options: [Int32]

    static func make(_ kind: EffectKind, inputNodeID: UUID? = nil) -> EffectNode {
        switch kind {
        case .spaceTimeTranspose:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [0, 0, 0, 0], options: [0, 0, 0, 0])
        case .lumaTimeShift:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [20, 0, 0, 0], options: [0, 0, 0, 0])
        case .radialChronoFunnel:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [0.5, 0.5, 0.08, 0], options: [1, 0, 0, 0])
        case .temporalPixelSort:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [0, 0, 0, 0], options: [0, 0, 0, 0])
        case .tensor3DRotation:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [0, 15, 0, 0], options: [0, 0, 0, 0])
        case .spectralFFTSwap:
            return .init(kind: kind, inputNodeID: inputNodeID, values: [0, 0, 0, 0], options: [0, 1, 0, 0])
        }
    }
}

enum RenderQuality: String, CaseIterable, Identifiable {
    case proxy
    case full

    var id: String { rawValue }
    var title: String { self == .proxy ? "Proxy" : "Full quality" }
}

enum AudioMode: String, CaseIterable, Identifiable, Codable {
    case none
    case preserveOriginal

    var id: String { rawValue }
    var title: String { self == .none ? "No audio" : "Preserve original" }
}
