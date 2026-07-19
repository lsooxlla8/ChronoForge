import AVFoundation
import ChronoForgeCoreBridge
import CoreVideo
import Foundation

enum IntegrationFailure: Error {
    case message(String)
}

@main
struct ChronoForgeIntegration {
    static func main() async throws {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("chronoforge-integration.mov")
        try? FileManager.default.removeItem(at: url)
        defer { try? FileManager.default.removeItem(at: url) }
        try await makeMovie(at: url)
        let tensor = try await decode(url)

        let effectValues: [Float] = [2]
        let effectOptions: [Int32] = [0, 1]
        let effect = effectValues.withUnsafeBufferPointer { values in
            effectOptions.withUnsafeBufferPointer { options in
                cf_effect_descriptor_v2_make(1, 1, 0, 0, values.baseAddress, 1, options.baseAddress, 2)
            }
        }

        func renderAny(_ requested: CFEffectDescriptorV2) throws -> (
            values: [Float], frames: Int, height: Int, width: Int, channels: Int
        ) {
            var descriptor = requested
            var output: OpaquePointer?
            var error = [CChar](repeating: 0, count: 1024)
            let errorCapacity = UInt64(error.count)
            let result = tensor.values.withUnsafeBufferPointer { values in
                withUnsafePointer(to: &descriptor) { effectPointer in
                    error.withUnsafeMutableBufferPointer { errorBuffer in
                        cf_render_effect_chain(
                            values.baseAddress,
                            UInt64(tensor.frames), 48, 64, 4,
                            8, 1,
                            effectPointer, 1,
                            128 * 1024 * 1024,
                            &output,
                            errorBuffer.baseAddress,
                            errorCapacity
                        )
                    }
                }
            }
            guard result == 0, let output else {
                throw IntegrationFailure.message(String(cString: error))
            }
            defer { cf_video_buffer_destroy(output) }
            let valueCount = Int(cf_video_buffer_value_count(output))
            guard valueCount > 0, let values = cf_video_buffer_values(output) else {
                throw IntegrationFailure.message("Core returned an unexpected output tensor")
            }
            return (
                Array(UnsafeBufferPointer(start: values, count: valueCount)),
                Int(cf_video_buffer_frames(output)),
                Int(cf_video_buffer_height(output)),
                Int(cf_video_buffer_width(output)),
                Int(cf_video_buffer_channels(output))
            )
        }

        func render(_ requested: CFEffectDescriptorV2) throws -> [Float] {
            let output = try renderAny(requested)
            guard output.frames == tensor.frames,
                  output.width == 64,
                  output.height == 48,
                  output.channels == 4,
                  output.values.count == tensor.values.count else {
                throw IntegrationFailure.message("Core returned an unexpected output tensor")
            }
            return output.values
        }

        func descriptor(
            kind: Int32,
            values: [Float],
            options: [Int32],
            seed: UInt64 = 0
        ) -> CFEffectDescriptorV2 {
            values.withUnsafeBufferPointer { valueBuffer in
                options.withUnsafeBufferPointer { optionBuffer in
                    cf_effect_descriptor_v2_make(
                        kind, 1, 0, seed,
                        valueBuffer.baseAddress, UInt32(values.count),
                        optionBuffer.baseAddress, UInt32(options.count)
                    )
                }
            }
        }

        func renderCross(_ requested: CFEffectDescriptorV2) throws -> [Float] {
            let driver = [Float](repeating: 0, count: tensor.values.count)
            var output: OpaquePointer?
            var error = [CChar](repeating: 0, count: 1024)
            let errorCapacity = UInt64(error.count)
            let result = tensor.values.withUnsafeBufferPointer { source in
                driver.withUnsafeBufferPointer { driverValues in
                    error.withUnsafeMutableBufferPointer { errorBuffer in
                        cf_render_cross_tensor_effect(
                            source.baseAddress, UInt64(tensor.frames), 48, 64, 4, 8, 1,
                            driverValues.baseAddress, UInt64(tensor.frames), 48, 64, 4,
                            requested, 128 * 1024 * 1024, &output,
                            errorBuffer.baseAddress, errorCapacity)
                    }
                }
            }
            guard result == 0, let output, let values = cf_video_buffer_values(output) else {
                throw IntegrationFailure.message(String(cString: error))
            }
            defer { cf_video_buffer_destroy(output) }
            return Array(UnsafeBufferPointer(start: values, count: tensor.values.count))
        }

        let first = try render(effect)
        let second = try render(effect)
        guard first == second else {
            throw IntegrationFailure.message("Identical descriptor and seed must render deterministically")
        }

        // Exercise every single-input preview dispatch that is not covered by the
        // more specific assertions below. This catches a missing or rejected
        // in-memory bridge path before it can silently break a large part of the
        // Preview menu while file rendering still works.
        let singleInputPreviewDescriptors: [(Int32, CFEffectDescriptorV2)] = [
            (0, descriptor(kind: 0, values: [], options: [0, 1])),
            (2, descriptor(kind: 2, values: [0.5, 0.5, 0.08, 0.75, 0], options: [1, 0, 0])),
            (3, descriptor(kind: 3, values: [0, 0], options: [0, 0])),
            (4, descriptor(kind: 4, values: [0, 15, 0], options: [3])),
            (5, descriptor(kind: 5, values: [0], options: [0, 1, 1, 0])),
            (6, descriptor(kind: 6, values: [], options: [0, 0])),
            (9, descriptor(kind: 9, values: [0.02, 4, 0, 180], options: [0])),
            (10, descriptor(kind: 10, values: [2, 0.35, 2, 0.15], options: [1])),
            (11, descriptor(kind: 11, values: [0.2, 8, 0.05], options: [0, 0, 0], seed: 42)),
            (12, descriptor(kind: 12, values: [2, 0.12, 1, 0], options: [0, 0, 0])),
            (12, descriptor(kind: 12, values: [3, 0.12, 0.85, 0.2], options: [3, 1, 0])),
            (12, descriptor(kind: 12, values: [3, 0.18, 1, 0], options: [4, 0, 0])),
            (23, descriptor(kind: 23, values: [0.08, 0.25, 5, 0.5], options: [2], seed: 73)),
        ]
        for (kind, requested) in singleInputPreviewDescriptors {
            let output = try renderAny(requested)
            guard output.frames > 0, output.height > 0, output.width > 0, output.channels == 4,
                  output.values.allSatisfy(\.isFinite) else {
                throw IntegrationFailure.message("Single-input preview dispatch failed for effect kind \(kind)")
            }
        }

        for requested in [
            descriptor(kind: 7, values: [], options: [0, 1, 2, 1]),
            descriptor(kind: 8, values: [12, 24, 24], options: [0, 1, 0]),
        ] {
            let output = try renderCross(requested)
            guard output.count == tensor.values.count, output.allSatisfy(\.isFinite) else {
                throw IntegrationFailure.message("Two-input preview dispatch returned an invalid tensor")
            }
        }

        let dryEffect = effectValues.withUnsafeBufferPointer { values in
            effectOptions.withUnsafeBufferPointer { options in
                cf_effect_descriptor_v2_make(1, 0, 0, 123, values.baseAddress, 1, options.baseAddress, 2)
            }
        }
        guard try render(dryEffect) == tensor.values else {
            throw IntegrationFailure.message("Amount zero must be a bit-exact identity")
        }

        let differenceEffect = effectValues.withUnsafeBufferPointer { values in
            effectOptions.withUnsafeBufferPointer { options in
                cf_effect_descriptor_v2_make(1, 1, 4, 123, values.baseAddress, 1, options.baseAddress, 2)
            }
        }
        let differenceOutput = try render(differenceEffect)
        for pixel in 0..<(tensor.frames * 48 * 64) {
            let offset = pixel * 4
            let alpha = min(max(abs(tensor.values[offset + 3] - first[offset + 3]), 0), 1)
            guard abs(differenceOutput[offset + 3] - alpha) < 0.0001 else {
                throw IntegrationFailure.message("Difference Amount mode did not produce its expected alpha")
            }
            for channel in 0..<3 {
                let rawDifference = abs(tensor.values[offset + channel] - first[offset + channel])
                let expected = min(max(rawDifference, 0), alpha)
                guard abs(differenceOutput[offset + channel] - expected) < 0.0001 else {
                    throw IntegrationFailure.message("Difference Amount mode did not preserve premultiplied RGB")
                }
            }
        }

        let displaceEffect = effectValues.withUnsafeBufferPointer { values in
            effectOptions.withUnsafeBufferPointer { options in
                cf_effect_descriptor_v2_make(1, 1, 5, 123, values.baseAddress, 1, options.baseAddress, 2)
            }
        }
        guard try render(displaceEffect) != tensor.values else {
            throw IntegrationFailure.message("Displace Amount mode did not use the effect as a displacement field")
        }

        let rgbValues: [Float] = [1, 0, -1, 2]
        let rgbOptions: [Int32] = [0, 1]
        let rgbTimeSlip = rgbValues.withUnsafeBufferPointer { values in
            rgbOptions.withUnsafeBufferPointer { options in
                cf_effect_descriptor_v2_make(13, 1, 0, 0, values.baseAddress, 4, options.baseAddress, 2)
            }
        }
        let rgbOutput = try render(rgbTimeSlip)
        guard rgbOutput != tensor.values,
              stride(from: 3, to: rgbOutput.count, by: 4).allSatisfy({ rgbOutput[$0] == tensor.values[$0] }) else {
            throw IntegrationFailure.message("RGB Time Slip bridge mapping did not separate channels while preserving current-frame alpha")
        }

        let syncValues: [Float] = [0.5, 0.2, 0.5, 1]
        let syncOptions: [Int32] = [0, 1, 0]
        func syncLoss(seed: UInt64) -> CFEffectDescriptorV2 {
            syncValues.withUnsafeBufferPointer { values in
                syncOptions.withUnsafeBufferPointer { options in
                    cf_effect_descriptor_v2_make(14, 1, 0, seed, values.baseAddress, 4, options.baseAddress, 3)
                }
            }
        }
        let syncA = try render(syncLoss(seed: 11))
        guard syncA == (try render(syncLoss(seed: 11))),
              syncA != (try render(syncLoss(seed: 12))) else {
            throw IntegrationFailure.message("Horizontal Sync Loss bridge path ignored deterministic seed semantics")
        }

        let chromaValues: [Float] = [12, 4, 1, 3]
        let chromaOptions: [Int32] = [1, 1]
        let chromaDrift = chromaValues.withUnsafeBufferPointer { values in
            chromaOptions.withUnsafeBufferPointer { options in
                cf_effect_descriptor_v2_make(15, 1, 0, 0, values.baseAddress, 4, options.baseAddress, 2)
            }
        }
        let chromaOutput = try render(chromaDrift)
        guard chromaOutput != tensor.values,
              stride(from: 3, to: chromaOutput.count, by: 4).allSatisfy({ chromaOutput[$0] == tensor.values[$0] }) else {
            throw IntegrationFailure.message("Chroma Carrier Drift bridge path failed to preserve current-frame alpha")
        }

        let strideValues: [Float] = [0.17, 0.11, 0.031]
        let strideOptions: [Int32] = [0, 1]
        let strideError = strideValues.withUnsafeBufferPointer { values in
            strideOptions.withUnsafeBufferPointer { options in
                cf_effect_descriptor_v2_make(16, 1, 0, 0, values.baseAddress, 3, options.baseAddress, 2)
            }
        }
        let strideOutput = try render(strideError)
        guard strideOutput != tensor.values,
              stride(from: 3, to: strideOutput.count, by: 4).allSatisfy({ strideOutput[$0] == tensor.values[$0] }) else {
            throw IntegrationFailure.message("Stride Error bridge path did not preserve current alpha in RGB Together mode")
        }

        let blockValues: [Float] = [0.35, 1, 2, 2]
        let blockOptions: [Int32] = [3, 1]
        func blockCorruption(seed: UInt64) -> CFEffectDescriptorV2 {
            blockValues.withUnsafeBufferPointer { values in
                blockOptions.withUnsafeBufferPointer { options in
                    cf_effect_descriptor_v2_make(17, 1, 0, seed, values.baseAddress, 4, options.baseAddress, 2)
                }
            }
        }
        let blockA = try render(blockCorruption(seed: 31))
        guard blockA == (try render(blockCorruption(seed: 31))),
              blockA != (try render(blockCorruption(seed: 32))) else {
            throw IntegrationFailure.message("Block Address Corruption bridge path ignored deterministic seed semantics")
        }

        let bitplaneValues: [Float] = [8, 255, 1]
        let bitplaneOptions: [Int32] = [3, 1]
        func bitplane(seed: UInt64) -> CFEffectDescriptorV2 {
            bitplaneValues.withUnsafeBufferPointer { values in
                bitplaneOptions.withUnsafeBufferPointer { options in
                    cf_effect_descriptor_v2_make(18, 1, 0, seed, values.baseAddress, 3, options.baseAddress, 2)
                }
            }
        }
        let bitplaneA = try render(bitplane(seed: 101))
        guard bitplaneA == (try render(bitplane(seed: 101))),
              bitplaneA != (try render(bitplane(seed: 102))) else {
            throw IntegrationFailure.message("Bitplane Forge bridge path ignored deterministic XOR seed semantics")
        }

        let weaveValues: [Float] = [0.25, 0.25, 0.2, 1]
        let weaveOptions: [Int32] = [2, 0]
        let weave = weaveValues.withUnsafeBufferPointer { values in
            weaveOptions.withUnsafeBufferPointer { options in
                cf_effect_descriptor_v2_make(19, 1, 0, 55, values.baseAddress, 4, options.baseAddress, 2)
            }
        }
        let weaveOutput = try renderCross(weave)
        guard weaveOutput != tensor.values, weaveOutput == (try renderCross(weave)) else {
            throw IntegrationFailure.message("Signal Weave bridge path did not combine A/B deterministically")
        }

        let graftValues: [Float] = [0.5, 1, 3, 0]
        let graftOptions: [Int32] = [0, 0]
        let graft = graftValues.withUnsafeBufferPointer { values in
            graftOptions.withUnsafeBufferPointer { options in
                cf_effect_descriptor_v2_make(20, 1, 0, 88, values.baseAddress, 4, options.baseAddress, 2)
            }
        }
        let graftOutput = try renderCross(graft)
        guard graftOutput != tensor.values, graftOutput == (try renderCross(graft)) else {
            throw IntegrationFailure.message("Block Graft bridge path did not graft deterministic B blocks")
        }

        let transplantValues: [Float] = [1, 4, -3]
        let transplantOptions: [Int32] = [1, 0, 1, 0, 0]
        let transplant = transplantValues.withUnsafeBufferPointer { values in
            transplantOptions.withUnsafeBufferPointer { options in
                cf_effect_descriptor_v2_make(21, 1, 0, 0, values.baseAddress, 3, options.baseAddress, 5)
            }
        }
        let transplantOutput = try renderCross(transplant)
        guard transplantOutput != tensor.values,
              stride(from: 3, to: transplantOutput.count, by: 4).allSatisfy({ transplantOutput[$0] == tensor.values[$0] }) else {
            throw IntegrationFailure.message("Channel Transplant bridge path did not preserve A alpha")
        }

        func affinityMigration(seed: UInt64) -> CFEffectDescriptorV2 {
            descriptor(kind: 23, values: [0.08, 0.25, 5, 0.5], options: [2], seed: seed)
        }
        let affinityA = try render(affinityMigration(seed: 73))
        guard affinityA == (try render(affinityMigration(seed: 73))),
              affinityA != (try render(affinityMigration(seed: 74))) else {
            throw IntegrationFailure.message("Affinity Migration bridge path ignored deterministic seed semantics")
        }

        var outdated = effect
        outdated.descriptor_version = 1
        do {
            _ = try render(outdated)
            throw IntegrationFailure.message("Core accepted an outdated descriptor version")
        } catch IntegrationFailure.message(let message) {
            guard message == "Unsupported effect descriptor version" else {
                throw IntegrationFailure.message(message)
            }
        }
        print("ChronoForge integration passed: descriptor V2 validation, Amount identity and production effect mappings")
    }

