import SwiftUI

@main
struct ChronoForgeMacApp: App {
    var body: some Scene {
        WindowGroup("ChronoForge") {
            WorkspaceView()
                .frame(minWidth: 1080, minHeight: 700)
        }
        .defaultSize(width: 1440, height: 900)
    }
}

private struct GraphNode: Identifiable {
    let id = UUID()
    var title: String
    var subtitle: String
    var point: CGPoint
    var tint: Color
}

private struct WorkspaceView: View {
    @State private var quality = RenderQuality.proxy
    @State private var nodes: [GraphNode] = [
        .init(title: "Input", subtitle: "clip.mov · 1920 × 1080", point: .init(x: 130, y: 200), tint: .blue),
        .init(title: "Temporal Pixel Sort", subtitle: "Luma · Ascending", point: .init(x: 410, y: 200), tint: .purple),
        .init(title: "Space-Time Transpose", subtitle: "X ↔ T", point: .init(x: 700, y: 200), tint: .orange),
        .init(title: "Output", subtitle: "H.264 · Proxy", point: .init(x: 990, y: 200), tint: .green),
    ]
    @State private var selectedNode: UUID?

    var body: some View {
        NavigationSplitView {
            List(selection: $selectedNode) {
                Section("Project") {
                    Label("Untitled chronograph", systemImage: "cube.transparent")
                    Label("Media", systemImage: "film.stack")
                }
                Section("Nodes") {
                    ForEach(nodes) { node in
                        Label(node.title, systemImage: "circle.hexagongrid")
                            .tag(node.id)
                    }
                }
            }
            .navigationSplitViewColumnWidth(min: 205, ideal: 240)
        } detail: {
            VStack(spacing: 0) {
                toolbar
                HStack(spacing: 0) {
                    graphCanvas
                    Divider()
                    inspector
                        .frame(width: 278)
                }
                Divider()
                timeline
            }
            .background(.background)
        }
    }

    private var toolbar: some View {
        HStack(spacing: 12) {
            Label("ChronoForge", systemImage: "sparkles.rectangle.stack")
                .font(.headline)
            Spacer()
            Picker("Render quality", selection: $quality) {
                ForEach(RenderQuality.allCases) { value in
                    Text(value.title).tag(value)
                }
            }
            .pickerStyle(.segmented)
            .frame(width: 190)
            Button("Render", systemImage: "play.fill") {}
                .buttonStyle(.borderedProminent)
            Button("Export", systemImage: "square.and.arrow.up") {}
                .buttonStyle(.bordered)
        }
        .padding(12)
    }

    private var graphCanvas: some View {
        GeometryReader { proxy in
            ZStack(alignment: .topLeading) {
                Canvas { context, size in
                    let grid = Path { path in
                        stride(from: 0, through: size.width, by: 32).forEach { x in
                            path.move(to: .init(x: x, y: 0))
                            path.addLine(to: .init(x: x, y: size.height))
                        }
                        stride(from: 0, through: size.height, by: 32).forEach { y in
                            path.move(to: .init(x: 0, y: y))
                            path.addLine(to: .init(x: size.width, y: y))
                        }
                    }
                    context.stroke(grid, with: .color(.secondary.opacity(0.12)), lineWidth: 0.5)
                    let ordered = nodes.sorted { $0.point.x < $1.point.x }
                    for pair in zip(ordered, ordered.dropFirst()) {
                        var connection = Path()
                        connection.move(to: .init(x: pair.0.point.x + 112, y: pair.0.point.y))
                        connection.addCurve(
                            to: .init(x: pair.1.point.x - 112, y: pair.1.point.y),
                            control1: .init(x: pair.0.point.x + 172, y: pair.0.point.y),
                            control2: .init(x: pair.1.point.x - 172, y: pair.1.point.y)
                        )
                        context.stroke(connection, with: .color(.secondary.opacity(0.55)), lineWidth: 2)
                    }
                }
                ForEach(nodes) { node in
                    NodeCard(node: node, isSelected: selectedNode == node.id)
                        .position(node.point)
                        .onTapGesture { selectedNode = node.id }
                }
                VStack(alignment: .leading, spacing: 5) {
                    Text("Node graph")
                        .font(.headline)
                    Text("Proxy is active: 320 × 180 · 10 fps")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .padding(18)
            }
        }
        .frame(minWidth: 650)
    }

    private var inspector: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Inspector")
                .font(.headline)
            if let selected = nodes.first(where: { $0.id == selectedNode }) {
                Text(selected.title)
                    .font(.title3.weight(.semibold))
                Text(selected.subtitle)
                    .foregroundStyle(.secondary)
                Divider()
                LabeledContent("Quality", value: quality.title)
                LabeledContent("Cache", value: "Ready")
                Text("Parameters will be backed by the C++ tensor node once the macOS bridge is attached.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                ContentUnavailableView("Select a node", systemImage: "slider.horizontal.3")
            }
            Spacer()
            Label("Render cache is local", systemImage: "externaldrive")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(16)
    }

    private var timeline: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Label("Timeline", systemImage: "timeline.selection")
                    .font(.caption.weight(.medium))
                Spacer()
                Text("00:00:00:00   00:00:10:00")
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
            }
            GeometryReader { proxy in
                ZStack(alignment: .leading) {
                    Capsule().fill(.quaternary).frame(height: 8)
                    Capsule().fill(.tint).frame(width: proxy.size.width * 0.32, height: 8)
                    Rectangle().fill(.white).frame(width: 2, height: 26).offset(x: proxy.size.width * 0.32 - 1, y: -9)
                }
            }
            .frame(height: 26)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }
}

private struct NodeCard: View {
    let node: GraphNode
    let isSelected: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 7) {
            HStack(spacing: 7) {
                Circle().fill(node.tint).frame(width: 9, height: 9)
                Text(node.title).font(.caption.weight(.semibold))
            }
            Text(node.subtitle)
                .font(.caption2)
                .foregroundStyle(.secondary)
                .lineLimit(1)
        }
        .padding(12)
        .frame(width: 220, alignment: .leading)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 10))
        .overlay {
            RoundedRectangle(cornerRadius: 10)
                .stroke(isSelected ? Color.accentColor : node.tint.opacity(0.5), lineWidth: isSelected ? 2 : 1)
        }
        .shadow(color: .black.opacity(0.09), radius: 8, y: 3)
    }
}

private enum RenderQuality: String, CaseIterable, Identifiable {
    case proxy
    case full

    var id: String { rawValue }
    var title: String { self == .proxy ? "Proxy" : "Full quality" }
}
