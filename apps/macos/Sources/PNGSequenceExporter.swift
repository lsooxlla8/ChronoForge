import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

enum PNGSequenceExporterError: LocalizedError {
    case destinationNotEmpty
    case cannotCreateFrame(Int)

    var errorDescription: String? {
        switch self {
        case .destinationNotEmpty:
            "Choose a new or empty folder for PNG sequence export. Existing files were not changed."
        case .cannotCreateFrame(let frame):
            "ChronoForge could not write PNG frame \(frame). Any earlier frames remain in the destination folder."
        }
    }
}

enum PNGSequenceExporter {
    static func export(
        _ tensor: DiskTensorData,
        to directoryURL: URL,
        progress: @escaping @Sendable (Double, String) -> Void
    ) async throws {
        try await Task.detached(priority: .userInitiated) {
            guard tensor.isValidOnDisk() else { throw CoreRendererError.invalidOutput }
            try FileManager.default.createDirectory(at: directoryURL, withIntermediateDirectories: true)
            let existing = try FileManager.default.contentsOfDirectory(
                at: directoryURL,
                includingPropertiesForKeys: nil,
                options: [.skipsHiddenFiles]
            )
            guard existing.isEmpty else { throw PNGSequenceExporterError.destinationNotEmpty }
            let mapped = try Data(contentsOf: tensor.fileURL, options: .mappedIfSafe)
            for frame in 0..<tensor.frames {
                try Task.checkCancellation()
                let rgba = mapped.withUnsafeBytes { raw in
                    makeRGBA8(
                        tensor: tensor,
                        frame: frame,
                        values: raw.bindMemory(to: Float.self)
                    )
                }
                let name = String(format: "ChronoForge_%06d.png", frame + 1)
                let url = directoryURL.appendingPathComponent(name)
                guard writePNG(rgba, width: tensor.width, height: tensor.height, to: url) else {
                    throw PNGSequenceExporterError.cannotCreateFrame(frame + 1)
                }
                progress(
                    Double(frame + 1) / Double(tensor.frames),
                    "Writing PNG frame \(frame + 1) of \(tensor.frames)"
                )
            }
        }.value
    }

    private static func makeRGBA8(
        tensor: DiskTensorData,
        frame: Int,
        values: UnsafeBufferPointer<Float>
    ) -> [UInt8] {
        var output = [UInt8](repeating: 0, count: tensor.width * tensor.height * 4)
        let frameOffset = frame * tensor.height * tensor.width * tensor.channels
        for pixel in 0..<(tensor.width * tensor.height) {
            let source = frameOffset + pixel * tensor.channels
            let destination = pixel * 4
            let alpha = tensor.channels >= 4 ? clamp(values[source + 3]) : 1
            let divisor = alpha > 0.000_01 ? alpha : 1
            output[destination] = byte(linearToSRGB(values[source] / divisor))
            output[destination + 1] = byte(linearToSRGB(values[source + 1] / divisor))
            output[destination + 2] = byte(linearToSRGB(values[source + 2] / divisor))
            output[destination + 3] = byte(alpha)
        }
        return output
    }

    private static func writePNG(_ rgba: [UInt8], width: Int, height: Int, to url: URL) -> Bool {
        guard let provider = CGDataProvider(data: Data(rgba) as CFData),
              let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
              let image = CGImage(
                width: width,
                height: height,
                bitsPerComponent: 8,
                bitsPerPixel: 32,
                bytesPerRow: width * 4,
                space: colorSpace,
                bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue | CGBitmapInfo.byteOrder32Big.rawValue),
                provider: provider,
                decode: nil,
                shouldInterpolate: false,
                intent: .defaultIntent
              ),
              let destination = CGImageDestinationCreateWithURL(
                url as CFURL,
                UTType.png.identifier as CFString,
                1,
                nil
              ) else { return false }
        CGImageDestinationAddImage(destination, image, nil)
        return CGImageDestinationFinalize(destination)
    }

    private static func clamp(_ value: Float) -> Float { max(0, min(1, value)) }
    private static func linearToSRGB(_ value: Float) -> Float {
        let value = clamp(value)
        return value <= 0.0031308 ? 12.92 * value : 1.055 * pow(value, 1 / 2.4) - 0.055
    }
    private static func byte(_ value: Float) -> UInt8 {
        UInt8(max(0, min(255, Int((value * 255).rounded()))))
    }
}
