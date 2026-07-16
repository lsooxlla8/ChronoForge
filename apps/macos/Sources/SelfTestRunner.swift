import AVFoundation
import CoreVideo
import Foundation

enum SelfTestRunner {
    static func run() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("ChronoForge-SelfTest-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("source.mov")
        let input = root.appendingPathComponent("input.raw")
        let output = root.appendingPathComponent("output.raw")
        let videoOnly = root.appendingPathComponent("video-only.mp4")
        let movie = root.appendingPathComponent("result.mp4")
        try await makeMovie(at: source)
        let cacheRoot = root.appendingPathComponent("cache", isDirectory: true)
        let proxyCache = cacheRoot.appendingPathComponent("Proxy", isDirectory: true)
        try FileManager.default.createDirectory(at: proxyCache, withIntermediateDirectories: true)
        try Data(repeating: 1, count: 4096).write(to: proxyCache.appendingPathComponent("old.cft"))
        try Data(repeating: 2, count: 4096).write(to: proxyCache.appendingPathComponent("new.cft"))
        let cacheManager = CacheManager(root: cacheRoot)
        _ = try await cacheManager.trim(to: 1024)
        guard await cacheManager.size() <= 1024 else {
            throw IntegrationSelfTestError.message("Automatic cache trimming did not enforce its limit")
        }
        let projectURL = root.appendingPathComponent("test.chronoforge")
        let projectSource = DecodedProxy(
            tensor: VideoTensorData(values: [0, 0, 0, 1], frames: 1, height: 1, width: 1, channels: 4, framesPerSecond: 8, duration: 0.125),
            displayName: source.lastPathComponent,
            sourceURL: source,
            sourceSize: CGSize(width: 64, height: 48),
            sourceDuration: 1,
            securityScopedBookmark: nil
        )
        let firstNode = EffectNode.make(.lumaTimeShift)
        let branchA = EffectNode.make(.radialChronoFunnel, inputNodeID: firstNode.id)
        let branchB = EffectNode.make(.temporalPixelSort, inputNodeID: firstNode.id)
        let savedProject = SavedChronoForgeProject(
            source: projectSource,
            effects: [firstNode, branchA, branchB],
            outputNodeID: branchB.id,
            quality: .full,
            proxyQuality: .high,
            spatialPrefilter: .light,
            temporalPrefilter: .strong,
            audioMode: .preserveOriginal
        )
        try ProjectPersistence.save(savedProject, to: projectURL)
        let restoredProject = try ProjectPersistence.load(from: projectURL)
        guard restoredProject.effects == savedProject.effects,
              restoredProject.quality == RenderQuality.full.rawValue,
              restoredProject.proxyQuality == ProxyQuality.high.rawValue,
              restoredProject.spatialPrefilter == PrefilterStrength.light.rawValue,
              restoredProject.temporalPrefilter == PrefilterStrength.strong.rawValue,
              restoredProject.audioMode == AudioMode.preserveOriginal.rawValue,
              restoredProject.outputNodeID == branchB.id,
              restoredProject.effects.last?.inputNodeID == firstNode.id,
              try restoredProject.sourceURL() == source else {
            throw IntegrationSelfTestError.message("Project persistence round-trip failed")
        }
        let legacyURL = root.appendingPathComponent("legacy-v3.chronoforge")
        var legacyObject = try JSONSerialization.jsonObject(with: Data(contentsOf: projectURL)) as! [String: Any]
        legacyObject["version"] = 3
        legacyObject.removeValue(forKey: "media")
        legacyObject.removeValue(forKey: "primaryMediaID")
        try JSONSerialization.data(withJSONObject: legacyObject).write(to: legacyURL)
        let legacyProject = try ProjectPersistence.load(from: legacyURL)
        guard legacyProject.mediaReferences().count == 1,
              try legacyProject.mediaReferences()[0].url() == source else {
            throw IntegrationSelfTestError.message("Version 3 single-media projects did not migrate to the media pool")
        }
        try await MainActor.run {
            let store = ProjectStore()
            guard EffectKind.addableKinds.count == 11,
                  EffectKind.spaceTimeTranspose.title == EffectKind.tensor3DRotation.title,
                  EffectKind.spaceTimeTranspose.title == "Space-Time Transform",
                  EffectKind.singleInputKinds.count == 9,
                  EffectKind.twoInputKinds.count == 2,
                  EffectKind.opticalFlowTimeWarp.symbol == "wind" else {
                throw IntegrationSelfTestError.message("Effect families were not exposed as a homogeneous effect stack")
            }
            store.addEffect(.lumaTimeShift)
            store.addEffect(.tensor3DRotation)
            guard EffectNode.make(.spaceTimeTranspose).options[1] == 1,
                  EffectNode.make(.tensor3DRotation).options[0] == 3,
                  EffectNode.make(.spectralFFTSwap).options[2] == 1,
                  EffectNode.make(.seamlessLoop).values[0] == 15 else {
                throw IntegrationSelfTestError.message("Size-changing effects did not default to Fit Source Size")
            }
            let duplicatedID = store.selectedNodeID!
            store.duplicateEffect(duplicatedID)
            store.moveEffect(from: IndexSet(integer: 2), to: 0)
            for index in store.effects.indices {
                let expectedInput = index == 0 ? nil : store.effects[index - 1].id
                guard store.effects[index].inputNodeID == expectedInput else {
                    throw IntegrationSelfTestError.message("Reordered effects were not reconnected sequentially")
                }
            }
            store.deleteEffect(duplicatedID)
            guard store.effects.count == 2 else {
                throw IntegrationSelfTestError.message("Context deletion did not safely update the stack")
            }
            store.clearEffectStack()
            guard store.effects.isEmpty, store.outputNodeID == nil else {
                throw IntegrationSelfTestError.message("Clear Effect Stack did not clear graph state")
            }
        }
        let proxy = try await VideoDecoder.decodeProxy(from: source)
        guard proxy.tensor.width == 48, proxy.tensor.height == 64, proxy.sourceFrameCount == 8, proxy.sourceFrameCountIsExact,
              proxy.tensor.values.count == proxy.tensor.valueCount else {
            throw IntegrationSelfTestError.message(
                "Proxy metadata mismatch: \(proxy.tensor.width)x\(proxy.tensor.height), frames=\(proxy.sourceFrameCount), exact=\(proxy.sourceFrameCountIsExact)"
            )
        }
        var splicer = EffectNode.make(.dimensionalSplicer)
        splicer.driverMediaID = proxy.id
        let crossProxy = try await CoreRenderer.render(
            input: proxy.tensor,
            effects: [splicer],
            drivers: [proxy.id: proxy.tensor]
        )
        guard crossProxy.frames == proxy.tensor.frames,
              crossProxy.width == proxy.tensor.width,
              crossProxy.height == proxy.tensor.height else {
            throw IntegrationSelfTestError.message("Cross-tensor proxy path returned invalid dimensions")
        }
        var loop = EffectNode.make(.seamlessLoop)
        loop.values[0] = 2
        let loopProxy = try await CoreRenderer.render(input: proxy.tensor, effects: [loop])
        guard loopProxy.frames == proxy.tensor.frames - 2,
              loopProxy.width == proxy.tensor.width,
              loopProxy.height == proxy.tensor.height else {
            throw IntegrationSelfTestError.message("Seamless Loop proxy path returned invalid dimensions")
        }
        let proxyFFT = try await CoreRenderer.render(input: proxy.tensor, effects: [EffectNode.make(.spectralFFTSwap)])
        guard proxyFFT.frames == proxy.tensor.frames, proxyFFT.width == proxy.tensor.width, proxyFFT.height == proxy.tensor.height else {
            throw IntegrationSelfTestError.message("Disk-backed proxy FFT did not preserve fitted dimensions")
        }
        let wrappedPolar = try await CoreRenderer.render(input: proxy.tensor, effects: [EffectNode.make(.radialChronoFunnel)])
        guard wrappedPolar.frames == proxy.tensor.frames,
              wrappedPolar.width == proxy.tensor.width,
              wrappedPolar.height == proxy.tensor.height else {
            throw IntegrationSelfTestError.message("Wrapped Polar Time Warp did not preserve proxy dimensions")
        }
        let filteredProxy = try await CoreRenderer.render(
            input: proxy.tensor,
            effects: [.makePrefilter(spatial: .light, temporal: .strong)]
        )
        guard filteredProxy.frames == proxy.tensor.frames,
              filteredProxy.width == proxy.tensor.width,
              filteredProxy.height == proxy.tensor.height else {
            throw IntegrationSelfTestError.message("Selective prefilter did not preserve proxy dimensions")
        }
        let decoded = try await FullVideoDecoder.decode(sourceURL: source, destinationURL: input) { _, _ in }
        guard decoded.frames == 8, decoded.width == 48, decoded.height == 64, decoded.isValidOnDisk() else {
            throw IntegrationSelfTestError.message("Full decoder returned an invalid disk tensor")
        }
        let effect = EffectNode.make(.lumaTimeShift)
        let fft = EffectNode.make(.spectralFFTSwap, inputNodeID: effect.id)
        let prefilter = EffectNode.makePrefilter(spatial: .light, temporal: .light)
        let rendered = try await FileCoreRenderer.render(
            input: decoded,
            effects: [effect, fft, prefilter],
            outputURL: output,
            scratchDirectory: root.appendingPathComponent("scratch")
        ) { _, _ in }
        guard rendered.isValidOnDisk() else { throw IntegrationSelfTestError.message("File renderer output is invalid") }
        let crossOutput = root.appendingPathComponent("cross-output.raw")
        let crossRendered = try await FileCoreRenderer.render(
            input: decoded,
            effects: [splicer],
            drivers: [proxy.id: decoded],
            outputURL: crossOutput,
            scratchDirectory: root.appendingPathComponent("cross-scratch")
        ) { _, _ in }
        guard crossRendered.isValidOnDisk(), crossRendered.frames == decoded.frames else {
            throw IntegrationSelfTestError.message("Cross-tensor full render path returned an invalid disk tensor")
        }
        try await FullVideoExporter.export(rendered, to: videoOnly) { _, _ in }
        try await MediaMuxer.addOriginalAudio(videoURL: videoOnly, sourceURL: source, destinationURL: movie)
        let resultAsset = AVURLAsset(url: movie)
        guard try await !resultAsset.loadTracks(withMediaType: .video).isEmpty,
              (try movie.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0) > 0 else {
            throw IntegrationSelfTestError.message("Full exporter did not create a playable MP4")
        }
    }

