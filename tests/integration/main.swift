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

        let effect = cf_effect_descriptor_make(1, 2, 0, 0, 0, 0, 1, 0, 0)
        var descriptor = effect
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
              cf_video_buffer_value_count(output) == UInt64(tensor.values.count) else {
            throw IntegrationFailure.message("Core returned an unexpected output tensor")
        }
        print("ChronoForge integration passed: AVFoundation decode -> C++ effect -> tensor output")
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
