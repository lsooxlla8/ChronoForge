import Foundation

struct MovieSource: Codable, Equatable, Sendable {
    var url: URL
    var securityScopedBookmark: Data?

    init(url: URL, securityScopedBookmark: Data? = nil) {
        self.url = url
        self.securityScopedBookmark = securityScopedBookmark
    }
}

struct FrameSequenceSource: Codable, Equatable, Sendable {
    var directoryURL: URL
    var frameNames: [String]
    var framesPerSecond: Double
    var securityScopedBookmark: Data?

    init(
        directoryURL: URL,
        frameNames: [String],
        framesPerSecond: Double,
        securityScopedBookmark: Data? = nil
    ) {
        self.directoryURL = directoryURL
        self.frameNames = frameNames
        self.framesPerSecond = framesPerSecond
        self.securityScopedBookmark = securityScopedBookmark
    }

    var frameURLs: [URL] {
        frameNames.map { directoryURL.appendingPathComponent($0) }
    }
}

enum MediaSource: Codable, Equatable, Sendable {
    case movie(MovieSource)
    case frameSequence(FrameSequenceSource)

    static func movie(url: URL, securityScopedBookmark: Data? = nil) -> MediaSource {
        .movie(MovieSource(url: url, securityScopedBookmark: securityScopedBookmark))
    }

    var accessURL: URL {
        switch self {
        case .movie(let source): source.url
        case .frameSequence(let source): source.directoryURL
        }
    }

    var securityScopedBookmark: Data? {
        switch self {
        case .movie(let source): source.securityScopedBookmark
        case .frameSequence(let source): source.securityScopedBookmark
        }
    }

    var displayName: String {
        switch self {
        case .movie(let source): source.url.lastPathComponent
        case .frameSequence(let source): source.directoryURL.lastPathComponent
        }
    }

    var isMovie: Bool {
        if case .movie = self { return true }
        return false
    }

    func startAccessingSecurityScopedResource() -> Bool {
        accessURL.startAccessingSecurityScopedResource()
    }

    func stopAccessingSecurityScopedResource() {
        accessURL.stopAccessingSecurityScopedResource()
    }

    func resolvedFromBookmarkIfAvailable() -> MediaSource {
        guard let securityScopedBookmark else { return self }
        var stale = false
        guard let resolved = try? URL(
            resolvingBookmarkData: securityScopedBookmark,
            options: [.withSecurityScope],
            relativeTo: nil,
            bookmarkDataIsStale: &stale
        ) else { return self }
        switch self {
        case .movie:
            return .movie(url: resolved, securityScopedBookmark: securityScopedBookmark)
        case .frameSequence(var source):
            source.directoryURL = resolved
            return .frameSequence(source)
        }
    }

    func cacheFiles() -> [(url: URL, relativeName: String)] {
        switch self {
        case .movie(let source):
            return [(source.url, source.url.lastPathComponent)]
        case .frameSequence(let source):
            return zip(source.frameURLs, source.frameNames).map { ($0.0, $0.1) }
        }
    }

    var sequenceFramesPerSecond: Double? {
        if case .frameSequence(let source) = self { return source.framesPerSecond }
        return nil
    }
}

enum MediaSourceError: LocalizedError {
    case imageSequenceDecodePending

    var errorDescription: String? {
        switch self {
        case .imageSequenceDecodePending:
            "PNG sequence decoding is not available in this build yet."
        }
    }
}

enum MediaSourceDecoder {
    static func decodeProxy(from source: MediaSource, quality: ProxyQuality = .standard) async throws -> DecodedProxy {
        switch source {
        case .movie(let movie):
            var decoded = try await VideoDecoder.decodeProxy(from: movie.url, quality: quality)
            decoded.mediaSource = source
            return decoded
        case .frameSequence:
            throw MediaSourceError.imageSequenceDecodePending
        }
    }

    static func decodeFull(
        from source: MediaSource,
        destinationURL: URL,
        progress: @escaping @Sendable (Double, String) -> Void
    ) async throws -> DiskTensorData {
        switch source {
        case .movie(let movie):
            return try await FullVideoDecoder.decode(
                sourceURL: movie.url,
                destinationURL: destinationURL,
                progress: progress
            )
        case .frameSequence:
            throw MediaSourceError.imageSequenceDecodePending
        }
    }
}
