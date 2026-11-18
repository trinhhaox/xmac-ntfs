// SPDX-License-Identifier: GPL-2.0-or-later
import FSKit
import Foundation

/// Extended attributes as NTFS named streams. Supporting them stops macOS from writing a
/// "._name" AppleDouble file beside every file, which Windows users would see.
extension NTFSVolume: FSVolume.XattrOperations {
    func getXattr(named name: FSFileName, of item: FSItem, replyHandler reply: @escaping (Data?, Error?) -> Void) {
        do {
            guard let key = name.string else { throw NTFSEngine.error(EINVAL) }
            reply(try engine.xattr(key, of: try Self.ntfsItem(item).ino), nil)
        } catch {
            reply(nil, error)
        }
    }

    func setXattr(named name: FSFileName, to value: Data?, on item: FSItem, policy: FSVolume.SetXattrPolicy,
                  replyHandler reply: @escaping (Error?) -> Void) {
        reply(Self.attempt {
            guard let key = name.string else { throw NTFSEngine.error(EINVAL) }
            let ino = try Self.ntfsItem(item).ino
            switch policy {
            case .delete:
                try engine.removeXattr(key, of: ino)
            case .mustCreate:
                try engine.setXattr(key, of: ino, to: value ?? Data(), mode: .create)
            case .mustReplace:
                try engine.setXattr(key, of: ino, to: value ?? Data(), mode: .replace)
            default:
                try engine.setXattr(key, of: ino, to: value ?? Data(), mode: .any)
            }
        })
    }

    func listXattrs(of item: FSItem, replyHandler reply: @escaping ([FSFileName]?, Error?) -> Void) {
        do {
            reply(try engine.xattrNames(of: try Self.ntfsItem(item).ino).map { FSFileName(string: $0) }, nil)
        } catch {
            reply(nil, error)
        }
    }
}
