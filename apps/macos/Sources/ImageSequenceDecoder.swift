import CoreGraphics
import Foundation
import ImageIO

struct FrameSequenceInspection: Sendable {
    let frameNames: [String]
    let width: Int
    let height: Int
    let missingFrameNumbers: [Int]

    var frameCount: Int { frameNames.count }
}

enum ImageSequenceError: LocalizedError {
    case noPNGFiles
    case unreadableFrame(String)
    case incompatibleDimensions(name: String, expectedWidth: Int, expectedHeight: Int, actualWidth: Int, actualHeight: Int)
    case invalidFramesPerSecond
    case insufficientDisk(required: Int64, available: Int64)

    var errorDescription: String? {
        switch self {
        case .noPNGFiles:
            "The selected folder contains no PNG files."
        case .unreadableFrame(let name):
            "ChronoForge could not decode PNG frame \(name)."
        case .incompatibleDimensions(let name, let expectedWidth, let expectedHeight, let actualWidth, let actualHeight):
            "PNG frame \(name) is \(actualWidth) × \(actualHeight); expected \(expectedWidth) × \(expectedHeight)."
        case .invalidFramesPerSecond:
            "Image sequence FPS must be greater than zero."
        case .insufficientDisk(let required, let available):
            "Full sequence decode needs about \(ByteCountFormatter.string(fromByteCount: required, countStyle: .file)); only \(ByteCountFormatter.string(fromByteCount: available, countStyle: .file)) is available."
        }
    }
}

enum FrameSequenceDiscovery {
    static func inspect(directoryURL: URL) throws -> FrameSequenceInspection {
        let urls = try FileManager.default.contentsOfDirectory(
            at: directoryURL,
            includingPropertiesForKeys: [.isRegularFileKey],
            options: [.skipsHiddenFiles]
        )
        .filter { url in
            url.pathExtension.lowercased() == "png"
                && ((try? url.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) ?? false)
        }
        .sorted { lhs, rhs in
            lhs.lastPathComponent.localizedStandardCompare(rhs.lastPathComponent) == .orderedAscending
        }
        guard !urls.isEmpty else { throw ImageSequenceError.noPNGFiles }

        var expectedSize: (width: Int, height: Int)?
        for url in urls {
            let size = try ImageFrameDecoder.orientedPixelSize(url: url)
            if let expectedSize, size != expectedSize {
                throw ImageSequenceError.incompatibleDimensions(
                    name: url.lastPathComponent,
                    expectedWidth: expectedSize.width,
                    expectedHeight: expectedSize.height,
                    actualWidth: size.width,
                    actualHeight: size.height
                )
            }
            expectedSize = size
        }
        let size = expectedSize!
        return FrameSequenceInspection(
            frameNames: urls.map(\.lastPathComponent),
            width: size.width,
            height: size.height,
            missingFrameNumbers: missingNumbers(in: urls.map { $0.deletingPathExtension().lastPathComponent })
        )
    }

    private static func missingNumbers(in names: [String]) -> [Int] {
        let numbers = names.compactMap(lastNumber)
        guard numbers.count == names.count, let first = numbers.first, let last = numbers.last, last >= first else { return [] }
        let present = Set(numbers)
        return Array(first...last).filter { !present.contains($0) }
    }

    private static func lastNumber(in name: String) -> Int? {
        var digits = ""
        var latest: String?
        for character in name {
            if character.isNumber {
                digits.append(character)
            } else if !digits.isEmpty {
                latest = digits
                digits = ""
            }
        }
        if !digits.isEmpty { latest = digits }
        return latest.flatMap(Int.init)
    }
}

