import AppKit
import Foundation
import SwiftUI

@MainActor
final class ProjectStore: ObservableObject {
    @Published var source: DecodedProxy?
    @Published var output: VideoTensorData?
    @Published var effects: [EffectNode] = []
    @Published var selectedNodeID: UUID?
    @Published var outputNodeID: UUID?
    @Published var currentFrame = 0
    @Published var quality = RenderQuality.proxy
    @Published var audioMode = AudioMode.none
    @Published var showsImporter = false
    @Published var isImporting = false
    @Published var isRendering = false
    @Published var isExporting = false
    @Published var errorMessage: String?
    @Published var statusMessage = "Choose a video to begin"
    @Published var renderProgress: Double?
    @Published var projectURL: URL?
    @Published var isDirty = false
    @Published var hasRecovery = RecoveryStore.exists
    @Published var cacheSizeDescription = "Calculating cache…"
    @Published var showsClearCacheConfirmation = false
    @Published var isPlaying = false
    @Published var renderQueue: [RenderQueueItem] = []
    @Published var isQueueRunning = false
    @Published var isPreviewStale = false

    private var task: Task<Void, Never>?
    private var playbackTask: Task<Void, Never>?

    init() {
        Task {
            _ = try? await CacheManager.shared.trim()
            await refreshCacheSize()
        }
    }

    var displayedTensor: VideoTensorData? { output ?? source?.tensor }
    var selectedNodeIndex: Int? { effects.firstIndex { $0.id == selectedNodeID } }

    func effect(withID id: UUID) -> EffectNode? {
        effects.first { $0.id == id }
    }

    func updateEffect(_ node: EffectNode) {
        guard let index = effects.firstIndex(where: { $0.id == node.id }) else { return }
        effects[index] = node
        markEdited()
    }

    func importVideo(from url: URL, markDirty: Bool = true) {
        stopPlayback()
        cancelWork()
        isImporting = true
        errorMessage = nil
        statusMessage = "Building proxy…"
        let accessGranted = url.startAccessingSecurityScopedResource()
        task = Task {
            defer {
                if accessGranted { url.stopAccessingSecurityScopedResource() }
                isImporting = false
            }
            do {
                let decoded = try await VideoDecoder.decodeProxy(from: url)
                try Task.checkCancellation()
                source = decoded
                output = decoded.tensor
                currentFrame = 0
                isPreviewStale = !effects.isEmpty
                statusMessage = "Proxy ready · \(decoded.tensor.frames) frames"
                if markDirty {
                    markEdited(invalidatePreview: !effects.isEmpty)
                } else {
                    isDirty = false
                }
            } catch is CancellationError {
                statusMessage = "Import cancelled"
            } catch {
                errorMessage = error.localizedDescription
                statusMessage = "Import failed"
            }
        }
    }

    func addEffect(_ kind: EffectKind) {
        let node = EffectNode.make(kind, inputNodeID: outputNodeID)
        effects.append(node)
        selectedNodeID = node.id
        outputNodeID = node.id
        markEdited()
    }

    func removeSelectedEffect() {
        guard let index = selectedNodeIndex else { return }
        selectedNodeID = nil
        let removed = effects.remove(at: index)
        for candidate in effects.indices where effects[candidate].inputNodeID == removed.id {
            effects[candidate].inputNodeID = removed.inputNodeID
        }
        if outputNodeID == removed.id { outputNodeID = removed.inputNodeID }
        selectedNodeID = effects.indices.contains(index) ? effects[index].id : effects.last?.id
        markEdited()
    }

    func removeMedia() {
        stopPlayback()
        cancelWork()
        source = nil
        output = nil
        isPreviewStale = false
        currentFrame = 0
        isDirty = true
        RecoveryStore.remove()
        hasRecovery = false
        statusMessage = "No media selected"
    }

    func moveEffect(from offsets: IndexSet, to destination: Int) {
        effects.move(fromOffsets: offsets, toOffset: destination)
        markEdited()
    }

