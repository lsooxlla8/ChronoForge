import Foundation

actor CacheManager {
    static let shared = CacheManager()
    static let automaticLimit: Int64 = 8 * 1024 * 1024 * 1024

    nonisolated static var defaultRoot: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first!
            .appendingPathComponent("ChronoForge", isDirectory: true)
    }

    /// App termination is synchronous, so it cannot await the actor-isolated
    /// `clear()`. Removing the root is sufficient; the next launch recreates it.
    nonisolated static func clearOnTermination(root: URL? = nil) throws {
        let root = root ?? defaultRoot
        if FileManager.default.fileExists(atPath: root.path) {
            try FileManager.default.removeItem(at: root)
        }
    }

    private struct Candidate {
        let url: URL
        let bytes: Int64
        let modified: Date
    }

    private let root: URL

    init(root: URL? = nil) {
        self.root = root ?? Self.defaultRoot
    }

    func size() -> Int64 {
        guard let enumerator = FileManager.default.enumerator(
            at: root,
            includingPropertiesForKeys: [.isRegularFileKey, .fileAllocatedSizeKey],
            options: [.skipsHiddenFiles]
        ) else { return 0 }
        var total: Int64 = 0
        for case let url as URL in enumerator {
            guard let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .fileAllocatedSizeKey]),
                  values.isRegularFile == true else { continue }
            total += Int64(values.fileAllocatedSize ?? 0)
        }
        return total
    }

    func clear() throws {
        if FileManager.default.fileExists(atPath: root.path) {
            try FileManager.default.removeItem(at: root)
        }
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    @discardableResult
    func trim(to maximumBytes: Int64 = automaticLimit) throws -> Int64 {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        var candidates: [Candidate] = []
        let proxy = root.appendingPathComponent("Proxy", isDirectory: true)
        let full = root.appendingPathComponent("Full", isDirectory: true)
        for directory in [proxy, full] {
            let children = (try? FileManager.default.contentsOfDirectory(
                at: directory,
                includingPropertiesForKeys: [.contentModificationDateKey, .isDirectoryKey, .fileAllocatedSizeKey],
                options: [.skipsHiddenFiles]
            )) ?? []
            for child in children {
                let values = try? child.resourceValues(forKeys: [.contentModificationDateKey])
                candidates.append(Candidate(
                    url: child,
                    bytes: allocatedSize(of: child),
                    modified: values?.contentModificationDate ?? .distantPast
                ))
            }
        }
        var total = candidates.reduce(Int64(0)) { $0 + $1.bytes }
        for candidate in candidates.sorted(by: { $0.modified < $1.modified }) where total > maximumBytes {
            try? FileManager.default.removeItem(at: candidate.url)
            total -= candidate.bytes
        }
        return max(0, total)
    }

    private func allocatedSize(of url: URL) -> Int64 {
        let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .fileAllocatedSizeKey])
        if values?.isRegularFile == true {
            return Int64(values?.fileAllocatedSize ?? 0)
        }
        guard let enumerator = FileManager.default.enumerator(
            at: url,
            includingPropertiesForKeys: [.isRegularFileKey, .fileAllocatedSizeKey],
            options: [.skipsHiddenFiles]
        ) else { return 0 }
        var total: Int64 = 0
        for case let file as URL in enumerator {
            let item = try? file.resourceValues(forKeys: [.isRegularFileKey, .fileAllocatedSizeKey])
            if item?.isRegularFile == true {
                total += Int64(item?.fileAllocatedSize ?? 0)
            }
        }
        return total
    }
}