enum ImageSequenceDecoder {
    static func decodeProxy(from source: FrameSequenceSource, quality: ProxyQuality) async throws -> DecodedProxy {
        try await Task.detached(priority: .userInitiated) {
            guard source.framesPerSecond > 0 else { throw ImageSequenceError.invalidFramesPerSecond }
            let inspection = try FrameSequenceDiscovery.inspect(directoryURL: source.directoryURL)
            guard inspection.frameNames == source.frameNames else {
                throw ImageSequenceError.unreadableFrame("sequence contents changed; import it again")
            }
            let maximumFrames = 180
            let maximumWidth = quality == .standard ? 320 : 480
            let maximumHeight = quality == .standard ? 180 : 270
            // Match movie proxies: High preserves common 24/25/30 fps
            // sources, while the frame-count limit caps long sequences.
            let maximumFPS = quality == .standard ? 10.0 : 30.0
            let duration = Double(inspection.frameCount) / source.framesPerSecond
            let proxyFPS = min(source.framesPerSecond, maximumFPS, Double(maximumFrames) / max(duration, 0.001))
            let size = proxySize(
                width: inspection.width,
                height: inspection.height,
                maximumWidth: maximumWidth,
                maximumHeight: maximumHeight
            )
            let selectedIndices = sampledIndices(
                frameCount: inspection.frameCount,
                sourceFPS: source.framesPerSecond,
                proxyFPS: proxyFPS,
                maximumFrames: maximumFrames
            )
            var values: [Float] = []
            values.reserveCapacity(selectedIndices.count * size.width * size.height * 4)
            for index in selectedIndices {
                try Task.checkCancellation()
                let url = source.directoryURL.appendingPathComponent(source.frameNames[index])
                values.append(contentsOf: try ImageFrameDecoder.linearPremultipliedRGBA(
                    url: url,
                    outputWidth: size.width,
                    outputHeight: size.height
                ))
            }
            let mediaSource = MediaSource.frameSequence(source)
            return DecodedProxy(
                tensor: VideoTensorData(
                    values: values,
                    frames: selectedIndices.count,
                    height: size.height,
                    width: size.width,
                    channels: 4,
                    framesPerSecond: proxyFPS,
                    duration: Double(selectedIndices.count) / proxyFPS
                ),
                displayName: source.directoryURL.lastPathComponent,
                mediaSource: mediaSource,
                sourceSize: CGSize(width: inspection.width, height: inspection.height),
                sourceDuration: duration,
                sourceFrameCount: inspection.frameCount,
                sourceFrameCountIsExact: true
            )
        }.value
    }

    static func decodeFull(
        from source: FrameSequenceSource,
        destinationURL: URL,
        progress: @escaping @Sendable (Double, String) -> Void
    ) async throws -> DiskTensorData {
        try await Task.detached(priority: .userInitiated) {
            guard source.framesPerSecond > 0 else { throw ImageSequenceError.invalidFramesPerSecond }
            let inspection = try FrameSequenceDiscovery.inspect(directoryURL: source.directoryURL)
            guard inspection.frameNames == source.frameNames else {
                throw ImageSequenceError.unreadableFrame("sequence contents changed; import it again")
            }
            let frameBytes = Int64(inspection.width) * Int64(inspection.height) * 4 * Int64(MemoryLayout<Float>.stride)
            let required = frameBytes * Int64(inspection.frameCount)
            let reserve: Int64 = 512 * 1024 * 1024
            let values = try destinationURL.deletingLastPathComponent().resourceValues(
                forKeys: [.volumeAvailableCapacityForImportantUsageKey]
            )
            let available = values.volumeAvailableCapacityForImportantUsage ?? 0
            guard available > 0, required <= available - min(available, reserve) else {
                throw ImageSequenceError.insufficientDisk(required: required + reserve, available: available)
            }
            try FileManager.default.createDirectory(
                at: destinationURL.deletingLastPathComponent(),
                withIntermediateDirectories: true
            )
            _ = FileManager.default.createFile(atPath: destinationURL.path, contents: nil)
            let handle = try FileHandle(forWritingTo: destinationURL)
            var succeeded = false
            defer {
                try? handle.close()
                if !succeeded { try? FileManager.default.removeItem(at: destinationURL) }
            }
            for (index, name) in source.frameNames.enumerated() {
                try Task.checkCancellation()
                let frame = try ImageFrameDecoder.linearPremultipliedRGBA(
                    url: source.directoryURL.appendingPathComponent(name),
                    outputWidth: inspection.width,
                    outputHeight: inspection.height
                )
                try frame.withUnsafeBytes { try handle.write(contentsOf: Data($0)) }
                progress(
                    Double(index + 1) / Double(inspection.frameCount),
                    "Decoding PNG frame \(index + 1) of \(inspection.frameCount)"
                )
            }
            try handle.synchronize()
            try handle.close()
            succeeded = true
            return DiskTensorData(
                fileURL: destinationURL,
                frames: inspection.frameCount,
                height: inspection.height,
                width: inspection.width,
                channels: 4,
                framesPerSecond: source.framesPerSecond,
                duration: Double(inspection.frameCount) / source.framesPerSecond,
                timestamps: nil
            )
        }.value
    }

