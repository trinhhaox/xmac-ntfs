// SPDX-License-Identifier: GPL-2.0-or-later
import FSKit
import Foundation

/// Swift face of the C engine (xntfs.h). Methods are synchronous and thread-safe: the C side
/// serialises on its volume lock, and `gate` keeps calls off the volume once it is unmounted
/// (FSKit calls unmount before deactivate, and late calls can still arrive).
final class NTFSEngine: @unchecked Sendable {
    enum ReadOnlyReason { case none, dirty, hibernated }

    private let handle: OpaquePointer
    private let deviceBox: Unmanaged<DeviceBox>
    private let gate = Gate()
    /// Writes sit in memory (the file kept open, the small-write cache) until a sync. Flush a
    /// second after the last change, so pulling the drive soon after copying loses nothing.
    private let syncQueue = DispatchQueue(label: "vn.xmac.ntfs.sync")
    private var pendingSync: DispatchWorkItem?
    private var pendingSince: Date?
    static let syncDelay: TimeInterval = 1
    /// During a long copy the delay keeps moving; never hold changes back longer than this.
    static let maxSyncDelay: TimeInterval = 5
    let readOnlyReason: ReadOnlyReason
    let isReadOnly: Bool

    /// Keeps the FSKit device alive for the C callbacks.
    fileprivate final class DeviceBox {
        let device: FSBlockDeviceResource
        init(_ device: FSBlockDeviceResource) { self.device = device }
    }

    init(device: FSBlockDeviceResource, wantWrite: Bool) throws {
        let box = Unmanaged.passRetained(DeviceBox(device))
        var io = xntfs_io(
            ctx: box.toOpaque(),
            pread: { ctx, buf, count, offset in
                NTFSEngine.box(ctx).transfer(buf, count, offset, write: false)
            },
            pwrite: { ctx, buf, count, offset in
                NTFSEngine.box(ctx).transfer(UnsafeMutableRawPointer(mutating: buf), count, offset, write: true)
            },
            sync: { ctx in NTFSEngine.box(ctx).flush() },
            size_bytes: Int64(device.blockCount * device.blockSize),
            // Logical blocks: partitions are whole logical blocks, not always whole 4K sectors.
            sector_size: UInt32(device.blockSize)
        )
        var vol: OpaquePointer?
        var reason: Int32 = 0
        let rc = xntfs_mount(&io, wantWrite ? 1 : 0, &vol, &reason)
        guard rc == 0, let vol else {
            box.release()
            throw NTFSEngine.error(rc == 0 ? EIO : rc)
        }
        handle = vol
        deviceBox = box
        isReadOnly = xntfs_is_readonly(vol) != 0
        switch Int(reason) {
        case Int(XNTFS_MOUNT_FORCED_RO_HIBERNATED): readOnlyReason = .hibernated
        case Int(XNTFS_MOUNT_FORCED_RO_DIRTY): readOnlyReason = .dirty
        default: readOnlyReason = .none
        }
    }

    /// Idempotent; waits for calls in flight, then refuses new ones with EIO.
    func unmount() throws {
        syncQueue.sync { pendingSync?.cancel() }
        guard gate.close() else { return }
        defer { deviceBox.release() }
        try check(xntfs_unmount(handle))
    }

    /// Runs `body` with the live volume, or throws EIO once unmounted.
    private func live<T>(_ body: (OpaquePointer) throws -> T) throws -> T {
        try gate.enter { try body(handle) }
    }

    private static func box(_ ctx: UnsafeMutableRawPointer?) -> DeviceBox {
        Unmanaged<DeviceBox>.fromOpaque(ctx!).takeUnretainedValue()  // swiftlint:disable:this force_unwrapping
    }

    static func error(_ code: Int32) -> Error { fs_errorForPOSIXError(code) }

    func check(_ rc: Int32) throws { if rc != 0 { throw NTFSEngine.error(rc) } }

    /// Debounced sync after a change; errors are logged (the next explicit sync reports them).
    private func scheduleSync() {
        syncQueue.async { [weak self] in
            guard let self else { return }
            let now = Date()
            if let since = self.pendingSince, now.timeIntervalSince(since) >= Self.maxSyncDelay {
                return  // the queued sync is overdue: let it run instead of postponing again
            }
            self.pendingSync?.cancel()
            if self.pendingSince == nil { self.pendingSince = now }
            let work = DispatchWorkItem { [weak self] in
                self?.pendingSince = nil
                do { try self?.sync() } catch { log.error("background sync: \(String(describing: error), privacy: .public)") }
            }
            self.pendingSync = work
            self.syncQueue.asyncAfter(deadline: .now() + Self.syncDelay, execute: work)
        }
    }

    // MARK: - Nodes

    func stat(_ ino: UInt64) throws -> xntfs_stat {
        var st = xntfs_stat()
        try check(live { xntfs_getattr($0, ino, &st) })
        return st
    }

