// SPDX-License-Identifier: GPL-2.0-or-later
import FSKit
import Foundation

extension NTFSVolume {
    func lookupItem(named name: FSFileName, inDirectory directory: FSItem,
                    replyHandler reply: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        do {
            let dir = try Self.ntfsItem(directory)
            guard let string = name.string else { throw NTFSEngine.error(ENOENT) }
            let st = try engine.lookup(string, in: dir.ino)
            reply(item(st.ino), name, nil)
        } catch {
            reply(nil, nil, error)
        }
    }

    func enumerateDirectory(_ directory: FSItem, startingAt cookie: FSDirectoryCookie, verifier: FSDirectoryVerifier,
                            attributes: FSItem.GetAttributesRequest?, packer: FSDirectoryEntryPacker,
                            replyHandler reply: @escaping (FSDirectoryVerifier, Error?) -> Void) {
        do {
            let dir = try Self.ntfsItem(directory)
            // A fresh listing starts at the initial cookie; later pages reuse its snapshot.
            let entries: [NTFSEngine.Entry]
            var currentVerifier = verifier.rawValue
            if cookie != .initial, let snapshot = listings.snapshot(currentVerifier) {
                entries = snapshot
            } else if cookie == .initial {
                entries = try engine.list(dir.ino)
                currentVerifier = listings.store(entries)
            } else {
                // The snapshot for this paging session is gone: restarting would skip or repeat
                // entries, so ask the caller to start over.
                throw NTFSEngine.error(EAGAIN)
            }
            var index = cookie == .initial ? 0 : Int(cookie.rawValue)
            while index < entries.count {
                let entry = entries[index]
                let attrs = attributes == nil ? nil : try? attributesFor(entry.ino)
                let packed = packer.packEntry(
                    name: FSFileName(string: entry.name), itemType: Self.itemType(dtype: entry.dtype),
                    itemID: Self.identifier(entry.ino), nextCookie: FSDirectoryCookie(rawValue: UInt64(index + 1)),
                    attributes: attrs)
                if !packed { break }
                index += 1
            }
            if index >= entries.count { listings.drop(currentVerifier) }
            reply(FSDirectoryVerifier(rawValue: currentVerifier), nil)
        } catch {
            reply(verifier, error)
        }
    }

    func createItem(named name: FSFileName, type: FSItem.ItemType, inDirectory directory: FSItem,
                    attributes newAttributes: FSItem.SetAttributesRequest,
                    replyHandler reply: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        do {
            let dir = try Self.ntfsItem(directory)
            guard let string = name.string else { throw NTFSEngine.error(EINVAL) }
            guard type == .file || type == .directory else { throw NTFSEngine.error(ENOTSUP) }
            let st = try engine.create(string, in: dir.ino, directory: type == .directory)
            let created = item(st.ino)
            _ = try? applyAttributes(newAttributes, to: created)
            reply(created, name, nil)
        } catch {
            reply(nil, nil, error)
        }
    }

    func createSymbolicLink(named name: FSFileName, inDirectory directory: FSItem,
                            attributes newAttributes: FSItem.SetAttributesRequest, linkContents contents: FSFileName,
                            replyHandler reply: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        reply(nil, nil, NTFSEngine.error(ENOTSUP))
    }

    func createLink(to item: FSItem, named name: FSFileName, inDirectory directory: FSItem,
                    replyHandler reply: @escaping (FSFileName?, Error?) -> Void) {
        reply(nil, NTFSEngine.error(ENOTSUP))
    }

    func readSymbolicLink(_ item: FSItem, replyHandler reply: @escaping (FSFileName?, Error?) -> Void) {
        do {
            reply(FSFileName(string: try engine.readLink(try Self.ntfsItem(item).ino)), nil)
        } catch {
            reply(nil, error)
        }
    }

    func removeItem(_ item: FSItem, named name: FSFileName, fromDirectory directory: FSItem,
                    replyHandler reply: @escaping (Error?) -> Void) {
        reply(Self.attempt {
            let dir = try Self.ntfsItem(directory)
            guard let string = name.string else { throw NTFSEngine.error(EINVAL) }
            try engine.remove(string, in: dir.ino)
            if let removed = item as? NTFSItem { forget(removed.ino) }
        })
    }

    func renameItem(_ item: FSItem, inDirectory sourceDirectory: FSItem, named sourceName: FSFileName,
                    to destinationName: FSFileName, inDirectory destinationDirectory: FSItem, overItem: FSItem?,
                    replyHandler reply: @escaping (FSFileName?, Error?) -> Void) {
        do {
            let src = try Self.ntfsItem(sourceDirectory), dst = try Self.ntfsItem(destinationDirectory)
            guard let from = sourceName.string, let to = destinationName.string else { throw NTFSEngine.error(EINVAL) }
            try engine.rename(from, in: src.ino, to: to, in: dst.ino)
            if let replaced = overItem as? NTFSItem { forget(replaced.ino) }
            reply(destinationName, nil)
        } catch {
            reply(nil, error)
        }
    }

    static func itemType(dtype: UInt32) -> FSItem.ItemType {
        switch Int32(dtype) {
        case DT_DIR: return .directory
        case DT_LNK: return .symlink
        default: return .file
        }
    }

    /// NTFS root (record 5) is FSKit's root identifier; other records keep their number.
    static func identifier(_ ino: UInt64) -> FSItem.Identifier {
        ino == UInt64(XNTFS_ROOT_INO) ? .rootDirectory : (FSItem.Identifier(rawValue: ino) ?? .invalid)
    }
}
