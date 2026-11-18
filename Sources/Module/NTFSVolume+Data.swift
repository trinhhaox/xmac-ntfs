// SPDX-License-Identifier: GPL-2.0-or-later
import FSKit
import Foundation

private let fileAttrReadOnly: UInt32 = 0x0001
private let fileAttrHidden: UInt32 = 0x0002

extension NTFSVolume {
    func getAttributes(_ desiredAttributes: FSItem.GetAttributesRequest, of item: FSItem,
                       replyHandler reply: @escaping (FSItem.Attributes?, Error?) -> Void) {
        do {
            reply(try attributesFor(try Self.ntfsItem(item).ino), nil)
        } catch {
            reply(nil, error)
        }
    }

    func setAttributes(_ newAttributes: FSItem.SetAttributesRequest, on item: FSItem,
                       replyHandler reply: @escaping (FSItem.Attributes?, Error?) -> Void) {
        do {
            let target = try Self.ntfsItem(item)
            try applyAttributes(newAttributes, to: target)
            reply(try attributesFor(target.ino), nil)
        } catch {
            reply(nil, error)
        }
    }

    func read(from item: FSItem, at offset: off_t, length: Int, into buffer: FSMutableFileDataBuffer,
              replyHandler reply: @escaping (Int, Error?) -> Void) {
        do {
            let ino = try Self.ntfsItem(item).ino
            let count = try buffer.withUnsafeMutableBytes { raw in
                try engine.read(ino, into: UnsafeMutableRawBufferPointer(rebasing: raw[0..<min(length, raw.count)]), at: offset)
            }
            reply(count, nil)
        } catch {
            reply(0, error)
        }
    }

    func write(contents: Data, to item: FSItem, at offset: off_t, replyHandler reply: @escaping (Int, Error?) -> Void) {
        do {
            reply(try engine.write(try Self.ntfsItem(item).ino, from: contents, at: offset), nil)
        } catch {
            reply(0, error)
        }
    }

    func attributesFor(_ ino: UInt64) throws -> FSItem.Attributes {
        let st = try engine.stat(ino)
        let attrs = FSItem.Attributes()
        attrs.uid = ownerUID
        attrs.gid = ownerGID
        attrs.fileID = Self.identifier(st.ino)
        attrs.parentID = st.ino == UInt64(XNTFS_ROOT_INO) ? .parentOfRoot : Self.identifier(st.parent)
        attrs.linkCount = st.nlink
        attrs.size = st.size
        attrs.allocSize = st.alloc_size
        // Read-only volumes drop write bits so Finder does not offer edits that would fail.
        let writeMask: UInt32 = engine.isReadOnly ? ~UInt32(0o222) : ~UInt32(0)
        attrs.mode = st.mode & writeMask
        switch st.mode & UInt32(S_IFMT) {
        case UInt32(S_IFDIR): attrs.type = .directory
        case UInt32(S_IFLNK): attrs.type = .symlink
        default: attrs.type = .file
        }
        var flags: UInt32 = 0
        if st.ntfs_flags & fileAttrHidden != 0 { flags |= UInt32(UF_HIDDEN) }
        if st.ntfs_flags & fileAttrReadOnly != 0, attrs.type == .file { flags |= UInt32(UF_IMMUTABLE) }
        attrs.flags = flags
        attrs.birthTime = st.birth
        attrs.modifyTime = st.mtime
        attrs.changeTime = st.ctime
        attrs.accessTime = st.atime
        return attrs
    }

    /// Applies what NTFS can store (size, times, hidden / read-only flags) and marks those
    /// attributes consumed; owner and mode changes are ignored like on exFAT.
    func applyAttributes(_ request: FSItem.SetAttributesRequest, to item: NTFSItem) throws {
        if request.isValid(.size) {
            try engine.truncate(item.ino, to: request.size)
            request.consumedAttributes.insert(.size)
        }
        let birth = request.isValid(.birthTime) ? request.birthTime : nil
        let modify = request.isValid(.modifyTime) ? request.modifyTime : nil
        let access = request.isValid(.accessTime) ? request.accessTime : nil
        if birth != nil || modify != nil || access != nil {
            try engine.setTimes(item.ino, birth: birth, modify: modify, access: access)
            if birth != nil { request.consumedAttributes.insert(.birthTime) }
            if modify != nil { request.consumedAttributes.insert(.modifyTime) }
            if access != nil { request.consumedAttributes.insert(.accessTime) }
        }
        if request.isValid(.flags) {
            var set: UInt32 = 0, clear: UInt32 = 0
            if request.flags & UInt32(UF_HIDDEN) != 0 { set |= fileAttrHidden } else { clear |= fileAttrHidden }
            if request.flags & UInt32(UF_IMMUTABLE) != 0 { set |= fileAttrReadOnly } else { clear |= fileAttrReadOnly }
            try engine.setFlags(item.ino, set: set, clear: clear)
            request.consumedAttributes.insert(.flags)
        }
        for ignored: FSItem.Attribute in [.uid, .gid, .mode] where request.isValid(ignored) {
            request.consumedAttributes.insert(ignored)
        }
    }
}
