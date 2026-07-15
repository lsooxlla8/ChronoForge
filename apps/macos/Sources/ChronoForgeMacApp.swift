import SwiftUI
import UniformTypeIdentifiers

@main
struct ChronoForgeMacApp: App {
    @StateObject private var project = ProjectStore()

    var body: some Scene {
        WindowGroup("ChronoForge") {
            WorkspaceView()
                .environmentObject(project)
                .frame(minWidth: 1120, minHeight: 720)
        }
        .defaultSize(width: 1480, height: 920)
        .commands {
            CommandGroup(after: .newItem) {
                Button("Import Video…") { project.showsImporter = true }
                    .keyboardShortcut("o", modifiers: .command)
                Button("Render Preview") { project.renderPreview() }
                    .keyboardShortcut("r", modifiers: .command)
            }
        }
    }
}

private struct WorkspaceView: View {
    @EnvironmentObject private var project: ProjectStore
    var body: some View {
        NavigationSplitView {
            sidebar
                .navigationSplitViewColumnWidth(min: 220, ideal: 250)
        } detail: {
            VStack(spacing: 0) {
                toolbar
                HSplitView {
                    VStack(spacing: 0) {
                        preview
                        Divider()
                        graph
                    }
                    inspector.frame(minWidth: 280, idealWidth: 310, maxWidth: 360)
                }
                Divider()
                timeline
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
    }

    private var sidebar: some View {
        List(selection: $project.selectedNodeID) {
            Section("Media") {
                if let source = project.source {
                    Label(source.displayName, systemImage: "film")
                    Text("\(Int(source.sourceSize.width)) × \(Int(source.sourceSize.height)) · \(source.sourceDuration, format: .number.precision(.fractionLength(1))) s")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } else {
                    Button("Import video…", systemImage: "plus.rectangle.on.folder") {
                        project.showsImporter = true
                    }
                }
            }
            Section("Effect graph") {
                ForEach(project.effects) { node in
                    Label(node.kind.title, systemImage: node.kind.symbol)
                        .tag(node.id)
                        .opacity(node.enabled ? 1 : 0.45)
                }
                .onMove(perform: project.moveEffect)
            }
        }
        .safeAreaInset(edge: .bottom) {
            HStack {
                Menu("Add effect", systemImage: "plus") {
                    ForEach(EffectKind.allCases) { kind in
                        Button(kind.title, systemImage: kind.symbol) { project.addEffect(kind) }
                    }
                }
                Spacer()
                Button("Remove", systemImage: "trash", role: .destructive) { project.removeSelectedEffect() }
                    .disabled(project.selectedNodeID == nil)
                    .labelStyle(.iconOnly)
            }
            .padding(10)
            .background(.bar)
        }
    }

    private var toolbar: some View {
        HStack(spacing: 12) {
            Label("ChronoForge", systemImage: "cube.transparent")
                .font(.headline)
            Text(project.statusMessage)
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(1)
            Spacer()
            Picker("Quality", selection: $project.quality) {
                ForEach(RenderQuality.allCases) { quality in Text(quality.title).tag(quality) }
            }
            .pickerStyle(.segmented)
            .frame(width: 190)
            if project.isRendering || project.isImporting || project.isExporting {
                ProgressView().controlSize(.small)
                Button("Cancel") { project.cancelWork() }
            }
            Button("Render", systemImage: "play.fill") { project.renderPreview() }
                .buttonStyle(.borderedProminent)
                .disabled(project.source == nil || project.isRendering || project.isImporting || project.isExporting)
            Button("Export MP4…", systemImage: "square.and.arrow.up") { project.chooseExportLocation() }
                .disabled(project.output == nil || project.isRendering || project.isImporting || project.isExporting)
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
                    Button("Import Video…") { project.showsImporter = true }
                }
                .foregroundStyle(.white.opacity(0.8))
            }
        }
        .frame(minHeight: 300)
    }

