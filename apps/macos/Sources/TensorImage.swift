import CoreGraphics
import Foundation

enum TensorImage {
    static func make(from tensor: VideoTensorData, frame: Int) -> CGImage? {
        guard tensor.channels >= 3, tensor.frames > 0 else { return nil }
        let source = tensor.frameValues(at: frame)
        var bytes = [UInt8](repeating: 0, count: tensor.width * tensor.height * 4)
        source.withContiguousStorageIfAvailable { buffer in
            for pixel in 0..<(tensor.width * tensor.height) {
                let sourceIndex = pixel * tensor.channels
                let destination = pixel * 4
                let alpha = tensor.channels >= 4 ? max(0, min(1, buffer[sourceIndex + 3])) : 1
                let divisor = alpha > 0.000_01 ? alpha : 1
                bytes[destination] = byte(linearToSRGB(buffer[sourceIndex] / divisor))
                bytes[destination + 1] = byte(linearToSRGB(buffer[sourceIndex + 1] / divisor))
                bytes[destination + 2] = byte(linearToSRGB(buffer[sourceIndex + 2] / divisor))
                bytes[destination + 3] = byte(alpha)
            }
        }
        let data = Data(bytes)
        guard let provider = CGDataProvider(data: data as CFData) else { return nil }
        return CGImage(
            width: tensor.width,
            height: tensor.height,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: tensor.width * 4,
            space: CGColorSpace(name: CGColorSpace.sRGB) ?? CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
            provider: provider,
            decode: nil,
            shouldInterpolate: true,
            intent: .defaultIntent
        )
    }

    private static func linearToSRGB(_ value: Float) -> Float {
        let clamped = max(0, min(1, value))
        return clamped <= 0.0031308 ? 12.92 * clamped : 1.055 * pow(clamped, 1 / 2.4) - 0.055
    }

    private static func byte(_ value: Float) -> UInt8 {
        UInt8(max(0, min(255, Int((value * 255).rounded()))))
    }
}
