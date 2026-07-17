import AVFoundation
import CoreVideo
import Foundation

enum FullVideoExporter {
    static func export(
        _ tensor: DiskTensorData,
        to url: URL,
        progress: @escaping @Sendable (Double, String) -> Void
    ) async throws {
        guard tensor.isValidOnDisk() else { throw CoreRendererError.invalidOutput }
        try? FileManager.default.removeItem(at: url)
        let mapped = try Data(contentsOf: tensor.fileURL, options: .mappedIfSafe)
        let encodedWidth = tensor.width + tensor.width % 2
        let encodedHeight = tensor.height + tensor.height % 2
        let writer = try AVAssetWriter(outputURL: url, fileType: .mp4)
        let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
            AVVideoCodecKey: AVVideoCodecType.h264,
            AVVideoWidthKey: encodedWidth,
            AVVideoHeightKey: encodedHeight,
            AVVideoCompressionPropertiesKey: [
                AVVideoAverageBitRateKey: max(2_000_000, encodedWidth * encodedHeight * 10),
                AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel,
            ],
        ])
        input.expectsMediaDataInRealTime = false
        let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: encodedWidth,
            kCVPixelBufferHeightKey as String: encodedHeight,
        ])
        guard writer.canAdd(input) else { throw VideoExporterError.cannotConfigure }
        writer.add(input)
        guard writer.startWriting() else { throw writer.error ?? VideoExporterError.cannotConfigure }
        writer.startSession(atSourceTime: .zero)

        for frame in 0..<tensor.frames {
            try Task.checkCancellation()
            while !input.isReadyForMoreMediaData { try await Task.sleep(for: .milliseconds(2)) }
            guard let pool = adaptor.pixelBufferPool else { throw VideoExporterError.cannotAllocateFrame }
            var optional: CVPixelBuffer?
            guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &optional) == kCVReturnSuccess,
                  let buffer = optional else { throw VideoExporterError.cannotAllocateFrame }
            mapped.withUnsafeBytes { raw in write(tensor: tensor, frame: frame, values: raw.bindMemory(to: Float.self), into: buffer) }
            let seconds = tensor.timestamps.flatMap { $0.indices.contains(frame) ? $0[frame] : nil }
                ?? Double(frame) / tensor.framesPerSecond
            guard adaptor.append(buffer, withPresentationTime: CMTime(seconds: seconds, preferredTimescale: 60_000)) else {
                throw writer.error ?? VideoExporterError.cannotConfigure
            }
            progress(Double(frame + 1) / Double(tensor.frames), "Encoding frame \(frame + 1) of \(tensor.frames)")
        }
        input.markAsFinished()
        await writer.finishWriting()
        guard writer.status == .completed else { throw writer.error ?? VideoExporterError.cannotConfigure }
    }

    private static func write(
        tensor: DiskTensorData,
        frame: Int,
        values: UnsafeBufferPointer<Float>,
        into buffer: CVPixelBuffer
    ) {
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        guard let base = CVPixelBufferGetBaseAddress(buffer) else { return }
        memset(base, 0, CVPixelBufferGetDataSize(buffer))
        let rowBytes = CVPixelBufferGetBytesPerRow(buffer)
        let frameOffset = frame * tensor.height * tensor.width * tensor.channels
        for y in 0..<tensor.height {
            let row = base.advanced(by: y * rowBytes).assumingMemoryBound(to: UInt8.self)
            for x in 0..<tensor.width {
                let source = frameOffset + (y * tensor.width + x) * tensor.channels
                let pixel = row.advanced(by: x * 4)
                // H.264 has no alpha channel. Tensor RGB is premultiplied, so writing it
                // directly produces an explicit composite over black.
                pixel[0] = byte(linearToSRGB(values[source + 2]))
                pixel[1] = byte(linearToSRGB(values[source + 1]))
                pixel[2] = byte(linearToSRGB(values[source]))
                pixel[3] = 255
            }
        }
    }

    private static func clamp(_ value: Float) -> Float { max(0, min(1, value)) }
    private static func linearToSRGB(_ value: Float) -> Float {
        let value = clamp(value)
        return value <= 0.0031308 ? 12.92 * value : 1.055 * pow(value, 1 / 2.4) - 0.055
    }
    private static func byte(_ value: Float) -> UInt8 { UInt8(max(0, min(255, Int((value * 255).rounded())))) }
}
