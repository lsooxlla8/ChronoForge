import SwiftUI

enum HelpSelection: Hashable, Sendable {
    case overview
    case workflow
    case controls
    case glossary
    case glossaryTerm(String)
    case effect(EffectKind)
}

struct EffectHelpEntry: Sendable {
    let kind: EffectKind
    let title: String
    let principle: String
    let summary: String
    let safeStart: String
    let controls: [String]
}

enum HelpCatalog {
    static let effects: [EffectHelpEntry] = [
        entry(.spaceTimeTranspose, "Space or time becomes another axis", "Literally swaps Width or Height with Time, creating cross-sections through the video volume.", "Start with X ↔ Time and Fit Source Size.", ["Swap axis — chooses Width or Height.", "Output size — Native Tensor keeps the literal swapped shape; Fit Source Size restores the visible canvas."]),
        entry(.lumaTimeShift, "Pixel value chooses a source moment", "Reads each output pixel from an earlier or later frame using luma, a colour channel, or alpha as the time map.", "Use 8–20 frames, Luma, and Clamp.", ["Shift multiplier — maximum temporal displacement in frames.", "Source — guide channel used as the time map.", "Edge behavior — Clamp, Wrap, or Mirror for out-of-range time."]),
        entry(.radialChronoFunnel, "Cartesian video → polar time field → image", "Builds braids, folds, and orbital echoes by mapping radius and angle into space and time.", "Use Time Loom, low Intensity, and Close angular seam.", ["Topology — Time Loom, Kaleido Fold, or Event Horizon.", "Center X/Y — normalized polar origin.", "Intensity — strength and direction.", "Angular twist — turns added across radius.", "Polar rotation — rotates the coordinate field, not the canvas.", "Close angular seam — makes the angular time field periodic.", "Edge behavior — handling outside the source."]),
        entry(.temporalPixelSort, "One pixel location → values across time → reordered line", "Sorts each spatial pixel's history instead of sorting rows in a single frame.", "Use Luma, Ascending, Threshold 0.", ["Criterion — Luma, Hue, or Saturation sorting key.", "Order — Ascending, Descending, Zigzag, or Center Out.", "Threshold — protects values below the cutoff.", "Hue Key Shift — rotates only the invisible hue sorting key."]),
        entry(.spectralFFTSwap, "Video volume → 3D spectrum → remapped axes → inverse FFT", "Moves or rotates frequency energy across Width, Height, and Time for global spectral structures.", "Use Fit Source Size, Normalize on, and a small X–Time angle.", ["Transform — Swap or Rotate.", "Swap/Rotation plane — affected axes.", "Spectral angle — rotation in degrees.", "Output size — literal tensor or fitted source size.", "Normalize output — remaps computed range to 0–1."]),
        entry(.dimensionalSplicer, "Driver B RGB → X/Y/Time coordinates in A", "Treats the second source as a three-channel coordinate map for sampling the primary source.", "Choose a high-contrast B and Linear interpolation.", ["Driver video (B) — coordinate-map source.", "Output X/Y/Time — assigns A coordinates or B's red/green/blue maps.", "Interpolation — Nearest, Linear 3D, or Cubic 3D."]),
        entry(.tensorDisplacement, "Driver B channel → bounded X/Y/Time offset in A", "Displaces the primary source through space and time using a selected channel of B.", "Start with a small X shift and Luma map.", ["Driver video (B) — displacement source.", "Time/X/Y shift — maximum offsets.", "Map channel — Luma, RGBA component.", "Size matching — Clamp, Stretch, or Crop B.", "Edge behavior — handling outside A."]),
        entry(.opticalFlowTimeWarp, "Frame pair → motion vectors → selective time bend", "Uses deterministic optical flow so motion direction and strength control local time sampling.", "Set Direction tolerance to 180°, then raise Time bend slowly.", ["Motion threshold — ignores weaker motion.", "Time bend — frames displaced per motion magnitude.", "Direction — preferred motion angle.", "Direction tolerance — accepted angular range.", "Edge behavior — temporal boundary handling."]),
        entry(.chronoFeedback, "Delayed frames → recursive colour or coordinate feedback", "Feeds bounded past and future samples into the current output for echoes and trails.", "Use Past delay 2, Past blend 0.2, Future blend 0.", ["Past/Future delay — sampled distance in frames.", "Past/Future blend — contribution strength.", "Blend mode — Add, Screen, Multiply, Lighten, Difference, or Displace."]),
        entry(.structuralDatamosh, "Trigger along an axis → hold previous samples", "Freezes runs across Time, Horizontal, or Vertical axes when edge, luma, or seeded random triggers fire.", "Use Time, Edge, low threshold, and a short hold.", ["Freeze axis — direction of held data.", "Trigger — Edge, Luma, or Random.", "Trigger threshold — activation cutoff.", "Maximum hold — longest held run.", "Trigger from darker values — reverses the luma test.", "Random probability — seeded trigger density."]),
        entry(.seamlessLoop, "Clip ends → transition/weave/ping-pong → loop", "Reshapes the end of the result so playback returns to the beginning without a hard cut.", "Try Crossfade with a short Transition.", ["Loop method — Crossfade, Luma Weave, Ping-Pong, Spectral Morph, or Difference Weave.", "Transition — frames used at the loop join.", "Weave softness — smoothness of luma/difference masks.", "Spectral amount/frequency blur — spectral transition controls.", "Phase curve and edge behavior — timing and boundaries."]),
        entry(.rgbTimeSlip, "R/G/B → independent moments and positions → recombine", "Offsets colour channels separately through time and space while preserving a coherent alpha policy.", "Use small opposite Red/Blue offsets.", ["Red/Green/Blue Offset — temporal channel shifts.", "Spatial Split — distance between channels.", "Direction — Horizontal, Vertical, or Radial.", "Edge behavior — sampling beyond source bounds."]),
        entry(.horizontalSyncLoss, "Seeded bands → per-band address shift", "Simulates sync tearing with resolution-independent bands and deterministic drift.", "Use low Shift and Tear Density.", ["Shift — fraction of frame width.", "Band Size — one pixel to one full-frame band.", "Drift Speed — band movement per frame.", "Tear Density — affected-band share.", "Pattern and direction — clean/noisy, horizontal/vertical.", "Edge behavior — shifted-address handling.", "Reseed — generates a new repeatable band field."]),
        entry(.chromaCarrierDrift, "RGB → luma + chroma → drift Cb/Cr → RGB", "Keeps current-frame luma while chroma carriers move independently in space and time.", "Use Split Cb–Cr with a small X offset.", ["Chroma X/Y/Time Offset — carrier displacement.", "Bleed — spatial chroma spread.", "Mode — Together, Split Cb–Cr, or Alternating.", "Edge behavior — displaced chroma sampling."]),
        entry(.strideError, "Frame buffer → deliberately wrong row address → safe wrap/mirror", "Creates memory-layout fractures without reading outside the current frame buffer.", "Use a tiny Stride Delta and Wrap.", ["Stride Delta — error relative to frame width.", "Base Offset — starting address shift.", "Temporal Drift — address movement per frame.", "Channel Mode — RGB together, separate, or alpha included.", "Address Edge — Wrap or Mirror."]),
        entry(.blockAddressCorruption, "Video → blocks → seeded address remap", "Replaces block addresses across space and time while holding each deterministic mapping for a bounded period.", "Use medium blocks, low Corruption, short Time Reach.", ["Block Size — normalized spatial block scale.", "Corruption — affected block share.", "Time Reach — maximum temporal address distance.", "Hold — mapping lifetime.", "Mapping — Swap, Repeat, Offset, or Cascade.", "Edge behavior and Reseed — address boundaries and deterministic pattern."]),
        entry(.bitplaneForge, "Linear colour → temporary integer bitplanes → operation → colour", "Quantizes channels and changes selected bitplanes for hard digital fractures.", "Use 8 Working Bits, a small Plane Mask, and Rotate.", ["Working Bits — temporary quantization depth.", "Plane Mask — bitplanes allowed to change.", "Shift — plane rotation distance.", "Operation — Shuffle, Rotate, Invert, or XOR.", "Channel — Luma, RGB, individual channel, or alpha.", "Reseed — changes Shuffle/XOR pattern."]),
        entry(.signalWeave, "A/B → alternating lines, fields, bands, or cells", "Interlaces two sources using a deterministic, drifting spatial pattern.", "Choose Lines, small Band Size, and low Irregularity.", ["Driver video (B) — alternate source.", "Pattern — Lines, Interlaced Fields, Bands, or Checker.", "Band Size — normalized cell/band scale.", "Phase Drift — pattern movement per frame.", "Irregularity — seeded boundary variation.", "B Time Offset — temporal alignment.", "Size Matching — Clamp, Stretch, or Crop."]),
        entry(.blockGraft, "Trigger map → copy complete blocks from B into A", "Grafts coherent colour blocks from a second source based on seeded, luma, difference, or edge triggers.", "Use Random, low Density, and a short Hold.", ["Driver video (B) — graft source.", "Block Size — normalized block scale.", "Density/Threshold — trigger strength.", "Hold — graft lifetime.", "B Time Offset — temporal alignment.", "Trigger — Random, A/B Luma, Difference, or A Edges.", "Size Matching and Reseed — B geometry and deterministic layout."]),
        entry(.channelTransplant, "Choose RGB/YCbCr components independently from A or B", "Builds a new colour sample by routing components from either source while retaining A's alpha.", "Start by replacing one chroma or RGB component.", ["Driver video (B) — donor source.", "Colour Model — RGB or YCbCr.", "Component pickers — Source A or Driver B for each component.", "B Time/X/Y Offset — donor alignment.", "Size Matching — Clamp, Stretch, or Crop."]),
        entry(.affinityMigration, "Pixels → colour-class cells → neighbour migration rounds", "Moves cell regions between nearby colour classes, accelerated by motion but never feeding previous-frame colour into the output.", "Use 3 classes, low Cell Scale, 2–3 Rounds.", ["Cell Scale — fraction of the larger frame dimension.", "Required Neighbour Majority — migration threshold.", "Rounds — simulation passes.", "Motion Response — extra migration in moving areas.", "Palette Classes — 2 through 8.", "Reseed — changes deterministic neighbour selection."])
    ]

