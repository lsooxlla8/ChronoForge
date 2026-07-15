import Foundation
import UniformTypeIdentifiers

extension UTType {
    static let chronoForgeProject = UTType(exportedAs: "com.lsooxlla8.chronoforge.project", conformingTo: .data)
}

struct SavedChronoForgeProject: Codable, Sendable {
    static let currentVersion = 2

    var version: Int
    var sourceBookmark: Data?
    var sourcePath: String
    var effects: [EffectNode]
    var outputNodeID: UUID?
    var quality: String
    var proxyQuality: String?
    var audioMode: String?
    var savedAt: Date

    init(
        source: DecodedProxy,
        effects: [EffectNode],
        outputNodeID: UUID? = nil,
        quality: RenderQuality = .full,
        proxyQuality: ProxyQuality = .standard,
        audioMode: AudioMode = .none
    ) {
        version = Self.currentVersion
        sourceBookmark = source.securityScopedBookmark
        sourcePath = source.sourceURL.path
        self.effects = effects
        self.outputNodeID = outputNodeID
        self.quality = quality.rawValue
        self.proxyQuality = proxyQuality.rawValue
        self.audioMode = audioMode.rawValue
        savedAt = Date()
    }

    func sourceURL() throws -> URL {
        if let sourceBookmark {
            var stale = false
            if let url = try? URL(
                resolvingBookmarkData: sourceBookmark,
                options: [.withSecurityScope],
                relativeTo: nil,
                bookmarkDataIsStale: &stale
            ) {
                return url
            }
        }
        let fallback = URL(fileURLWithPath: sourcePath)
        guard FileManager.default.fileExists(atPath: fallback.path) else {
            throw CocoaError(.fileNoSuchFile, userInfo: [NSFilePathErrorKey: sourcePath])
        }
        return fallback
    }
}

enum ProjectPersistence {
    static func save(_ project: SavedChronoForgeProject, to url: URL) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        try encoder.encode(project).write(to: url, options: .atomic)
    }

    static func load(from url: URL) throws -> SavedChronoForgeProject {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let project = try decoder.decode(SavedChronoForgeProject.self, from: Data(contentsOf: url))
        guard project.version <= SavedChronoForgeProject.currentVersion else {
            throw CocoaError(.fileReadUnsupportedScheme)
        }
        return project
    }
}

enum RecoveryStore {
    static var url: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
            .appendingPathComponent("ChronoForge/Recovery.chronoforge")
    }

    static var exists: Bool { FileManager.default.fileExists(atPath: url.path) }

    static func save(_ project: SavedChronoForgeProject) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try ProjectPersistence.save(project, to: url)
    }

    static func remove() {
        try? FileManager.default.removeItem(at: url)
    }
}
