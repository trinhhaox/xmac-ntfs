// SPDX-License-Identifier: GPL-2.0-or-later
// File contents and metadata: read, write, truncate, times, Windows attribute flags.
#include "xntfs_internal.h"

#include <errno.h>

static int err_or(int fallback) { return errno ? errno : fallback; }

/// Opens the unnamed $DATA stream of a regular file.
static int open_data(xntfs_vol *v, uint64_t ino, ntfs_inode **ni, ntfs_attr **na) {
    *na = NULL;
    *ni = ntfs_inode_open(v->vol, ino);
    if (!*ni) return err_or(ENOENT);
    if ((*ni)->mrec->flags & MFT_RECORD_IS_DIRECTORY) { ntfs_inode_close(*ni); return EISDIR; }
    // Reparse points other than symlinks (WOF/CompactOS compression, dedup, cloud files) keep
    // their real contents elsewhere: the $DATA stub would read as zeros and writes would be
    // ignored by Windows. Refuse, like ntfs-3g without plugins.
    if ((*ni)->flags & FILE_ATTR_REPARSE_POINT) { ntfs_inode_close(*ni); return EOPNOTSUPP; }
    *na = ntfs_attr_open(*ni, AT_DATA, AT_UNNAMED, 0);
    if (!*na) { int err = err_or(EIO); ntfs_inode_close(*ni); return err; }
    return 0;
}

/// Closes both; returns the first error.
static int close_data(ntfs_inode *ni, ntfs_attr *na, int rc) {
    ntfs_attr_close(na);
    if (ntfs_inode_close(ni) && !rc) rc = err_or(EIO);
    return rc;
}

void xntfs_close_open_file(xntfs_vol *v) {
    if (!v->open_ni) return;
    errno = 0;
    int rc = close_data(v->open_ni, v->open_na, 0);
    if (rc && !v->deferred_err) v->deferred_err = rc;
    v->open_ni = NULL;
    v->open_na = NULL;
    v->open_ino = 0;
}

/// The $DATA stream of `ino`, kept open for following reads and writes.
static int kept_data(xntfs_vol *v, uint64_t ino, ntfs_attr **na) {
    if (v->open_ni && v->open_ino == ino) { *na = v->open_na; return 0; }
    xntfs_close_open_file(v);
    ntfs_inode *ni;
    int rc = open_data(v, ino, &ni, na);
    if (rc) return rc;
    v->open_ni = ni;
    v->open_na = *na;
    v->open_ino = ino;
    return 0;
}

ntfs_inode *xntfs_kept_inode(xntfs_vol *v, uint64_t ino) {
    return v->open_ni && v->open_ino == ino ? v->open_ni : NULL;
}

static int64_t read_locked(xntfs_vol *v, uint64_t ino, void *buf, int64_t count, int64_t offset) {
    ntfs_attr *na;
    int rc = kept_data(v, ino, &na);
    if (rc) return -rc;
    int64_t done = 0;
    if (offset < na->data_size) {
        if (offset + count > na->data_size) count = na->data_size - offset;
        while (done < count) {
            s64 n = ntfs_attr_pread(na, offset + done, count - done, (char *)buf + done);
            if (n <= 0) { if (n < 0) return -err_or(EIO); break; }
            done += n;
        }
    }
    return done;
}

int64_t xntfs_read(xntfs_vol *v, uint64_t ino, void *buf, int64_t count, int64_t offset) {
    return XNTFS_LOCKED_KEEP(v, read_locked(v, ino, buf, count, offset));
}