    static let glossary: [(String, String)] = [
        ("Alpha", "Pixel opacity. ChronoForge carries it through the same spatial and temporal operations as colour."),
        ("Premultiplied", "RGB is stored multiplied by alpha, preventing bright fringes around transparent edges."),
        ("Proxy", "A reduced working copy used for fast Preview; export always returns to original-resolution media."),
        ("Luma", "Perceived brightness derived from colour."),
        ("Chroma", "Colour information separated from brightness."),
        ("FFT", "A transform that represents the video volume as spatial and temporal frequencies."),
        ("Stride", "The address distance between adjacent image rows."),
        ("Address", "A location used to read a sample from the video tensor."),
        ("Datamosh", "Visual displacement inspired by broken prediction or memory continuity; ChronoForge does it safely without corrupting a codec bitstream."),
        ("Seed", "A stored 64-bit number that makes random-looking choices exactly repeatable.")
    ]

    static func effect(_ kind: EffectKind) -> EffectHelpEntry? { effects.first { $0.kind == kind } }

    static func validationErrors() -> [String] {
        let expected = Set(EffectRegistry.addableDefinitions.map(\.kind))
        let documented = Set(effects.map(\.kind))
        var errors: [String] = []
        for kind in expected.subtracting(documented) { errors.append("Missing Help page for \(kind.title)") }
        for kind in documented.subtracting(expected) { errors.append("Help page exists for non-addable \(kind.title)") }
        for item in effects {
            if item.title != item.kind.title { errors.append("Help title differs from EffectDefinition for \(item.kind.title)") }
            if item.controls.isEmpty { errors.append("Help controls are empty for \(item.title)") }
        }
        return errors.sorted()
    }

