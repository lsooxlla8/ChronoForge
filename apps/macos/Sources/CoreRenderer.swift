import Foundation
import ChronoForgeCoreBridge

enum CoreRendererError: LocalizedError {
    case renderFailed(String)
    case invalidOutput
    case missingDriver

    var errorDescription: String? {
        switch self {
        case .renderFailed(let message): message
        case .invalidOutput: "The processing core returned an invalid tensor."
        case .missingDriver: "Choose a driver video for every two-input effect."
        }
    }
}

struct SelectedEffectCapture: Sendable {
    let nodeID: UUID
    let input: VideoTensorData
    let output: VideoTensorData
}

struct PreviewRenderResult: Sendable {
    let output: VideoTensorData
    let selectedEffect: SelectedEffectCapture?
}

enum CoreRenderer {
    static let defaultBudget: UInt64 = 768 * 1024 * 1024

    static func render(
        input: VideoTensorData,
        effects: [EffectNode],
        drivers: [UUID: VideoTensorData] = [:],
        budget: UInt64 = defaultBudget
    ) async throws -> VideoTensorData {
        if effects.contains(where: { $0.enabled && $0.kind.requiresDriver }) {
            var current = input
            for effect in effects where effect.enabled {
                try Task.checkCancellation()
                if effect.kind.requiresDriver {
                    guard let id = effect.driverMediaID, let driver = drivers[id] else {
                        throw CoreRendererError.missingDriver
                    }
                    current = try await renderCrossEffect(source: current, driver: driver, effect: effect, budget: budget)
                } else {
                    current = try await render(input: current, effects: [effect], budget: budget)
                }
            }
            return current
        }
        if effects.contains(where: { $0.enabled && $0.kind == .spectralFFTSwap }) {
            return try await renderDiskBacked(input: input, effects: effects, budget: budget)
        }
        return try await Task.detached(priority: .userInitiated) {
            try Task.checkCancellation()
            let descriptors = effects.filter(\.enabled).map { $0.coreDescriptor() }
            var output: OpaquePointer?
            var error = [CChar](repeating: 0, count: 1024)
            let errorCapacity = UInt64(error.count)
            let status = input.values.withUnsafeBufferPointer { inputBuffer in
                descriptors.withUnsafeBufferPointer { descriptorBuffer in
                    error.withUnsafeMutableBufferPointer { errorBuffer in
                        cf_render_effect_chain(
                            inputBuffer.baseAddress,
                            UInt64(input.frames),
                            UInt64(input.height),
                            UInt64(input.width),
                            UInt64(input.channels),
                            UInt32(max(1, Int((input.framesPerSecond * 1000).rounded()))),
                            1000,
                            descriptorBuffer.baseAddress,
                            UInt64(descriptors.count),
                            budget,
                            &output,
                            errorBuffer.baseAddress,
                            errorCapacity
                        )
                    }
                }
            }
            guard status == 0 else {
                throw CoreRendererError.renderFailed(String(cString: error))
            }
            guard let output else {
                throw CoreRendererError.invalidOutput
            }
            defer { cf_video_buffer_destroy(output) }
            try Task.checkCancellation()

            let valueCount = Int(cf_video_buffer_value_count(output))
            guard let pointer = cf_video_buffer_values(output), valueCount > 0 else {
                throw CoreRendererError.invalidOutput
            }
            return VideoTensorData(
                values: Array(UnsafeBufferPointer(start: pointer, count: valueCount)),
                frames: Int(cf_video_buffer_frames(output)),
                height: Int(cf_video_buffer_height(output)),
                width: Int(cf_video_buffer_width(output)),
                channels: Int(cf_video_buffer_channels(output)),
                framesPerSecond: input.framesPerSecond,
                duration: Double(cf_video_buffer_frames(output)) / input.framesPerSecond
            )
        }.value
    }

    static func renderCapturingSelectedEffect(
        input: VideoTensorData,
        effects: [EffectNode],
        drivers: [UUID: VideoTensorData] = [:],
        selectedNodeID: UUID?,
        budget: UInt64 = defaultBudget
    ) async throws -> PreviewRenderResult {
        guard let selectedNodeID,
              effects.contains(where: { $0.id == selectedNodeID }) else {
            return PreviewRenderResult(
                output: try await render(input: input, effects: effects, drivers: drivers, budget: budget),
                selectedEffect: nil
            )
        }
        return try await renderSequentially(
            input: input,
            effects: effects,
            drivers: drivers,
            selectedNodeID: selectedNodeID,
            stopAfterCapture: false,
            budget: budget
        )
    }

    static func captureSelectedEffect(
        input: VideoTensorData,
        effects: [EffectNode],
        drivers: [UUID: VideoTensorData] = [:],
        selectedNodeID: UUID,
        budget: UInt64 = defaultBudget
    ) async throws -> SelectedEffectCapture? {
        guard effects.contains(where: { $0.id == selectedNodeID }) else { return nil }
        return try await renderSequentially(
            input: input,
            effects: effects,
            drivers: drivers,
            selectedNodeID: selectedNodeID,
            stopAfterCapture: true,
            budget: budget
        ).selectedEffect
    }

