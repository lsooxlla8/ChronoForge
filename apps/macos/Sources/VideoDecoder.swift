import AVFoundation
import CoreVideo
import Foundation

struct DecodedProxy: Sendable {
    var id = UUID()
    var tensor: VideoTensorData
    var displayName: String
    var mediaSource: MediaSource
    var sourceSize: CGSize
    var sourceDuration: Double
    var sourceFrameCount: Int = 0
    var sourceFrameCountIsExact: Bool = false
    var sourceURL: URL { mediaSource.accessURL }
    var securityScopedBookmark: Data? { mediaSource.securityScopedBookmark }

    init(
        id: UUID = UUID(),
        tensor: VideoTensorData,
        displayName: String,
        sourceURL: URL,
        sourceSize: CGSize,
        sourceDuration: Double,
        sourceFrameCount: Int = 0,
        sourceFrameCountIsExact: Bool = false,
        securityScopedBookmark: Data? = nil
    ) {
        self.id = id
        self.tensor = tensor
        self.displayName = displayName
        mediaSource = .movie(url: sourceURL, securityScopedBookmark: securityScopedBookmark)
        self.sourceSize = sourceSize
        self.sourceDuration = sourceDuration
        self.sourceFrameCount = sourceFrameCount
        self.sourceFrameCountIsExact = sourceFrameCountIsExact
    }

    init(
        id: UUID = UUID(),
        tensor: VideoTensorData,
        displayName: String,
        mediaSource: MediaSource,
        sourceSize: CGSize,
        sourceDuration: Double,
        sourceFrameCount: Int = 0,
        sourceFrameCountIsExact: Bool = false
    ) {
        self.id = id
        self.tensor = tensor
        self.displayName = displayName
        self.mediaSource = mediaSource
        self.sourceSize = sourceSize
        self.sourceDuration = sourceDuration
        self.sourceFrameCount = sourceFrameCount
        self.sourceFrameCountIsExact = sourceFrameCountIsExact
    }
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
    static func decodeProxy(from url: URL, quality: ProxyQuality = .high) async throws -> DecodedProxy {
        let maximumProxyFrames = 180
        let maximumWidth = quality == .standard ? 320 : 480
        let maximumHeight = quality == .standard ? 180 : 270
        // High-quality proxies keep ordinary source cadence. The frame-count
        // limit below still bounds memory for long or high-frame-rate clips.
        let maximumFPS = quality == .standard ? 10.0 : 30.0
        let asset = AVURLAsset(url: url)
        guard let track = try await asset.loadTracks(withMediaType: .video).first else {
            throw VideoDecoderError.noVideoTrack
        }
        let durationTime = try await asset.load(.duration)
        let duration = max(CMTimeGetSeconds(durationTime), 1.0 / 30.0)
        let naturalSize = try await track.load(.naturalSize)
        let preferredTransform = try await track.load(.preferredTransform)
        let displayBounds = CGRect(origin: .zero, size: naturalSize).applying(preferredTransform).standardized
        let sourceSize = displayBounds.size
        let nominalFPS = max(1.0, Double(try await track.load(.nominalFrameRate)))
        let estimatedFrameCount = max(1, Int((duration * nominalFPS).rounded()))
        let targetSize = proxySize(for: sourceSize, maximumWidth: maximumWidth, maximumHeight: maximumHeight)
        let proxyFPS = min(nominalFPS, maximumFPS, max(0.1, Double(maximumProxyFrames) / duration))

        let reader = try AVAssetReader(asset: asset)
        let settings: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
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
            appendLinearRGBA(
                pixelBuffer,
                transform: preferredTransform,
                displayBounds: displayBounds,
                outputWidth: Int(targetSize.width),
                outputHeight: Int(targetSize.height),
                to: &samples
            )
            frameCount += 1
            nextSampleTime += interval
            if frameCount >= maximumProxyFrames { break }
        }
        reader.cancelReading()
        guard frameCount > 0 else { throw VideoDecoderError.noFrames }
        let exactFrameCount = try? countFrames(asset: asset, track: track)

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
            sourceDuration: duration,
            sourceFrameCount: exactFrameCount ?? estimatedFrameCount,
            sourceFrameCountIsExact: exactFrameCount != nil,
            securityScopedBookmark: try? url.bookmarkData(options: [.withSecurityScope], includingResourceValuesForKeys: nil, relativeTo: nil)
        )
    }

    private static func countFrames(asset: AVAsset, track: AVAssetTrack) throws -> Int {
        let reader = try AVAssetReader(asset: asset)
        let output = AVAssetReaderSampleReferenceOutput(track: track)
        guard reader.canAdd(output) else { throw VideoDecoderError.readerCouldNotStart }
        reader.add(output)
        guard reader.startReading() else { throw reader.error ?? VideoDecoderError.readerCouldNotStart }
        var count = 0
        while reader.status == .reading, let sample = output.copyNextSampleBuffer() {
            if Task.isCancelled {
                reader.cancelReading()
                throw CancellationError()
            }
            let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[CFString: Any]]
            let doNotDisplay = attachments?.first?[kCMSampleAttachmentKey_DoNotDisplay] as? Bool ?? false
            if !doNotDisplay {
                count += CMSampleBufferGetNumSamples(sample)
            }
        }
        guard reader.status == .completed, count > 0 else {
            throw reader.error ?? VideoDecoderError.noFrames
        }
        return count
    }

    private static func proxySize(for source: CGSize, maximumWidth: Int, maximumHeight: Int) -> CGSize {
        guard source.width > 0, source.height > 0 else {
            return CGSize(width: maximumWidth, height: maximumHeight)
        }
        let scale = min(Double(maximumWidth) / source.width, Double(maximumHeight) / source.height, 1.0)
        let width = max(2, Int((source.width * scale).rounded()) / 2 * 2)
        let height = max(2, Int((source.height * scale).rounded()) / 2 * 2)
        return CGSize(width: width, height: height)
    }

    private static func appendLinearRGBA(
        _ pixelBuffer: CVPixelBuffer,
        transform: CGAffineTransform,
        displayBounds: CGRect,
        outputWidth: Int,
        outputHeight: Int,
        to values: inout [Float]
    ) {
        CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(pixelBuffer) else { return }
        let sourceWidth = CVPixelBufferGetWidth(pixelBuffer)
        let sourceHeight = CVPixelBufferGetHeight(pixelBuffer)
        let bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer)
        let inverse = transform.inverted()

        for y in 0..<outputHeight {
            for x in 0..<outputWidth {
                let displayPoint = CGPoint(
                    x: displayBounds.minX + (CGFloat(x) + 0.5) * displayBounds.width / CGFloat(outputWidth),
                    y: displayBounds.minY + (CGFloat(y) + 0.5) * displayBounds.height / CGFloat(outputHeight)
                ).applying(inverse)
                let sourceX = min(max(Int(floor(displayPoint.x)), 0), sourceWidth - 1)
                let sourceY = min(max(Int(floor(displayPoint.y)), 0), sourceHeight - 1)
                let pixel = base.advanced(by: sourceY * bytesPerRow + sourceX * 4).assumingMemoryBound(to: UInt8.self)
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