    private static func makeMovie(at url: URL) async throws {
        let writer = try AVAssetWriter(outputURL: url, fileType: .mov)
        let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
            AVVideoCodecKey: AVVideoCodecType.h264,
            AVVideoWidthKey: 64,
            AVVideoHeightKey: 48,
        ])
        let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: 64,
            kCVPixelBufferHeightKey as String: 48,
        ])
        guard writer.canAdd(input) else { throw IntegrationFailure.message("Cannot add video writer input") }
        writer.add(input)
        guard writer.startWriting() else { throw writer.error ?? IntegrationFailure.message("Writer did not start") }
        writer.startSession(atSourceTime: .zero)
        for frame in 0..<8 {
            while !input.isReadyForMoreMediaData { try await Task.sleep(for: .milliseconds(2)) }
            guard let pool = adaptor.pixelBufferPool else { throw IntegrationFailure.message("No pixel buffer pool") }
            var optional: CVPixelBuffer?
            guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &optional) == kCVReturnSuccess,
                  let buffer = optional else { throw IntegrationFailure.message("Cannot allocate pixel buffer") }
            fill(buffer, frame: frame)
            guard adaptor.append(buffer, withPresentationTime: CMTime(value: CMTimeValue(frame), timescale: 8)) else {
                throw writer.error ?? IntegrationFailure.message("Cannot append frame")
            }
        }
        input.markAsFinished()
        await writer.finishWriting()
        guard writer.status == .completed else { throw writer.error ?? IntegrationFailure.message("Writer did not finish") }
    }

    private static func decode(_ url: URL) async throws -> (values: [Float], frames: Int) {
        let asset = AVURLAsset(url: url)
        guard let track = try await asset.loadTracks(withMediaType: .video).first else {
            throw IntegrationFailure.message("No video track")
        }
        let reader = try AVAssetReader(asset: asset)
        let output = AVAssetReaderTrackOutput(track: track, outputSettings: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
        ])
        reader.add(output)
        guard reader.startReading() else { throw reader.error ?? IntegrationFailure.message("Reader did not start") }
        var values: [Float] = []
        var frames = 0
        while let sample = output.copyNextSampleBuffer(), let buffer = CMSampleBufferGetImageBuffer(sample) {
            CVPixelBufferLockBaseAddress(buffer, .readOnly)
            defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
            guard let base = CVPixelBufferGetBaseAddress(buffer) else { continue }
            let rowBytes = CVPixelBufferGetBytesPerRow(buffer)
            for y in 0..<48 {
                let row = base.advanced(by: y * rowBytes).assumingMemoryBound(to: UInt8.self)
                for x in 0..<64 {
                    let pixel = row.advanced(by: x * 4)
                    values.append(Float(pixel[2]) / 255)
                    values.append(Float(pixel[1]) / 255)
                    values.append(Float(pixel[0]) / 255)
                    values.append(1)
                }
            }
            frames += 1
        }
        guard frames > 1 else { throw IntegrationFailure.message("Too few decoded frames") }
        return (values, frames)
    }

    private static func fill(_ buffer: CVPixelBuffer, frame: Int) {
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        guard let base = CVPixelBufferGetBaseAddress(buffer) else { return }
        let rowBytes = CVPixelBufferGetBytesPerRow(buffer)
        for y in 0..<48 {
            let row = base.advanced(by: y * rowBytes).assumingMemoryBound(to: UInt8.self)
            for x in 0..<64 {
                let pixel = row.advanced(by: x * 4)
                pixel[0] = UInt8((x * 4 + frame * 10) % 255)
                pixel[1] = UInt8((y * 5 + frame * 20) % 255)
                pixel[2] = UInt8((frame * 30) % 255)
                pixel[3] = 255
            }
        }
    }
}