    private static func entry(
        _ kind: EffectKind,
        _ principle: String,
        _ summary: String,
        _ safeStart: String,
        _ controls: [String]
    ) -> EffectHelpEntry {
        EffectHelpEntry(kind: kind, title: kind.title, principle: principle, summary: summary, safeStart: safeStart, controls: controls)
    }
}

struct ChronoForgeHelpView: View {
    @EnvironmentObject private var project: SessionStore
    @State private var query = ""

    private var filteredEffects: [EffectHelpEntry] {
        guard !query.isEmpty else { return HelpCatalog.effects }
        return HelpCatalog.effects.filter {
            [$0.title, $0.principle, $0.summary, $0.controls.joined(separator: " ")]
                .joined(separator: " ").localizedCaseInsensitiveContains(query)
        }
    }

    private var filteredGlossary: [(String, String)] {
        guard !query.isEmpty else { return [] }
        return HelpCatalog.glossary.filter {
            "\($0.0) \($0.1)".localizedCaseInsensitiveContains(query)
        }
    }

    var body: some View {
        NavigationSplitView {
            List(selection: $project.helpSelection) {
                Section("Start Here") {
                    Label("Overview", systemImage: "sparkles").tag(HelpSelection.overview)
                    Label("Session Workflow", systemImage: "arrow.triangle.branch").tag(HelpSelection.workflow)
                    Label("Controls & Export", systemImage: "slider.horizontal.3").tag(HelpSelection.controls)
                    Label("Glossary", systemImage: "text.book.closed").tag(HelpSelection.glossary)
                }
                Section("Effects") {
                    ForEach(filteredEffects, id: \.kind) { item in
                        Label(item.title, systemImage: item.kind.symbol).tag(HelpSelection.effect(item.kind))
                    }
                }
                if !filteredGlossary.isEmpty {
                    Section("Glossary Matches") {
                        ForEach(filteredGlossary, id: \.0) { term, _ in
                            Label(term, systemImage: "text.magnifyingglass")
                                .tag(HelpSelection.glossaryTerm(term))
                        }
                    }
                }
            }
            .searchable(text: $query, prompt: "Search Help")
            .navigationSplitViewColumnWidth(min: 230, ideal: 280)
        } detail: {
            ScrollView { detail.padding(28).frame(maxWidth: 760, alignment: .leading) }
        }
    }