    private static func renderSequentially(
        input: VideoTensorData,
        effects: [EffectNode],
        drivers: [UUID: VideoTensorData],
        selectedNodeID: UUID,
        stopAfterCapture: Bool,
        budget: UInt64
    ) async throws -> PreviewRenderResult {
        var current = input
        var capture: SelectedEffectCapture?
        for effect in effects {
            try Task.checkCancellation()
            let effectInput = current
            if effect.enabled {
                if effect.kind.requiresDriver {
                    guard let id = effect.driverMediaID, let driver = drivers[id] else {
                        throw CoreRendererError.missingDriver
                    }
                    current = try await renderCrossEffect(
                        source: current,
                        driver: driver,
                        effect: effect,
                        budget: budget
                    )
                } else {
                    current = try await render(input: current, effects: [effect], budget: budget)
                }
            }
            if effect.id == selectedNodeID {
                capture = SelectedEffectCapture(nodeID: effect.id, input: effectInput, output: current)
                if stopAfterCapture { break }
            }
        }
        return PreviewRenderResult(output: current, selectedEffect: capture)
    }

    private static func renderCrossEffect(
        source: VideoTensorData,
        driver: VideoTensorData,
        effect: EffectNode,
        budget: UInt64
    ) async throws -> VideoTensorData {
        try await Task.detached(priority: .userInitiated) {
            let descriptor = effect.coreDescriptor()
            var output: OpaquePointer?
            var error = [CChar](repeating: 0, count: 1024)
            let errorCapacity = UInt64(error.count)
            let status = source.values.withUnsafeBufferPointer { sourceBuffer in
                driver.values.withUnsafeBufferPointer { driverBuffer in
                    error.withUnsafeMutableBufferPointer { errorBuffer in
                        cf_render_cross_tensor_effect(
                            sourceBuffer.baseAddress,
                            UInt64(source.frames), UInt64(source.height), UInt64(source.width), UInt64(source.channels),
                            UInt32(max(1, Int((source.framesPerSecond * 1000).rounded()))), 1000,
                            driverBuffer.baseAddress,
                            UInt64(driver.frames), UInt64(driver.height), UInt64(driver.width), UInt64(driver.channels),
                            descriptor, budget, &output, errorBuffer.baseAddress, errorCapacity
                        )
                    }
                }
            }
            guard status == 0 else { throw CoreRendererError.renderFailed(String(cString: error)) }
            guard let output else { throw CoreRendererError.invalidOutput }
            defer { cf_video_buffer_destroy(output) }
            let count = Int(cf_video_buffer_value_count(output))
            guard let pointer = cf_video_buffer_values(output), count > 0 else { throw CoreRendererError.invalidOutput }
            let frames = Int(cf_video_buffer_frames(output))
            let outputFPS = effect.kind == .dimensionalSplicer && effect.options[2] == 5
                ? driver.framesPerSecond
                : source.framesPerSecond
            return VideoTensorData(
                values: Array(UnsafeBufferPointer(start: pointer, count: count)),
                frames: frames,
                height: Int(cf_video_buffer_height(output)),
                width: Int(cf_video_buffer_width(output)),
                channels: Int(cf_video_buffer_channels(output)),
                framesPerSecond: outputFPS,
                duration: Double(frames) / outputFPS
            )
        }.value
    }

    private static func renderDiskBacked(
        input: VideoTensorData,
        effects: [EffectNode],
        budget: UInt64
    ) async throws -> VideoTensorData {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("ChronoForge-Proxy-FFT-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let inputURL = root.appendingPathComponent("input.raw")
        let inputData = input.values.withUnsafeBytes { Data($0) }
        try inputData.write(to: inputURL, options: .atomic)
        let diskInput = DiskTensorData(
            fileURL: inputURL,
            frames: input.frames,
            height: input.height,
            width: input.width,
            channels: input.channels,
            framesPerSecond: input.framesPerSecond,
            duration: input.duration,
            timestamps: nil
        )
        let outputURL = root.appendingPathComponent("output.raw")
        let diskOutput = try await FileCoreRenderer.render(
            input: diskInput,
            effects: effects,
            outputURL: outputURL,
            scratchDirectory: root.appendingPathComponent("scratch"),
            budget: budget
        ) { _, _ in }
        try Task.checkCancellation()
        let mapped = try Data(contentsOf: diskOutput.fileURL, options: .mappedIfSafe)
        guard mapped.count == diskOutput.valueCount * MemoryLayout<Float>.stride else {
            throw CoreRendererError.invalidOutput
        }
        var values = [Float](repeating: 0, count: diskOutput.valueCount)
        _ = values.withUnsafeMutableBytes { destination in
            mapped.copyBytes(to: destination)
        }
        return VideoTensorData(
            values: values,
            frames: diskOutput.frames,
            height: diskOutput.height,
            width: diskOutput.width,
            channels: diskOutput.channels,
            framesPerSecond: diskOutput.framesPerSecond,
            duration: diskOutput.duration
        )
    }
}