static int64_t write_locked(xntfs_vol *v, uint64_t ino, const void *buf, int64_t count, int64_t offset) {
    if (NVolReadOnly(v->vol)) return -EROFS;
    ntfs_attr *na;
    int rc = kept_data(v, ino, &na);
    if (rc) return -rc;
    int64_t done = 0;
    errno = 0;
    while (done < count) {
        // Extends the stream when writing past the end.
        s64 n = ntfs_attr_pwrite(na, offset + done, count - done, (const char *)buf + done);
        if (n <= 0) { rc = err_or(EIO); break; }
        done += n;
    }
    if (done > 0) ntfs_inode_update_times(v->open_ni, NTFS_UPDATE_MCTIME);
    // A partial write reports what landed, like write(2). The new size and runlist reach the
    // disk when the file is closed (next unrelated operation, xntfs_sync, or unmount).
    return done > 0 ? done : -rc;
}

int64_t xntfs_write(xntfs_vol *v, uint64_t ino, const void *buf, int64_t count, int64_t offset) {
    return XNTFS_LOCKED_KEEP(v, write_locked(v, ino, buf, count, offset));
}

static int truncate_locked(xntfs_vol *v, uint64_t ino, uint64_t size) {
    if (NVolReadOnly(v->vol)) return EROFS;
    ntfs_inode *ni; ntfs_attr *na;
    int rc = open_data(v, ino, &ni, &na);
    if (rc) return rc;
    if ((s64)size != na->data_size) {
        errno = 0;
        rc = ntfs_attr_truncate(na, (s64)size) ? err_or(EIO) : 0;
        if (!rc) ntfs_inode_update_times(ni, NTFS_UPDATE_MCTIME);
    }
    return close_data(ni, na, rc);
}

int xntfs_truncate(xntfs_vol *v, uint64_t ino, uint64_t size) {
    return XNTFS_LOCKED(v, truncate_locked(v, ino, size));
}

static int set_times_locked(xntfs_vol *v, uint64_t ino, const struct timespec *birth,
                            const struct timespec *mtime, const struct timespec *atime) {
    if (NVolReadOnly(v->vol)) return EROFS;
    ntfs_inode *ni = ntfs_inode_open(v->vol, ino);
    if (!ni) return err_or(ENOENT);
    if (birth) ni->creation_time = timespec2ntfs(*birth);
    if (mtime) ni->last_data_change_time = timespec2ntfs(*mtime);
    if (atime) ni->last_access_time = timespec2ntfs(*atime);
    ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
    NInoFileNameSetDirty(ni);  // keep the $FILE_NAME copies in the parent index in step
    return ntfs_inode_close(ni) ? err_or(EIO) : 0;
}

int xntfs_set_times(xntfs_vol *v, uint64_t ino, const struct timespec *birth,
                    const struct timespec *mtime, const struct timespec *atime) {
    return XNTFS_LOCKED(v, set_times_locked(v, ino, birth, mtime, atime));
}

static int set_flags_locked(xntfs_vol *v, uint64_t ino, uint32_t set, uint32_t clear) {
    if (NVolReadOnly(v->vol)) return EROFS;
    const uint32_t allowed = le32_to_cpu(FILE_ATTR_HIDDEN | FILE_ATTR_READONLY);
    ntfs_inode *ni = ntfs_inode_open(v->vol, ino);
    if (!ni) return err_or(ENOENT);
    // On folders Windows uses READONLY to mark customised folders (desktop.ini): leave it alone.
    if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
        set &= ~le32_to_cpu(FILE_ATTR_READONLY);
        clear &= ~le32_to_cpu(FILE_ATTR_READONLY);
    }
    uint32_t flags = le32_to_cpu(ni->flags);
    uint32_t updated = (flags | (set & allowed)) & ~(clear & allowed);
    if (updated != flags) {
        ni->flags = cpu_to_le32(updated);
        ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
        NInoSetDirty(ni);
        NInoFileNameSetDirty(ni);
    }
    return ntfs_inode_close(ni) ? err_or(EIO) : 0;
}

int xntfs_set_flags(xntfs_vol *v, uint64_t ino, uint32_t set, uint32_t clear) {
    return XNTFS_LOCKED(v, set_flags_locked(v, ino, set, clear));
}