    @ViewBuilder private var detail: some View {
        switch project.helpSelection {
        case .overview: overview
        case .workflow: workflow
        case .controls: controls
        case .glossary: glossary
        case .glossaryTerm(let term): glossaryTerm(term)
        case .effect(let kind):
            if let item = HelpCatalog.effect(kind) { effectPage(item) } else { overview }
        }
    }

    private var overview: some View {
        helpPage("ChronoForge Help", subtitle: "Offline video-file glitch laboratory") {
            Text("Import media, build an effect stack, experiment in the proxy Preview, then export from the original-resolution source. ChronoForge never modifies your input files and keeps no normal project document.")
            callout("Fast start", "Import Video or PNG Sequence → Add Effect → adjust Amount and parameters → compare with Before/After → Export.")
            Text("Use the sidebar to read workflow, controls, glossary terms, or a page for every production effect.")
        }
    }

    private var workflow: some View {
        helpPage("Session Workflow", subtitle: "One disposable session, recoverable only after interruption") {
            helpSection("A and B", "A is the primary picture. Effects in Multi-Source may use another imported source as Driver B.")
            helpSection("Preview", "High preserves source cadence up to 30 FPS; Standard targets faster interaction at up to 10 FPS. Preview is a proxy, while export returns to full resolution.")
            helpSection("A/B compare", "Before/After or the \\ key compares the stack result with the source. Compare Selected Effect prepares that node's immediate input/output only when requested.")
            helpSection("Automatic updates", "A committed edit schedules Preview automatically. Global effects wait longer to avoid repeated heavy analysis.")
            helpSection("Undo and recovery", "Creative edits support Undo/Redo. A hidden crash snapshot is removed on a normal quit; there is no Save/Open Project workflow.")
        }
    }

