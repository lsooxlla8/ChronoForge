import AppKit
import SwiftUI
import UniformTypeIdentifiers

private final class ChronoForgeAppDelegate: NSObject, NSApplicationDelegate {
    func applicationWillTerminate(_ notification: Notification) {
        if !CommandLine.arguments.contains("--ui-acceptance") {
            SessionRecoveryStore.remove()
            try? CacheManager.clearOnTermination()
        }
    }
}

@main
struct ChronoForgeMacApp: App {
    @NSApplicationDelegateAdaptor(ChronoForgeAppDelegate.self) private var appDelegate
    @StateObject private var project = SessionStore()
    private static var globalShortcutMonitor: Any?

    init() {
        if Self.globalShortcutMonitor == nil {
            Self.globalShortcutMonitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp]) { event in
                let modifiers = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
                let isUnmodifiedBackslash = event.type == .keyDown &&
                    event.charactersIgnoringModifiers == "\\" &&
                    modifiers.intersection([.command, .option, .control, .shift]).isEmpty
                if isUnmodifiedBackslash && event.isARepeat { return nil }
                guard !event.isARepeat else { return event }

                guard event.type == .keyDown else { return event }

                if event.keyCode == 49,
                   modifiers.intersection([.command, .option, .control, .shift]).isEmpty {
                    if event.window?.firstResponder is NSTextView || event.window?.sheetParent != nil {
                        return event
                    }
                    NotificationCenter.default.post(name: .togglePreviewPlayback, object: nil)
                    return nil
                }

                if event.charactersIgnoringModifiers?.lowercased() == "s",
                   modifiers.contains(.shift),
                   modifiers.intersection([.command, .option, .control]).isEmpty {
                    NSApp.sendAction(#selector(NSSplitViewController.toggleSidebar(_:)), to: nil, from: nil)
                    return nil
                }

                return event
            }
        }
        if CommandLine.arguments.contains("--self-test") {
            Task {
                do {
                    try await SelfTestRunner.run()
                    print("ChronoForge full pipeline self-test passed")
                    Foundation.exit(EXIT_SUCCESS)
                } catch {
                    fputs("ChronoForge full pipeline self-test failed: \(error)\n", stderr)
                    Foundation.exit(EXIT_FAILURE)
                }
            }
        }
    }

    var body: some Scene {
        WindowGroup("ChronoForge") {
            WorkspaceView()
                .environmentObject(project)
                .frame(minWidth: 1120, minHeight: 720)
        }
        .defaultSize(width: 1480, height: 920)
        .windowStyle(.hiddenTitleBar)
        .commands {
            CommandGroup(replacing: .undoRedo) {
                Button("Undo") { project.undo() }
                    .keyboardShortcut("z", modifiers: .command)
                    .disabled(!project.canUndo)
                Button("Redo") { project.redo() }
                    .keyboardShortcut("z", modifiers: [.command, .shift])
                    .disabled(!project.canRedo)
            }
            CommandGroup(replacing: .newItem) {
                Button("Import Video…") { project.addMedia() }
                    .keyboardShortcut("i", modifiers: .command)
                Button("Import Image Sequence…") { project.addImageSequence() }
                    .keyboardShortcut("i", modifiers: [.command, .shift])
                Divider()
                Button("Add to Render Queue…") { project.addCurrentRenderToQueue() }
                    .keyboardShortcut("r", modifiers: [.command, .option])
                    .disabled(project.source == nil)
                Button("Start Render Queue") { project.startRenderQueue() }
                    .keyboardShortcut("r", modifiers: [.command, .shift])
                    .disabled(project.renderQueue.isEmpty || project.isQueueRunning)
                Divider()
                Button("Random Stack") { project.replaceWithRandomStack() }
                    .keyboardShortcut("r", modifiers: [.shift])
                    .disabled(project.source == nil)
                Button("Clear Effect Stack") { project.clearEffectStack() }
                    .keyboardShortcut(.delete, modifiers: [.shift])
                    .disabled(project.effects.isEmpty)
                Divider()
                Button("Play/Pause Preview") { project.togglePlayback() }
                    .keyboardShortcut(.space, modifiers: [])
            }
            CommandGroup(after: .saveItem) {
                Button("Export Current Frame…") { project.chooseCurrentFrameExportLocation() }
                    .keyboardShortcut("e", modifiers: [.command, .shift])
                    .disabled(project.source == nil || project.isRendering || project.isImporting || project.isExporting)
                Divider()
                Button("Clear Render Cache…") { project.showsClearCacheConfirmation = true }
                    .disabled(project.isRendering || project.isImporting || project.isExporting)
            }
            CommandGroup(replacing: .sidebar) {
                Button("Toggle Sidebar") {
                    NSApp.sendAction(#selector(NSSplitViewController.toggleSidebar(_:)), to: nil, from: nil)
                }
                .keyboardShortcut("s", modifiers: [.shift])
                Button("Before / After") {
                    NotificationCenter.default.post(name: .toggleSourceComparison, object: nil)
                }
                    .keyboardShortcut("\\", modifiers: [])
            }
            CommandGroup(replacing: .help) {
                Button("ChronoForge Help") {
                    project.helpSelection = .overview
                    NotificationCenter.default.post(name: .showChronoForgeHelp, object: nil)
                }
                .keyboardShortcut("?", modifiers: .command)
            }
        }
        Window("ChronoForge Help", id: "help") {
            ChronoForgeHelpView().environmentObject(project)
        }
        .defaultSize(width: 1040, height: 760)
    }
}

