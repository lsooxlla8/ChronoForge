import Foundation
import ChronoForgeCoreBridge

enum CoreRendererError: LocalizedError {
    case renderFailed(String)
    case invalidOutput

    var errorDescription: String? {
        switch self {
        case .renderFailed(let message): message
        case .invalidOutput: "The processing core returned an invalid tensor."
        }
    }
}

enum CoreRenderer {
    static let defaultBudget: UInt64 = 768 * 1024 * 1024

    static func render(
        input: VideoTensorData,
        effects: [EffectNode],
        budget: UInt64 = defaultBudget
    ) async throws -> VideoTensorData {
        if effects.contains(where: { $0.enabled && $0.kind == .spectralFFTSwap }) {
            return try await renderDiskBacked(input: input, effects: effects, budget: budget)
        }
        return try await Task.detached(priority: .userInitiated) {
            try Task.checkCancellation()
            let descriptors = effects.filter(\.enabled).map { effect in
                cf_effect_descriptor_make(
                    effect.kind.rawValue,
                    effect.values[0], effect.values[1], effect.values[2], effect.values[3],
                    effect.options[0], effect.options[1], effect.options[2], effect.options[3]
                )
            }
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
