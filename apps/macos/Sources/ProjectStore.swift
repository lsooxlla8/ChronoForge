import AppKit
import Foundation
import SwiftUI

@MainActor
final class ProjectStore: ObservableObject {
    @Published var source: DecodedProxy?
    @Published var output: VideoTensorData?
    @Published var effects: [EffectNode] = []
    @Published var selectedNodeID: UUID?
    @Published var currentFrame = 0
    @Published var quality = RenderQuality.proxy
    @Published var showsImporter = false
    @Published var isImporting = false
    @Published var isRendering = false
    @Published var isExporting = false
    @Published var errorMessage: String?
    @Published var statusMessage = "Choose a video to begin"

    private var task: Task<Void, Never>?

    var displayedTensor: VideoTensorData? { output ?? source?.tensor }
    var selectedNodeIndex: Int? { effects.firstIndex { $0.id == selectedNodeID } }

    func importVideo(from url: URL) {
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
                statusMessage = "Proxy ready · \(decoded.tensor.frames) frames"
            } catch is CancellationError {
                statusMessage = "Import cancelled"
            } catch {
                errorMessage = error.localizedDescription
                statusMessage = "Import failed"
            }
        }
    }

    func addEffect(_ kind: EffectKind) {
        let node = EffectNode.make(kind)
        effects.append(node)
        selectedNodeID = node.id
    }

    func removeSelectedEffect() {
        guard let index = selectedNodeIndex else { return }
        effects.remove(at: index)
        selectedNodeID = effects.indices.contains(index) ? effects[index].id : effects.last?.id
    }

    func moveEffect(from offsets: IndexSet, to destination: Int) {
        effects.move(fromOffsets: offsets, toOffset: destination)
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
        let graph = effects
        let cacheKey = ProxyCache.key(source: source!.sourceURL, input: input, effects: graph)
        task = Task {
            defer { isRendering = false }
            do {
                if let cached = await ProxyCache.shared.load(key: cacheKey) {
                    output = cached
                    currentFrame = min(currentFrame, max(0, cached.frames - 1))
                    statusMessage = "Loaded from render cache"
                    return
                }
                let result = try await CoreRenderer.render(input: input, effects: graph)
                try Task.checkCancellation()
                output = result
                currentFrame = min(currentFrame, max(0, result.frames - 1))
                statusMessage = "Rendered · \(result.frames) × \(result.width) × \(result.height)"
                try? await ProxyCache.shared.store(result, key: cacheKey)
            } catch is CancellationError {
                statusMessage = "Render cancelled"
            } catch {
                errorMessage = error.localizedDescription
                statusMessage = "Render failed"
            }
        }
    }

    func chooseExportLocation() {
        guard output != nil else { return }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.mpeg4Movie]
        panel.canCreateDirectories = true
        panel.nameFieldStringValue = "ChronoForge-Render.mp4"
        panel.begin { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            self?.exportVideo(to: url)
        }
    }

    func exportVideo(to url: URL) {
        guard let output else { return }
        cancelWork()
        isExporting = true
        errorMessage = nil
        statusMessage = "Exporting MP4…"
        task = Task {
            defer { isExporting = false }
            do {
                try await VideoExporter.export(output, to: url)
                statusMessage = "Export complete · \(url.lastPathComponent)"
                NSWorkspace.shared.activateFileViewerSelecting([url])
            } catch is CancellationError {
                statusMessage = "Export cancelled"
            } catch {
                errorMessage = error.localizedDescription
                statusMessage = "Export failed"
            }
        }
    }

    func cancelWork() {
        task?.cancel()
        task = nil
    }
}
