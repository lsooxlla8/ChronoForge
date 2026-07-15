import Foundation

enum FullRenderPipeline {
    static func export(
        source: DecodedProxy,
        effects: [EffectNode],
        audioMode: AudioMode,
        to destination: URL,
        progress: @escaping @Sendable (Double, String) -> Void
    ) async throws {
        let cacheRoot = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first!
            .appendingPathComponent("ChronoForge/Full", isDirectory: true)
        let sourceKey = ProxyCache.key(source: source.sourceURL, input: source.tensor, effects: [])
        let graphKey = ProxyCache.key(source: source.sourceURL, input: source.tensor, effects: effects)
        let sourceDirectory = cacheRoot.appendingPathComponent(sourceKey, isDirectory: true)
        let graphDirectory = cacheRoot.appendingPathComponent(graphKey, isDirectory: true)
        try FileManager.default.createDirectory(at: sourceDirectory, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: graphDirectory, withIntermediateDirectories: true)

        let decodedMetadataURL = sourceDirectory.appendingPathComponent("input.json")
        var decoded: DiskTensorData
        if let cached = loadMetadata(decodedMetadataURL), cached.isValidOnDisk() {
            decoded = cached
            progress(0.25, "Using decoded source cache")
        } else {
            decoded = try await FullVideoDecoder.decode(
                sourceURL: source.sourceURL,
                destinationURL: sourceDirectory.appendingPathComponent("input.raw")
            ) { fraction, stage in progress(fraction * 0.25, stage) }
            try saveMetadata(decoded, to: decodedMetadataURL)
        }

        let renderMetadataURL = graphDirectory.appendingPathComponent("output.json")
        var rendered: DiskTensorData
        if let cached = loadMetadata(renderMetadataURL), cached.isValidOnDisk() {
            rendered = cached
            progress(0.80, "Using full-resolution render cache")
        } else {
            let scratch = graphDirectory.appendingPathComponent("scratch", isDirectory: true)
            try? FileManager.default.removeItem(at: scratch)
            rendered = try await FileCoreRenderer.render(
                input: decoded,
                effects: effects,
                outputURL: graphDirectory.appendingPathComponent("output.raw"),
                scratchDirectory: scratch
            ) { fraction, stage in progress(0.25 + fraction * 0.55, stage) }
            try saveMetadata(rendered, to: renderMetadataURL)
            try? FileManager.default.removeItem(at: scratch)
        }

        let encodedDestination = audioMode == .preserveOriginal
            ? graphDirectory.appendingPathComponent("video-only-\(UUID().uuidString).mp4")
            : destination
        try await FullVideoExporter.export(rendered, to: encodedDestination) { fraction, stage in
            progress(0.80 + fraction * 0.20, stage)
        }
        if audioMode == .preserveOriginal {
            progress(0.99, "Muxing original audio")
            try await MediaMuxer.addOriginalAudio(videoURL: encodedDestination, sourceURL: source.sourceURL, destinationURL: destination)
        }
        progress(1, "Full-quality export complete")
    }

    private static func loadMetadata(_ url: URL) -> DiskTensorData? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        return try? JSONDecoder().decode(DiskTensorData.self, from: data)
    }

    private static func saveMetadata(_ tensor: DiskTensorData, to url: URL) throws {
        try JSONEncoder().encode(tensor).write(to: url, options: .atomic)
    }
}
