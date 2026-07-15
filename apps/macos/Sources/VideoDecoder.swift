import AVFoundation
import CoreVideo
import Foundation

struct DecodedProxy: Sendable {
    var tensor: VideoTensorData
    var displayName: String
    var sourceURL: URL
    var sourceSize: CGSize
    var sourceDuration: Double
}

enum VideoDecoderError: LocalizedError {
    case noVideoTrack
    case readerCouldNotStart
    case noFrames
    case pixelBufferUnavailable

    var errorDescription: String? {
        switch self {
        case .noVideoTrack: "The selected file has no readable video track."
        case .readerCouldNotStart: "ChronoForge could not start decoding this video."
        case .noFrames: "No frames could be decoded from this video."
        case .pixelBufferUnavailable: "The decoder returned an unsupported frame buffer."
        }
    }
}

enum VideoDecoder {
    private static let maximumProxyFrames = 180
    private static let maximumWidth = 320
    private static let maximumHeight = 180

    static func decodeProxy(from url: URL) async throws -> DecodedProxy {
        let asset = AVURLAsset(url: url)
        guard let track = try await asset.loadTracks(withMediaType: .video).first else {
            throw VideoDecoderError.noVideoTrack
        }
        let durationTime = try await asset.load(.duration)
        let duration = max(CMTimeGetSeconds(durationTime), 1.0 / 30.0)
        let naturalSize = try await track.load(.naturalSize)
        let preferredTransform = try await track.load(.preferredTransform)
        let transformed = naturalSize.applying(preferredTransform)
        let sourceSize = CGSize(width: abs(transformed.width), height: abs(transformed.height))
        let targetSize = proxySize(for: sourceSize)
        let proxyFPS = min(10.0, max(0.1, Double(maximumProxyFrames) / duration))

        let reader = try AVAssetReader(asset: asset)
        let settings: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: targetSize.width,
            kCVPixelBufferHeightKey as String: targetSize.height,
            AVVideoScalingModeKey: AVVideoScalingModeResizeAspect,
        ]
        let output = AVAssetReaderTrackOutput(track: track, outputSettings: settings)
        output.alwaysCopiesSampleData = false
        guard reader.canAdd(output) else {
            throw VideoDecoderError.readerCouldNotStart
        }
        reader.add(output)
        guard reader.startReading() else {
            throw reader.error ?? VideoDecoderError.readerCouldNotStart
        }

        var samples: [Float] = []
        let frameValueCount = Int(targetSize.width) * Int(targetSize.height) * 4
        samples.reserveCapacity(frameValueCount * maximumProxyFrames)
        var nextSampleTime = 0.0
        let interval = 1.0 / proxyFPS
        var frameCount = 0

        while reader.status == .reading, let sample = output.copyNextSampleBuffer() {
            try Task.checkCancellation()
            let presentationTime = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample))
            guard presentationTime + 0.000_001 >= nextSampleTime else { continue }
            guard let pixelBuffer = CMSampleBufferGetImageBuffer(sample) else {
                throw VideoDecoderError.pixelBufferUnavailable
            }
            appendLinearRGBA(pixelBuffer, to: &samples)
            frameCount += 1
            nextSampleTime += interval
            if frameCount >= maximumProxyFrames { break }
        }
        reader.cancelReading()
        guard frameCount > 0 else { throw VideoDecoderError.noFrames }

        return DecodedProxy(
            tensor: VideoTensorData(
                values: samples,
                frames: frameCount,
                height: Int(targetSize.height),
                width: Int(targetSize.width),
                channels: 4,
                framesPerSecond: proxyFPS,
                duration: Double(frameCount) / proxyFPS
            ),
            displayName: url.lastPathComponent,
            sourceURL: url,
            sourceSize: sourceSize,
            sourceDuration: duration
        )
    }

    private static func proxySize(for source: CGSize) -> CGSize {
        guard source.width > 0, source.height > 0 else {
            return CGSize(width: maximumWidth, height: maximumHeight)
        }
        let scale = min(Double(maximumWidth) / source.width, Double(maximumHeight) / source.height, 1.0)
        let width = max(2, Int((source.width * scale).rounded()) / 2 * 2)
        let height = max(2, Int((source.height * scale).rounded()) / 2 * 2)
        return CGSize(width: width, height: height)
    }

    private static func appendLinearRGBA(_ pixelBuffer: CVPixelBuffer, to values: inout [Float]) {
        CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(pixelBuffer) else { return }
        let width = CVPixelBufferGetWidth(pixelBuffer)
        let height = CVPixelBufferGetHeight(pixelBuffer)
        let bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer)

        for y in 0..<height {
            let row = base.advanced(by: y * bytesPerRow).assumingMemoryBound(to: UInt8.self)
            for x in 0..<width {
                let pixel = row.advanced(by: x * 4)
                let blue = srgbToLinear(Float(pixel[0]) / 255)
                let green = srgbToLinear(Float(pixel[1]) / 255)
                let red = srgbToLinear(Float(pixel[2]) / 255)
                let alpha = Float(pixel[3]) / 255
                values.append(red * alpha)
                values.append(green * alpha)
                values.append(blue * alpha)
                values.append(alpha)
            }
        }
    }

    private static func srgbToLinear(_ value: Float) -> Float {
        value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4)
    }
}