    func newProject() {
        stopPlayback()
        cancelWork()
        source = nil
        output = nil
        isPreviewStale = false
        effects = []
        renderQueue = []
        selectedNodeID = nil
        outputNodeID = nil
        currentFrame = 0
        projectURL = nil
        isDirty = false
        RecoveryStore.remove()
        hasRecovery = false
        statusMessage = "Choose a video to begin"
    }

    func markEdited(invalidatePreview: Bool = true) {
        isDirty = true
        if invalidatePreview, source != nil {
            isPreviewStale = true
        }
        guard let source else { return }
        try? RecoveryStore.save(SavedChronoForgeProject(
            source: source, effects: effects, outputNodeID: outputNodeID, quality: quality, audioMode: audioMode))
        hasRecovery = true
    }

    func restoreRecovery() {
        guard RecoveryStore.exists else { return }
        openProject(from: RecoveryStore.url)
    }

    func chooseProjectToOpen() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.chronoForgeProject]
        panel.allowsMultipleSelection = false
        panel.begin { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            self?.openProject(from: url)
        }
    }

    func openProject(from url: URL) {
        do {
            let saved = try ProjectPersistence.load(from: url)
            effects = saved.effects
            outputNodeID = saved.outputNodeID ?? saved.effects.last?.id
            quality = RenderQuality(rawValue: saved.quality) ?? .proxy
            audioMode = saved.audioMode.flatMap(AudioMode.init(rawValue:)) ?? .none
            projectURL = url
            isDirty = false
            importVideo(from: try saved.sourceURL(), markDirty: false)
            statusMessage = "Opening \(url.lastPathComponent)…"
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func saveProject(saveAs: Bool = false) {
        guard let source else {
            errorMessage = "Import a video before saving the project."
            return
        }
        if let projectURL, !saveAs {
            writeProject(source: source, to: projectURL)
            return
        }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.chronoForgeProject]
        panel.canCreateDirectories = true
        panel.nameFieldStringValue = "Untitled.chronoforge"
        panel.begin { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            self?.writeProject(source: source, to: url)
        }
    }

    private func writeProject(source: DecodedProxy, to url: URL) {
        do {
            try ProjectPersistence.save(SavedChronoForgeProject(
                source: source, effects: effects, outputNodeID: outputNodeID, quality: quality, audioMode: audioMode), to: url)
            projectURL = url
            isDirty = false
            RecoveryStore.remove()
            hasRecovery = false
            statusMessage = "Saved \(url.lastPathComponent)"
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func renderPreview() {
        guard let input = source?.tensor else {
            errorMessage = "Import a video before rendering."
            return
        }
        cancelWork()
        isRendering = true
        errorMessage = nil
        statusMessage = "Rendering graph…"
        let graph: [EffectNode]
        do {
            graph = try activeEffectChain()
        } catch {
            errorMessage = error.localizedDescription
            isRendering = false
            return
        }
        let cacheKey = ProxyCache.key(source: source!.sourceURL, input: input, effects: graph)
        task = Task {
            defer { isRendering = false }
            do {
                if let cached = await ProxyCache.shared.load(key: cacheKey) {
                    output = cached
                    isPreviewStale = false
                    currentFrame = min(currentFrame, max(0, cached.frames - 1))
                    statusMessage = "Loaded from render cache"
                    return
                }
                let result = try await CoreRenderer.render(input: input, effects: graph)
                try Task.checkCancellation()
                output = result
                isPreviewStale = false
                currentFrame = min(currentFrame, max(0, result.frames - 1))
                statusMessage = "Rendered · \(result.frames) × \(result.width) × \(result.height)"
                try? await ProxyCache.shared.store(result, key: cacheKey)
                _ = try? await CacheManager.shared.trim()
                await refreshCacheSize()
            } catch is CancellationError {
                statusMessage = "Render cancelled"
            } catch {
                errorMessage = error.localizedDescription
                statusMessage = "Render failed"
            }
            _ = try? await CacheManager.shared.trim()
            await refreshCacheSize()
        }
    }

    func chooseExportLocation() {
        guard source != nil else { return }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.mpeg4Movie]
        panel.canCreateDirectories = true
        panel.nameFieldStringValue = quality == .full ? "ChronoForge-Full-Render.mp4" : "ChronoForge-Proxy-Render.mp4"
        panel.begin { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            self?.exportVideo(to: url)
        }
    }

    func exportVideo(to url: URL) {
        guard let source else { return }
        cancelWork()
        isExporting = true
        renderProgress = 0
        errorMessage = nil
        statusMessage = "Exporting MP4…"
        task = Task {
            defer {
                isExporting = false
                renderProgress = nil
            }
            do {
                let renderEffects = try activeEffectChain()
                try await performExport(
                    source: source,
                    effects: renderEffects,
                    quality: quality,
                    audioMode: audioMode,
                    destination: url
                ) { fraction, stage in
                    Task { @MainActor in
                        self.renderProgress = fraction
                        self.statusMessage = stage
                    }
                }
                statusMessage = "Export complete · \(url.lastPathComponent)"
                _ = try? await CacheManager.shared.trim()
                await refreshCacheSize()
                NSWorkspace.shared.activateFileViewerSelecting([url])
            } catch is CancellationError {
                statusMessage = "Export cancelled"
            } catch {
                errorMessage = error.localizedDescription
                statusMessage = "Export failed"
            }
            _ = try? await CacheManager.shared.trim()
            await refreshCacheSize()
        }
    }

    func addCurrentRenderToQueue() {
        guard let source else {
            errorMessage = "Import a video before adding a render."
            return
        }
        let renderEffects: [EffectNode]
        do {
            renderEffects = try activeEffectChain()
        } catch {
            errorMessage = error.localizedDescription
            return
        }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.mpeg4Movie]
        panel.canCreateDirectories = true
        panel.nameFieldStringValue = "ChronoForge-Queue-\(renderQueue.count + 1).mp4"
        panel.begin { [weak self] response in
            guard response == .OK, let destination = panel.url, let self else { return }
            self.renderQueue.append(RenderQueueItem(
                source: source,
                effects: renderEffects,
                quality: self.quality,
                audioMode: self.audioMode,
                destinationURL: destination
            ))
            self.statusMessage = "Added to render queue · \(destination.lastPathComponent)"
        }
    }

    func removeQueueItem(_ id: UUID) {
        guard !isQueueRunning else { return }
        renderQueue.removeAll { $0.id == id }
    }

    func startRenderQueue() {
        guard !isQueueRunning, renderQueue.contains(where: { $0.status == .waiting }) else { return }
        cancelWork()
        isQueueRunning = true
        isExporting = true
        renderProgress = 0
        errorMessage = nil
        task = Task {
            defer {
                isQueueRunning = false
                isExporting = false
                renderProgress = nil
            }
            for index in renderQueue.indices where renderQueue[index].status == .waiting {
                if Task.isCancelled {
                    renderQueue[index].status = .cancelled
                    break
                }
                renderQueue[index].status = .running
                let item = renderQueue[index]
                statusMessage = "Queue \(index + 1)/\(renderQueue.count) · \(item.destinationURL.lastPathComponent)"
                do {
                    try await performExport(
                        source: item.source,
                        effects: item.effects,
                        quality: item.quality,
                        audioMode: item.audioMode,
                        destination: item.destinationURL
                    ) { fraction, stage in
                        Task { @MainActor in
                            self.renderProgress = fraction
                            self.statusMessage = "Queue \(index + 1)/\(self.renderQueue.count) · \(stage)"
                        }
                    }
                    renderQueue[index].status = .completed
                } catch is CancellationError {
                    renderQueue[index].status = .cancelled
                    break
                } catch {
                    renderQueue[index].status = .failed(error.localizedDescription)
                }
                try? await CacheManager.shared.clear()
                await refreshCacheSize()
            }
            statusMessage = Task.isCancelled ? "Render queue cancelled" : "Render queue finished"
        }
    }

    private func performExport(
        source: DecodedProxy,
        effects: [EffectNode],
        quality: RenderQuality,
        audioMode: AudioMode,
        destination: URL,
        progress: @escaping @Sendable (Double, String) -> Void
    ) async throws {
        let access = source.sourceURL.startAccessingSecurityScopedResource()
        defer { if access { source.sourceURL.stopAccessingSecurityScopedResource() } }
        if quality == .full {
            try await FullRenderPipeline.export(
                source: source,
                effects: effects,
                audioMode: audioMode,
                to: destination,
                progress: progress
            )
            return
        }
        progress(0.05, "Rendering proxy")
        let proxyOutput = try await CoreRenderer.render(input: source.tensor, effects: effects)
        try Task.checkCancellation()
        progress(0.55, "Encoding proxy")
        if audioMode == .preserveOriginal {
            let temporary = destination.deletingLastPathComponent()
                .appendingPathComponent(".chronoforge-video-\(UUID().uuidString).mp4")
            try await VideoExporter.export(proxyOutput, to: temporary)
            progress(0.90, "Muxing original audio")
            try await MediaMuxer.addOriginalAudio(
                videoURL: temporary,
                sourceURL: source.sourceURL,
                destinationURL: destination
            )
        } else {
            try await VideoExporter.export(proxyOutput, to: destination)
        }
        progress(1, "Proxy export complete")
    }

    func cancelWork() {
        task?.cancel()
        task = nil
    }

    func togglePlayback() {
        isPlaying ? stopPlayback() : startPlayback()
    }

    func startPlayback() {
        guard let tensor = displayedTensor, tensor.frames > 1 else { return }
        stopPlayback()
        isPlaying = true
        playbackTask = Task {
            while !Task.isCancelled {
                let fps = displayedTensor?.framesPerSecond ?? tensor.framesPerSecond
                try? await Task.sleep(for: .seconds(1 / max(0.1, fps)))
                guard !Task.isCancelled, let latest = displayedTensor else { break }
                currentFrame = (currentFrame + 1) % latest.frames
            }
        }
    }

    func stopPlayback() {
        playbackTask?.cancel()
        playbackTask = nil
        isPlaying = false
    }

    func activeEffectChain() throws -> [EffectNode] {
        guard let outputNodeID else { return [] }
        return try chainEnding(at: outputNodeID)
    }

    private func chainEnding(at nodeID: UUID) throws -> [EffectNode] {
        var chain: [EffectNode] = []
        var current: UUID? = nodeID
        var visited = Set<UUID>()
        while let id = current {
            guard visited.insert(id).inserted else {
                throw CocoaError(.validationMultipleErrors, userInfo: [NSLocalizedDescriptionKey: "The node graph contains a cycle."])
            }
            guard let node = effects.first(where: { $0.id == id }) else {
                throw CocoaError(.coderReadCorrupt, userInfo: [NSLocalizedDescriptionKey: "The graph references a missing node."])
            }
            chain.append(node)
            current = node.inputNodeID
        }
        return chain.reversed()
    }

    func availableInputs(for nodeID: UUID) -> [EffectNode] {
        guard let index = effects.firstIndex(where: { $0.id == nodeID }) else { return [] }
        return Array(effects[..<index])
    }

    func setInput(_ inputID: UUID?, for nodeID: UUID) {
        guard let index = effects.firstIndex(where: { $0.id == nodeID }), inputID != nodeID else { return }
        let previous = effects[index].inputNodeID
        effects[index].inputNodeID = inputID
        do {
            _ = try chainEnding(at: nodeID)
            markEdited()
        } catch {
            effects[index].inputNodeID = previous
            errorMessage = error.localizedDescription
        }
    }

    func nodeName(_ id: UUID?) -> String {
        guard let id else { return "Input" }
        return effects.first(where: { $0.id == id })?.kind.title ?? "Missing node"
    }

    func refreshCacheSize() async {
        let bytes = await CacheManager.shared.size()
        cacheSizeDescription = ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }

    func clearRenderCache() {
        guard !isRendering, !isImporting, !isExporting else {
            errorMessage = "Wait for the active operation to finish or cancel it before clearing the cache."
            return
        }
        task = Task {
            do {
                try await CacheManager.shared.clear()
                await refreshCacheSize()
                statusMessage = "Render cache cleared"
            } catch {
                errorMessage = error.localizedDescription
            }
        }
    }
}
