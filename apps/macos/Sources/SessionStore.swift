import AppKit
import Foundation
import SwiftUI

@MainActor
final class SessionStore: ObservableObject {
    @Published var source: DecodedProxy?
    @Published var mediaPool: [DecodedProxy] = []
    @Published var output: VideoTensorData?
    @Published private(set) var selectedEffectCapture: SelectedEffectCapture?
    @Published var effects: [EffectNode] = []
    @Published var selectedNodeID: UUID?
    @Published var outputNodeID: UUID?
    @Published var currentFrame = 0
    @Published var proxyQuality = ProxyQuality.standard
    @Published var spatialPrefilter = PrefilterStrength.off
    @Published var temporalPrefilter = PrefilterStrength.off
    @Published var audioMode = AudioMode.none
    @Published var playbackFPSPreset = PlaybackFPSPreset.result
    @Published var customPlaybackFPS = 24.0
    @Published var showsImporter = false
    @Published var isImporting = false
    @Published var isRendering = false
    @Published var isExporting = false
    @Published var errorMessage: String?
    @Published var statusMessage = "Choose media to begin"
    @Published var renderProgress: Double?
    @Published var hasInterruptedSession = SessionRecoveryStore.exists
    @Published var cacheSizeDescription = "Calculating cache…"
    @Published var showsClearCacheConfirmation = false
    @Published var showsClearEffectsConfirmation = false
    @Published var isPlaying = false
    @Published var renderQueue: [RenderQueueItem] = []
    @Published var isQueueRunning = false
    @Published var isPreviewStale = false
    @Published var autoUpdate: Bool {
        didSet { UserDefaults.standard.set(autoUpdate, forKey: Self.autoUpdatePreferenceKey) }
    }
    @Published private(set) var canUndo = false
    @Published private(set) var canRedo = false
    private(set) var previewLaunchCountForDiagnostics = 0
    var mediaReplacementID: UUID?

    private static let autoUpdatePreferenceKey = "ChronoForge.autoUpdatePreview"
    private var task: Task<Void, Never>?
    private var previewTask: Task<Void, Never>?
    private var autoPreviewTask: Task<Void, Never>?
    private var playbackTask: Task<Void, Never>?
    private var recoveryTask: Task<Void, Never>?
    private var previewGeneration = UUID()
    private let sessionUndoManager = UndoManager()
    private var coalescedEditStart: CreativeSessionState?

    init() {
        autoUpdate = UserDefaults.standard.object(forKey: Self.autoUpdatePreferenceKey) as? Bool ?? true
        sessionUndoManager.levelsOfUndo = 100
        sessionUndoManager.groupsByEvent = false
        Task {
            _ = try? await CacheManager.shared.trim()
            await refreshCacheSize()
        }
    }

    var displayedTensor: VideoTensorData? { output ?? source?.tensor }
    var selectedNodeIndex: Int? { effects.firstIndex { $0.id == selectedNodeID } }
    var outputFramesPerSecond: Double? {
        playbackFPSPreset == .custom ? max(0.001, customPlaybackFPS) : playbackFPSPreset.framesPerSecond
    }
    var canPreserveOriginalAudio: Bool {
        source?.mediaSource.isMovie == true && playbackFPSPreset == .result
    }
    var outputDuration: Double? {
        guard let tensor = displayedTensor else { return nil }
        return Double(tensor.frames) / (outputFramesPerSecond ?? tensor.framesPerSecond)
    }
    var selectedEffectCaptureForSelection: SelectedEffectCapture? {
        guard let selectedNodeID, selectedEffectCapture?.nodeID == selectedNodeID else { return nil }
        return selectedEffectCapture
    }

    func effect(withID id: UUID) -> EffectNode? {
        effects.first { $0.id == id }
    }

    func updateEffect(_ node: EffectNode) {
        guard let index = effects.firstIndex(where: { $0.id == node.id }) else { return }
        if coalescedEditStart != nil {
            effects[index] = node
            if source != nil {
                isPreviewStale = true
                selectedEffectCapture = nil
            }
        } else {
            performCreativeEdit(named: "Change Effect") { effects[index] = node }
        }
    }

