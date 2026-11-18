// SPDX-License-Identifier: GPL-2.0-or-later
import FSKit
import Foundation
import os

let log = Logger(subsystem: "vn.xmac.ntfs", category: "fs")

/// FSKit entry: recognises NTFS partitions and mounts them through the libntfs-3g engine.
final class XmacNTFSFileSystem: FSUnaryFileSystem, FSUnaryFileSystemOperations,
    FSManageableResourceMaintenanceOperations, @unchecked Sendable
{
    private var volume: NTFSVolume?

    func probeResource(resource: FSResource, replyHandler reply: @escaping (FSProbeResult?, Error?) -> Void) {
        guard let device = resource as? FSBlockDeviceResource else {
            log.error("probe: not a block device")
            return reply(.notRecognized, nil)
        }
        log.notice("probe \(device.bsdName, privacy: .public) block=\(device.blockSize) physical=\(device.physicalBlockSize) count=\(device.blockCount) writable=\(device.isWritable)")
        do {
            let sector = try BootSector.read(from: device)
            log.notice("probe OEM='\(sector.oemID, privacy: .public)' serial=\(sector.serial, privacy: .public)")
            guard sector.isNTFS else { return reply(.notRecognized, nil) }
            // The probe name becomes the mount point (/Volumes/<name>), so report the label.
            reply(.usable(name: Self.label(of: device) ?? "NTFS", containerID: FSContainerIdentifier(uuid: sector.containerUUID)), nil)
        } catch {
            log.error("probe read failed: \(String(describing: error), privacy: .public)")
            reply(nil, error)
        }
    }

    /// Volume label read through a short read-only mount; nil when it cannot be read or is empty.
    private static func label(of device: FSBlockDeviceResource) -> String? {
        guard let engine = try? NTFSEngine(device: device, wantWrite: false) else { return nil }
        let label = engine.label()
        try? engine.unmount()
        return label.isEmpty ? nil : label
    }

    func loadResource(resource: FSResource, options: FSTaskOptions, replyHandler reply: @escaping (FSVolume?, Error?) -> Void) {
        guard let device = resource as? FSBlockDeviceResource else { return reply(nil, fs_errorForPOSIXError(ENODEV)) }
        let mountOptions = MountOptions(options.taskOptions)
        do {
            let sector = try BootSector.read(from: device)
            guard sector.isNTFS else { throw fs_errorForPOSIXError(EINVAL) }
            let engine = try NTFSEngine(device: device, wantWrite: device.isWritable && !mountOptions.readOnly)
            let label = engine.label()
            log.notice("load \(device.bsdName, privacy: .public) label='\(label, privacy: .public)' readOnly=\(engine.isReadOnly) reason=\(String(describing: engine.readOnlyReason), privacy: .public)")
            let volume = NTFSVolume(engine: engine, name: label.isEmpty ? "NTFS" : label, uuid: sector.containerUUID,
                                    ownerUID: mountOptions.uid ?? getuid(), ownerGID: mountOptions.gid ?? getgid())
            self.volume = volume
            let reason: String
            switch engine.readOnlyReason {
            case .none: reason = "none"
            case .dirty: reason = "dirty"
            case .hibernated: reason = "hibernated"
            }
            ModuleStatus.publish(.init(bsdName: device.bsdName, readOnly: engine.isReadOnly, reason: reason, updatedAt: Date()))
            // FSKit rejects the load (EAGAIN, "unexpected container state") unless the container is ready.
            containerStatus = .ready
            reply(volume, nil)
        } catch {
            log.error("load failed: \(String(describing: error), privacy: .public)")
            reply(nil, error)
        }
    }

    func unloadResource(resource: FSResource, options: FSTaskOptions, replyHandler reply: @escaping (Error?) -> Void) {
        if let device = resource as? FSBlockDeviceResource { ModuleStatus.clear(bsdName: device.bsdName) }
        volume = nil
        reply(nil)
    }

    /// DiskArbitration only automounts after a successful check; the real check comes with libntfs-3g.
    func startCheck(task: FSTask, options: FSTaskOptions) throws -> Progress {
        let progress = Progress(totalUnitCount: 1)
        progress.completedUnitCount = 1
        task.didComplete(error: nil)
        return progress
    }

    func startFormat(task: FSTask, options: FSTaskOptions) throws -> Progress {
        throw fs_errorForPOSIXError(ENOTSUP)
    }
}

/// The first sector of an NTFS volume: OEM id "NTFS    " at offset 3, volume serial at 0x48.
struct BootSector {
    let oemID: String
    let serial: UInt64

    var isNTFS: Bool { oemID == "NTFS    " }

    /// Stable per volume, so FSKit sees the same container on every probe.
    var containerUUID: UUID {
        var bytes = [UInt8](repeating: 0, count: 16)
        withUnsafeBytes(of: serial.littleEndian) { bytes.replaceSubrange(0..<8, with: $0) }
        bytes.replaceSubrange(8..<12, with: Array("NTFS".utf8))
        return UUID(uuid: (bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                           bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]))
    }

    static func read(from device: FSBlockDeviceResource) throws -> BootSector {
        let length = Int(max(device.physicalBlockSize, 512))
        let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: length, alignment: 4096)
        defer { buffer.deallocate() }
        _ = try device.read(into: buffer, startingAt: 0, length: length)
        let oem = String(decoding: buffer[3..<11], as: UTF8.self)
        let serial = buffer.loadUnaligned(fromByteOffset: 0x48, as: UInt64.self)
        return BootSector(oemID: oem, serial: UInt64(littleEndian: serial))
    }
}

/// `mount -o` style options FSKit passes through (-o rdonly, -u uid, -g gid).
struct MountOptions {
    var readOnly = false
    var uid: uid_t?
    var gid: gid_t?

    init(_ args: [String]) {
        var index = 0
        while index < args.count {
            let arg = args[index]
            let value = index + 1 < args.count ? args[index + 1] : nil
            switch arg {
            case "-o":
                let words = (value ?? "").split(separator: ",")
                if words.contains("rdonly") || words.contains("ro") { readOnly = true }
                index += 1
            case "-u": uid = value.flatMap { uid_t($0) }; index += 1
            case "-g": gid = value.flatMap { gid_t($0) }; index += 1
            default: if arg == "-r" { readOnly = true }
            }
            index += 1
        }
    }
}

@main
struct XmacNTFSExtension: UnaryFileSystemExtension {
    private static let shared = XmacNTFSFileSystem()
    var fileSystem: FSUnaryFileSystem & FSUnaryFileSystemOperations { Self.shared }
}
