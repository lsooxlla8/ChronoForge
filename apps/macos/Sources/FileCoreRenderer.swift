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
        drivers: [UUID: DiskTensorData] = [:],
        outputURL: URL,
        scratchDirectory: URL,
        budget: UInt64 = CoreRenderer.defaultBudget,
        progress: @escaping @Sendable (Double, String) -> Void
    ) async throws -> DiskTensorData {
        if effects.contains(where: { $0.enabled && $0.kind.requiresDriver }) {
            return try await renderMixed(
                input: input, effects: effects, drivers: drivers, outputURL: outputURL,
                scratchDirectory: scratchDirectory, budget: budget, progress: progress)
        }
        let context = FileProgressContext(handler: progress)
        return try await withTaskCancellationHandler {
            try await Task.detached(priority: .userInitiated) {
                let descriptors = effects.filter(\.enabled).map { $0.coreDescriptor() }
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

    private static func renderMixed(
        input: DiskTensorData,
        effects: [EffectNode],
        drivers: [UUID: DiskTensorData],
        outputURL: URL,
        scratchDirectory: URL,
        budget: UInt64,
        progress: @escaping @Sendable (Double, String) -> Void
    ) async throws -> DiskTensorData {
        let active = effects.filter(\.enabled)
        try FileManager.default.createDirectory(at: scratchDirectory, withIntermediateDirectories: true)
        var current = input
        for (index, effect) in active.enumerated() {
            try Task.checkCancellation()
            let destination = index == active.count - 1
                ? outputURL
                : scratchDirectory.appendingPathComponent("mixed-\(index).raw")
            let base = Double(index) / Double(max(1, active.count))
            let scale = 1.0 / Double(max(1, active.count))
            let next: DiskTensorData
            if effect.kind.requiresDriver {
                guard let id = effect.driverMediaID, let driver = drivers[id] else { throw CoreRendererError.missingDriver }
                next = try await renderCross(
                    source: current, driver: driver, effect: effect, outputURL: destination
                ) { fraction, stage in progress(base + fraction * scale, stage) }
            } else {
                next = try await render(
                    input: current, effects: [effect], outputURL: destination,
                    scratchDirectory: scratchDirectory.appendingPathComponent("node-\(index)"), budget: budget
                ) { fraction, stage in progress(base + fraction * scale, stage) }
            }
            if current.fileURL != input.fileURL && current.fileURL != outputURL {
                try? FileManager.default.removeItem(at: current.fileURL)
            }
            current = next
        }
        if active.isEmpty {
            try FileManager.default.copyItem(at: input.fileURL, to: outputURL)
            current.fileURL = outputURL
        }
        return current
    }

    private static func renderCross(
        source: DiskTensorData,
        driver: DiskTensorData,
        effect: EffectNode,
        outputURL: URL,
        progress: @escaping @Sendable (Double, String) -> Void
    ) async throws -> DiskTensorData {
        let context = FileProgressContext(handler: progress)
        return try await withTaskCancellationHandler {
            try await Task.detached(priority: .userInitiated) {
                let descriptor = effect.coreDescriptor()
                let info: (DiskTensorData) -> CFFileTensorInfo = { tensor in
                    CFFileTensorInfo(
                        frames: UInt64(tensor.frames), height: UInt64(tensor.height),
                        width: UInt64(tensor.width), channels: UInt64(tensor.channels),
                        frame_rate_numerator: UInt32(max(1, Int((tensor.framesPerSecond * 1000).rounded()))),
                        frame_rate_denominator: 1000)
                }
                var outputInfo = CFFileTensorInfo()
                var error = [CChar](repeating: 0, count: 1024)
                let errorCapacity = UInt64(error.count)
                let opaque = Unmanaged.passRetained(context).toOpaque()
                defer { Unmanaged<FileProgressContext>.fromOpaque(opaque).release() }
                let status = source.fileURL.path.withCString { sourcePath in
                    driver.fileURL.path.withCString { driverPath in
                        outputURL.path.withCString { outputPath in
                            error.withUnsafeMutableBufferPointer { errorBuffer in
                                cf_render_file_cross_tensor_effect(
                                    sourcePath, info(source), driverPath, info(driver), outputPath, descriptor,
                                    chronoForgeFileProgress, opaque, &outputInfo,
                                    errorBuffer.baseAddress, errorCapacity)
                            }
                        }
                    }
                }
                if status == 3 { throw CancellationError() }
                guard status == 0 else { throw CoreRendererError.renderFailed(String(cString: error)) }
                let fps = Double(outputInfo.frame_rate_numerator) / Double(outputInfo.frame_rate_denominator)
                return DiskTensorData(
                    fileURL: outputURL, frames: Int(outputInfo.frames), height: Int(outputInfo.height),
                    width: Int(outputInfo.width), channels: Int(outputInfo.channels), framesPerSecond: fps,
                    duration: Double(outputInfo.frames) / fps, timestamps: nil)
            }.value
        } onCancel: { context.cancel() }
    }
}
