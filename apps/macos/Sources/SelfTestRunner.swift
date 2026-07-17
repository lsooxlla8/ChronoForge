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
        let snapshotURL = root.appendingPathComponent("recovery.json")
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
        let savedSnapshot = SessionRecoverySnapshot(
            source: projectSource,
            effects: [firstNode, branchA, branchB],
            outputNodeID: branchB.id,
            proxyQuality: .high,
            spatialPrefilter: .light,
            temporalPrefilter: .strong,
            audioMode: .preserveOriginal
        )
        try SessionRecoveryStore.save(savedSnapshot, to: snapshotURL)
        let restoredSnapshot = try SessionRecoveryStore.load(from: snapshotURL)
        guard restoredSnapshot.effects == savedSnapshot.effects,
              restoredSnapshot.proxyQuality == ProxyQuality.high.rawValue,
              restoredSnapshot.spatialPrefilter == PrefilterStrength.light.rawValue,
              restoredSnapshot.temporalPrefilter == PrefilterStrength.strong.rawValue,
              restoredSnapshot.audioMode == AudioMode.preserveOriginal.rawValue,
              restoredSnapshot.outputNodeID == branchB.id,
              restoredSnapshot.effects.last?.inputNodeID == firstNode.id,
              try restoredSnapshot.sourceURL() == source else {
            throw IntegrationSelfTestError.message("Session recovery snapshot round-trip failed")
        }
        let driverSource: DecodedProxy = {
            var value = projectSource
            value.id = UUID()
            return value
        }()
        let deterministicStackA = try RandomStackGenerator.generate(
            mediaPool: [projectSource, driverSource], primaryMediaID: projectSource.id, seed: 0xC0FFEE)
        let deterministicStackB = try RandomStackGenerator.generate(
            mediaPool: [projectSource, driverSource], primaryMediaID: projectSource.id, seed: 0xC0FFEE)
        guard deterministicStackA == deterministicStackB else {
            throw IntegrationSelfTestError.message("Random Stack was not deterministic for an injected seed")
        }
        var lengthCounts = [0, 0, 0, 0]
        for seed in 0..<600 {
            let stack = try RandomStackGenerator.generate(
                mediaPool: [projectSource, driverSource], primaryMediaID: projectSource.id, seed: UInt64(seed))
            lengthCounts[stack.count] += 1
            guard stack.filter({ $0.kind.definition.costClass == .global }).count <= 1,
                  stack.dropLast().allSatisfy({ $0.kind != .seamlessLoop }),
                  stack.allSatisfy({ $0.amount == 1 || $0.supportsAmount }) else {
                throw IntegrationSelfTestError.message("Random Stack generated an incompatible combination")
            }
        }
        guard (160...260).contains(lengthCounts[1]),
              (220...320).contains(lengthCounts[2]),
              (80...160).contains(lengthCounts[3]) else {
            throw IntegrationSelfTestError.message("Random Stack length weights drifted outside their expected ranges")
        }
        try await MainActor.run {
            let store = SessionStore()
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
            store.undo()
            guard store.effects.count == 2, store.outputNodeID == store.effects.last?.id else {
                throw IntegrationSelfTestError.message("Undo did not restore a structural creative-state change")
            }
            store.redo()
            guard store.effects.isEmpty else {
                throw IntegrationSelfTestError.message("Redo did not reapply a structural creative-state change")
            }
            store.addEffect(.lumaTimeShift)
            let editedID = store.selectedNodeID!
            store.beginContinuousEffectEdit()
            var edited = store.effect(withID: editedID)!
            edited.amount = 0.7
            store.updateEffect(edited)
            edited.amount = 0.4
            store.updateEffect(edited)
            store.endContinuousEffectEdit()
            store.undo()
            guard store.effect(withID: editedID)?.amount == 1 else {
                throw IntegrationSelfTestError.message("A continuous slider gesture was not grouped into one Undo operation")
            }
            store.redo()
            guard store.effect(withID: editedID)?.amount == 0.4 else {
                throw IntegrationSelfTestError.message("Redo did not restore the grouped slider result")
            }
            store.source = projectSource
            store.mediaPool = [projectSource, driverSource]
            store.output = projectSource.tensor
            let beforeRandom = store.effects
            store.replaceWithRandomStack(seed: 0xC0FFEE)
            guard 1...3 ~= store.effects.count else {
                throw IntegrationSelfTestError.message("Random Stack did not replace the current effect stack")
            }
            store.undo()
            guard store.effects == beforeRandom else {
                throw IntegrationSelfTestError.message("Random Stack was not reversible as one Undo operation")
            }
            store.removeMedia(projectSource.id)
            guard store.source?.id == driverSource.id, store.mediaPool.map(\.id) == [driverSource.id] else {
                throw IntegrationSelfTestError.message("Media removal did not clear the active source")
            }
            store.undo()
            guard store.source?.id == projectSource.id, store.mediaPool.map(\.id) == [projectSource.id, driverSource.id] else {
                throw IntegrationSelfTestError.message("Undo did not restore a decoded proxy that remained in memory")
            }
            store.startFreshSession()
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