    func lookup(_ name: String, in dir: UInt64) throws -> xntfs_stat {
        var st = xntfs_stat()
        try existingName(name) { candidate in
            try check(live { h in candidate.withCString { xntfs_lookup(h, dir, $0, &st) } })
        }
        return st
    }

    struct Entry { let name: String; let ino: UInt64; let dtype: UInt32 }

    func list(_ dir: UInt64) throws -> [Entry] {
        final class Sink { var entries: [Entry] = [] }
        let sink = Sink()
        let rc = try live { h in
            xntfs_readdir(h, dir, { ctx, name, ino, dtype in
                let sink = Unmanaged<Sink>.fromOpaque(ctx!).takeUnretainedValue()  // swiftlint:disable:this force_unwrapping
                sink.entries.append(Entry(name: String(cString: name!), ino: ino, dtype: dtype))  // swiftlint:disable:this force_unwrapping
                return 0
            }, Unmanaged.passUnretained(sink).toOpaque())
        }
        try check(rc)
        return sink.entries
    }

    func create(_ name: String, in dir: UInt64, directory: Bool) throws -> xntfs_stat {
        defer { scheduleSync() }
        var st = xntfs_stat()
        let type = UInt32(directory ? S_IFDIR : S_IFREG)
        try check(live { h in Self.newName(name).withCString { xntfs_create(h, dir, $0, type, &st) } })
        return st
    }

    func remove(_ name: String, in dir: UInt64) throws {
        defer { scheduleSync() }
        try existingName(name) { candidate in
            try check(live { h in candidate.withCString { xntfs_remove(h, dir, $0) } })
        }
    }

    func rename(_ name: String, in dir: UInt64, to newName: String, in newDir: UInt64) throws {
        defer { scheduleSync() }
        try existingName(name) { candidate in
            let rc = try live { h in
                candidate.withCString { src in
                    Self.newName(newName).withCString { dst in xntfs_rename(h, dir, src, newDir, dst) }
                }
            }
            try check(rc)
        }
    }

    func symlink(_ name: String, in dir: UInt64, target: String) throws -> xntfs_stat {
        defer { scheduleSync() }
        var st = xntfs_stat()
        let rc = try live { h in
            Self.newName(name).withCString { n in target.withCString { xntfs_symlink(h, dir, n, $0, &st) } }
        }
        try check(rc)
        return st
    }

    func link(_ ino: UInt64, as name: String, in dir: UInt64) throws {
        defer { scheduleSync() }
        try check(live { h in Self.newName(name).withCString { xntfs_link(h, ino, dir, $0) } })
    }

    func readLink(_ ino: UInt64) throws -> String {
        var buffer = [CChar](repeating: 0, count: Int(PATH_MAX))
        try check(live { xntfs_readlink($0, ino, &buffer, buffer.count) })
        return String(cString: buffer)
    }

    // MARK: - Data

    func read(_ ino: UInt64, into buffer: UnsafeMutableRawBufferPointer, at offset: Int64) throws -> Int {
        let n = try live { xntfs_read($0, ino, buffer.baseAddress, Int64(buffer.count), offset) }
        if n < 0 { throw NTFSEngine.error(Int32(-n)) }
        return Int(n)
    }

    func write(_ ino: UInt64, from data: Data, at offset: Int64) throws -> Int {
        defer { scheduleSync() }
        let n = try live { h in data.withUnsafeBytes { xntfs_write(h, ino, $0.baseAddress, Int64($0.count), offset) } }
        if n < 0 { throw NTFSEngine.error(Int32(-n)) }
        return Int(n)
    }

    func truncate(_ ino: UInt64, to size: UInt64) throws {
        defer { scheduleSync() }
        try check(live { xntfs_truncate($0, ino, size) })
    }

    func setTimes(_ ino: UInt64, birth: timespec?, modify: timespec?, access: timespec?) throws {
        defer { scheduleSync() }
        var b = birth ?? timespec(), m = modify ?? timespec(), a = access ?? timespec()
        let rc = try live { h in
            withUnsafePointer(to: &b) { bp in
                withUnsafePointer(to: &m) { mp in
                    withUnsafePointer(to: &a) { ap in
                        xntfs_set_times(h, ino, birth == nil ? nil : bp, modify == nil ? nil : mp, access == nil ? nil : ap)
                    }
                }
            }
        }
        try check(rc)
    }

    func setFlags(_ ino: UInt64, set: UInt32, clear: UInt32) throws {
        defer { scheduleSync() }
        try check(live { xntfs_set_flags($0, ino, set, clear) })
    }

    func statfs() throws -> xntfs_statfs {
        var out = xntfs_statfs()
        try check(live { xntfs_get_statfs($0, &out) })
        return out
    }

    /// Volume label, empty when unset.
    func label() -> String {
        var buffer = [CChar](repeating: 0, count: 256)
        guard (try? live({ xntfs_get_label($0, &buffer, buffer.count) })) == 0 else { return "" }
        return String(cString: buffer)
    }

    func sync() throws { try check(live { xntfs_sync($0) }) }

