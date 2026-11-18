// SPDX-License-Identifier: GPL-2.0-or-later
// Symbolic links (as WSL reparse points) and hard links.
#include "xntfs_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>

static int err_or(int fallback) { return errno ? errno : fallback; }

/// Symlinks are written as WSL symlinks (IO_REPARSE_TAG_LX_SYMLINK): the POSIX target is kept
/// byte for byte, relative or absolute, and WSL and ntfs-3g read them back. (Windows-native
/// symlinks would need the target's type and drive-letter paths, which a Mac cannot know.)
static int symlink_locked(xntfs_vol *v, uint64_t dir, const char *name, const char *target, xntfs_stat *out) {
    if (NVolReadOnly(v->vol)) return EROFS;
    ntfschar *uname = NULL, *utarget = NULL;
    int len = xntfs_name_to_ucs(name, &uname);
    if (len < 0) return -len;
    int tlen = ntfs_mbstoucs(target, &utarget);
    if (tlen <= 0) { free(uname); return tlen < 0 ? err_or(EILSEQ) : EINVAL; }

    int rc = 0;
    ntfs_inode *dir_ni = ntfs_inode_open(v->vol, dir);
    if (!dir_ni) { rc = err_or(ENOENT); goto out; }
    if (ntfs_inode_lookup_by_name(dir_ni, uname, len) != (u64)-1) { rc = EEXIST; goto close_dir; }
    v->vol->special_files = NTFS_FILES_WSL;
    errno = 0;
    ntfs_inode *ni = ntfs_create_symlink(dir_ni, const_cpu_to_le32(0), uname, (u8)len, utarget, tlen);
    if (!ni) { rc = err_or(EIO); goto close_dir; }
    rc = out ? xntfs_fill_stat(ni, out) : 0;
    if (ntfs_inode_close_in_dir(ni, dir_ni)) {
        if (!rc) rc = err_or(EIO);
        ntfs_inode_close(ni);
    }
    ntfs_inode_update_times(dir_ni, NTFS_UPDATE_MCTIME);
close_dir:
    if (ntfs_inode_close(dir_ni) && !rc) rc = err_or(EIO);
out:
    free(uname); free(utarget);
    return rc;
}

int xntfs_symlink(xntfs_vol *v, uint64_t dir, const char *name, const char *target, xntfs_stat *out) {
    return XNTFS_LOCKED(v, symlink_locked(v, dir, name, target, out));
}

static int link_locked(xntfs_vol *v, uint64_t ino, uint64_t dir, const char *name) {
    if (NVolReadOnly(v->vol)) return EROFS;
    xntfs_stat st;
    int rc = xntfs_getattr_unlocked(v, ino, &st);
    if (rc) return rc;
    if (S_ISDIR(st.mode)) return EPERM;  // like every POSIX file system
    if (st.nlink >= 1024) return EMLINK;
    ntfschar *uname = NULL;
    int len = xntfs_name_to_ucs(name, &uname);
    if (len < 0) return -len;
    rc = xntfs_lookup_in(v->vol, dir, uname, len) ? EEXIST : xntfs_link_name(v->vol, ino, dir, uname, len);
    free(uname);
    return rc;
}

int xntfs_link(xntfs_vol *v, uint64_t ino, uint64_t dir, const char *name) {
    return XNTFS_LOCKED(v, link_locked(v, ino, dir, name));
}