    private var controls: some View {
        helpPage("Controls & Export", subtitle: "What the shared toolbar and Inspector controls mean") {
            helpSection("Amount", "0% is the original input and 100% is the effect result when the effect preserves tensor shape.")
            helpSection("Amount Blend", "Normal mixes; Add and Screen brighten; Multiply darkens; Difference compares; Displace treats the effect result as a 3D coordinate field; XOR Glitch combines quantized values.")
            helpSection("Image AA / Time AA", "Optional spatial and temporal prefilters reduce resampling jaggies or flicker, but can soften hard glitches.")
            helpSection("FPS", "Result uses the rendered cadence. A preset or Custom value reinterprets completed frames without hidden frame synthesis. Original Audio is disabled when cadence changes.")
            helpSection("Queue", "Add to Queue stores an immutable snapshot. Cancel Preview does not cancel an already running queued export.")
            helpSection("Export Current Frame", "Maps the selected Viewer position to the final result timeline, evaluates the complete full-resolution stack, and writes one 8-bit RGBA PNG. It reuses the same full-render cache as video and PNG Sequence export.")
            helpSection("PNG Sequence / MP4", "PNG Sequence preserves alpha. MP4 is H.264 and may preserve original movie audio only when Result FPS is used.")
        }
    }

    private var glossary: some View {
        helpPage("Glossary", subtitle: "Video tensor and glitch terminology") {
            ForEach(HelpCatalog.glossary, id: \.0) { term, definition in helpSection(term, definition) }
        }
    }

    private func glossaryTerm(_ term: String) -> some View {
        let definition = HelpCatalog.glossary.first { $0.0 == term }?.1 ?? "Definition unavailable."
        return helpPage(term, subtitle: "Glossary") {
            Text(definition)
            Button("Show complete glossary") { project.helpSelection = .glossary }
        }
    }

    private func effectPage(_ item: EffectHelpEntry) -> some View {
        helpPage(item.title, subtitle: item.kind.definition.category.title) {
            HStack(spacing: 10) {
                processNode("Input", "rectangle")
                Image(systemName: "arrow.right")
                processNode(item.principle, item.kind.symbol)
                Image(systemName: "arrow.right")
                processNode("Output", "rectangle.fill")
            }
            .padding(.vertical, 8)
            Text(item.summary)
            callout("Safe starting point", item.safeStart)
            Text("Controls").font(.title2.weight(.semibold)).padding(.top, 8)
            ForEach(item.controls, id: \.self) { Text("• \($0)") }
            helpSection("Shared controls", "Enabled bypasses the node. Amount and Amount Blend control combination with the input. Reseed appears only when the effect uses a deterministic random field.")
        }
    }

    private func processNode(_ text: String, _ symbol: String) -> some View {
        VStack(spacing: 8) { Image(systemName: symbol).font(.title2); Text(text).font(.caption).multilineTextAlignment(.center) }
            .frame(maxWidth: .infinity, minHeight: 88).padding(8).background(.quaternary, in: RoundedRectangle(cornerRadius: 10))
    }

    private func helpPage<Content: View>(_ title: String, subtitle: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(title).font(.largeTitle.bold())
            Text(subtitle).font(.title3).foregroundStyle(.secondary)
            Divider()
            content()
        }
    }

    private func helpSection(_ title: String, _ text: String) -> some View {
        VStack(alignment: .leading, spacing: 4) { Text(title).font(.headline); Text(text) }
    }

    private func callout(_ title: String, _ text: String) -> some View {
        VStack(alignment: .leading, spacing: 4) { Label(title, systemImage: "lightbulb").font(.headline); Text(text) }
            .padding(12).frame(maxWidth: .infinity, alignment: .leading).background(.blue.opacity(0.10), in: RoundedRectangle(cornerRadius: 10))
    }
}
