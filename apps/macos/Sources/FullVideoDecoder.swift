import AVFoundation
import CoreGraphics
import CoreVideo
import Foundation

enum FullVideoDecoderError: LocalizedError {
    case noTrack
    case invalidDimensions
    case insufficientDisk(required: Int64, available: Int64)
    case noFrames

    var errorDescription: String? {
        switch self {
        case .noTrack: "The source has no readable video track."
        case .invalidDimensions: "The source video has invalid dimensions."
        case .insufficientDisk(let required, let available):
            "Full render needs about \(ByteCountFormatter.string(fromByteCount: required, countStyle: .file)); only \(ByteCountFormatter.string(fromByteCount: available, countStyle: .file)) is available."
        case .noFrames: "No full-resolution frames could be decoded."
        }
    }
}

enum FullVideoDecoder {
    static func decode(
        sourceURL: URL,
        destinationURL: URL,
        progress: @escaping @Sendable (Double, String) -> Void
    ) async throws -> DiskTensorData {
        let asset = AVURLAsset(url: sourceURL)
        guard let track = try await asset.loadTracks(withMediaType: .video).first else { throw FullVideoDecoderError.noTrack }
        let durationTime = try await asset.load(.duration)
        let duration = max(0.001, CMTimeGetSeconds(durationTime))
        let naturalSize = try await track.load(.naturalSize)
        let transform = try await track.load(.preferredTransform)
        let nominalFPS = max(1, Double(try await track.load(.nominalFrameRate)))
        let bounds = CGRect(origin: .zero, size: naturalSize).applying(transform).standardized
        let width = Int(bounds.width.rounded())
        let height = Int(bounds.height.rounded())
        guard width > 0, height > 0 else { throw FullVideoDecoderError.invalidDimensions }

        try FileManager.default.createDirectory(at: destinationURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        let estimatedFrames = max(1, Int(ceil(duration * max(nominalFPS, 60))))
        let estimatedBytes = Int64(estimatedFrames) * Int64(width) * Int64(height) * 4 * Int64(MemoryLayout<Float>.stride)
        let volumeValues = try destinationURL.deletingLastPathComponent().resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        let available = volumeValues.volumeAvailableCapacityForImportantUsage ?? 0
        let reserve: Int64 = 512 * 1024 * 1024
        guard available > 0, estimatedBytes <= available - min(available, reserve) else {
            throw FullVideoDecoderError.insufficientDisk(required: estimatedBytes + reserve, available: available)
        }

        _ = FileManager.default.createFile(atPath: destinationURL.path, contents: nil)
        let handle = try FileHandle(forWritingTo: destinationURL)
        var succeeded = false
        defer {
            try? handle.close()
            if !succeeded { try? FileManager.default.removeItem(at: destinationURL) }
        }

        let reader = try AVAssetReader(asset: asset)
        let output = AVAssetReaderTrackOutput(track: track, outputSettings: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
        ])
        output.alwaysCopiesSampleData = false
        guard reader.canAdd(output) else { throw VideoDecoderError.readerCouldNotStart }
        reader.add(output)
        guard reader.startReading() else { throw reader.error ?? VideoDecoderError.readerCouldNotStart }

        var timestamps: [Double] = []
        var firstTimestamp: Double?
        while reader.status == .reading, let sample = output.copyNextSampleBuffer() {
            try Task.checkCancellation()
            guard let pixelBuffer = CMSampleBufferGetImageBuffer(sample) else { continue }
            let rawTime = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample))
            let origin = firstTimestamp ?? rawTime
            firstTimestamp = origin
            let timestamp = max(0, rawTime - origin)
            timestamps.append(timestamp)
            try appendOrientedLinearRGBA(
                pixelBuffer,
                transform: transform,
                displayBounds: bounds,
                outputWidth: width,
                outputHeight: height,
                to: handle
            )
            progress(min(1, rawTime / duration), "Decoding full-resolution frame \(timestamps.count)")
            if timestamps.count % 32 == 0 {
                let currentAvailable = try destinationURL.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
                    .volumeAvailableCapacityForImportantUsage ?? 0
                let frameBytes = Int64(width) * Int64(height) * 4 * Int64(MemoryLayout<Float>.stride)
                if currentAvailable < frameBytes + reserve {
                    reader.cancelReading()
                    throw FullVideoDecoderError.insufficientDisk(required: frameBytes + reserve, available: currentAvailable)
                }
            }
        }
        guard reader.status == .completed || reader.status == .cancelled else {
            throw reader.error ?? VideoDecoderError.readerCouldNotStart
        }
        guard !timestamps.isEmpty else { throw FullVideoDecoderError.noFrames }
        try handle.synchronize()
        try handle.close()
        succeeded = true

        let measuredDuration = timestamps.last ?? 0
        let measuredFPS = timestamps.count > 1 && measuredDuration > 0
            ? Double(timestamps.count - 1) / measuredDuration
            : nominalFPS
        return DiskTensorData(
            fileURL: destinationURL,
            frames: timestamps.count,
            height: height,
            width: width,
            channels: 4,
            framesPerSecond: measuredFPS,
            duration: measuredDuration + 1 / measuredFPS,
            timestamps: timestamps
        )
    }

    private static func appendOrientedLinearRGBA(
        _ pixelBuffer: CVPixelBuffer,
        transform: CGAffineTransform,
        displayBounds: CGRect,
        outputWidth: Int,
        outputHeight: Int,
        to handle: FileHandle
    ) throws {
        CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(pixelBuffer) else { throw VideoDecoderError.pixelBufferUnavailable }
        let sourceWidth = CVPixelBufferGetWidth(pixelBuffer)
        let sourceHeight = CVPixelBufferGetHeight(pixelBuffer)
        let rowBytes = CVPixelBufferGetBytesPerRow(pixelBuffer)
        let inverse = transform.inverted()
        let rowsPerChunk = max(1, min(16, 1_048_576 / max(1, outputWidth * 4 * MemoryLayout<Float>.stride)))
        var values = [Float](repeating: 0, count: outputWidth * rowsPerChunk * 4)

        for firstY in stride(from: 0, to: outputHeight, by: rowsPerChunk) {
            let rowCount = min(rowsPerChunk, outputHeight - firstY)
            for localY in 0..<rowCount {
                let destinationY = firstY + localY
                for destinationX in 0..<outputWidth {
                    let displayPoint = CGPoint(
                        x: CGFloat(destinationX) + displayBounds.minX + 0.5,
                        y: CGFloat(destinationY) + displayBounds.minY + 0.5
                    ).applying(inverse)
                    let sourceX = min(max(Int(floor(displayPoint.x)), 0), sourceWidth - 1)
                    let sourceY = min(max(Int(floor(displayPoint.y)), 0), sourceHeight - 1)
                    let pixel = base.advanced(by: sourceY * rowBytes + sourceX * 4).assumingMemoryBound(to: UInt8.self)
                    let index = (localY * outputWidth + destinationX) * 4
                    let alpha = Float(pixel[3]) / 255
                    values[index] = srgbToLinear(Float(pixel[2]) / 255) * alpha
                    values[index + 1] = srgbToLinear(Float(pixel[1]) / 255) * alpha
                    values[index + 2] = srgbToLinear(Float(pixel[0]) / 255) * alpha
                    values[index + 3] = alpha
                }
            }
            let byteCount = rowCount * outputWidth * 4 * MemoryLayout<Float>.stride
            try values.withUnsafeBytes { bytes in
                try handle.write(contentsOf: Data(bytes: bytes.baseAddress!, count: byteCount))
            }
        }
    }

    private static func srgbToLinear(_ value: Float) -> Float {
        value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4)
    }
}
