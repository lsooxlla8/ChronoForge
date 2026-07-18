import AVFoundation
import CoreVideo
import Foundation
import ImageIO
import UniformTypeIdentifiers

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
        let sequenceDirectory = root.appendingPathComponent("sequence", isDirectory: true)
        try FileManager.default.createDirectory(at: sequenceDirectory, withIntermediateDirectories: true)
        let sequenceFrameA = sequenceDirectory.appendingPathComponent("frame_1.png")
        let sequenceFrameB = sequenceDirectory.appendingPathComponent("frame_2.png")
        try Data([1]).write(to: sequenceFrameA)
        try Data([2]).write(to: sequenceFrameB)
        let sequence24 = MediaSource.frameSequence(FrameSequenceSource(
            directoryURL: sequenceDirectory,
            frameNames: [sequenceFrameA.lastPathComponent, sequenceFrameB.lastPathComponent],
            framesPerSecond: 24
        ))
        let sequenceRoundTrip = try JSONDecoder().decode(
            MediaSource.self,
            from: JSONEncoder().encode(sequence24)
        )
        let sequenceKey24 = ProxyCache.key(source: sequence24, input: projectSource.tensor, effects: [])
        var sequence25Source = sequence24
        if case .frameSequence(var sequence) = sequence25Source {
            sequence.framesPerSecond = 25
            sequence25Source = .frameSequence(sequence)
        }
        let sequenceKey25 = ProxyCache.key(source: sequence25Source, input: projectSource.tensor, effects: [])
        try Data([1, 3]).write(to: sequenceFrameA)
        let sequenceChangedKey = ProxyCache.key(source: sequence24, input: projectSource.tensor, effects: [])
        guard sequenceRoundTrip == sequence24,
              sequenceKey24 != sequenceKey25,
              sequenceKey24 != sequenceChangedKey else {
            throw IntegrationSelfTestError.message("MediaSource sequence metadata did not round-trip or invalidate its cache fingerprint")
        }
        let pngDirectory = root.appendingPathComponent("png-fixture", isDirectory: true)
        try FileManager.default.createDirectory(at: pngDirectory, withIntermediateDirectories: true)
        let redFrame = pngDirectory.appendingPathComponent("shot_0001.png")
        let greenFrame = pngDirectory.appendingPathComponent("shot_0003.png")
        try makePNG(at: redFrame, width: 2, height: 2, rgba: [255, 0, 0, 255])
        try makePNG(at: greenFrame, width: 2, height: 2, rgba: [0, 128, 0, 128])
        let inspection = try FrameSequenceDiscovery.inspect(directoryURL: pngDirectory)
        guard inspection.frameNames == [redFrame.lastPathComponent, greenFrame.lastPathComponent],
              inspection.width == 2,
              inspection.height == 2,
              inspection.missingFrameNumbers == [2] else {
            throw IntegrationSelfTestError.message("PNG sequence discovery did not preserve natural order or report numbering gaps")
        }
        let incompatibleFrame = pngDirectory.appendingPathComponent("shot_0004.png")
        try makePNG(at: incompatibleFrame, width: 3, height: 2, rgba: [0, 0, 255, 255])
        do {
            _ = try FrameSequenceDiscovery.inspect(directoryURL: pngDirectory)
            throw IntegrationSelfTestError.message("PNG sequence accepted an incompatible frame size")
        } catch ImageSequenceError.incompatibleDimensions(let name, _, _, let actualWidth, let actualHeight) {
            guard name == incompatibleFrame.lastPathComponent, actualWidth == 3, actualHeight == 2 else {
                throw IntegrationSelfTestError.message("PNG dimension mismatch did not identify the concrete frame")
            }
        }
        try FileManager.default.removeItem(at: incompatibleFrame)
        let pngSequence = FrameSequenceSource(
            directoryURL: pngDirectory,
            frameNames: inspection.frameNames,
            framesPerSecond: 8
        )
        let pngProxy = try await ImageSequenceDecoder.decodeProxy(from: pngSequence, quality: .standard)
        guard pngProxy.tensor.frames == 2,
              pngProxy.tensor.width == 2,
              pngProxy.tensor.height == 2,
              abs(pngProxy.tensor.values[0] - 1) < 0.000_1,
              abs(pngProxy.tensor.values[3] - 1) < 0.000_1,
              abs(pngProxy.tensor.values[16 + 1] - Float(128.0 / 255.0)) < 0.01,
              abs(pngProxy.tensor.values[16 + 3] - Float(128.0 / 255.0)) < 0.01 else {
            throw IntegrationSelfTestError.message("PNG proxy decode did not preserve RGB and premultiplied alpha")
        }
        let pngRaw = root.appendingPathComponent("png-sequence.raw")
        let pngFull = try await ImageSequenceDecoder.decodeFull(
            from: pngSequence,
            destinationURL: pngRaw
        ) { _, _ in }
        let pngMapped = try Data(contentsOf: pngRaw, options: .mappedIfSafe)
        let pngFullValues = pngMapped.withUnsafeBytes { Array($0.bindMemory(to: Float.self)) }
        guard pngFull.isValidOnDisk(),
              pngFull.frames == 2,
              pngFull.framesPerSecond == 8,
              pngFullValues.count == pngProxy.tensor.values.count,
              zip(pngFullValues, pngProxy.tensor.values).allSatisfy({ abs($0 - $1) < 0.000_1 }) else {
            throw IntegrationSelfTestError.message("PNG full decode did not match proxy colour and alpha semantics")
        }
        let pngExportDirectory = root.appendingPathComponent("png-export", isDirectory: true)
        try await PNGSequenceExporter.export(pngFull, to: pngExportDirectory) { _, _ in }
        let exportedInspection = try FrameSequenceDiscovery.inspect(directoryURL: pngExportDirectory)
        guard exportedInspection.frameNames == ["ChronoForge_000001.png", "ChronoForge_000002.png"] else {
            throw IntegrationSelfTestError.message("PNG sequence export did not use stable six-digit frame names")
        }
        let exportedSource = FrameSequenceSource(
            directoryURL: pngExportDirectory,
            frameNames: exportedInspection.frameNames,
            framesPerSecond: 8
        )
        let exportedRoundTrip = try await ImageSequenceDecoder.decodeProxy(from: exportedSource, quality: .standard)
        guard exportedRoundTrip.tensor.values.count == pngProxy.tensor.values.count,
              zip(exportedRoundTrip.tensor.values, pngProxy.tensor.values).allSatisfy({ abs($0 - $1) < 0.01 }) else {
            throw IntegrationSelfTestError.message("PNG sequence export did not preserve premultiplied RGBA through an 8-bit round-trip")
        }
        do {
            try await PNGSequenceExporter.export(pngFull, to: pngExportDirectory) { _, _ in }
            throw IntegrationSelfTestError.message("PNG sequence export silently reused a non-empty folder")
        } catch PNGSequenceExporterError.destinationNotEmpty {
            // Expected: user files must never be overwritten implicitly.
        }
        let cancelledExportDirectory = root.appendingPathComponent("png-export-cancelled", isDirectory: true)
        do {
            try await PNGSequenceExporter.export(pngFull, to: cancelledExportDirectory) { fraction, _ in
                if fraction > 0 { withUnsafeCurrentTask { $0?.cancel() } }
            }
            throw IntegrationSelfTestError.message("PNG sequence cancellation did not stop export")
        } catch is CancellationError {
            let survivingFrames = try FileManager.default.contentsOfDirectory(
                at: cancelledExportDirectory, includingPropertiesForKeys: nil)
            guard survivingFrames.map(\.lastPathComponent) == ["ChronoForge_000001.png"] else {
                throw IntegrationSelfTestError.message("PNG cancellation removed completed frames or wrote additional frames")
            }
        }
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
            audioMode: .preserveOriginal,
            playbackFPSPreset: .custom,
            customPlaybackFPS: 18
        )
        try SessionRecoveryStore.save(savedSnapshot, to: snapshotURL)
        let restoredSnapshot = try SessionRecoveryStore.load(from: snapshotURL)
        guard restoredSnapshot.effects == savedSnapshot.effects,
              restoredSnapshot.source == projectSource.mediaSource,
              restoredSnapshot.proxyQuality == ProxyQuality.high.rawValue,
              restoredSnapshot.spatialPrefilter == PrefilterStrength.light.rawValue,
              restoredSnapshot.temporalPrefilter == PrefilterStrength.strong.rawValue,
              restoredSnapshot.audioMode == AudioMode.preserveOriginal.rawValue,
              restoredSnapshot.playbackFPSPreset == PlaybackFPSPreset.custom.rawValue,
              restoredSnapshot.customPlaybackFPS == 18,
              restoredSnapshot.outputNodeID == branchB.id,
              restoredSnapshot.effects.last?.inputNodeID == firstNode.id,
              try restoredSnapshot.sourceURL() == source else {
            throw IntegrationSelfTestError.message("Session recovery snapshot round-trip failed")
        }
        var legacyObject = try JSONSerialization.jsonObject(with: Data(contentsOf: snapshotURL)) as! [String: Any]
        legacyObject["version"] = 1
        legacyObject.removeValue(forKey: "source")
        legacyObject.removeValue(forKey: "playbackFPSPreset")
        legacyObject.removeValue(forKey: "customPlaybackFPS")
        if var legacyMedia = legacyObject["media"] as? [[String: Any]] {
            for index in legacyMedia.indices { legacyMedia[index].removeValue(forKey: "source") }
            legacyObject["media"] = legacyMedia
        }
        let legacySnapshotURL = root.appendingPathComponent("legacy-recovery.json")
        try JSONSerialization.data(withJSONObject: legacyObject).write(to: legacySnapshotURL)
        let legacySnapshot = try SessionRecoveryStore.load(from: legacySnapshotURL)
        guard legacySnapshot.source == nil, try legacySnapshot.sourceURL() == source else {
            throw IntegrationSelfTestError.message("Legacy movie-only recovery did not remain readable")
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
        var rgbTimeSlipSamples = 0
        var sampledKinds = Set<EffectKind>()
        for seed in 0..<10_000 {
            let stack = try RandomStackGenerator.generate(
                mediaPool: [projectSource, driverSource], primaryMediaID: projectSource.id, seed: UInt64(seed))
            lengthCounts[stack.count] += 1
            sampledKinds.formUnion(stack.map(\.kind))
            for node in stack where node.kind == .rgbTimeSlip {
                rgbTimeSlipSamples += 1
                let offsets = Array(node.values.prefix(3))
                guard offsets.contains(where: { abs($0) <= 2.0001 }),
                      offsets.contains(where: { $0 < -3.9 }),
                      offsets.contains(where: { $0 > 3.9 }) else {
                    throw IntegrationSelfTestError.message("RGB Time Slip randomization did not anchor one channel and separate the other two")
                }
            }
            guard stack.filter({ $0.kind.definition.costClass == .global }).count <= 1,
                  stack.dropLast().allSatisfy({ $0.kind != .seamlessLoop }),
                  stack.allSatisfy({ $0.amount == 1 || $0.supportsAmount }) else {
                throw IntegrationSelfTestError.message("Random Stack generated an incompatible combination")
            }
        }
        guard (3_300...3_700).contains(lengthCounts[1]),
              (4_300...4_700).contains(lengthCounts[2]),
              (1_800...2_200).contains(lengthCounts[3]),
              rgbTimeSlipSamples > 400,
              sampledKinds == Set(EffectKind.addableKinds) else {
            throw IntegrationSelfTestError.message("Random Stack length weights drifted outside their expected ranges")
        }
        for seed in 0..<1_000 {
            let stack = try RandomStackGenerator.generate(
                mediaPool: [projectSource], primaryMediaID: projectSource.id, seed: UInt64(seed))
            guard stack.allSatisfy({ !$0.kind.requiresDriver }) else {
                throw IntegrationSelfTestError.message("Random Stack selected a two-input effect without driver B")
            }
        }
        let compareInput = VideoTensorData(
            values: [
                0, 0, 0, 1,
                0.25, 0.25, 0.25, 1,
                0.75, 0.75, 0.75, 1,
                1, 1, 1, 1,
            ],
            frames: 4,
            height: 1,
            width: 1,
            channels: 4,
            framesPerSecond: 8,
            duration: 0.5
        )
        var compareFirst = EffectNode.make(.lumaTimeShift)
        compareFirst.values[0] = 1
        let compareSecond = EffectNode.make(.temporalPixelSort, inputNodeID: compareFirst.id)
        let expectedSelectedInput = try await CoreRenderer.render(input: compareInput, effects: [compareFirst])
        let expectedFinal = try await CoreRenderer.render(input: compareInput, effects: [compareFirst, compareSecond])
        let compared = try await CoreRenderer.renderCapturingSelectedEffect(
            input: compareInput,
            effects: [compareFirst, compareSecond],
            selectedNodeID: compareSecond.id
        )
        guard compared.selectedEffect?.nodeID == compareSecond.id,
              compared.selectedEffect?.input.values == expectedSelectedInput.values,
              compared.selectedEffect?.output.values == expectedFinal.values,
              compared.output.values == expectedFinal.values else {
            throw IntegrationSelfTestError.message("Selected Effect compare did not capture the node's immediate input and output")
        }
        try await MainActor.run {
            let outputSettings = SessionStore()
            outputSettings.source = projectSource
            outputSettings.output = projectSource.tensor
            outputSettings.setAudioMode(.preserveOriginal)
            guard outputSettings.audioMode == .preserveOriginal else {
                throw IntegrationSelfTestError.message("Original movie audio could not be enabled at Result FPS")
            }
            outputSettings.setPlaybackFPSPreset(.fps24)
            outputSettings.setAudioMode(.preserveOriginal)
            guard outputSettings.audioMode == .none,
                  outputSettings.outputFramesPerSecond == 24,
                  outputSettings.outputDuration == Double(projectSource.tensor.frames) / 24 else {
                throw IntegrationSelfTestError.message("Playback FPS did not reinterpret duration or restrict unsynchronised audio")
            }
            outputSettings.source = pngProxy
            outputSettings.setPlaybackFPSPreset(.result)
            outputSettings.setAudioMode(.preserveOriginal)
            guard outputSettings.audioMode == .none else {
                throw IntegrationSelfTestError.message("Image sequence incorrectly allowed Original audio")
            }
            let store = SessionStore()
            guard EffectKind.addableKinds.count == 20,
                  EffectKind.spaceTimeTranspose.title == EffectKind.tensor3DRotation.title,
                  EffectKind.spaceTimeTranspose.title == "Space-Time Transform",
                  EffectKind.singleInputKinds.count == 15,
                  EffectKind.twoInputKinds.count == 5,
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
            let queuedEffects = store.effects
            store.renderQueue.append(RenderQueueItem(
                source: projectSource,
                effects: queuedEffects,
                mediaPool: store.mediaPool,
                audioMode: .none,
                destinationURL: root.appendingPathComponent("snapshot.mp4")
            ))
            var postQueueEdit = store.effects[0]
            postQueueEdit.amount = 0.2
            store.updateEffect(postQueueEdit)
            store.undo()
            guard store.renderQueue.first?.effects == queuedEffects else {
                throw IntegrationSelfTestError.message("Render queue snapshot changed after a later creative edit and Undo")
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
        try await verifyProxyQualityRefresh(source: projectSource)
        try await verifyAutoUpdateDebounce(source: projectSource)
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
        let reinterpreted = FullRenderPipeline.reinterpreting(rendered, at: 12)
        guard reinterpreted.fileURL == rendered.fileURL,
              reinterpreted.frames == rendered.frames,
              reinterpreted.framesPerSecond == 12,
              reinterpreted.duration == Double(rendered.frames) / 12,
              reinterpreted.timestamps == nil else {
            throw IntegrationSelfTestError.message("Playback FPS changed pixels or frame count instead of metadata only")
        }
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

    @MainActor
    private static func verifyAutoUpdateDebounce(source: DecodedProxy) async throws {
        let store = SessionStore()
        store.source = source
        store.mediaPool = [source]
        store.output = source.tensor
        store.addEffect(.lumaTimeShift)
        store.cancelWork()
        let previewLaunchesBeforeBurst = store.previewLaunchCountForDiagnostics
        for amount in stride(from: Float(0.15), through: 0.75, by: 0.1) {
            var edit = store.effects[0]
            edit.amount = amount
            store.updateEffect(edit)
        }
        try await Task.sleep(for: .milliseconds(700))
        guard store.previewLaunchCountForDiagnostics == previewLaunchesBeforeBurst + 1 else {
            throw IntegrationSelfTestError.message("Auto Update launched more than one preview for a rapid edit burst")
        }
        store.cancelWork()
        store.startFreshSession()
    }

    @MainActor
    private static func verifyProxyQualityRefresh(source: DecodedProxy) async throws {
        let store = SessionStore()
        guard store.proxyQuality == .high else {
            throw IntegrationSelfTestError.message("High must be the default Preview quality")
        }
        store.source = source
        store.mediaPool = [source]
        store.output = source.tensor
        store.addEffect(.lumaTimeShift)
        store.cancelWork()
        let launchesBeforeQualityChange = store.previewLaunchCountForDiagnostics
        store.changeProxyQuality(to: .standard)
        for _ in 0..<200 {
            if !store.isImporting && !store.isRendering { break }
            try await Task.sleep(for: .milliseconds(25))
        }
        guard store.proxyQuality == .standard,
              !store.isImporting,
              !store.isRendering,
              !store.isPreviewStale,
              store.previewLaunchCountForDiagnostics == launchesBeforeQualityChange + 1 else {
            throw IntegrationSelfTestError.message("Changing Preview quality did not rebuild media and refresh the effect preview automatically")
        }
        store.startFreshSession()
    }

    static func makeMovie(at url: URL, phase: Int = 0) async throws {
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
            fill(buffer, frame: frame + phase)
            guard adaptor.append(buffer, withPresentationTime: CMTime(value: CMTimeValue(frame), timescale: 8)) else {
                throw writer.error ?? IntegrationSelfTestError.message("Cannot append source frame")
            }
        }
        input.markAsFinished()
        await writer.finishWriting()
        guard writer.status == .completed else { throw writer.error ?? IntegrationSelfTestError.message("Source writer did not finish") }
    }

    private static func makePNG(
        at url: URL,
        width: Int,
        height: Int,
        rgba: [UInt8]
    ) throws {
        let pixels = Array(repeating: rgba, count: width * height).flatMap { $0 }
        guard let provider = CGDataProvider(data: Data(pixels) as CFData),
              let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
              let image = CGImage(
                width: width,
                height: height,
                bitsPerComponent: 8,
                bitsPerPixel: 32,
                bytesPerRow: width * 4,
                space: colorSpace,
                bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue),
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
              ) else {
            throw IntegrationSelfTestError.message("Cannot create PNG self-test fixture")
        }
        CGImageDestinationAddImage(destination, image, nil)
        guard CGImageDestinationFinalize(destination) else {
            throw IntegrationSelfTestError.message("Cannot write PNG self-test fixture")
        }
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
