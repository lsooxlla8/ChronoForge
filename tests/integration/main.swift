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

        func render(_ requested: CFEffectDescriptorV2) throws -> [Float] {
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
            guard cf_video_buffer_frames(output) == UInt64(tensor.frames),
                  cf_video_buffer_width(output) == 64,
                  cf_video_buffer_height(output) == 48,
                  cf_video_buffer_value_count(output) == UInt64(tensor.values.count),
                  let values = cf_video_buffer_values(output) else {
                throw IntegrationFailure.message("Core returned an unexpected output tensor")
            }
            return Array(UnsafeBufferPointer(start: values, count: tensor.values.count))
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
        guard zip(differenceOutput, zip(tensor.values, first)).allSatisfy({ element in
            let (output, pair) = element
            return abs(output - abs(pair.0 - pair.1)) < 0.0001
        }) else {
            throw IntegrationFailure.message("Difference Amount mode was not applied by the proxy bridge")
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
        print("ChronoForge integration passed: descriptor V2 validation, Amount identity and Wave A bridge mappings")
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
