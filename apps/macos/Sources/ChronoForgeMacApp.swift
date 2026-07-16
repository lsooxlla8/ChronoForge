import AppKit
import SwiftUI
import UniformTypeIdentifiers

@main
struct ChronoForgeMacApp: App {
    @StateObject private var project = ProjectStore()
    private static var globalShortcutMonitor: Any?

    init() {
        if Self.globalShortcutMonitor == nil {
            Self.globalShortcutMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { event in
                let modifiers = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
                guard !event.isARepeat else { return event }

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
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("New Project") { project.newProject() }
                    .keyboardShortcut("n", modifiers: .command)
                Button("Open Project…") { project.chooseProjectToOpen() }
                    .keyboardShortcut("o", modifiers: .command)
                Button("Import Video…") { project.addMedia() }
                    .keyboardShortcut("i", modifiers: .command)
                Divider()
                Button("Save Project") { project.saveProject() }
                    .keyboardShortcut("s", modifiers: .command)
                Button("Save Project As…") { project.saveProject(saveAs: true) }
                    .keyboardShortcut("s", modifiers: [.command, .shift])
                Divider()
                Button("Update Preview") { project.renderPreview() }
                    .keyboardShortcut("r", modifiers: .command)
                Button("Add to Render Queue…") { project.addCurrentRenderToQueue() }
                Button("Start Render Queue") { project.startRenderQueue() }
                    .keyboardShortcut("r", modifiers: [.command, .shift])
                    .disabled(project.renderQueue.isEmpty || project.isQueueRunning)
                Divider()
                Button("Play/Pause Preview") { project.togglePlayback() }
                    .keyboardShortcut(.space, modifiers: [])
            }
            CommandGroup(after: .saveItem) {
                Button("Clear Render Cache…") { project.showsClearCacheConfirmation = true }
                    .disabled(project.isRendering || project.isImporting || project.isExporting)
            }
            CommandGroup(replacing: .sidebar) {
                Button("Toggle Sidebar") {
                    NSApp.sendAction(#selector(NSSplitViewController.toggleSidebar(_:)), to: nil, from: nil)
                }
                .keyboardShortcut("s", modifiers: [.shift])
            }
        }
    }
}

private struct WorkspaceView: View {
    @EnvironmentObject private var project: ProjectStore
    @AppStorage("ChronoForge.darkAppearance") private var darkAppearance = false
    var body: some View {
        NavigationSplitView {
            sidebar
                .navigationSplitViewColumnWidth(min: 220, ideal: 250)
        } detail: {
            VStack(spacing: 0) {
                toolbar
                HSplitView {
                    preview
                    inspector.frame(minWidth: 280, idealWidth: 310, maxWidth: 360)
                }
                Divider()
                timeline
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .togglePreviewPlayback)) { _ in
            project.togglePlayback()
        }
        .preferredColorScheme(darkAppearance ? .dark : .light)
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
            Text("Source videos and saved projects will not be affected. Cached previews and full renders will be rebuilt when needed.")
        }
        .confirmationDialog(
            "Clear the entire effect stack?",
            isPresented: $project.showsClearEffectsConfirmation,
            titleVisibility: .visible
        ) {
            Button("Clear Effect Stack", role: .destructive) { project.clearEffectStack() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("This removes every effect. The imported video and render queue are not affected.")
        }
        .onOpenURL { url in
            if url.pathExtension.lowercased() == "chronoforge" {
                project.openProject(from: url)
            } else {
                project.importVideo(from: url)
            }
        }
    }

