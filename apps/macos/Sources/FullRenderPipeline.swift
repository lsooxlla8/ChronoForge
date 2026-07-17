import Foundation

enum FullRenderPipeline {
    static func export(
        source: DecodedProxy,
        effects: [EffectNode],
        mediaPool: [DecodedProxy] = [],
        audioMode: AudioMode,
        to destination: URL,
        progress: @escaping @Sendable (Double, String) -> Void
    ) async throws {
        let cacheRoot = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first!
            .appendingPathComponent("ChronoForge/Full", isDirectory: true)
        let sourceKey = ProxyCache.key(source: source.mediaSource, input: source.tensor, effects: [])
        let graphDrivers = effects.compactMap { effect in
            effect.driverMediaID.flatMap { id in mediaPool.first(where: { $0.id == id })?.mediaSource }
        }
        let graphKey = ProxyCache.key(source: source.mediaSource, input: source.tensor, effects: effects, drivers: graphDrivers)
        let sourceDirectory = cacheRoot.appendingPathComponent(sourceKey, isDirectory: true)
        let graphDirectory = cacheRoot.appendingPathComponent(graphKey, isDirectory: true)
        try FileManager.default.createDirectory(at: sourceDirectory, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: graphDirectory, withIntermediateDirectories: true)

        let decodedMetadataURL = sourceDirectory.appendingPathComponent("input.json")
        var decoded: DiskTensorData
        if let cached = loadMetadata(decodedMetadataURL), cached.isValidOnDisk() {
            decoded = cached
            try? FileManager.default.setAttributes([.modificationDate: Date()], ofItemAtPath: sourceDirectory.path)
            progress(0.15, "Using decoded source cache")
        } else {
            decoded = try await MediaSourceDecoder.decodeFull(
                from: source.mediaSource,
                destinationURL: sourceDirectory.appendingPathComponent("input.raw")
            ) { fraction, stage in progress(fraction * 0.15, stage) }
            try saveMetadata(decoded, to: decodedMetadataURL)
        }

        let driverIDs = Set(effects.compactMap { $0.enabled && $0.kind.requiresDriver ? $0.driverMediaID : nil })
        var decodedDrivers: [UUID: DiskTensorData] = [:]
        let driverList = driverIDs.compactMap { id in mediaPool.first { $0.id == id } }
        for (index, driver) in driverList.enumerated() {
            if driver.id == source.id {
                decodedDrivers[driver.id] = decoded
                continue
            }
            let key = ProxyCache.key(source: driver.mediaSource, input: driver.tensor, effects: [])
            let directory = cacheRoot.appendingPathComponent(key, isDirectory: true)
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let metadataURL = directory.appendingPathComponent("input.json")
            let start = 0.15 + 0.10 * Double(index) / Double(max(1, driverList.count))
            let span = 0.10 / Double(max(1, driverList.count))
            if let cached = loadMetadata(metadataURL), cached.isValidOnDisk() {
                decodedDrivers[driver.id] = cached
                progress(start + span, "Using driver cache")
            } else {
                let tensor = try await MediaSourceDecoder.decodeFull(
                    from: driver.mediaSource,
                    destinationURL: directory.appendingPathComponent("input.raw")
                ) { fraction, stage in progress(start + fraction * span, stage) }
                try saveMetadata(tensor, to: metadataURL)
                decodedDrivers[driver.id] = tensor
            }
        }
        progress(0.25, driverList.isEmpty ? "Source ready" : "Source and drivers ready")

        let renderMetadataURL = graphDirectory.appendingPathComponent("output.json")
        var rendered: DiskTensorData
        if let cached = loadMetadata(renderMetadataURL), cached.isValidOnDisk() {
            rendered = cached
            try? FileManager.default.setAttributes([.modificationDate: Date()], ofItemAtPath: graphDirectory.path)
            progress(0.80, "Using full-resolution render cache")
        } else {
            let scratch = graphDirectory.appendingPathComponent("scratch", isDirectory: true)
            try? FileManager.default.removeItem(at: scratch)
            rendered = try await FileCoreRenderer.render(
                input: decoded,
                effects: effects,
                drivers: decodedDrivers,
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
