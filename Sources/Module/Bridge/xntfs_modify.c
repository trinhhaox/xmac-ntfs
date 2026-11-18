// SPDX-License-Identifier: GPL-2.0-or-later
// Namespace primitives (lookup, link, unlink) plus create and remove. Rename: xntfs_rename.c.
//
// Each primitive opens and closes its own inodes. libntfs-3g re-opens parent directories
// internally (e.g. to sync $FILE_NAME copies), so a directory handle held across two namespace
// changes goes stale and can be freed underneath us.
#include "xntfs_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>

static int err_or(int fallback) { return errno ? errno : fallback; }

/// Node id of `name` in the open directory, or 0 with errno set.
static u64 lookup_open(ntfs_inode *dir_ni, const ntfschar *uname, int len) {
    u64 mref = ntfs_inode_lookup_by_name(dir_ni, uname, len);
    return mref == (u64)-1 ? 0 : xntfs_ref_from_mref(mref);
}

u64 xntfs_lookup_in(ntfs_volume *vol, uint64_t dir, const ntfschar *uname, int len) {
    ntfs_inode *dir_ni = ntfs_inode_open(vol, dir);
    if (!dir_ni) return 0;
    u64 ino = lookup_open(dir_ni, uname, len);
    int err = errno;
    ntfs_inode_close(dir_ni);
    errno = err;
    return ino;
}

int xntfs_unlink_name(ntfs_volume *vol, uint64_t dir, const ntfschar *uname, int len) {
    ntfs_inode *dir_ni = ntfs_inode_open(vol, dir);
    if (!dir_ni) return err_or(ENOENT);
    u64 ino = lookup_open(dir_ni, uname, len);
    ntfs_inode *ni = ino ? ntfs_inode_open(vol, ino) : NULL;
    if (!ni) {
        int err = err_or(ENOENT);
        ntfs_inode_close(dir_ni);
        return err;
    }
    errno = 0;
    // ntfs_delete closes both `ni` and `dir_ni`, on success and on failure. It refuses
    // (ENOTEMPTY) to drop the last name of a non-empty directory, but allows it when the
    // directory has another name (the middle of a rename).
    return ntfs_delete(vol, NULL, ni, dir_ni, uname, (u8)len) ? err_or(EIO) : 0;
}

int xntfs_link_name(ntfs_volume *vol, uint64_t ino, uint64_t dir, const ntfschar *uname, int len) {
    ntfs_inode *dir_ni = ntfs_inode_open(vol, dir);
    if (!dir_ni) return err_or(ENOENT);
    ntfs_inode *ni = ntfs_inode_open(vol, ino);
    if (!ni) { int err = err_or(EIO); ntfs_inode_close(dir_ni); return err; }
    errno = 0;
    int rc = ntfs_link(ni, dir_ni, uname, (u8)len) ? err_or(EIO) : 0;
    if (!rc) {
        ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
        ntfs_inode_update_times(dir_ni, NTFS_UPDATE_MCTIME);
    }
    // Directory first: syncing `ni`'s $FILE_NAME copies needs the new index entry on disk.
    if (ntfs_inode_close(dir_ni) && !rc) rc = err_or(EIO);
    if (ntfs_inode_close(ni) && !rc) rc = err_or(EIO);
    return rc;
}

static int create_locked(xntfs_vol *v, uint64_t dir, const char *name, uint32_t type, xntfs_stat *out) {
    if (NVolReadOnly(v->vol)) return EROFS;
    if (type != S_IFREG && type != S_IFDIR) return EINVAL;
    ntfschar *uname = NULL;
    int len = xntfs_name_to_ucs(name, &uname);
    if (len < 0) return -len;
    int rc = 0;
    ntfs_inode *dir_ni = ntfs_inode_open(v->vol, dir);
    if (!dir_ni) { rc = err_or(ENOENT); goto out; }
    if (lookup_open(dir_ni, uname, len)) { rc = EEXIST; goto close_dir; }
    errno = 0;
    ntfs_inode *ni = ntfs_create(dir_ni, const_cpu_to_le32(0), uname, (u8)len, (mode_t)type);
    if (!ni) { rc = err_or(EIO); goto close_dir; }
    rc = out ? xntfs_fill_stat(ni, out) : 0;
    if (ntfs_inode_close_in_dir(ni, dir_ni)) {
        if (!rc) rc = err_or(EIO);
        ntfs_inode_close(ni);  // close_in_dir leaves `ni` open when its sync fails
    }
    ntfs_inode_update_times(dir_ni, NTFS_UPDATE_MCTIME);
close_dir:
    if (ntfs_inode_close(dir_ni) && !rc) rc = err_or(EIO);
out:
    free(uname);
    return rc;
}

int xntfs_create(xntfs_vol *v, uint64_t dir, const char *name, uint32_t type, xntfs_stat *out) {
    return XNTFS_LOCKED(v, create_locked(v, dir, name, type, out));
}

static int remove_locked(xntfs_vol *v, uint64_t dir, const char *name) {
    if (NVolReadOnly(v->vol)) return EROFS;
    ntfschar *uname = NULL;
    int len = xntfs_name_to_ucs(name, &uname);
    if (len < 0) return -len;
    int rc = xntfs_unlink_name(v->vol, dir, uname, len);
    free(uname);
    return rc;
}

int xntfs_remove(xntfs_vol *v, uint64_t dir, const char *name) {
    return XNTFS_LOCKED(v, remove_locked(v, dir, name));
}
