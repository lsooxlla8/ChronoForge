import AVFoundation
import CoreVideo
import Foundation

enum VideoExporterError: LocalizedError {
    case cannotConfigure
    case cannotAllocateFrame

    var errorDescription: String? {
        switch self {
        case .cannotConfigure: "ChronoForge could not configure the MP4 encoder."
        case .cannotAllocateFrame: "ChronoForge could not allocate an export frame."
        }
    }
}

enum VideoExporter {
    static func export(_ tensor: VideoTensorData, to url: URL) async throws {
        try? FileManager.default.removeItem(at: url)
        let encodedWidth = tensor.width + tensor.width % 2
        let encodedHeight = tensor.height + tensor.height % 2
        let writer = try AVAssetWriter(outputURL: url, fileType: .mp4)
        let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
            AVVideoCodecKey: AVVideoCodecType.h264,
            AVVideoWidthKey: encodedWidth,
            AVVideoHeightKey: encodedHeight,
            AVVideoCompressionPropertiesKey: [
                AVVideoAverageBitRateKey: max(1_000_000, encodedWidth * encodedHeight * 12),
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

        let timescale: CMTimeScale = 60_000
        for frame in 0..<tensor.frames {
            try Task.checkCancellation()
            while !input.isReadyForMoreMediaData {
                try await Task.sleep(for: .milliseconds(2))
            }
            guard let pool = adaptor.pixelBufferPool else { throw VideoExporterError.cannotAllocateFrame }
            var optional: CVPixelBuffer?
            guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &optional) == kCVReturnSuccess,
                  let buffer = optional else { throw VideoExporterError.cannotAllocateFrame }
            write(tensor: tensor, frame: frame, into: buffer)
            let presentation = CMTime(seconds: Double(frame) / tensor.framesPerSecond, preferredTimescale: timescale)
            guard adaptor.append(buffer, withPresentationTime: presentation) else {
                throw writer.error ?? VideoExporterError.cannotConfigure
            }
        }
        input.markAsFinished()
        await writer.finishWriting()
        guard writer.status == .completed else { throw writer.error ?? VideoExporterError.cannotConfigure }
    }

    private static func write(tensor: VideoTensorData, frame: Int, into buffer: CVPixelBuffer) {
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        guard let base = CVPixelBufferGetBaseAddress(buffer) else { return }
        memset(base, 0, CVPixelBufferGetDataSize(buffer))
        let rowBytes = CVPixelBufferGetBytesPerRow(buffer)
        let source = tensor.frameValues(at: frame)
        source.withContiguousStorageIfAvailable { values in
            for y in 0..<tensor.height {
                let row = base.advanced(by: y * rowBytes).assumingMemoryBound(to: UInt8.self)
                for x in 0..<tensor.width {
                    let sourceIndex = (y * tensor.width + x) * tensor.channels
                    let pixel = row.advanced(by: x * 4)
                    let alpha = tensor.channels >= 4 ? clamp(values[sourceIndex + 3]) : 1
                    let divisor = alpha > 0.000_01 ? alpha : 1
                    pixel[0] = byte(linearToSRGB(values[sourceIndex + 2] / divisor))
                    pixel[1] = byte(linearToSRGB(values[sourceIndex + 1] / divisor))
                    pixel[2] = byte(linearToSRGB(values[sourceIndex] / divisor))
                    pixel[3] = byte(alpha)
                }
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