private struct WorkspaceView: View {
    @EnvironmentObject private var project: SessionStore
    @Environment(\.openWindow) private var openWindow
    @AppStorage("ChronoForge.darkAppearance") private var darkAppearance = false
    @State private var isComparingSource = false
    @State private var isComparingSelectedEffect = false
    @State private var isShowingSelectedEffectInput = false
    var body: some View {
        NavigationSplitView {
            sidebar
                .navigationSplitViewColumnWidth(min: 220, ideal: 250)
        } detail: {
            VStack(spacing: 0) {
                toolbar
                HSplitView {
                    preview
                    inspector.frame(width: 340)
                }
                Divider()
                timeline
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .togglePreviewPlayback)) { _ in
            project.togglePlayback()
        }
        .onReceive(NotificationCenter.default.publisher(for: .toggleSourceComparison)) { _ in
            isComparingSource.toggle()
        }
        .onReceive(NotificationCenter.default.publisher(for: .showChronoForgeHelp)) { _ in
            openWindow(id: "help")
        }
        .onReceive(NotificationCenter.default.publisher(for: NSMenu.didBeginTrackingNotification)) { _ in
            // SwiftUI menu tracking and high-frequency @Published playback updates
            // can deadlock older Apple Silicon/macOS combinations. Freeze the
            // viewer at its current frame before AppKit enters menu tracking.
            project.stopPlayback()
        }
        .onChange(of: project.selectedNodeID) { _, _ in
            isComparingSelectedEffect = false
            isShowingSelectedEffectInput = false
        }
        .onChange(of: project.isPreviewStale) { _, stale in
            if stale {
                isComparingSelectedEffect = false
                isShowingSelectedEffectInput = false
            }
        }
        .preferredColorScheme(darkAppearance ? .dark : .light)
        .toolbar {
            ToolbarItem(placement: .navigation) {
                Button {
                    darkAppearance.toggle()
                } label: {
                    Image(systemName: darkAppearance ? "moon.fill" : "sun.max.fill")
                }
                .focusable(false)
                .accessibilityLabel(darkAppearance ? "Switch to light appearance" : "Switch to dark appearance")
                .help(darkAppearance ? "Switch to light appearance" : "Switch to dark appearance")
            }
            ToolbarItem(placement: .principal) {
                Spacer()
            }
            ToolbarItemGroup(placement: .confirmationAction) {
                if project.isRendering || project.isImporting || project.isExporting {
                    if let progress = project.renderProgress {
                        ProgressView(value: progress).frame(width: 72)
                    } else {
                        ProgressView().controlSize(.small)
                    }
                    Button("Cancel") { project.cancelWork() }
                }
                Button { project.addCurrentRenderToQueue() } label: {
                    Label("Add to Queue", systemImage: "text.badge.plus")
                        .labelStyle(.titleAndIcon)
                }
                    .disabled(project.source == nil || project.isRendering || project.isImporting || project.isExporting)
                Menu {
                    Button("Current Frame as PNG…", systemImage: "photo") { project.chooseCurrentFrameExportLocation() }
                    Divider()
                    Button("MP4 Video…", systemImage: "film") { project.chooseExportLocation() }
                    Button("PNG Sequence…", systemImage: "photo.stack") { project.choosePNGSequenceExportLocation() }
                } label: {
                    Label("Export…", systemImage: "square.and.arrow.up")
                        .labelStyle(.titleAndIcon)
                }
                .disabled(project.source == nil || project.isRendering || project.isImporting || project.isExporting)
                .help("Render the current effect stack from the original media at full quality.")
            }
        }
        .fileImporter(
            isPresented: $project.showsImporter,
            allowedContentTypes: [.movie, .mpeg4Movie, .quickTimeMovie],
            allowsMultipleSelection: false
        ) { result in
            if case .success(let urls) = result, let url = urls.first {
                project.importVideo(from: url)
            } else if case .failure(let error) = result {
                project.errorMessage = error.localizedDescription
            }
        }
        .alert("ChronoForge", isPresented: Binding(
            get: { project.errorMessage != nil },
            set: { if !$0 { project.errorMessage = nil } }
        )) {
            Button("OK") { project.errorMessage = nil }
        } message: {
            Text(project.errorMessage ?? "Unknown error")
        }
        .confirmationDialog(
            "Clear the \(project.cacheSizeDescription) render cache?",
            isPresented: $project.showsClearCacheConfirmation,
            titleVisibility: .visible
        ) {
            Button("Clear Render Cache", role: .destructive) { project.clearRenderCache() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Source videos and the current session will not be affected. Cached previews and full renders will be rebuilt when needed.")
        }
        .confirmationDialog(
            "Recover interrupted session?",
            isPresented: Binding(
                get: { project.hasInterruptedSession && project.source == nil && !project.isImporting },
                set: { _ in }
            ),
            titleVisibility: .visible
        ) {
            Button("Recover Interrupted Session") { project.restoreRecovery() }
            Button("Start Fresh", role: .destructive) { project.startFreshSession() }
        } message: {
            Text("ChronoForge found a hidden recovery snapshot from a session that did not close normally.")
        }
        .onOpenURL { url in
            project.importVideo(from: url)
        }
        .task {
            await project.loadUIAcceptanceFixtureIfNeeded()
        }
    }

    private var sidebar: some View {
        List(selection: $project.selectedNodeID) {
            Section("Media") {
                if !project.mediaPool.isEmpty {
                    ForEach(project.mediaPool, id: \.id) { media in
                        VStack(alignment: .leading, spacing: 2) {
                            HStack {
                                Label(
                                    media.displayName,
                                    systemImage: media.mediaSource.isMovie
                                        ? (project.source?.id == media.id ? "film.fill" : "film")
                                        : (project.source?.id == media.id ? "photo.stack.fill" : "photo.stack")
                                )
                                if project.source?.id == media.id {
                                    Text("A").font(.caption2.bold()).padding(.horizontal, 5).background(.quaternary, in: Capsule())
                                }
                            }
                            Text("\(Int(media.sourceSize.width)) × \(Int(media.sourceSize.height)) · \(media.sourceDuration, format: .number.precision(.fractionLength(1))) s · \(media.sourceFrameCountIsExact ? "" : "≈")\(media.sourceFrameCount) frames")
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                        .contextMenu {
                            if project.source?.id != media.id {
                                Button("Use as Primary (A)") { project.setPrimaryMedia(media.id) }
                            }
                            Button("Replace Media with Video…") { project.replaceMedia(media.id) }
                            Button("Remove Media", role: .destructive) { project.removeMedia(media.id) }
                        }
                    }
                    Button("Add video…", systemImage: "plus.rectangle.on.folder") { project.addMedia() }
                        .font(.caption)
                    Button("Add PNG sequence…", systemImage: "photo.stack") { project.addImageSequence() }
                        .font(.caption)
                } else {
                    Menu("Import media…", systemImage: "plus.rectangle.on.folder") {
                        Button("Video…") { project.addMedia() }
                        Button("PNG Sequence…") { project.addImageSequence() }
                    }
                }
            }
            Section {
                effectStackControls
                ForEach(project.effects) { node in
                    VStack(alignment: .leading, spacing: 2) {
                        Label(node.kind.title, systemImage: node.kind.symbol)
                        Text(node.modeTitle)
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                    .tag(node.id)
                    .opacity(node.enabled ? 1 : 0.45)
                        .contextMenu {
                            Button(node.enabled ? "Bypass" : "Enable", systemImage: node.enabled ? "pause.circle" : "play.circle") {
                                project.toggleEffectEnabled(node.id)
                            }
                            Button("Duplicate", systemImage: "plus.square.on.square") {
                                project.duplicateEffect(node.id)
                            }
                            Divider()
                            Button("Delete", systemImage: "trash", role: .destructive) {
                                project.deleteEffect(node.id)
                            }
                        }
                }
                .onMove(perform: project.moveEffect)
            } header: {
                Text("Effect stack")
            }
            if !project.renderQueue.isEmpty {
                Section("Render queue") {
                    ForEach(project.renderQueue) { item in
                        HStack {
                            Label(item.destinationURL.lastPathComponent, systemImage: item.status.symbol)
                                .lineLimit(1)
                            Spacer()
                            Text(item.status.title).font(.caption2).foregroundStyle(.secondary)
                            Button("Remove", systemImage: "xmark", role: .destructive) {
                                project.removeQueueItem(item.id)
                            }
                            .labelStyle(.iconOnly)
                            .disabled(project.isQueueRunning)
                        }
                    }
                    Button("Start Queue", systemImage: "play.fill") { project.startRenderQueue() }
                        .disabled(project.isQueueRunning || !project.renderQueue.contains(where: { $0.status == .waiting }))
                }
            }
        }
        .safeAreaInset(edge: .bottom, spacing: 0) {
            VStack(alignment: .leading, spacing: 5) {
                Divider()
                Text("Cache: \(project.cacheSizeDescription) · auto limit 8 GB")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(.bar)
        }
    }

    private var effectStackControls: some View {
        HStack(spacing: 6) {
            Menu("Add effect", systemImage: "plus") {
                ForEach(EffectCategory.allCases) { category in
                    let definitions = EffectRegistry.definitions(in: category)
                    if !definitions.isEmpty {
                        Section(category.title) {
                            ForEach(definitions, id: \.kind) { definition in
                                Button(definition.title, systemImage: definition.symbol) {
                                    project.addEffect(definition.kind)
                                }
                            }
                        }
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            Button("Random", systemImage: "dice") { project.replaceWithRandomStack() }
                .labelStyle(.iconOnly)
                .disabled(project.source == nil)
                .help("Random: replace the stack with 1–3 compatible effects. Undo restores the previous stack.")
            Button("Clear", systemImage: "trash", role: .destructive) {
                project.clearEffectStack()
            }
            .labelStyle(.iconOnly)
            .disabled(project.effects.isEmpty)
            .help("Clear the effect stack")
        }
    }

    private var toolbar: some View {
        VStack(spacing: 7) {
            ViewThatFits(in: .horizontal) {
                HStack(spacing: 12) {
                    beforeAfterControl
                    imageSettingsControls
                    outputSettingsControls
                    Spacer(minLength: 0)
                }
                VStack(alignment: .leading, spacing: 7) {
                    HStack(spacing: 12) {
                        beforeAfterControl
                        imageSettingsControls
                        Spacer(minLength: 0)
                    }
                    HStack(spacing: 12) {
                        outputSettingsControls
                        Spacer(minLength: 0)
                    }
                }
            }
        }
        .padding(12)
    }

    private var beforeAfterControl: some View {
        Toggle(isOn: $isComparingSource) {
            Label("Before / After", systemImage: "rectangle.on.rectangle")
        }
            .toggleStyle(.button)
            .disabled(project.source == nil || project.output == nil)
            .help("Toggle between the source Before view and the processed After view. The \\ key uses the same toggle.")
            .fixedSize(horizontal: true, vertical: false)
    }

    private var imageSettingsControls: some View {
        HStack(spacing: 12) {
            Picker("Preview", selection: Binding(
                get: { project.proxyQuality },
                set: { project.changeProxyQuality(to: $0) }
            )) {
                ForEach(ProxyQuality.allCases) { quality in Text(quality.title).tag(quality) }
            }
            .pickerStyle(.segmented)
            .frame(width: 205)
            .help(project.proxyQuality.detail)
            Picker("Image AA", selection: Binding(
                get: { project.spatialPrefilter },
                set: { project.setSpatialPrefilter($0) }
            )) {
                ForEach(PrefilterStrength.allCases) { strength in Text(strength.title).tag(strength) }
            }
            .frame(width: 145)
            .help("Reduces jagged spatial edges and shimmer after geometric resampling. Light is a subtle axial low-pass; Strong is smoother.")
            Picker("Time AA", selection: Binding(
                get: { project.temporalPrefilter },
                set: { project.setTemporalPrefilter($0) }
            )) {
                ForEach(PrefilterStrength.allCases) { strength in Text(strength.title).tag(strength) }
            }
            .frame(width: 145)
            .help("Blends adjacent output frames to reduce temporal aliasing and flicker. It can soften deliberately abrupt motion.")
        }
        .fixedSize(horizontal: true, vertical: false)
    }

    private var outputSettingsControls: some View {
        HStack(spacing: 12) {
            Picker("Audio", selection: Binding(
                get: { project.audioMode },
                set: { project.setAudioMode($0) }
            )) {
                ForEach(AudioMode.allCases) { mode in
                    Text(mode.title)
                        .tag(mode)
                        .disabled(mode == .preserveOriginal && !project.canPreserveOriginalAudio)
                }
            }
            .frame(width: 150)
            .help(project.canPreserveOriginalAudio
                ? "Keep the original movie audio without re-timing it."
                : "Original audio is unavailable for image sequences or reinterpreted playback FPS.")
            Picker("FPS", selection: Binding(
                get: { project.playbackFPSPreset },
                set: { project.setPlaybackFPSPreset($0) }
            )) {
                ForEach(PlaybackFPSPreset.allCases) { preset in Text(preset.title).tag(preset) }
            }
            .frame(width: 125)
            if project.playbackFPSPreset == .custom {
                TextField("FPS", value: Binding(
                    get: { project.customPlaybackFPS },
                    set: { project.setCustomPlaybackFPS($0) }
                ), format: .number.precision(.fractionLength(0...3)))
                .frame(width: 65)
                .textFieldStyle(.roundedBorder)
            }
            if let duration = project.outputDuration {
                Text("\(duration, format: .number.precision(.fractionLength(2))) s output")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .fixedSize(horizontal: true, vertical: false)
    }

    private var preview: some View {
        ZStack {
            Color.black.opacity(0.94)
            if let presentation = previewPresentation,
               let image = TensorImage.make(from: presentation.tensor, frame: presentation.frame) {
                Image(decorative: image, scale: 1)
                    .resizable()
                    .interpolation(.high)
                    .aspectRatio(contentMode: .fit)
                    .padding(16)
            } else {
                ContentUnavailableView {
                    Label("No media", systemImage: "film.stack")
                } description: {
                    Text("Import a movie or PNG sequence to create a proxy tensor.")
                } actions: {
                    Menu("Import Media…") {
                        Button("Video…") { project.addMedia() }
                        Button("PNG Sequence…") { project.addImageSequence() }
                    }
                }
                .foregroundStyle(.white.opacity(0.8))
            }
            VStack {
                HStack {
                    Label(
                        previewModeLabel,
                        systemImage: previewModeSymbol
                    )
                        .font(.caption.weight(.semibold))
                        .padding(.horizontal, 9)
                        .padding(.vertical, 5)
                        .background(.black.opacity(0.68), in: Capsule())
                        .foregroundStyle(.white)
                    Spacer()
                }
                Spacer()
            }
            .padding(12)
        }
        .frame(minHeight: 300)
    }

    private var previewPresentation: (tensor: VideoTensorData, frame: Int)? {
        guard let result = project.displayedTensor else { return nil }
        if isComparingSource, let source = project.source?.tensor {
            return normalizedPresentation(of: source, relativeTo: result)
        }
        if isComparingSelectedEffect,
           let capture = project.selectedEffectCaptureForSelection {
            let tensor = isShowingSelectedEffectInput ? capture.input : capture.output
            return normalizedPresentation(of: tensor, relativeTo: result)
        }
        return (result, project.currentFrame)
    }

    private func normalizedPresentation(
        of tensor: VideoTensorData,
        relativeTo result: VideoTensorData
    ) -> (tensor: VideoTensorData, frame: Int) {
        let position = Double(project.currentFrame) / Double(max(1, result.frames - 1))
        let frame = Int((position * Double(max(0, tensor.frames - 1))).rounded())
        return (tensor, frame)
    }

    private var previewModeLabel: String {
        if isComparingSource { return "Source A" }
        if isComparingSelectedEffect, project.selectedEffectCaptureForSelection != nil {
            return isShowingSelectedEffectInput ? "Selected Effect Input" : "Selected Effect Output"
        }
        return "Preview · \(project.proxyQuality.title)"
    }

    private var previewModeSymbol: String {
        if isComparingSource { return "film" }
        if isComparingSelectedEffect, project.selectedEffectCaptureForSelection != nil {
            return isShowingSelectedEffectInput ? "arrow.right.to.line" : "arrow.right.circle"
        }
        return "eye"
    }

    @ViewBuilder
    private var inspector: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Text("Inspector").font(.headline)
                    Spacer()
                    if let nodeID = project.selectedNodeID, let node = project.effect(withID: nodeID) {
                        Button {
                            project.helpSelection = .effect(node.kind)
                            openWindow(id: "help")
                        } label: {
                            Image(systemName: "questionmark.circle")
                        }
                        .buttonStyle(.plain)
                        .help("Open Help for \(node.kind.title)")
                    }
                }
                if let nodeID = project.selectedNodeID, let selectedNode = project.effect(withID: nodeID) {
                    LabeledContent("Input", value: project.nodeName(selectedNode.inputNodeID))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    selectedEffectCompareControls
                    EffectInspector(node: Binding(
                        get: { project.effect(withID: nodeID) ?? selectedNode },
                        set: { project.updateEffect($0) }
                    ), mediaPool: project.mediaPool) { editing in
                        if editing {
                            project.beginContinuousEffectEdit()
                        } else {
                            project.endContinuousEffectEdit()
                        }
                    } onReseed: {
                        project.reseedEffect(nodeID)
                    }
                } else {
                    ContentUnavailableView("No effect is selected", systemImage: "slider.horizontal.3")
                }
                Spacer()
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(16)
        }
    }

    @ViewBuilder
    private var selectedEffectCompareControls: some View {
        let isAvailable = project.selectedEffectCaptureForSelection != nil
        let canRequest = project.selectedNodeID != nil && project.output != nil && !project.isPreviewStale
        Toggle("Compare Selected Effect", isOn: Binding(
            get: { isComparingSelectedEffect },
            set: { enabled in
                isComparingSelectedEffect = enabled
                if enabled {
                    project.captureSelectedEffectForComparison()
                } else {
                    isShowingSelectedEffectInput = false
                }
            }
        ))
        .toggleStyle(.switch)
        .disabled(!canRequest)
        .help(isAvailable
            ? "Compare the selected effect's immediate input and output."
            : "The comparison is prepared only when requested, so normal preview updates use less memory.")
        if isComparingSelectedEffect, isAvailable {
            Button("Selected Effect Input", systemImage: "rectangle.on.rectangle") {}
                .simultaneousGesture(
                    DragGesture(minimumDistance: 0)
                        .onChanged { _ in isShowingSelectedEffectInput = true }
                        .onEnded { _ in isShowingSelectedEffectInput = false }
                )
                .help("Hold to show the selected effect's immediate input. Release to show its immediate output.")
        }
    }

    private var timeline: some View {
        VStack(spacing: 7) {
            HStack {
                Button(project.isPlaying ? "Pause" : "Play", systemImage: project.isPlaying ? "pause.fill" : "play.fill") {
                    project.togglePlayback()
                }
                .labelStyle(.iconOnly)
                .disabled(project.displayedTensor == nil)
                Label("Frame \(project.currentFrame + 1)", systemImage: "timeline.selection")
                Spacer()
                if let tensor = project.displayedTensor {
                    Text("\(tensor.frames) frames · \(tensor.framesPerSecond, format: .number.precision(.fractionLength(2))) fps")
                        .foregroundStyle(.secondary)
                }
            }
            .font(.caption)
            if let tensor = project.displayedTensor {
                ResettableSlider(
                    value: Binding(
                        get: { Double(project.currentFrame) },
                        set: { project.currentFrame = Int($0.rounded()) }
                    ),
                    in: 0...Double(max(1, tensor.frames - 1)),
                    step: 1,
                    defaultValue: 0
                )
            } else {
                ResettableSlider(value: .constant(0), in: 0...1, defaultValue: 0).disabled(true)
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }
}

private extension Notification.Name {
    static let togglePreviewPlayback = Notification.Name("ChronoForge.togglePreviewPlayback")
    static let toggleSourceComparison = Notification.Name("ChronoForge.toggleSourceComparison")
    static let showChronoForgeHelp = Notification.Name("ChronoForge.showHelp")
}

private struct EffectInspector: View {
    @Binding var node: EffectNode
    let mediaPool: [DecodedProxy]
    let onContinuousEditChanged: (Bool) -> Void
    let onReseed: () -> Void

    var body: some View {
        Toggle("Enabled", isOn: $node.enabled)
        amountControl
        if node.kind.definition.usesRandomSeed {
            Button("Reseed", systemImage: "arrow.triangle.2.circlepath", action: onReseed)
                .disabled((node.kind == .horizontalSyncLoss && node.options[0] != 0) ||
                          (node.kind == .bitplaneForge && node.options[0] != 0 && node.options[0] != 3) ||
                          (node.kind == .blockGraft && node.options[0] != 0))
                .help("Generate a new deterministic pattern for this effect.")
        }
        Text(node.kind.title).font(.title3.weight(.semibold))
        Divider()
        switch node.kind {
        case .spaceTimeTranspose, .tensor3DRotation:
            Picker("Mode", selection: tensorTransformMode) {
                Text("Axis Swap").tag(Int32(0))
                Text("3D Rotation").tag(Int32(1))
            }
            if node.kind == .spaceTimeTranspose {
                optionPicker("Swap axis", value: option(0), options: ["X ↔ Time", "Y ↔ Time"])
                optionPicker("Output size", value: option(1), options: ["Native Tensor", "Fit Source Size"])
                Text("Fit Source Size keeps the visible width and height of the input. Native Tensor performs a literal axis swap; its duration still follows the swapped spatial axis.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                valueSlider("X–Y", index: 0, range: -180...180, format: "%.3f°")
                valueSlider("X–Time", index: 1, range: -180...180, format: "%.3f°")
                valueSlider("Y–Time", index: 2, range: -180...180, format: "%.3f°")
                optionPicker("Fill", value: option(0), options: ["Black", "Transparent", "Repeat", "Fit Source Size"])
            }
        case .lumaTimeShift:
            valueSlider("Shift multiplier", index: 0, range: -100...100, format: "%.0f frames")
            optionPicker("Source", value: option(0), options: ["Luma", "Red", "Green", "Blue", "Alpha"])
            edgePicker(option(1))
        case .radialChronoFunnel:
            optionPicker("Topology", value: option(1), options: ["Time Loom", "Kaleido Fold", "Event Horizon"])
            valueSlider("Center X", index: 0, range: 0...1, format: "%.3f")
            valueSlider("Center Y", index: 1, range: 0...1, format: "%.3f")
            valueSlider("Intensity", index: 2, range: -1...1, format: "%.4f")
            valueSlider("Angular twist", index: 3, range: -3...3, format: "%.3f turns")
            valueSlider("Polar rotation", index: 4, range: -180...180, format: "%.2f°")
            if node.options[1] == 0 {
                Toggle("Close angular seam", isOn: Binding(
                    get: { node.options[2] != 0 },
                    set: { node.options[2] = $0 ? 1 : 0 }
                ))
            }
            edgePicker(option(0))
            Text("Polar rotation offsets the coordinate system without rotating the canvas. Close angular seam replaces the branch-cut time ramp with a periodic mapping, removing the radial join line. Time Loom deforms radius, angle and fractional time into moving double braids; Kaleido Fold creates animated sectors; Event Horizon pulls the image into temporal echoes.")
                .font(.caption)
                .foregroundStyle(.secondary)
        case .temporalPixelSort:
            optionPicker("Criterion", value: option(0), options: ["Luma", "Hue", "Saturation"])
            optionPicker("Order", value: option(1), options: ["Ascending", "Descending", "Zigzag", "Center Out"])
            valueSlider("Threshold", index: 0, range: 0...1, format: "%.4f")
            if node.options[0] == 1 {
                valueSlider("Hue Key Shift", index: 1, range: -180...180, format: "%.1f°")
                Text("Hue Key Shift rotates only the invisible sorting key. It changes temporal ordering without recolouring the output pixels.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        case .spectralFFTSwap:
            optionPicker("Transform", value: option(3), options: ["Swap", "Rotate"])
            optionPicker(node.options[3] == 0 ? "Swap" : "Rotation plane", value: option(0), options: node.options[3] == 0 ? ["X ↔ Time", "Y ↔ Time", "All axes"] : ["X–Time", "Y–Time", "X–Y"])
            if node.options[3] == 1 {
                valueSlider("Spectral angle", index: 0, range: -180...180, format: "%.3f°")
            }
            optionPicker("Output size", value: option(2), options: ["Native Tensor", "Fit Source Size"])
            Toggle("Normalize output to 0–1", isOn: Binding(
                get: { node.options[1] != 0 },
                set: { node.options[1] = $0 ? 1 : 0 }
            ))
            Text("Normalize remaps the darkest and brightest computed values to 0 and 1. A pure swap preserves the range, so the difference can be subtle; it becomes more visible after spectral rotation. Fit Source Size is the default.")
                .font(.caption)
                .foregroundStyle(.secondary)
        case .selectivePrefilter:
            Text("The output prefilter is controlled from the toolbar and is not part of the editable effect stack.")
                .font(.caption)
                .foregroundStyle(.secondary)
        case .dimensionalSplicer:
            driverPicker()
            spaceTimeAxisPicker("Output X", index: 0)
            spaceTimeAxisPicker("Output Y", index: 1)
            spaceTimeAxisPicker("Output Time", index: 2)
            optionPicker("Interpolation", value: option(3), options: ["Nearest", "Linear (3D)", "Cubic (3D)"])
            Text("A supplies the picture. B is an RGB coordinate map: red chooses X, green chooses Y and blue chooses Time. Choosing an axis already used elsewhere swaps the two assignments automatically.")
                .font(.caption).foregroundStyle(.secondary)
                .onAppear(perform: normalizeSpaceTimeAxes)
        case .tensorDisplacement:
            driverPicker()
            valueSlider("Time shift", index: 0, range: -240...240, format: "%.2f frames")
            valueSlider("X shift", index: 1, range: -1000...1000, format: "%.2f px")
            valueSlider("Y shift", index: 2, range: -1000...1000, format: "%.2f px")
            optionPicker("Map channel", value: option(0), options: ["Luma", "Red", "Green", "Blue", "Alpha"])
            optionPicker("Size matching", value: option(1), options: ["Clamp", "Stretch", "Crop"])
            edgePicker(option(2))
        case .opticalFlowTimeWarp:
            valueSlider("Motion threshold", index: 0, range: 0...2, format: "%.4f px")
            valueSlider("Time bend", index: 1, range: -120...120, format: "%.3f frames/px")
            valueSlider("Direction", index: 2, range: -180...180, format: "%.2f°")
            valueSlider("Direction tolerance", index: 3, range: 0...180, format: "%.2f°")
            edgePicker(option(0))
            Text("Only motion near the chosen direction bends time. Set tolerance to 180° to include all movement.")
                .font(.caption).foregroundStyle(.secondary)
        case .chronoFeedback:
            valueSlider("Past delay", index: 0, range: 0...300, format: "%.0f frames")
            valueSlider("Past blend", index: 1, range: 0...1, format: "%.4f")
            valueSlider("Future delay", index: 2, range: 0...300, format: "%.0f frames")
            valueSlider("Future blend", index: 3, range: 0...1, format: "%.4f")
            optionPicker("Blend mode", value: option(0), options: ["Add", "Screen", "Multiply", "Lighten", "Difference", "Displace"])
            if node.options[0] == 5 {
                Text("Past feedback displaces X; the future frame displaces Y. Mid-gray is neutral, while Past/Future Blend control displacement strength.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        case .structuralDatamosh:
            optionPicker("Freeze axis", value: option(0), options: ["Time", "Horizontal", "Vertical"])
            optionPicker("Trigger", value: option(1), options: ["Edge", "Luma", "Random"])
            valueSlider("Trigger threshold", index: 0, range: 0...1, format: "%.4f")
            valueSlider("Maximum hold", index: 1, range: 0...600, format: "%.0f samples")
            if node.options[1] == 1 {
                Toggle("Trigger from darker values", isOn: Binding(
                    get: { node.options[2] != 0 },
                    set: { node.options[2] = $0 ? 1 : 0 }
                ))
            }
            if node.options[1] == 2 {
                valueSlider("Random probability", index: 2, range: 0...1, format: "%.5f")
            }
        case .seamlessLoop:
            optionPicker("Loop method", value: option(0), options: ["Crossfade", "Luma Weave", "Ping-Pong", "Spectral Morph", "Difference Weave"])
            if node.options[0] != 2 {
                valueSlider("Transition", index: 0, range: 2...600, format: "%.0f frames")
                optionPicker("Transition position", value: option(1), options: ["Start", "End"])
                if node.options[0] == 1 || node.options[0] == 4 {
                    valueSlider("Weave softness", index: 1, range: 0.01...0.5, format: "%.4f")
                    Text(node.options[0] == 1
                         ? "Luma Weave lets different image details cross the seam at different moments, hiding the full-frame dissolve of a normal crossfade."
                         : "Difference Weave finds the least-visible crossover moment independently for every pixel, then controls its transition width with Weave Softness.")
                        .font(.caption).foregroundStyle(.secondary)
                } else if node.options[0] == 3 {
                    valueSlider("Spectral amount", index: 2, range: 0...1, format: "%.4f")
                    valueSlider("Frequency blur", index: 3, range: 0...1, format: "%.4f")
                    optionPicker("Phase timing", value: option(2), options: ["Even", "Hold Tail", "Hold Head"])
                    Text("Spectral Amount mixes between a normal dissolve and FFT texture morphing. Frequency Blur suppresses high-frequency detail near the middle of the overlap. Phase can travel by the shortest path or stay locked to either endpoint texture.")
                        .font(.caption).foregroundStyle(.secondary)
                } else {
                    Text("Overlaps the end with the beginning and shortens the result by the transition length.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Text(node.options[1] == 0
                     ? "Start places the generated overlap in the first Transition frames."
                     : "End rotates the same seamless result so the generated overlap occupies the final Transition frames.")
                    .font(.caption).foregroundStyle(.secondary)
            } else {
                Text("Plays forward and then backward. This always closes cleanly, but the direction reverses at both ends.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Label("Keep Seamless Loop last in the stack so later effects do not reopen the seam.", systemImage: "arrow.down.to.line")
                .font(.caption).foregroundStyle(.secondary)
        case .rgbTimeSlip:
            valueSlider("Red Offset", index: 0, range: -240...240, format: "%.2f frames")
            valueSlider("Green Offset", index: 1, range: -240...240, format: "%.2f frames")
            valueSlider("Blue Offset", index: 2, range: -240...240, format: "%.2f frames")
            valueSlider("Spatial Split", index: 3, range: -200...200, format: "%.2f px")
            optionPicker("Split Axis", value: option(0), options: ["Horizontal", "Vertical", "Radial"])
            edgePicker(option(1))
            Text("Each colour channel reads an independent moment. Alpha stays on the current frame and RGB is re-premultiplied to it.")
                .font(.caption)
                .foregroundStyle(.secondary)
        case .horizontalSyncLoss:
            optionPicker("Direction", value: option(2), options: ["Horizontal", "Vertical"])
            valueSlider("Shift", index: 0, range: 0...1, format: "%.3f × width")
            valueSlider("Band Size", index: 1, range: 0...1, format: "%.4f")
            valueSlider("Drift Speed", index: 2, range: -10...10, format: "%.3f bands/frame")
            valueSlider("Tear Density", index: 3, range: 0...1, format: "%.4f")
            optionPicker("Driver", value: option(0), options: ["Deterministic Noise", "Luma", "Edges"])
            edgePicker(option(1))
            Text("Band Size is resolution-independent: 0 is one pixel and 1 is one full-frame band. Horizontal shifts row bands; Vertical shifts column bands. Noise is deterministic and responds to Reseed.")
                .font(.caption)
                .foregroundStyle(.secondary)
        case .chromaCarrierDrift:
            valueSlider("Chroma X Offset", index: 0, range: -500...500, format: "%.2f px")
            valueSlider("Chroma Y Offset", index: 1, range: -500...500, format: "%.2f px")
            valueSlider("Chroma Time Offset", index: 2, range: -120...120, format: "%.2f frames")
            valueSlider("Bleed", index: 3, range: 0...100, format: "%.2f px")
            optionPicker("Mode", value: option(0), options: ["Together", "Split Cb–Cr", "Alternating"])
            edgePicker(option(1))
            Text("Luma remains on the current frame while the two chroma carriers drift and bleed independently.")
                .font(.caption)
                .foregroundStyle(.secondary)
        case .strideError:
            valueSlider("Stride Delta", index: 0, range: -0.5...0.5, format: "%.4f × width")
            valueSlider("Base Offset", index: 1, range: -1...1, format: "%.4f frame lengths")
            valueSlider("Temporal Drift", index: 2, range: -1...1, format: "%.5f frame lengths/frame")
            optionPicker("Channel Mode", value: option(0), options: ["RGB Together", "Separate Channels", "Alpha Included"])
            optionPicker("Address Edge", value: option(1), options: ["Wrap", "Mirror"])
            Text("The current frame is read with a deliberately wrong row stride. Every address is wrapped or mirrored inside that frame buffer.")
                .font(.caption)
                .foregroundStyle(.secondary)
        case .blockAddressCorruption:
            valueSlider("Block Size", index: 0, range: 0...1, format: "%.4f")
            valueSlider("Corruption", index: 1, range: 0...1, format: "%.4f")
            valueSlider("Time Reach", index: 2, range: 0...240, format: "%.0f frames")
            valueSlider("Hold", index: 3, range: 1...240, format: "%.0f frames")
            optionPicker("Mapping", value: option(0), options: ["Swap", "Repeat", "Offset", "Cascade"])
            edgePicker(option(1))
            Text("Block Size is resolution-independent: 0 is one pixel and 1 covers the frame. Corrupted blocks retain their mapping for Hold frames.")
                .font(.caption)
                .foregroundStyle(.secondary)
        case .bitplaneForge:
            valueSlider("Working Bits", index: 0, range: 2...16, format: "%.0f bits")
            valueSlider("Plane Mask", index: 1, range: 0...65535, format: "%.0f")
            valueSlider("Shift", index: 2, range: -15...15, format: "%.0f planes")
            optionPicker("Operation", value: option(0), options: ["Shuffle", "Rotate", "Invert", "XOR"])
            optionPicker("Channel", value: option(1), options: ["Luma", "RGB Together", "Red", "Green", "Blue", "Alpha"])
            Text("The mask selects which bitplanes may change after temporary integer quantization. Reseed applies only to Shuffle and XOR.")
                .font(.caption)
                .foregroundStyle(.secondary)
        case .signalWeave:
            driverPicker()
            optionPicker("Pattern", value: option(0), options: ["Lines", "Interlaced Fields", "Bands", "Checker"])
            valueSlider("Band Size", index: 0, range: 0...1, format: "%.4f")
            valueSlider("Phase Drift", index: 1, range: -20...20, format: "%.3f units/frame")
            valueSlider("Irregularity", index: 2, range: 0...1, format: "%.4f")
            valueSlider("B Time Offset", index: 3, range: -240...240, format: "%.0f frames")
            optionPicker("Size Matching", value: option(1), options: ["Clamp", "Stretch", "Crop"])
            Text("Band Size is resolution-independent: 0 is one pixel and 1 spans the frame. Alternating rows, fields, bands or checker cells come from A and B.")
                .font(.caption)
                .foregroundStyle(.secondary)
        case .blockGraft:
            driverPicker()
            valueSlider("Block Size", index: 0, range: 0...1, format: "%.4f")
            valueSlider(node.options[0] == 0 ? "Density" : "Threshold", index: 1, range: 0...1, format: "%.4f")
            valueSlider("Hold", index: 2, range: 1...240, format: "%.0f frames")
            valueSlider("B Time Offset", index: 3, range: -240...240, format: "%.0f frames")
            optionPicker("Trigger", value: option(0), options: ["Random", "A Luma", "B Luma", "Difference", "A Edges"])
            optionPicker("Size Matching", value: option(1), options: ["Clamp", "Stretch", "Crop"])
            Text("Block Size is resolution-independent: 0 is one pixel and 1 covers the frame. Selected blocks from B replace A as complete colour samples.")
                .font(.caption)
                .foregroundStyle(.secondary)
        case .channelTransplant:
            driverPicker()
            optionPicker("Colour Model", value: option(3), options: ["RGB", "YCbCr"])
            let componentNames = node.options[3] == 0 ? ["Red", "Green", "Blue"] : ["Luma Y", "Chroma Cb", "Chroma Cr"]
            optionPicker(componentNames[0], value: option(0), options: ["Source A", "Driver B"])
            optionPicker(componentNames[1], value: option(1), options: ["Source A", "Driver B"])
            optionPicker(componentNames[2], value: option(2), options: ["Source A", "Driver B"])
            valueSlider("B Time Offset", index: 0, range: -240...240, format: "%.0f frames")
            valueSlider("B X Offset", index: 1, range: -1000...1000, format: "%.0f px")
            valueSlider("B Y Offset", index: 2, range: -1000...1000, format: "%.0f px")
            optionPicker("Size Matching", value: option(4), options: ["Clamp", "Stretch", "Crop"])
            Text("Each destination component independently stays with A or comes from a time- and space-offset sample of B. Alpha remains A's.")
                .font(.caption)
                .foregroundStyle(.secondary)
        case .affinityMigration:
            valueSlider("Cell Scale", index: 0, range: 0...0.5, format: "%.3f × frame")
            valueSlider("Required Neighbour Majority", index: 1, range: 0...1, format: "%.0f%%", displayMultiplier: 100)
            valueSlider("Rounds", index: 2, range: 1...12, format: "%.0f")
            valueSlider("Motion Response", index: 3, range: 0...1, format: "%.3f")
            optionPicker("Palette Classes", value: option(0), options: ["2", "3", "4", "5", "6", "7", "8"])
            Text("Cell Scale is a fraction of the larger frame dimension. Required Neighbour Majority is the share of nearby cells in another colour class needed before this cell migrates. Motion Response makes moving areas flow into the new state faster.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    private var tensorTransformMode: Binding<Int32> {
        Binding(
            get: { node.kind == .tensor3DRotation ? 1 : 0 },
            set: { mode in
                let kind: EffectKind = mode == 0 ? .spaceTimeTranspose : .tensor3DRotation
                guard kind != node.kind else { return }
                var replacement = EffectNode.make(kind, inputNodeID: node.inputNodeID)
                replacement.id = node.id
                replacement.enabled = node.enabled
                replacement.amount = node.amount
                replacement.amountBlendMode = node.amountBlendMode
                replacement.randomSeed = node.randomSeed
                if !replacement.supportsAmount {
                    replacement.amount = 1
                    replacement.amountBlendMode = .normal
                }
                node = replacement
            }
        )
    }

    private func option(_ index: Int) -> Binding<Int32> {
        Binding(get: { node.options[index] }, set: {
            node.options[index] = $0
            if !node.supportsAmount {
                node.amount = 1
                node.amountBlendMode = .normal
            }
        })
    }

    private var amountControl: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack {
                Text("Amount")
                Spacer()
                ExactValueField(value: $node.amount, range: 0...1)
                    .help("Enter an exact Amount from 0 to 1.")
            }
            ResettableSlider(
                value: Binding(
                    get: { Double(node.amount) },
                    set: { node.amount = Float($0) }
                ),
                in: 0...1,
                defaultValue: 1,
                onEditingChanged: onContinuousEditChanged
            )
            Text(String(format: "%.0f%%", node.amount * 100))
                .font(.caption2)
                .monospacedDigit()
                .foregroundStyle(.secondary)
            Picker("Amount Blend", selection: $node.amountBlendMode) {
                ForEach(AmountBlendMode.allCases) { mode in
                    Text(mode.title).tag(mode)
                }
            }
            Text(node.amountBlendMode == .displace
                 ? "The effect output becomes a 3D displacement field for the original image."
                 : node.amountBlendMode == .xorGlitch
                 ? "XOR Glitch combines quantized source and effect values into hard digital colour fractures."
                 : "Amount mixes the effect using the selected compositing operation. For shape-changing effects, the input is fitted to the effect output first.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    private func valueSlider(
        _ title: String,
        index: Int,
        range: ClosedRange<Float>,
        format: String,
        displayMultiplier: Float = 1
    ) -> some View {
        let binding = Binding<Float>(
            get: { node.values[index] },
            set: { node.values[index] = min(max($0, range.lowerBound), range.upperBound) }
        )
        let defaultValue = EffectNode.make(node.kind).values[index]
        return VStack(alignment: .leading, spacing: 5) {
            HStack {
                Text(title)
                Spacer()
                ExactValueField(value: binding, range: range)
                    .help(String(format: format, binding.wrappedValue * displayMultiplier))
            }
            ResettableSlider(
                value: Binding(
                    get: { Double(binding.wrappedValue) },
                    set: { binding.wrappedValue = Float($0) }
                ),
                in: Double(range.lowerBound)...Double(range.upperBound),
                defaultValue: Double(defaultValue),
                onEditingChanged: { editing in
                    onContinuousEditChanged(editing)
                    if editing { NSApp.keyWindow?.makeFirstResponder(nil) }
                }
            )
            Text(String(format: format, binding.wrappedValue * displayMultiplier))
                .font(.caption2)
                .monospacedDigit()
                .foregroundStyle(.secondary)
        }
    }

    private func optionPicker(_ title: String, value: Binding<Int32>, options: [String]) -> some View {
        Picker(title, selection: value) {
            ForEach(options.indices, id: \.self) { index in Text(options[index]).tag(Int32(index)) }
        }
        .simultaneousGesture(TapGesture().onEnded { NSApp.keyWindow?.makeFirstResponder(nil) })
    }

    private func spaceTimeAxisPicker(_ title: String, index: Int) -> some View {
        let axes = ["A · X position", "A · Y position", "A · Time position", "B Red → X", "B Green → Y", "B Blue → Time"]
        return Picker(title, selection: spaceTimeAxisBinding(index)) {
            ForEach(axes.indices, id: \.self) { candidate in
                Text(axes[candidate])
                    .tag(Int32(candidate))
            }
        }
        .simultaneousGesture(TapGesture().onEnded { NSApp.keyWindow?.makeFirstResponder(nil) })
    }

    private func spaceTimeAxisBinding(_ index: Int) -> Binding<Int32> {
        Binding(
            get: { node.options[index] },
            set: { selection in
                let previous = node.options[index]
                let semantic = Int(selection) % 3
                if let occupied = (0..<3).first(where: {
                    $0 != index && Int(node.options[$0]) % 3 == semantic
                }) {
                    node.options[occupied] = previous
                }
                node.options[index] = selection
                normalizeSpaceTimeAxes()
            }
        )
    }

    private func normalizeSpaceTimeAxes() {
        var used = Set<Int>()
        for index in 0..<3 {
            let current = min(max(Int(node.options[index]), 0), 5)
            let semantic = current % 3
            if used.contains(semantic) {
                let sourceBase = current >= 3 ? 3 : 0
                let replacement = (0..<3).first(where: { !used.contains($0) }) ?? index
                node.options[index] = Int32(sourceBase + replacement)
                used.insert(replacement)
            } else {
                node.options[index] = Int32(current)
                used.insert(semantic)
            }
        }
    }

    private func edgePicker(_ value: Binding<Int32>) -> some View {
        optionPicker("Edge behavior", value: value, options: ["Clamp", "Wrap", "Mirror"])
    }

    private func driverPicker() -> some View {
        Picker("Driver video (B)", selection: $node.driverMediaID) {
            Text("Choose a video…").tag(nil as UUID?)
            ForEach(mediaPool, id: \.id) { media in
                Text(media.displayName).tag(Optional(media.id))
            }
        }
    }
}

private struct ExactValueField: View {
    @Binding var value: Float
    let range: ClosedRange<Float>
    @State private var text: String
    @FocusState private var isFocused: Bool

    init(value: Binding<Float>, range: ClosedRange<Float>) {
        _value = value
        self.range = range
        _text = State(initialValue: Self.display(value.wrappedValue))
    }

    var body: some View {
        TextField("Value", text: $text)
            .multilineTextAlignment(.trailing)
            .monospacedDigit()
            .textFieldStyle(.roundedBorder)
            .frame(width: 92)
            .focused($isFocused)
            .onSubmit(finishEditing)
            .onExitCommand(perform: cancelEditing)
            .onChange(of: isFocused) { _, focused in
                if !focused { commit() }
            }
            .onChange(of: value) { _, updated in
                if !isFocused { text = Self.display(updated) }
            }
    }

    private func commit() {
        let normalized = text.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: ",", with: ".")
        guard let parsed = Float(normalized), parsed.isFinite else {
            text = Self.display(value)
            return
        }
        value = min(max(parsed, range.lowerBound), range.upperBound)
        text = Self.display(value)
    }

    private func finishEditing() {
        commit()
        isFocused = false
        DispatchQueue.main.async { NSApp.keyWindow?.makeFirstResponder(nil) }
    }

    private func cancelEditing() {
        text = Self.display(value)
        isFocused = false
        DispatchQueue.main.async { NSApp.keyWindow?.makeFirstResponder(nil) }
    }

    private static func display(_ value: Float) -> String {
        var result = String(format: "%.5f", locale: Locale(identifier: "en_US_POSIX"), value)
        while result.contains(".") && result.last == "0" { result.removeLast() }
        if result.last == "." { result.removeLast() }
        return result
    }
}

private struct ResettableSlider: NSViewRepresentable {
    @Binding var value: Double
    let range: ClosedRange<Double>
    var step: Double?
    let defaultValue: Double
    var onEditingChanged: (Bool) -> Void

    init(
        value: Binding<Double>,
        in range: ClosedRange<Double>,
        step: Double? = nil,
        defaultValue: Double,
        onEditingChanged: @escaping (Bool) -> Void = { _ in }
    ) {
        _value = value
        self.range = range
        self.step = step
        self.defaultValue = defaultValue
        self.onEditingChanged = onEditingChanged
    }

    func makeCoordinator() -> Coordinator {
        Coordinator(value: $value, onEditingChanged: onEditingChanged)
    }

    func makeNSView(context: Context) -> DirectResetNSSlider {
        let slider = DirectResetNSSlider(
            value: value,
            minValue: range.lowerBound,
            maxValue: range.upperBound,
            target: context.coordinator,
            action: #selector(Coordinator.valueChanged(_:))
        )
        slider.isContinuous = true
        slider.resetValue = defaultValue
        slider.editingChanged = context.coordinator.editingChanged
        let doubleClick = NSClickGestureRecognizer(
            target: context.coordinator,
            action: #selector(Coordinator.resetSlider(_:))
        )
        doubleClick.numberOfClicksRequired = 2
        doubleClick.buttonMask = 1
        slider.addGestureRecognizer(doubleClick)
        let rightClick = NSClickGestureRecognizer(
            target: context.coordinator,
            action: #selector(Coordinator.resetSlider(_:))
        )
        rightClick.numberOfClicksRequired = 1
        rightClick.buttonMask = 2
        slider.addGestureRecognizer(rightClick)
        if let step {
            slider.numberOfTickMarks = 0
            slider.allowsTickMarkValuesOnly = false
            slider.altIncrementValue = step
        }
        return slider
    }

    func updateNSView(_ slider: DirectResetNSSlider, context: Context) {
        context.coordinator.value = $value
        context.coordinator.onEditingChanged = onEditingChanged
        slider.minValue = range.lowerBound
        slider.maxValue = range.upperBound
        slider.resetValue = defaultValue
        slider.editingChanged = context.coordinator.editingChanged
        slider.doubleValue = value
    }

    final class Coordinator: NSObject {
        var value: Binding<Double>
        var onEditingChanged: (Bool) -> Void

        init(value: Binding<Double>, onEditingChanged: @escaping (Bool) -> Void) {
            self.value = value
            self.onEditingChanged = onEditingChanged
        }

        var editingChanged: (Bool) -> Void {
            { [weak self] editing in self?.onEditingChanged(editing) }
        }

        @objc func valueChanged(_ sender: NSSlider) {
            value.wrappedValue = sender.doubleValue
        }

        @objc func resetSlider(_ recognizer: NSClickGestureRecognizer) {
            guard recognizer.state == .ended,
                  let slider = recognizer.view as? DirectResetNSSlider else { return }
            let resolvedValue = min(max(slider.resetValue, slider.minValue), slider.maxValue)
            slider.doubleValue = resolvedValue
            value.wrappedValue = resolvedValue
            NSApp.keyWindow?.makeFirstResponder(nil)
        }
    }
}

private final class DirectResetNSSlider: NSSlider {
    var resetValue = 0.0
    var editingChanged: ((Bool) -> Void)?

    override func mouseDown(with event: NSEvent) {
        editingChanged?(true)
        super.mouseDown(with: event)
        editingChanged?(false)
    }
}
