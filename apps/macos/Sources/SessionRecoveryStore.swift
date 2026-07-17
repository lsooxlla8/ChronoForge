import Foundation

struct SessionRecoverySnapshot: Codable, Sendable {
    static let currentVersion = 2

    var version: Int
    var source: MediaSource?
    var sourceBookmark: Data?
    var sourcePath: String
    var media: [RecoveryMediaReference]?
    var primaryMediaID: UUID?
    var effects: [EffectNode]
    var outputNodeID: UUID?
    var proxyQuality: String?
    var spatialPrefilter: Int32?
    var temporalPrefilter: Int32?
    var audioMode: String?
    var savedAt: Date

    init(
        source: DecodedProxy,
        mediaPool: [DecodedProxy]? = nil,
        effects: [EffectNode],
        outputNodeID: UUID? = nil,
        proxyQuality: ProxyQuality = .standard,
        spatialPrefilter: PrefilterStrength = .off,
        temporalPrefilter: PrefilterStrength = .off,
        audioMode: AudioMode = .none
    ) {
        version = Self.currentVersion
        self.source = source.mediaSource
        sourceBookmark = source.securityScopedBookmark
        sourcePath = source.sourceURL.path
        let allMedia = mediaPool ?? [source]
        media = allMedia.map(RecoveryMediaReference.init)
        primaryMediaID = source.id
        self.effects = effects
        self.outputNodeID = outputNodeID
        self.proxyQuality = proxyQuality.rawValue
        self.spatialPrefilter = spatialPrefilter.rawValue
        self.temporalPrefilter = temporalPrefilter.rawValue
        self.audioMode = audioMode.rawValue
        savedAt = Date()
    }

    func sourceURL() throws -> URL {
        if let source {
            let resolved = source.resolvedFromBookmarkIfAvailable()
            guard FileManager.default.fileExists(atPath: resolved.accessURL.path) else {
                throw CocoaError(.fileNoSuchFile, userInfo: [NSFilePathErrorKey: resolved.accessURL.path])
            }
            return resolved.accessURL
        }
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


    func mediaReferences() -> [RecoveryMediaReference] {
        if let media, !media.isEmpty { return media }
        return [RecoveryMediaReference(
            id: primaryMediaID ?? UUID(),
            source: source,
            bookmark: sourceBookmark,
            path: sourcePath
        )]
    }
}

struct RecoveryMediaReference: Codable, Sendable {
    var id: UUID
    var source: MediaSource?
    var bookmark: Data?
    var path: String

    init(_ media: DecodedProxy) {
        id = media.id
        source = media.mediaSource
        bookmark = media.securityScopedBookmark
        path = media.sourceURL.path
    }

    init(id: UUID, source: MediaSource? = nil, bookmark: Data?, path: String) {
        self.id = id
        self.source = source
        self.bookmark = bookmark
        self.path = path
    }

    func url() throws -> URL {
        try resolvedMediaSource().accessURL
    }

    func resolvedMediaSource() throws -> MediaSource {
        if let source {
            let resolved = source.resolvedFromBookmarkIfAvailable()
            guard FileManager.default.fileExists(atPath: resolved.accessURL.path) else {
                throw CocoaError(.fileNoSuchFile, userInfo: [NSFilePathErrorKey: resolved.accessURL.path])
            }
            return resolved
        }
        if let bookmark {
            var stale = false
            if let url = try? URL(
                resolvingBookmarkData: bookmark,
                options: [.withSecurityScope],
                relativeTo: nil,
                bookmarkDataIsStale: &stale
            ) { return .movie(url: url, securityScopedBookmark: bookmark) }
        }
        let fallback = URL(fileURLWithPath: path)
        guard FileManager.default.fileExists(atPath: fallback.path) else {
            throw CocoaError(.fileNoSuchFile, userInfo: [NSFilePathErrorKey: path])
        }
        return .movie(url: fallback, securityScopedBookmark: bookmark)
    }
}

private enum SessionRecoveryCodec {
    static func save(_ project: SessionRecoverySnapshot, to url: URL) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        try encoder.encode(project).write(to: url, options: .atomic)
    }

    static func load(from url: URL) throws -> SessionRecoverySnapshot {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let project = try decoder.decode(SessionRecoverySnapshot.self, from: Data(contentsOf: url))
        guard project.version <= SessionRecoverySnapshot.currentVersion else {
            throw CocoaError(.fileReadUnsupportedScheme)
        }
        return project
    }
}

enum SessionRecoveryStore {
    static var url: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
            .appendingPathComponent("ChronoForge/InterruptedSession.json")
    }

    static var exists: Bool { FileManager.default.fileExists(atPath: url.path) }

    static func save(_ project: SessionRecoverySnapshot) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try SessionRecoveryCodec.save(project, to: url)
    }

    static func save(_ project: SessionRecoverySnapshot, to testURL: URL) throws {
        try SessionRecoveryCodec.save(project, to: testURL)
    }

    static func load() throws -> SessionRecoverySnapshot {
        try SessionRecoveryCodec.load(from: url)
    }

    static func load(from testURL: URL) throws -> SessionRecoverySnapshot {
        try SessionRecoveryCodec.load(from: testURL)
    }

    static func remove() {
        try? FileManager.default.removeItem(at: url)
    }
}
