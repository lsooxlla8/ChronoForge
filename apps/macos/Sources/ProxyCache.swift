import CryptoKit
import Foundation

actor ProxyCache {
    static let shared = ProxyCache()

    private struct Header: Codable {
        var frames: Int
        var height: Int
        var width: Int
        var channels: Int
        var framesPerSecond: Double
        var duration: Double
    }

    private let root: URL

    init() {
        let support = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first!
        root = support.appendingPathComponent("ChronoForge/Proxy", isDirectory: true)
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    nonisolated static func key(source: URL, input: VideoTensorData, effects: [EffectNode]) -> String {
        struct Signature: Encodable {
            var sourcePath: String
            var sourceSize: UInt64
            var modified: TimeInterval
            var frames: Int
            var height: Int
            var width: Int
            var effects: [EffectNode]
            var engineVersion: String
        }
        let attributes = try? FileManager.default.attributesOfItem(atPath: source.path)
        let signature = Signature(
            sourcePath: source.standardizedFileURL.path,
            sourceSize: (attributes?[.size] as? NSNumber)?.uint64Value ?? 0,
            modified: (attributes?[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0,
            frames: input.frames,
            height: input.height,
            width: input.width,
            effects: effects,
            engineVersion: "0.4.0"
        )
        let data = (try? JSONEncoder().encode(signature)) ?? Data()
        return SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    func load(key: String) -> VideoTensorData? {
        let url = root.appendingPathComponent(key).appendingPathExtension("cft")
        guard let data = try? Data(contentsOf: url), data.count >= 8 else { return nil }
        let headerLength = data.prefix(8).withUnsafeBytes { bytes in
            UInt64(littleEndian: bytes.loadUnaligned(as: UInt64.self))
        }
        guard headerLength <= UInt64(data.count - 8) else { return nil }
        let headerStart = 8
        let headerEnd = headerStart + Int(headerLength)
        guard let header = try? JSONDecoder().decode(Header.self, from: data[headerStart..<headerEnd]) else { return nil }
        let valueCount = header.frames * header.height * header.width * header.channels
        guard valueCount > 0, data.count - headerEnd == valueCount * MemoryLayout<Float>.stride else { return nil }
        var values = [Float](repeating: 0, count: valueCount)
        _ = values.withUnsafeMutableBytes { destination in
            data.copyBytes(to: destination, from: headerEnd..<data.count)
        }
        try? FileManager.default.setAttributes([.modificationDate: Date()], ofItemAtPath: url.path)
        return VideoTensorData(
            values: values,
            frames: header.frames,
            height: header.height,
            width: header.width,
            channels: header.channels,
            framesPerSecond: header.framesPerSecond,
            duration: header.duration
        )
    }

    func store(_ tensor: VideoTensorData, key: String) throws {
        let header = Header(
            frames: tensor.frames,
            height: tensor.height,
            width: tensor.width,
            channels: tensor.channels,
            framesPerSecond: tensor.framesPerSecond,
            duration: tensor.duration
        )
        let headerData = try JSONEncoder().encode(header)
        var headerLength = UInt64(headerData.count).littleEndian
        var data = Data(bytes: &headerLength, count: MemoryLayout<UInt64>.stride)
        data.append(headerData)
        tensor.values.withUnsafeBytes { data.append(contentsOf: $0) }
        try data.write(to: root.appendingPathComponent(key).appendingPathExtension("cft"), options: .atomic)
    }
}