    private static func makeMovie(at url: URL) async throws {
        let writer = try AVAssetWriter(outputURL: url, fileType: .mov)
        let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
            AVVideoCodecKey: AVVideoCodecType.h264,
            AVVideoWidthKey: 64,
            AVVideoHeightKey: 48,
        ])
        input.transform = CGAffineTransform(a: 0, b: 1, c: -1, d: 0, tx: 48, ty: 0)
        let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: 64,
            kCVPixelBufferHeightKey as String: 48,
        ])
        guard writer.canAdd(input) else { throw IntegrationSelfTestError.message("Cannot configure source writer") }
        writer.add(input)
        guard writer.startWriting() else { throw writer.error ?? IntegrationSelfTestError.message("Source writer failed") }
        writer.startSession(atSourceTime: .zero)
        for frame in 0..<8 {
            while !input.isReadyForMoreMediaData { try await Task.sleep(for: .milliseconds(2)) }
            guard let pool = adaptor.pixelBufferPool else { throw IntegrationSelfTestError.message("No source pixel pool") }
            var optional: CVPixelBuffer?
            guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &optional) == kCVReturnSuccess,
                  let buffer = optional else { throw IntegrationSelfTestError.message("Cannot create source frame") }
            fill(buffer, frame: frame)
            guard adaptor.append(buffer, withPresentationTime: CMTime(value: CMTimeValue(frame), timescale: 8)) else {
                throw writer.error ?? IntegrationSelfTestError.message("Cannot append source frame")
            }
        }
        input.markAsFinished()
        await writer.finishWriting()
        guard writer.status == .completed else { throw writer.error ?? IntegrationSelfTestError.message("Source writer did not finish") }
    }

    private static func fill(_ buffer: CVPixelBuffer, frame: Int) {
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        guard let base = CVPixelBufferGetBaseAddress(buffer) else { return }
        let rowBytes = CVPixelBufferGetBytesPerRow(buffer)
        for y in 0..<48 {
            let row = base.advanced(by: y * rowBytes).assumingMemoryBound(to: UInt8.self)
            for x in 0..<64 {
                let pixel = row.advanced(by: x * 4)
                pixel[0] = UInt8((x * 4 + frame * 8) % 255)
                pixel[1] = UInt8((y * 5 + frame * 12) % 255)
                pixel[2] = UInt8((frame * 30) % 255)
                pixel[3] = 255
            }
        }
    }
}

private enum IntegrationSelfTestError: Error {
    case message(String)
}