    private var graph: some View {
        ScrollView(.horizontal) {
            HStack(spacing: 52) {
                GraphCard(title: "Input", subtitle: project.source?.displayName ?? "No media", symbol: "film", selected: false)
                ForEach(project.effects) { node in
                    GraphCard(
                        title: node.kind.title,
                        subtitle: node.enabled ? "Enabled" : "Bypassed",
                        symbol: node.kind.symbol,
                        selected: project.selectedNodeID == node.id
                    )
                    .onTapGesture { project.selectedNodeID = node.id }
                }
                GraphCard(title: "Output", subtitle: project.output == nil ? "Not rendered" : "Proxy ready", symbol: "rectangle.inset.filled.and.person.filled", selected: false)
            }
            .padding(24)
        }
        .background(Color(nsColor: .controlBackgroundColor))
        .frame(minHeight: 170, idealHeight: 200, maxHeight: 230)
    }

    @ViewBuilder
    private var inspector: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Inspector").font(.headline)
            if let index = project.selectedNodeIndex {
                EffectInspector(node: $project.effects[index])
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

private struct GraphCard: View {
    let title: String
    let subtitle: String
    let symbol: String
    let selected: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label(title, systemImage: symbol).font(.caption.weight(.semibold))
            Text(subtitle).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
        }
        .padding(12)
        .frame(width: 210, alignment: .leading)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 10))
        .overlay { RoundedRectangle(cornerRadius: 10).stroke(selected ? Color.accentColor : .secondary.opacity(0.35), lineWidth: selected ? 2 : 1) }
        .shadow(color: .black.opacity(0.08), radius: 6, y: 3)
    }
}

private struct EffectInspector: View {
    @Binding var node: EffectNode

    var body: some View {
        Toggle("Enabled", isOn: $node.enabled)
        Text(node.kind.title).font(.title3.weight(.semibold))
        Divider()
        switch node.kind {
        case .spaceTimeTranspose:
            optionPicker("Swap axis", value: option(0), options: ["X ↔ Time", "Y ↔ Time"])
        case .lumaTimeShift:
            valueSlider("Shift multiplier", value: value(0), range: -100...100, format: "%.0f frames")
            optionPicker("Source", value: option(0), options: ["Luma", "Red", "Green", "Blue", "Alpha"])
            edgePicker(option(1))
        case .radialChronoFunnel:
            valueSlider("Center X", value: value(0), range: 0...1, format: "%.2f")
            valueSlider("Center Y", value: value(1), range: 0...1, format: "%.2f")
            valueSlider("Intensity", value: value(2), range: -1...1, format: "%.3f")
            edgePicker(option(0))
        case .temporalPixelSort:
            optionPicker("Criterion", value: option(0), options: ["Luma", "Hue", "Saturation"])
            optionPicker("Direction", value: option(1), options: ["Ascending", "Descending"])
            valueSlider("Threshold", value: value(0), range: 0...1, format: "%.2f")
        case .tensor3DRotation:
            valueSlider("X–Y", value: value(0), range: -180...180, format: "%.1f°")
            valueSlider("X–T", value: value(1), range: -180...180, format: "%.1f°")
            valueSlider("Y–T", value: value(2), range: -180...180, format: "%.1f°")
            optionPicker("Fill", value: option(0), options: ["Black", "Transparent", "Repeat"])
        case .spectralFFTSwap:
            optionPicker("Swap", value: option(0), options: ["X ↔ Time", "Y ↔ Time", "All axes"])
            Toggle("Normalize", isOn: Binding(
                get: { node.options[1] != 0 },
                set: { node.options[1] = $0 ? 1 : 0 }
            ))
        }
    }

    private func value(_ index: Int) -> Binding<Float> {
        Binding(get: { node.values[index] }, set: { node.values[index] = $0 })
    }

    private func option(_ index: Int) -> Binding<Int32> {
        Binding(get: { node.options[index] }, set: { node.options[index] = $0 })
    }

    private func valueSlider(_ title: String, value: Binding<Float>, range: ClosedRange<Float>, format: String) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack { Text(title); Spacer(); Text(String(format: format, value.wrappedValue)).monospacedDigit().foregroundStyle(.secondary) }
            Slider(value: value, in: range)
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
}