    private var sidebar: some View {
        List(selection: $project.selectedNodeID) {
            Section("Media") {
                if !project.mediaPool.isEmpty {
                    ForEach(project.mediaPool, id: \.id) { media in
                        VStack(alignment: .leading, spacing: 2) {
                            HStack {
                                Label(media.displayName, systemImage: project.source?.id == media.id ? "film.fill" : "film")
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
                            Button("Replace Video…") { project.replaceMedia(media.id) }
                            Button("Remove Video", role: .destructive) { project.removeMedia(media.id) }
                        }
                    }
                    Button("Add video…", systemImage: "plus.rectangle.on.folder") { project.addMedia() }
                        .font(.caption)
                } else {
                    Button("Import video…", systemImage: "plus.rectangle.on.folder") {
                        project.addMedia()
                    }
                    if project.hasRecovery {
                        Button("Recover last autosave", systemImage: "clock.arrow.circlepath") {
                            project.restoreRecovery()
                        }
                    }
                }
            }
            Section("Effect stack") {
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
        .safeAreaInset(edge: .bottom) {
            VStack(spacing: 7) {
                Menu("Add effect", systemImage: "plus") {
                    Section("ONE VIDEO · A") {
                        ForEach(EffectKind.singleInputKinds) { kind in
                            Button(kind.title, systemImage: kind.symbol) { project.addEffect(kind) }
                        }
                    }
                    Section("TWO VIDEOS · A + B") {
                        ForEach(EffectKind.twoInputKinds) { kind in
                            Button(kind.title, systemImage: kind.symbol) { project.addEffect(kind) }
                        }
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                Button("Clear Effect Stack", systemImage: "trash", role: .destructive) {
                    project.showsClearEffectsConfirmation = true
                }
                .disabled(project.effects.isEmpty)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(10)
            .background(.bar)
        }
        .overlay(alignment: .bottomLeading) {
            Text("Cache: \(project.cacheSizeDescription) · auto limit 8 GB")
                .font(.caption2)
                .foregroundStyle(.secondary)
                .padding(.leading, 12)
                .padding(.bottom, 80)
                .allowsHitTesting(false)
        }
    }

    private var toolbar: some View {
        VStack(spacing: 8) {
            HStack(spacing: 12) {
                Label(project.projectURL?.deletingPathExtension().lastPathComponent ?? "ChronoForge", systemImage: "cube.transparent")
                    .font(.headline)
                if project.isDirty { Text("Edited").font(.caption2).foregroundStyle(.orange) }
                Text(project.statusMessage)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                Spacer()
                if project.isRendering || project.isImporting || project.isExporting {
                    if let progress = project.renderProgress {
                        ProgressView(value: progress).frame(width: 90)
                    } else {
                        ProgressView().controlSize(.small)
                    }
                    Button("Cancel") { project.cancelWork() }
                }
                Button("Update Preview", systemImage: "eye") { project.renderPreview() }
                    .buttonStyle(.borderedProminent)
                    .disabled(project.source == nil || project.isRendering || project.isImporting || project.isExporting)
                    .help("Recalculate the small proxy shown in the viewer. This does not create a video file.")
                Button("Add to Queue", systemImage: "text.badge.plus") { project.addCurrentRenderToQueue() }
                    .disabled(project.source == nil || project.isRendering || project.isImporting || project.isExporting)
                Button("Export MP4…", systemImage: "square.and.arrow.up") { project.chooseExportLocation() }
                    .disabled(project.source == nil || project.isRendering || project.isImporting || project.isExporting)
                    .help("Render the current effect stack from the original media at full quality and write an MP4 file.")
                Divider().frame(height: 22)
                HStack(spacing: 6) {
                    Image(systemName: darkAppearance ? "moon.fill" : "sun.max.fill")
                        .foregroundStyle(.secondary)
                    Toggle("Dark appearance", isOn: $darkAppearance)
                        .labelsHidden()
                        .toggleStyle(.switch)
                }
                .help(darkAppearance ? "Switch to light appearance" : "Switch to dark appearance")
            }
            HStack(spacing: 12) {
                Picker("Proxy preview", selection: Binding(
                    get: { project.proxyQuality },
                    set: { project.changeProxyQuality(to: $0) }
                )) {
                    ForEach(ProxyQuality.allCases) { quality in Text(quality.title).tag(quality) }
                }
                .pickerStyle(.segmented)
                .frame(width: 245)
                .help(project.proxyQuality.detail)
                Picker("Spatial prefilter", selection: Binding(
                    get: { project.spatialPrefilter },
                    set: { project.setSpatialPrefilter($0) }
                )) {
                    ForEach(PrefilterStrength.allCases) { strength in Text(strength.title).tag(strength) }
                }
                .frame(width: 165)
                .help("Reduces jagged spatial edges and shimmer after geometric resampling. Light is a subtle axial low-pass; Strong is smoother.")
                Picker("Temporal prefilter", selection: Binding(
                    get: { project.temporalPrefilter },
                    set: { project.setTemporalPrefilter($0) }
                )) {
                    ForEach(PrefilterStrength.allCases) { strength in Text(strength.title).tag(strength) }
                }
                .frame(width: 175)
                .help("Blends adjacent output frames to reduce temporal aliasing and flicker. It can soften deliberately abrupt motion.")
                Picker("Audio", selection: Binding(
                    get: { project.audioMode },
                    set: {
                        project.audioMode = $0
                        if project.source != nil { project.markEdited(invalidatePreview: false) }
                    }
                )) {
                    ForEach(AudioMode.allCases) { mode in Text(mode.title).tag(mode) }
                }
                .frame(width: 170)
                Spacer()
            }
        }
        .padding(12)
    }

    private var preview: some View {
        ZStack {
            Color.black.opacity(0.94)
            if let tensor = project.displayedTensor,
               let image = TensorImage.make(from: tensor, frame: project.currentFrame) {
                Image(decorative: image, scale: 1)
                    .resizable()
                    .interpolation(.high)
                    .aspectRatio(contentMode: .fit)
                    .padding(16)
            } else {
                ContentUnavailableView {
                    Label("No video", systemImage: "film.stack")
                } description: {
                    Text("Import a movie to create a proxy tensor.")
                } actions: {
                    Button("Import Video…") { project.addMedia() }
                }
                .foregroundStyle(.white.opacity(0.8))
            }
            VStack {
                HStack {
                    Label("Proxy Preview · \(project.proxyQuality.title)", systemImage: "eye")
                        .font(.caption.weight(.semibold))
                        .padding(.horizontal, 9)
                        .padding(.vertical, 5)
                        .background(.black.opacity(0.68), in: Capsule())
                        .foregroundStyle(.white)
                    Spacer()
                    if project.isPreviewStale {
                        Label("Update Preview", systemImage: "exclamationmark.arrow.triangle.2.circlepath")
                            .font(.caption.weight(.semibold))
                            .padding(.horizontal, 9)
                            .padding(.vertical, 5)
                            .background(.orange.opacity(0.85), in: Capsule())
                            .foregroundStyle(.white)
                    }
                }
                Spacer()
            }
            .padding(12)
        }
        .frame(minHeight: 300)
    }

    @ViewBuilder
    private var inspector: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Inspector").font(.headline)
            if let nodeID = project.selectedNodeID, let selectedNode = project.effect(withID: nodeID) {
                LabeledContent("Input", value: project.nodeName(selectedNode.inputNodeID))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                EffectInspector(node: Binding(
                    get: { project.effect(withID: nodeID) ?? selectedNode },
                    set: { project.updateEffect($0) }
                ), mediaPool: project.mediaPool)
            } else {
                ContentUnavailableView("Select an effect", systemImage: "slider.horizontal.3")
            }
            Spacer()
        }
        .padding(16)
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
                Slider(
                    value: Binding(
                        get: { Double(project.currentFrame) },
                        set: { project.currentFrame = Int($0.rounded()) }
                    ),
                    in: 0...Double(max(1, tensor.frames - 1)),
                    step: 1
                )
            } else {
                Slider(value: .constant(0), in: 0...1).disabled(true)
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }
}

private extension Notification.Name {
    static let togglePreviewPlayback = Notification.Name("ChronoForge.togglePreviewPlayback")
}

private struct EffectInspector: View {
    @Binding var node: EffectNode
    let mediaPool: [DecodedProxy]

    var body: some View {
        Toggle("Enabled", isOn: $node.enabled)
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
            edgePicker(option(0))
            Text("Time Loom deforms radius, angle and fractional time into moving double braids. Kaleido Fold creates animated spatial sectors; Event Horizon pulls the image into orbiting temporal echoes.")
                .font(.caption)
                .foregroundStyle(.secondary)
        case .temporalPixelSort:
            optionPicker("Criterion", value: option(0), options: ["Luma", "Hue", "Saturation"])
            optionPicker("Direction", value: option(1), options: ["Ascending", "Descending"])
            valueSlider("Threshold", index: 0, range: 0...1, format: "%.4f")
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
            let axes = ["A · X position", "A · Y position", "A · Time position", "B Red → X", "B Green → Y", "B Blue → Time"]
            optionPicker("Output X", value: option(0), options: axes)
            optionPicker("Output Y", value: option(1), options: axes)
            optionPicker("Output Time", value: option(2), options: axes)
            optionPicker("Interpolation", value: option(3), options: ["Nearest", "Linear (3D)", "Cubic (3D)"])
            Text("A supplies the picture. B is an RGB coordinate map: red chooses where to read X in A, green chooses Y and blue chooses Time. X, Y and Time must each appear exactly once.")
                .font(.caption).foregroundStyle(.secondary)
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
            if node.options[1] == 2 {
                valueSlider("Random probability", index: 2, range: 0...1, format: "%.5f")
            }
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
                node = replacement
            }
        )
    }

    private func option(_ index: Int) -> Binding<Int32> {
        Binding(get: { node.options[index] }, set: { node.options[index] = $0 })
    }

    private func valueSlider(_ title: String, index: Int, range: ClosedRange<Float>, format: String) -> some View {
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
                    .help(String(format: format, binding.wrappedValue))
            }
            Slider(value: binding, in: range)
                .contextMenu {
                    Button("Reset to Default") { binding.wrappedValue = defaultValue }
                }
            Text(String(format: format, binding.wrappedValue))
                .font(.caption2)
                .monospacedDigit()
                .foregroundStyle(.secondary)
        }
    }

    private func optionPicker(_ title: String, value: Binding<Int32>, options: [String]) -> some View {
        Picker(title, selection: value) {
            ForEach(options.indices, id: \.self) { index in Text(options[index]).tag(Int32(index)) }
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
            .onSubmit(commit)
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

    private static func display(_ value: Float) -> String {
        var result = String(format: "%.5f", locale: Locale(identifier: "en_US_POSIX"), value)
        while result.contains(".") && result.last == "0" { result.removeLast() }
        if result.last == "." { result.removeLast() }
        return result
    }
}
