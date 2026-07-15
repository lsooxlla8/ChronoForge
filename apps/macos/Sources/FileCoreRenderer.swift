import ChronoForgeCoreBridge
import Foundation

private final class FileProgressContext: @unchecked Sendable {
    private let lock = NSLock()
    private var cancelled = false
    let handler: @Sendable (Double, String) -> Void

    init(handler: @escaping @Sendable (Double, String) -> Void) { self.handler = handler }
    func cancel() { lock.withLock { cancelled = true } }
    func isCancelled() -> Bool { lock.withLock { cancelled } }
}

private let chronoForgeFileProgress: @convention(c) (Double, UnsafePointer<CChar>?, UnsafeMutableRawPointer?) -> Int32 = {
    fraction, stage, opaque in
    guard let opaque else { return 1 }
    let context = Unmanaged<FileProgressContext>.fromOpaque(opaque).takeUnretainedValue()
    if context.isCancelled() { return 0 }
    context.handler(fraction, stage.map(String.init(cString:)) ?? "Rendering")
    return 1
}

enum FileCoreRenderer {
    static func render(
        input: DiskTensorData,
        effects: [EffectNode],
        outputURL: URL,
        scratchDirectory: URL,
        budget: UInt64 = CoreRenderer.defaultBudget,
        progress: @escaping @Sendable (Double, String) -> Void
    ) async throws -> DiskTensorData {
        let context = FileProgressContext(handler: progress)
        return try await withTaskCancellationHandler {
            try await Task.detached(priority: .userInitiated) {
                let descriptors = effects.filter(\.enabled).map { effect in
                    cf_effect_descriptor_make(
                        effect.kind.rawValue,
                        effect.values[0], effect.values[1], effect.values[2], effect.values[3],
                        effect.options[0], effect.options[1], effect.options[2], effect.options[3]
                    )
                }
                let fpsNumerator = UInt32(max(1, Int((input.framesPerSecond * 1000).rounded())))
                let inputInfo = CFFileTensorInfo(
                    frames: UInt64(input.frames),
                    height: UInt64(input.height),
                    width: UInt64(input.width),
                    channels: UInt64(input.channels),
                    frame_rate_numerator: fpsNumerator,
                    frame_rate_denominator: 1000
                )
                var outputInfo = CFFileTensorInfo()
                var error = [CChar](repeating: 0, count: 1024)
                let errorCapacity = UInt64(error.count)
                let opaque = Unmanaged.passRetained(context).toOpaque()
                defer { Unmanaged<FileProgressContext>.fromOpaque(opaque).release() }
                let status = input.fileURL.path.withCString { inputPath in
                    outputURL.path.withCString { outputPath in
                        scratchDirectory.path.withCString { scratchPath in
                            descriptors.withUnsafeBufferPointer { descriptorBuffer in
                                error.withUnsafeMutableBufferPointer { errorBuffer in
                                    cf_render_file_effect_chain(
                                        inputPath, outputPath, scratchPath, inputInfo,
                                        descriptorBuffer.baseAddress, UInt64(descriptors.count), budget,
                                        chronoForgeFileProgress, opaque, &outputInfo,
                                        errorBuffer.baseAddress, errorCapacity
                                    )
                                }
                            }
                        }
                    }
                }
                if status == 3 { throw CancellationError() }
                guard status == 0 else { throw CoreRendererError.renderFailed(String(cString: error)) }
                let changesTimelineExtent = effects.contains {
                    $0.enabled && ($0.kind == .spaceTimeTranspose || ($0.kind == .spectralFFTSwap && $0.options[2] == 0))
                }
                let outputFPS = Double(outputInfo.frame_rate_numerator) / Double(outputInfo.frame_rate_denominator)
                let outputTimestamps = changesTimelineExtent ? nil : input.timestamps
                return DiskTensorData(
                    fileURL: outputURL,
                    frames: Int(outputInfo.frames),
                    height: Int(outputInfo.height),
                    width: Int(outputInfo.width),
                    channels: Int(outputInfo.channels),
                    framesPerSecond: outputFPS,
                    duration: outputTimestamps == nil ? Double(outputInfo.frames) / outputFPS : input.duration,
                    timestamps: outputTimestamps
                )
            }.value
        } onCancel: {
            context.cancel()
        }
    }
}
