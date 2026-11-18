// SPDX-License-Identifier: GPL-2.0-or-later
import FSKit
import Foundation

/// An FSItem is one NTFS inode (MFT record number).
final class NTFSItem: FSItem, @unchecked Sendable {
    let ino: UInt64
    init(ino: UInt64) {
        self.ino = ino
        super.init()
    }
}

/// The mounted NTFS volume. Namespace operations live in NTFSVolume+Namespace.swift, contents
/// and attributes in NTFSVolume+Data.swift.
final class NTFSVolume: FSVolume, FSVolume.Operations, FSVolume.PathConfOperations,
    FSVolume.OpenCloseOperations, FSVolume.ReadWriteOperations, @unchecked Sendable
{
    let engine: NTFSEngine
    let ownerUID: uid_t
    let ownerGID: gid_t
    private let itemLock = NSLock()
    private var items: [UInt64: NTFSItem] = [:]
    /// Directory listings snapshotted per verifier, so paging with cookies stays consistent.
    let listings = DirectoryListings()

    init(engine: NTFSEngine, name: String, uuid: UUID, ownerUID: uid_t, ownerGID: gid_t) {
        self.engine = engine
        self.ownerUID = ownerUID
        self.ownerGID = ownerGID
        super.init(volumeID: FSVolume.Identifier(uuid: uuid), volumeName: FSFileName(string: name))
    }

    /// One FSItem per inode for the life of the mount (FSKit compares items by identity).
    func item(_ ino: UInt64) -> NTFSItem {
        itemLock.withLock {
            if let existing = items[ino] { return existing }
            let created = NTFSItem(ino: ino)
            items[ino] = created
            return created
        }
    }

    func forget(_ ino: UInt64) {
        guard ino != UInt64(XNTFS_ROOT_INO) else { return }
        _ = itemLock.withLock { items.removeValue(forKey: ino) }
    }

    // MARK: - PathConf

    var maximumLinkCount: Int { 1024 }
    var maximumNameLength: Int { 255 }
    var restrictsOwnershipChanges: Bool { true }
    var truncatesLongNames: Bool { false }
    var maximumFileSize: UInt64 { UInt64(Int64.max) }

    // MARK: - Volume

    var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
        let caps = FSVolume.SupportedCapabilities()
        caps.supportsPersistentObjectIDs = true
        caps.supportsHardLinks = true
        caps.supportsSymbolicLinks = true
        caps.supportsSparseFiles = true
        caps.supportsHiddenFiles = true
        caps.supports2TBFiles = true
        caps.supports64BitObjectIDs = true
        caps.supportsFastStatFS = false
        caps.doesNotSupportRootTimes = false
        caps.caseFormat = .insensitiveCasePreserving
        return caps
    }

    var volumeStatistics: FSStatFSResult {
        let result = FSStatFSResult(fileSystemTypeName: "ntfs")
        guard let fs = try? engine.statfs() else { return result }
        let block = UInt64(max(fs.cluster_size, 512))
        result.blockSize = Int(block)
        result.ioSize = 1 << 20
        result.totalBytes = fs.total_bytes
        result.freeBytes = fs.free_bytes
        result.availableBytes = engine.isReadOnly ? 0 : fs.free_bytes
        result.usedBytes = fs.total_bytes - fs.free_bytes
        result.totalBlocks = fs.total_bytes / block
        result.freeBlocks = fs.free_bytes / block
        result.availableBlocks = engine.isReadOnly ? 0 : fs.free_bytes / block
        result.usedBlocks = result.totalBlocks - result.freeBlocks
        result.totalFiles = fs.total_files
        result.freeFiles = fs.free_files
        return result
    }

    func mount(options: FSTaskOptions, replyHandler reply: @escaping (Error?) -> Void) { reply(nil) }

    func unmount(replyHandler reply: @escaping () -> Void) {
        do { try engine.unmount() } catch { log.error("unmount: \(String(describing: error), privacy: .public)") }
        reply()
    }

    func synchronize(flags: FSSyncFlags, replyHandler reply: @escaping (Error?) -> Void) {
        reply(Self.attempt { try engine.sync() })
    }

    func activate(options: FSTaskOptions, replyHandler reply: @escaping (FSItem?, Error?) -> Void) {
        reply(item(UInt64(XNTFS_ROOT_INO)), nil)
    }

    /// FSKit calls this after unmount, which already flushed and closed the engine.
    func deactivate(options: FSDeactivateOptions = [], replyHandler reply: @escaping (Error?) -> Void) {
        reply(nil)
    }

    func reclaimItem(_ item: FSItem, replyHandler reply: @escaping (Error?) -> Void) {
        if let item = item as? NTFSItem { forget(item.ino) }
        reply(nil)
    }

    func openItem(_ item: FSItem, modes: FSVolume.OpenModes, replyHandler reply: @escaping (Error?) -> Void) {
        reply(modes.contains(.write) && engine.isReadOnly ? NTFSEngine.error(EROFS) : nil)
    }

    func closeItem(_ item: FSItem, modes: FSVolume.OpenModes, replyHandler reply: @escaping (Error?) -> Void) {
        reply(nil)
    }

    static func attempt(_ body: () throws -> Void) -> Error? {
        do { try body(); return nil } catch { return error }
    }

    static func ntfsItem(_ item: FSItem) throws -> NTFSItem {
        guard let item = item as? NTFSItem else { throw NTFSEngine.error(EINVAL) }
        return item
    }
}

/// Snapshots of directory listings keyed by verifier: libntfs-3g has no stable resume cookie.
final class DirectoryListings: @unchecked Sendable {
    private let lock = NSLock()
    private var next: UInt64 = 1
    private var snapshots: [UInt64: [NTFSEngine.Entry]] = [:]

    func snapshot(_ verifier: UInt64) -> [NTFSEngine.Entry]? { lock.withLock { snapshots[verifier] } }

    func store(_ entries: [NTFSEngine.Entry]) -> UInt64 {
        lock.withLock {
            let id = next
            next += 1
            snapshots[id] = entries
            if snapshots.count > 64, let oldest = snapshots.keys.min() { snapshots.removeValue(forKey: oldest) }
            return id
        }
    }

    func drop(_ verifier: UInt64) { _ = lock.withLock { snapshots.removeValue(forKey: verifier) } }
}