    // MARK: - Extended attributes (NTFS named streams)

    /// Largest value read back; resource forks are rarely more than a few MB.
    static let maxXattrSize = 64 << 20

    func xattr(_ name: String, of ino: UInt64) throws -> Data {
        var size = 0
        try check(live { h in name.withCString { xntfs_xattr_get(h, ino, $0, nil, 0, &size) } })
        guard size <= Self.maxXattrSize else { throw NTFSEngine.error(E2BIG) }
        var data = Data(count: size)
        if size > 0 {
            let rc = try live { h in
                data.withUnsafeMutableBytes { buf in
                    name.withCString { xntfs_xattr_get(h, ino, $0, buf.baseAddress, buf.count, &size) }
                }
            }
            try check(rc)
        }
        return data.prefix(size)
    }

    enum XattrMode { case any, create, replace }

    func setXattr(_ name: String, of ino: UInt64, to value: Data, mode: XattrMode) throws {
        defer { scheduleSync() }
        let flags: Int32 = mode == .create ? Int32(XNTFS_XATTR_CREATE) : mode == .replace ? Int32(XNTFS_XATTR_REPLACE) : 0
        let rc = try live { h in
            value.withUnsafeBytes { buf in
                name.withCString { xntfs_xattr_set(h, ino, $0, buf.baseAddress, buf.count, flags) }
            }
        }
        try check(rc)
    }

    func removeXattr(_ name: String, of ino: UInt64) throws {
        defer { scheduleSync() }
        try check(live { h in name.withCString { xntfs_xattr_remove(h, ino, $0) } })
    }

    func xattrNames(of ino: UInt64) throws -> [String] {
        final class Sink { var names: [String] = [] }
        let sink = Sink()
        let rc = try live { h in
            xntfs_xattr_list(h, ino, { ctx, name in
                let sink = Unmanaged<Sink>.fromOpaque(ctx!).takeUnretainedValue()  // swiftlint:disable:this force_unwrapping
                sink.names.append(String(cString: name!))  // swiftlint:disable:this force_unwrapping
                return 0
            }, Unmanaged.passUnretained(sink).toOpaque())
        }
        try check(rc)
        return sink.names
    }

    // MARK: - Names

    /// New names are stored precomposed (NFC), as Windows writes them; macOS may hand over
    /// decomposed names, and storing both forms would make "Việt" two different files.
    static func newName(_ name: String) -> String { name.precomposedStringWithCanonicalMapping }

    /// Existing names are tried exactly as given first (a decomposed name written by another
    /// tool must stay reachable), then in NFC.
    private func existingName(_ name: String, _ body: (String) throws -> Void) throws {
        do {
            try body(name)
        } catch let error as NSError where error.code == Int(ENOENT) {
            let nfc = Self.newName(name)
            guard nfc != name else { throw error }
            try body(nfc)
        }
    }
}

/// Read-write gate: calls share it; `close` waits for them and shuts it for good.
private final class Gate: @unchecked Sendable {
    private var lock = pthread_rwlock_t()
    private var closed = false

    init() { pthread_rwlock_init(&lock, nil) }
    deinit { pthread_rwlock_destroy(&lock) }

    func enter<T>(_ body: () throws -> T) throws -> T {
        pthread_rwlock_rdlock(&lock)
        defer { pthread_rwlock_unlock(&lock) }
        guard !closed else { throw NTFSEngine.error(EIO) }
        return try body()
    }

    /// True for the caller that actually closed it.
    func close() -> Bool {
        pthread_rwlock_wrlock(&lock)
        defer { pthread_rwlock_unlock(&lock) }
        guard !closed else { return false }
        closed = true
        return true
    }
}

private extension NTFSEngine.DeviceBox {
    /// Aligned I/O as the C engine guarantees; returns bytes moved or a negative errno.
    func transfer(_ buf: UnsafeMutableRawPointer?, _ count: Int64, _ offset: Int64, write: Bool) -> Int64 {
        guard let buf, count > 0 else { return 0 }
        do {
            let n = write
                ? try device.write(from: UnsafeRawBufferPointer(start: buf, count: Int(count)), startingAt: offset, length: Int(count))
                : try device.read(into: UnsafeMutableRawBufferPointer(start: buf, count: Int(count)), startingAt: offset, length: Int(count))
            return Int64(n)
        } catch {
            log.error("device \(write ? "write" : "read", privacy: .public) \(count)@\(offset) failed: \(String(describing: error), privacy: .public)")
            return -Int64((error as NSError).domain == NSPOSIXErrorDomain ? (error as NSError).code : Int(EIO))
        }
    }

    /// Writes go straight to the device (direct I/O, no buffer cache), so only FSKit's metadata
    /// cache needs flushing. Whether this also flushes the drive's own write cache is unverified.
    func flush() -> Int32 {
        do {
            try device.metadataFlush()
            return 0
        } catch {
            log.error("flush failed: \(String(describing: error), privacy: .public)")
            return EIO
        }
    }
}