    func toggleEffectEnabled(_ id: UUID) {
        guard let index = effects.firstIndex(where: { $0.id == id }) else { return }
        performCreativeEdit(named: effects[index].enabled ? "Bypass Effect" : "Enable Effect") {
            effects[index].enabled.toggle()
        }
    }

    func importVideo(from url: URL) {
        stopPlayback()
        cancelWork()
        isImporting = true
        errorMessage = nil
        statusMessage = "Building proxy…"
        let accessGranted = url.startAccessingSecurityScopedResource()
        let replacementID = mediaReplacementID
        mediaReplacementID = nil
        let mediaSource = MediaSource.movie(
            url: url,
            securityScopedBookmark: try? url.bookmarkData(
                options: [.withSecurityScope],
                includingResourceValuesForKeys: nil,
                relativeTo: nil
            )
        )
        task = Task {
            defer {
                if accessGranted { url.stopAccessingSecurityScopedResource() }
                isImporting = false
            }
            do {
                var decoded = try await MediaSourceDecoder.decodeProxy(from: mediaSource, quality: proxyQuality)
                try Task.checkCancellation()
                if let replacementID, let index = mediaPool.firstIndex(where: { $0.id == replacementID }) {
                    decoded.id = replacementID
                    mediaPool[index] = decoded
                    if source?.id == replacementID {
                        source = decoded
                        output = decoded.tensor
                        currentFrame = 0
                    }
                } else {
                    mediaPool.append(decoded)
                    if source == nil {
                        source = decoded
                        output = decoded.tensor
                        currentFrame = 0
                    }
                }
                isPreviewStale = !effects.isEmpty
                statusMessage = source?.id == decoded.id
                    ? "Proxy ready · \(decoded.tensor.frames) frames"
                    : "Media added · \(decoded.tensor.frames) proxy frames"
                markEdited(invalidatePreview: !effects.isEmpty)
            } catch is CancellationError {
                statusMessage = "Import cancelled"
            } catch {
                errorMessage = error.localizedDescription
                statusMessage = "Import failed"
            }
        }
    }

    func addMedia() {
        mediaReplacementID = nil
        showsImporter = true
    }