    private static func proxySize(
        width: Int,
        height: Int,
        maximumWidth: Int,
        maximumHeight: Int
    ) -> (width: Int, height: Int) {
        let scale = min(Double(maximumWidth) / Double(width), Double(maximumHeight) / Double(height), 1)
        return (
            max(1, Int((Double(width) * scale).rounded())),
            max(1, Int((Double(height) * scale).rounded()))
        )
    }

    private static func sampledIndices(
        frameCount: Int,
        sourceFPS: Double,
        proxyFPS: Double,
        maximumFrames: Int
    ) -> [Int] {
        let interval = 1 / proxyFPS
        var nextTime = 0.0
        var result: [Int] = []
        for index in 0..<frameCount {
            let time = Double(index) / sourceFPS
            guard time + 0.000_001 >= nextTime else { continue }
            result.append(index)
            nextTime += interval
            if result.count == maximumFrames { break }
        }
        return result
    }
}

enum ImageFrameDecoder {
    static func orientedPixelSize(url: URL) throws -> (width: Int, height: Int) {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              let width = properties[kCGImagePropertyPixelWidth] as? Int,
              let height = properties[kCGImagePropertyPixelHeight] as? Int else {
            throw ImageSequenceError.unreadableFrame(url.lastPathComponent)
        }
        let orientation = (properties[kCGImagePropertyOrientation] as? NSNumber)?.intValue ?? 1
        return [5, 6, 7, 8].contains(orientation) ? (height, width) : (width, height)
    }

    static func linearPremultipliedRGBA(url: URL, outputWidth: Int, outputHeight: Int) throws -> [Float] {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              let image = CGImageSourceCreateThumbnailAtIndex(source, 0, [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceThumbnailMaxPixelSize: max(outputWidth, outputHeight),
                kCGImageSourceShouldCacheImmediately: true,
              ] as CFDictionary) else {
            throw ImageSequenceError.unreadableFrame(url.lastPathComponent)
        }
        let rowBytes = outputWidth * 4
        var bytes = [UInt8](repeating: 0, count: rowBytes * outputHeight)
        guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(
                data: &bytes,
                width: outputWidth,
                height: outputHeight,
                bitsPerComponent: 8,
                bytesPerRow: rowBytes,
                space: colorSpace,
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue
              ) else {
            throw ImageSequenceError.unreadableFrame(url.lastPathComponent)
        }
        context.interpolationQuality = .high
        context.draw(image, in: CGRect(x: 0, y: 0, width: outputWidth, height: outputHeight))
        var values = [Float](repeating: 0, count: outputWidth * outputHeight * 4)
        for pixel in 0..<(outputWidth * outputHeight) {
            let byteIndex = pixel * 4
            let valueIndex = pixel * 4
            let alpha = Float(bytes[byteIndex + 3]) / 255
            let divisor = alpha > 0.000_01 ? alpha : 1
            values[valueIndex] = srgbToLinear(Float(bytes[byteIndex]) / 255 / divisor) * alpha
            values[valueIndex + 1] = srgbToLinear(Float(bytes[byteIndex + 1]) / 255 / divisor) * alpha
            values[valueIndex + 2] = srgbToLinear(Float(bytes[byteIndex + 2]) / 255 / divisor) * alpha
            values[valueIndex + 3] = alpha
        }
        return values
    }

    private static func srgbToLinear(_ value: Float) -> Float {
        let value = max(0, min(1, value))
        return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4)
    }
}
