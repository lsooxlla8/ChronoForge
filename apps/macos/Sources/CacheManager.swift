import Foundation

actor CacheManager {
    static let shared = CacheManager()

    private var root: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first!
            .appendingPathComponent("ChronoForge", isDirectory: true)
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
}
