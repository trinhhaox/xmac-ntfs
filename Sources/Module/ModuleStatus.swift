// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Tells the host app (Xmac) why a volume is read-only. The module is sandboxed, so it writes
/// one small JSON file per mounted partition into the shared app group container.
enum ModuleStatus {
    static let groupID = "4DU4BTV46U.vn.xmac.app"

    struct Volume: Codable, Equatable {
        let bsdName: String
        let readOnly: Bool
        /// "none", "dirty" (Windows did not shut down cleanly) or "hibernated" (hibernation or
        /// Fast Startup): why a writable volume was mounted read-only.
        let reason: String
        let updatedAt: Date
    }

    static var directory: URL? {
        FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: groupID)?
            .appendingPathComponent("ntfs-volumes", isDirectory: true)
    }

    static func publish(_ volume: Volume) {
        guard let dir = directory else { return }
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let data = try JSONEncoder().encode(volume)
            try data.write(to: dir.appendingPathComponent("\(volume.bsdName).json"), options: .atomic)
        } catch {
            log.error("status publish failed: \(String(describing: error), privacy: .public)")
        }
    }

    static func clear(bsdName: String) {
        guard let dir = directory else { return }
        try? FileManager.default.removeItem(at: dir.appendingPathComponent("\(bsdName).json"))
    }
}
