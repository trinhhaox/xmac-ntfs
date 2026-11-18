// SPDX-License-Identifier: GPL-2.0-or-later
// Xmac NTFS engine: a small, thread-safe C facade over libntfs-3g for the FSKit module.
// Every call takes the volume lock; libntfs-3g itself is not thread-safe.
// Errors are positive errno values; 0 means success.
#ifndef XNTFS_H
#define XNTFS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Raw device the engine reads and writes. Offsets and counts handed to these callbacks are
/// always multiples of `sector_size`; the engine bounces unaligned requests from libntfs-3g.
typedef struct xntfs_io {
    void *ctx;
    int64_t (*pread)(void *ctx, void *buf, int64_t count, int64_t offset);
    int64_t (*pwrite)(void *ctx, const void *buf, int64_t count, int64_t offset);
    int (*sync)(void *ctx);
    int64_t size_bytes;
    uint32_t sector_size;
} xntfs_io;

typedef struct xntfs_vol xntfs_vol;

/// The NTFS root directory's id. Other ids are full MFT references (record | sequence << 48).
#define XNTFS_ROOT_INO 5ULL

typedef struct xntfs_stat {
    uint64_t ino;
    uint64_t parent;        // MFT number of the directory holding its first name
    uint32_t mode;          // S_IFDIR | 0755, S_IFREG | 0644, S_IFLNK | 0777
    uint32_t nlink;
    uint64_t size;
    uint64_t alloc_size;
    uint32_t ntfs_flags;    // FILE_ATTR_* (hidden, system, readonly, ...)
    struct timespec birth, mtime, ctime, atime;
} xntfs_stat;

typedef struct xntfs_statfs {
    uint64_t total_bytes, free_bytes;
    uint32_t cluster_size;
    uint64_t total_files, free_files;
    char label[256];        // UTF-8
    uint64_t serial;
} xntfs_statfs;

/// Mount state that forces read-only: unclean shutdown, Windows hibernation / Fast Startup.
enum { XNTFS_MOUNT_WRITABLE = 0, XNTFS_MOUNT_FORCED_RO_DIRTY = 1, XNTFS_MOUNT_FORCED_RO_HIBERNATED = 2 };

/// Mounts the volume. With `want_write`, falls back to read-only when the volume is unsafe to
/// write and reports why in `*ro_reason` (XNTFS_MOUNT_*).
int xntfs_mount(const xntfs_io *io, int want_write, xntfs_vol **out, int *ro_reason);
int xntfs_unmount(xntfs_vol *vol);
int xntfs_is_readonly(xntfs_vol *vol);
int xntfs_sync(xntfs_vol *vol);
/// Cheap: free space is counted once at mount and kept current by libntfs-3g.
int xntfs_get_statfs(xntfs_vol *vol, xntfs_statfs *out);
/// Volume label (UTF-8), empty when unset.
int xntfs_get_label(xntfs_vol *vol, char *buf, size_t bufsize);

int xntfs_getattr(xntfs_vol *vol, uint64_t ino, xntfs_stat *out);
/// `name` is UTF-8 (NFC). ENOENT when absent.
int xntfs_lookup(xntfs_vol *vol, uint64_t dir, const char *name, xntfs_stat *out);

/// Lists the whole directory, calling `cb` once per entry ("." "..", DOS 8.3 aliases and
/// metafiles are skipped). `dtype` is a DT_* value. Return non-zero from `cb` to stop early.
/// Callers that page (FSKit) snapshot the list: libntfs-3g positions are not stable cookies.
typedef int (*xntfs_dirent_cb)(void *ctx, const char *name, uint64_t ino, uint32_t dtype);
int xntfs_readdir(xntfs_vol *vol, uint64_t dir, xntfs_dirent_cb cb, void *ctx);

/// `type` is S_IFREG or S_IFDIR. EEXIST when the name is taken.
int xntfs_create(xntfs_vol *vol, uint64_t dir, const char *name, uint32_t type, xntfs_stat *out);
/// Target of a Windows symlink / junction or Interix symlink, as a POSIX path.
int xntfs_readlink(xntfs_vol *vol, uint64_t ino, char *buf, size_t bufsize);
/// Removes a file, or an empty directory (ENOTEMPTY otherwise).
int xntfs_remove(xntfs_vol *vol, uint64_t dir, const char *name);
/// Replaces an existing destination file or empty directory, like rename(2).
int xntfs_rename(xntfs_vol *vol, uint64_t src_dir, const char *src_name, uint64_t dst_dir, const char *dst_name);

/// Byte counts actually transferred, or a negative errno.
int64_t xntfs_read(xntfs_vol *vol, uint64_t ino, void *buf, int64_t count, int64_t offset);
int64_t xntfs_write(xntfs_vol *vol, uint64_t ino, const void *buf, int64_t count, int64_t offset);
int xntfs_truncate(xntfs_vol *vol, uint64_t ino, uint64_t size);
/// NULL leaves that time unchanged.
int xntfs_set_times(xntfs_vol *vol, uint64_t ino, const struct timespec *birth,
                    const struct timespec *mtime, const struct timespec *atime);
/// Sets or clears FILE_ATTR_HIDDEN / FILE_ATTR_READONLY.
int xntfs_set_flags(xntfs_vol *vol, uint64_t ino, uint32_t set, uint32_t clear);

/// Extended attributes, stored as NTFS named data streams. Missing attribute: ENOATTR.
enum { XNTFS_XATTR_CREATE = 1, XNTFS_XATTR_REPLACE = 2 };
/// With `buf` NULL only reports the size; ERANGE when `bufsize` is too small.
int xntfs_xattr_get(xntfs_vol *vol, uint64_t ino, const char *name, void *buf, size_t bufsize, size_t *size_out);
int xntfs_xattr_set(xntfs_vol *vol, uint64_t ino, const char *name, const void *value, size_t size, int flags);
int xntfs_xattr_remove(xntfs_vol *vol, uint64_t ino, const char *name);
/// Return non-zero from `cb` to stop early.
typedef int (*xntfs_xattr_cb)(void *ctx, const char *name);
int xntfs_xattr_list(xntfs_vol *vol, uint64_t ino, xntfs_xattr_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif
#endif