    func addImageSequence() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = "Choose Sequence"
        panel.message = "Choose a folder containing a numbered PNG sequence."
        panel.begin { [weak self] response in
            guard response == .OK, let directoryURL = panel.url else { return }
            self?.importImageSequence(from: directoryURL)
        }
    }

    func replaceMedia(_ id: UUID) {
        mediaReplacementID = id
        showsImporter = true
    }

    private func importImageSequence(from directoryURL: URL) {
        stopPlayback()
        cancelWork()
        isImporting = true
        errorMessage = nil
        statusMessage = "Inspecting PNG sequence…"
        let accessGranted = directoryURL.startAccessingSecurityScopedResource()
        task = Task {
            defer {
                if accessGranted { directoryURL.stopAccessingSecurityScopedResource() }
                isImporting = false
            }
            do {
                let inspection = try await Task.detached(priority: .userInitiated) {
                    try FrameSequenceDiscovery.inspect(directoryURL: directoryURL)
                }.value
                try Task.checkCancellation()
                guard let framesPerSecond = chooseSequenceFramesPerSecond(for: inspection) else {
                    statusMessage = "Image sequence import cancelled"
                    return
                }
                let bookmark = try? directoryURL.bookmarkData(
                    options: [.withSecurityScope],
                    includingResourceValuesForKeys: nil,
                    relativeTo: nil
                )
                let sequence = FrameSequenceSource(
                    directoryURL: directoryURL,
                    frameNames: inspection.frameNames,
                    framesPerSecond: framesPerSecond,
                    securityScopedBookmark: bookmark
                )
                let decoded = try await MediaSourceDecoder.decodeProxy(
                    from: .frameSequence(sequence),
                    quality: proxyQuality
                )
                try Task.checkCancellation()
                mediaPool.append(decoded)
                if source == nil {
                    source = decoded
                    output = decoded.tensor
                    currentFrame = 0
                }
                isPreviewStale = !effects.isEmpty
                statusMessage = source?.id == decoded.id
                    ? "PNG sequence ready · \(inspection.frameCount) frames at \(framesPerSecond.formatted(.number.precision(.fractionLength(0...3)))) fps"
                    : "PNG sequence added · \(inspection.frameCount) frames"
                markEdited(invalidatePreview: !effects.isEmpty)
            } catch is CancellationError {
                statusMessage = "Image sequence import cancelled"
            } catch {
                errorMessage = error.localizedDescription
                statusMessage = "Image sequence import failed"
            }
        }
    }

    private func chooseSequenceFramesPerSecond(for inspection: FrameSequenceInspection) -> Double? {
        let alert = NSAlert()
        alert.messageText = "Import PNG Sequence"
        var details = "\(inspection.frameCount) frames · \(inspection.width) × \(inspection.height)"
        if !inspection.missingFrameNumbers.isEmpty {
            let preview = inspection.missingFrameNumbers.prefix(8).map(String.init).joined(separator: ", ")
            let suffix = inspection.missingFrameNumbers.count > 8 ? ", …" : ""
            details += "\nMissing frame numbers: \(preview)\(suffix). Existing files will be imported consecutively."
        }
        alert.informativeText = details
        alert.addButton(withTitle: "Import")
        alert.addButton(withTitle: "Cancel")

        let accessory = NSStackView()
        accessory.orientation = .horizontal
        accessory.alignment = .centerY
        accessory.spacing = 8
        let label = NSTextField(labelWithString: "Playback FPS")
        let combo = NSComboBox()
        combo.addItems(withObjectValues: ["12", "15", "23.976", "24", "25", "29.97", "30", "50", "59.94", "60"])
        combo.stringValue = "24"
        combo.isEditable = true
        combo.numberOfVisibleItems = 10
        combo.frame.size.width = 110
        accessory.addArrangedSubview(label)
        accessory.addArrangedSubview(combo)
        alert.accessoryView = accessory

        guard alert.runModal() == .alertFirstButtonReturn else { return nil }
        guard let fps = Double(combo.stringValue.replacingOccurrences(of: ",", with: ".")), fps > 0 else {
            errorMessage = ImageSequenceError.invalidFramesPerSecond.localizedDescription
            return nil
        }
        return fps
    }

    func setPrimaryMedia(_ id: UUID) {
        guard let media = mediaPool.first(where: { $0.id == id }), source?.id != id else { return }
        performCreativeEdit(named: "Change Primary Media") {
            source = media
            output = media.tensor
            currentFrame = 0
            if !media.mediaSource.isMovie { audioMode = .none }
        }
    }

    func setPlaybackFPSPreset(_ preset: PlaybackFPSPreset) {
        guard playbackFPSPreset != preset else { return }
        playbackFPSPreset = preset
        if preset != .result { audioMode = .none }
        markEdited(invalidatePreview: false)
    }

    func setCustomPlaybackFPS(_ value: Double) {
        let value = max(0.001, value)
        guard customPlaybackFPS != value else { return }
        customPlaybackFPS = value
        if playbackFPSPreset == .custom { audioMode = .none }
        markEdited(invalidatePreview: false)
    }

    func setAudioMode(_ mode: AudioMode) {
        audioMode = mode == .preserveOriginal && !canPreserveOriginalAudio ? .none : mode
        markEdited(invalidatePreview: false)
    }

    func addEffect(_ kind: EffectKind) {
        var node = EffectNode.make(kind, inputNodeID: effects.last?.id)
        if kind.requiresDriver {
            node.driverMediaID = mediaPool.first(where: { $0.id != source?.id })?.id ?? source?.id
        }
        performCreativeEdit(named: "Add Effect") {
            effects.append(node)
            selectedNodeID = node.id
            outputNodeID = node.id
        }
    }

    func replaceWithRandomStack(seed: UInt64 = .random(in: UInt64.min...UInt64.max)) {
        guard source != nil else { return }
        do {
            let generated = try RandomStackGenerator.generate(
                mediaPool: mediaPool,
                primaryMediaID: source?.id,
                seed: seed
            )
            performCreativeEdit(named: "Replace with Random Stack") {
                effects = generated
                outputNodeID = generated.last?.id
                selectedNodeID = generated.first(where: { $0.kind != .seamlessLoop })?.id ?? generated.first?.id
            }
            statusMessage = "Random Stack · \(generated.count) effect\(generated.count == 1 ? "" : "s")"
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func reseedEffect(_ id: UUID) {
        guard let index = effects.firstIndex(where: { $0.id == id }),
              effects[index].kind.definition.usesRandomSeed else { return }
        performCreativeEdit(named: "Reseed Effect") {
            effects[index].randomSeed = .random(in: UInt64.min...UInt64.max)
        }
    }

    func duplicateEffect(_ id: UUID) {
        guard let index = effects.firstIndex(where: { $0.id == id }) else { return }
        performCreativeEdit(named: "Duplicate Effect") {
            var copy = effects[index]
            copy.id = UUID()
            effects.insert(copy, at: index + 1)
            reconnectEffectStack()
            selectedNodeID = copy.id
        }
    }

    func deleteEffect(_ id: UUID) {
        guard let index = effects.firstIndex(where: { $0.id == id }) else { return }
        performCreativeEdit(named: "Delete Effect") {
            effects.remove(at: index)
            reconnectEffectStack()
            selectedNodeID = effects.indices.contains(index) ? effects[index].id : effects.last?.id
        }
    }

    func clearEffectStack() {
        performCreativeEdit(named: "Clear Effect Stack") {
            effects.removeAll()
            selectedNodeID = nil
            outputNodeID = nil
        }
    }

    func removeSelectedEffect() {
        guard let index = selectedNodeIndex else { return }
        performCreativeEdit(named: "Delete Effect") {
            selectedNodeID = nil
            let removed = effects.remove(at: index)
            for candidate in effects.indices where effects[candidate].inputNodeID == removed.id {
                effects[candidate].inputNodeID = removed.inputNodeID
            }
            if outputNodeID == removed.id { outputNodeID = removed.inputNodeID }
            selectedNodeID = effects.indices.contains(index) ? effects[index].id : effects.last?.id
        }
    }

    func removeMedia(_ id: UUID? = nil) {
        stopPlayback()
        autoPreviewTask?.cancel()
        previewTask?.cancel()
        let targetID = id ?? source?.id
        guard let targetID,
              let removedIndex = mediaPool.firstIndex(where: { $0.id == targetID }) else { return }
        let removedMedia = mediaPool[removedIndex]
        let creativeBefore = creativeState
        mediaPool.remove(at: removedIndex)
        for index in effects.indices where effects[index].driverMediaID == targetID {
            effects[index].driverMediaID = nil
        }
        if source?.id == targetID {
            source = mediaPool.first
            output = source?.tensor
        }
        isPreviewStale = false
        currentFrame = 0
        if source == nil {
            recoveryTask?.cancel()
            SessionRecoveryStore.remove()
            hasInterruptedSession = false
        } else {
            markEdited(invalidatePreview: false)
        }
        registerUndoAction(named: "Remove Media") { target in
            target.restoreRemovedMedia(removedMedia, at: removedIndex, creativeState: creativeBefore)
        }
        statusMessage = source == nil ? "No media selected" : "Media removed"
    }

    private func restoreRemovedMedia(
        _ media: DecodedProxy,
        at index: Int,
        creativeState restoredState: CreativeSessionState
    ) {
        let insertionIndex = min(max(index, 0), mediaPool.count)
        mediaPool.insert(media, at: insertionIndex)
        registerUndoAction(named: "Remove Media") { target in target.removeMedia(media.id) }
        applyCreativeState(restoredState)
        statusMessage = "Media removal undone"
    }

    func moveEffect(from offsets: IndexSet, to destination: Int) {
        performCreativeEdit(named: "Reorder Effects") {
            effects.move(fromOffsets: offsets, toOffset: destination)
            reconnectEffectStack()
        }
    }

    private func reconnectEffectStack() {
        for index in effects.indices {
            effects[index].inputNodeID = index == effects.startIndex ? nil : effects[effects.index(before: index)].id
        }
        outputNodeID = effects.last?.id
    }

    func changeProxyQuality(to quality: ProxyQuality) {
        guard quality != proxyQuality else { return }
        proxyQuality = quality
        guard !mediaPool.isEmpty else { return }
        markEdited(invalidatePreview: false)
        stopPlayback()
        cancelWork()
        isImporting = true
        let existing = mediaPool
        let primaryID = source?.id
        task = Task {
            defer { isImporting = false }
            do {
                var rebuilt: [DecodedProxy] = []
                for media in existing {
                    let access = media.mediaSource.startAccessingSecurityScopedResource()
                    var decoded = try await MediaSourceDecoder.decodeProxy(from: media.mediaSource, quality: quality)
                    if access { media.mediaSource.stopAccessingSecurityScopedResource() }
                    decoded.id = media.id
                    rebuilt.append(decoded)
                }
                mediaPool = rebuilt
                source = rebuilt.first(where: { $0.id == primaryID }) ?? rebuilt.first
                output = source?.tensor
                isPreviewStale = !effects.isEmpty
                statusMessage = "Proxy quality updated"
            } catch {
                errorMessage = error.localizedDescription
                statusMessage = "Proxy update failed"
            }
        }
    }

    func setSpatialPrefilter(_ strength: PrefilterStrength) {
        guard strength != spatialPrefilter else { return }
        performCreativeEdit(named: "Change Spatial Prefilter") { spatialPrefilter = strength }
    }

    func setTemporalPrefilter(_ strength: PrefilterStrength) {
        guard strength != temporalPrefilter else { return }
        performCreativeEdit(named: "Change Temporal Prefilter") { temporalPrefilter = strength }
    }

    func startFreshSession() {
        stopPlayback()
        cancelWork()
        source = nil
        mediaPool = []
        output = nil
        selectedEffectCapture = nil
        isPreviewStale = false
        effects = []
        renderQueue = []
        selectedNodeID = nil
        outputNodeID = nil
        currentFrame = 0
        spatialPrefilter = .off
        temporalPrefilter = .off
        audioMode = .none
        playbackFPSPreset = .result
        customPlaybackFPS = 24
        sessionUndoManager.removeAllActions()
        refreshUndoState()
        recoveryTask?.cancel()
        SessionRecoveryStore.remove()
        hasInterruptedSession = false
        statusMessage = "Choose media to begin"
    }

    func markEdited(invalidatePreview: Bool = true) {
        if invalidatePreview, source != nil {
            isPreviewStale = true
            selectedEffectCapture = nil
            scheduleAutoPreview()
        }
        scheduleRecoverySave()
    }

    private var creativeState: CreativeSessionState {
        CreativeSessionState(
            primaryMediaID: source?.id,
            effects: effects,
            outputNodeID: outputNodeID,
            selectedNodeID: selectedNodeID,
            spatialPrefilter: spatialPrefilter,
            temporalPrefilter: temporalPrefilter
        )
    }

    private func performCreativeEdit(named actionName: String, _ edit: () -> Void) {
        let before = creativeState
        edit()
        guard creativeState != before else { return }
        registerUndo(restoring: before, actionName: actionName)
        markEdited()
    }

    private func registerUndo(restoring state: CreativeSessionState, actionName: String) {
        registerUndoAction(named: actionName) { target in
            let inverse = target.creativeState
            target.registerUndo(restoring: inverse, actionName: actionName)
            target.applyCreativeState(state)
        }
    }

    private func registerUndoAction(named actionName: String, _ action: @escaping (SessionStore) -> Void) {
        let ownsGroup = sessionUndoManager.groupingLevel == 0
        if ownsGroup { sessionUndoManager.beginUndoGrouping() }
        sessionUndoManager.registerUndo(withTarget: self, handler: action)
        sessionUndoManager.setActionName(actionName)
        if ownsGroup { sessionUndoManager.endUndoGrouping() }
        refreshUndoState()
    }

    private func applyCreativeState(_ state: CreativeSessionState) {
        let primaryChanged = source?.id != state.primaryMediaID
        source = state.primaryMediaID.flatMap { id in mediaPool.first(where: { $0.id == id }) }
        effects = state.effects
        outputNodeID = state.outputNodeID
        selectedNodeID = state.selectedNodeID.flatMap { id in effects.contains(where: { $0.id == id }) ? id : nil }
        spatialPrefilter = state.spatialPrefilter
        temporalPrefilter = state.temporalPrefilter
        if primaryChanged {
            output = source?.tensor
            currentFrame = 0
        }
        markEdited()
        DispatchQueue.main.async { [weak self] in self?.refreshUndoState() }
    }

    func beginContinuousEffectEdit() {
        if coalescedEditStart == nil { coalescedEditStart = creativeState }
    }

    func endContinuousEffectEdit() {
        guard let before = coalescedEditStart else { return }
        coalescedEditStart = nil
        guard creativeState != before else { return }
        registerUndo(restoring: before, actionName: "Change Effect")
        markEdited()
    }

    func undo() {
        endContinuousEffectEdit()
        sessionUndoManager.undo()
        refreshUndoState()
    }

    func redo() {
        endContinuousEffectEdit()
        sessionUndoManager.redo()
        refreshUndoState()
    }

    private func refreshUndoState() {
        canUndo = sessionUndoManager.canUndo
        canRedo = sessionUndoManager.canRedo
    }

    func setAutoUpdate(_ enabled: Bool) {
        autoUpdate = enabled
        if enabled, isPreviewStale { scheduleAutoPreview() }
        if !enabled { autoPreviewTask?.cancel() }
    }

    private func scheduleAutoPreview() {
        autoPreviewTask?.cancel()
        guard autoUpdate, source != nil else { return }
        previewTask?.cancel()
        let hasGlobalCost = effects.contains { $0.enabled && $0.kind.definition.costClass == .global }
        let delay = hasGlobalCost ? 800 : 450
        autoPreviewTask = Task {
            try? await Task.sleep(for: .milliseconds(delay))
            guard !Task.isCancelled else { return }
            renderPreview()
        }
    }

    private func scheduleRecoverySave() {
        recoveryTask?.cancel()
        guard let source else { return }
        let saved = SessionRecoverySnapshot(
            source: source, mediaPool: mediaPool, effects: effects, outputNodeID: outputNodeID,
            proxyQuality: proxyQuality, spatialPrefilter: spatialPrefilter,
            temporalPrefilter: temporalPrefilter, audioMode: audioMode,
            playbackFPSPreset: playbackFPSPreset, customPlaybackFPS: customPlaybackFPS)
        recoveryTask = Task {
            try? await Task.sleep(for: .milliseconds(400))
            guard !Task.isCancelled else { return }
            try? SessionRecoveryStore.save(saved)
            guard !Task.isCancelled else { return }
            hasInterruptedSession = true
        }
    }

    func restoreRecovery() {
        do {
            let saved = try SessionRecoveryStore.load()
            effects = saved.effects
            reconnectEffectStack()
            proxyQuality = saved.proxyQuality.flatMap(ProxyQuality.init(rawValue:)) ?? proxyQuality
            spatialPrefilter = saved.spatialPrefilter.flatMap(PrefilterStrength.init(rawValue:)) ?? .off
            temporalPrefilter = saved.temporalPrefilter.flatMap(PrefilterStrength.init(rawValue:)) ?? .off
            audioMode = saved.audioMode.flatMap(AudioMode.init(rawValue:)) ?? .none
            playbackFPSPreset = saved.playbackFPSPreset.flatMap(PlaybackFPSPreset.init(rawValue:)) ?? .result
            customPlaybackFPS = max(0.001, saved.customPlaybackFPS ?? 24)
            sessionUndoManager.removeAllActions()
            refreshUndoState()
            loadSavedMedia(saved)
            statusMessage = "Recovering interrupted session…"
        } catch {
            errorMessage = error.localizedDescription
            startFreshSession()
        }
    }

    func discardRecovery() {
        SessionRecoveryStore.remove()
        hasInterruptedSession = false
    }

    private func loadSavedMedia(_ saved: SessionRecoverySnapshot) {
        stopPlayback()
        cancelWork()
        isImporting = true
        let references = saved.mediaReferences()
        let primaryID = saved.primaryMediaID ?? references.first?.id
        task = Task {
            defer { isImporting = false }
            do {
                var decodedMedia: [DecodedProxy] = []
                for reference in references {
                    let mediaSource = try reference.resolvedMediaSource()
                    let access = mediaSource.startAccessingSecurityScopedResource()
                    defer { if access { mediaSource.stopAccessingSecurityScopedResource() } }
                    var decoded = try await MediaSourceDecoder.decodeProxy(from: mediaSource, quality: proxyQuality)
                    decoded.id = reference.id
                    decodedMedia.append(decoded)
                }
                mediaPool = decodedMedia
                source = decodedMedia.first(where: { $0.id == primaryID }) ?? decodedMedia.first
                if !canPreserveOriginalAudio { audioMode = .none }
                output = source?.tensor
                currentFrame = 0
                isPreviewStale = !effects.isEmpty
                statusMessage = "Recovered · \(decodedMedia.count) media"
            } catch {
                errorMessage = error.localizedDescription
                statusMessage = "Interrupted session could not be recovered"
            }
        }
    }

    func renderPreview() {
        guard let input = source?.tensor else {
            errorMessage = "Import a video before rendering."
            return
        }
        previewLaunchCountForDiagnostics += 1
        autoPreviewTask?.cancel()
        previewTask?.cancel()
        let generation = UUID()
        previewGeneration = generation
        isRendering = true
        errorMessage = nil
        statusMessage = "Updating proxy preview…"
        let graph: [EffectNode]
        do {
            graph = try renderEffectChain()
        } catch {
            errorMessage = error.localizedDescription
            isRendering = false
            return
        }
        let driverSources = graph.compactMap { effect in
            effect.driverMediaID.flatMap { id in mediaPool.first(where: { $0.id == id })?.mediaSource }
        }
        let selectedEffectID = selectedNodeID
        let cacheKey = ProxyCache.key(source: source!.mediaSource, input: input, effects: graph, drivers: driverSources)
        previewTask = Task {
            defer {
                if previewGeneration == generation { isRendering = false }
            }
            do {
                let drivers = Dictionary(uniqueKeysWithValues: mediaPool.map { ($0.id, $0.tensor) })
                if let cached = await ProxyCache.shared.load(key: cacheKey) {
                    let capture: SelectedEffectCapture? = if let selectedEffectID {
                        try await CoreRenderer.captureSelectedEffect(
                            input: input,
                            effects: graph,
                            drivers: drivers,
                            selectedNodeID: selectedEffectID
                        )
                    } else {
                        nil
                    }
                    try Task.checkCancellation()
                    output = cached
                    selectedEffectCapture = capture
                    isPreviewStale = false
                    currentFrame = min(currentFrame, max(0, cached.frames - 1))
                    statusMessage = "Loaded from render cache"
                    return
                }
                let preview = try await CoreRenderer.renderCapturingSelectedEffect(
                    input: input,
                    effects: graph,
                    drivers: drivers,
                    selectedNodeID: selectedEffectID
                )
                try Task.checkCancellation()
                let result = preview.output
                output = result
                selectedEffectCapture = preview.selectedEffect
                isPreviewStale = false
                currentFrame = min(currentFrame, max(0, result.frames - 1))
                statusMessage = "Rendered · \(result.frames) × \(result.width) × \(result.height)"
                try? await ProxyCache.shared.store(result, key: cacheKey)
                _ = try? await CacheManager.shared.trim()
                await refreshCacheSize()
            } catch is CancellationError {
                if previewGeneration == generation { statusMessage = "Render cancelled" }
            } catch {
                if previewGeneration == generation {
                    errorMessage = error.localizedDescription
                    statusMessage = "Render failed"
                }
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
        panel.nameFieldStringValue = "ChronoForge-Full-Render.mp4"
        panel.begin { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            self?.exportCurrent(to: url, format: .mp4)
        }
    }

    func choosePNGSequenceExportLocation() {
        guard source != nil else { return }
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.prompt = "Export Here"
        panel.message = "Choose a new or empty folder. Frames will be named ChronoForge_000001.png and so on."
        panel.begin { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            self?.exportCurrent(to: url, format: .pngSequence)
        }
    }

    func exportVideo(to url: URL) {
        exportCurrent(to: url, format: .mp4)
    }

    private func exportCurrent(to url: URL, format: RenderOutputFormat) {
        guard let source else { return }
        cancelWork()
        isExporting = true
        renderProgress = 0
        errorMessage = nil
        statusMessage = "Exporting \(format.title)…"
        let destinationAccess = url.startAccessingSecurityScopedResource()
        task = Task {
            defer {
                if destinationAccess { url.stopAccessingSecurityScopedResource() }
                isExporting = false
                renderProgress = nil
            }
            do {
                let renderEffects = try renderEffectChain()
                try await performExport(
                    source: source,
                    effects: renderEffects,
                    audioMode: canPreserveOriginalAudio ? audioMode : .none,
                    outputFormat: format,
                    outputFramesPerSecond: outputFramesPerSecond,
                    destination: url
                ) { fraction, stage in
                    Task { @MainActor in
                        self.renderProgress = fraction
                        self.statusMessage = stage
                    }
                }
                statusMessage = "\(format.title) export complete · \(url.lastPathComponent)"
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
            renderEffects = try renderEffectChain()
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
                mediaPool: self.mediaPool,
                audioMode: self.canPreserveOriginalAudio ? self.audioMode : .none,
                outputFramesPerSecond: self.outputFramesPerSecond,
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
                        mediaPool: item.mediaPool,
                        audioMode: item.audioMode,
                        outputFramesPerSecond: item.outputFramesPerSecond,
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
            if Task.isCancelled {
                statusMessage = "Render queue cancelled"
            } else {
                statusMessage = "Render queue finished"
                if let sound = NSSound(named: NSSound.Name("Glass")) ?? NSSound(named: NSSound.Name("Ping")) {
                    sound.play()
                } else {
                    NSSound.beep()
                }
            }
        }
    }

    private func performExport(
        source: DecodedProxy,
        effects: [EffectNode],
        mediaPool: [DecodedProxy]? = nil,
        audioMode: AudioMode,
        outputFormat: RenderOutputFormat = .mp4,
        outputFramesPerSecond: Double? = nil,
        destination: URL,
        progress: @escaping @Sendable (Double, String) -> Void
    ) async throws {
        let allMedia = mediaPool ?? self.mediaPool
        let accessed = allMedia.map { ($0.mediaSource, $0.mediaSource.startAccessingSecurityScopedResource()) }
        defer { for (mediaSource, granted) in accessed where granted { mediaSource.stopAccessingSecurityScopedResource() } }
        try await FullRenderPipeline.export(
            source: source,
            effects: effects,
            mediaPool: allMedia,
            audioMode: audioMode,
            outputFormat: outputFormat,
            outputFramesPerSecond: outputFramesPerSecond,
            to: destination,
            progress: progress
        )
    }

    func cancelWork() {
        autoPreviewTask?.cancel()
        autoPreviewTask = nil
        previewTask?.cancel()
        previewTask = nil
        task?.cancel()
        task = nil
        if isRendering { isRendering = false }
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

    func renderEffectChain() throws -> [EffectNode] {
        var chain = try activeEffectChain()
        if spatialPrefilter != .off || temporalPrefilter != .off {
            chain.append(.makePrefilter(spatial: spatialPrefilter, temporal: temporalPrefilter))
        }
        return chain
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
        let before = creativeState
        let previous = effects[index].inputNodeID
        effects[index].inputNodeID = inputID
        do {
            _ = try chainEnding(at: nodeID)
            if creativeState != before {
                registerUndo(restoring: before, actionName: "Change Effect Input")
                markEdited()
            }
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
