// SPDX-License-Identifier: GPL-2.0-or-later
// Extended attributes stored as NTFS named data streams ("alternate data streams"): the xattr
// "com.apple.FinderInfo" is the stream file:com.apple.FinderInfo. Windows keeps these with
// the file, and streams Windows created (e.g. Zone.Identifier) show up as xattrs on the Mac.
// Without this macOS writes a "._name" AppleDouble file next to every file.
#include "xntfs_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/xattr.h>

static int err_or(int fallback) { return errno ? errno : fallback; }

/// Opens the inode and converts the xattr name to an NTFS stream name.
static int open_named(xntfs_vol *v, uint64_t ino, const char *name, ntfs_inode **ni, ntfschar **uname) {
    *ni = NULL;
    int len = xntfs_name_to_ucs(name, uname);
    if (len < 0) return len;
    *ni = ntfs_inode_open(v->vol, ino);
    if (!*ni) { free(*uname); *uname = NULL; return -err_or(ENOENT); }
    return len;
}

static int close_named(ntfs_inode *ni, ntfschar *uname, int rc) {
    free(uname);
    if (ntfs_inode_close(ni) && !rc) rc = err_or(EIO);
    return rc;
}

static int get_locked(xntfs_vol *v, uint64_t ino, const char *name, void *buf, size_t bufsize, size_t *size_out) {
    ntfs_inode *ni; ntfschar *uname;
    int len = open_named(v, ino, name, &ni, &uname);
    if (len < 0) return -len;
    int rc = 0;
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, uname, len);
    if (!na) {
        rc = errno == ENOENT ? ENOATTR : err_or(EIO);
    } else {
        *size_out = (size_t)na->data_size;
        if (buf) {
            if (bufsize < (size_t)na->data_size) {
                rc = ERANGE;
            } else {
                s64 done = 0;
                while (done < na->data_size) {
                    s64 n = ntfs_attr_pread(na, done, na->data_size - done, (char *)buf + done);
                    if (n <= 0) { rc = err_or(EIO); break; }
                    done += n;
                }
            }
        }
        ntfs_attr_close(na);
    }
    return close_named(ni, uname, rc);
}

int xntfs_xattr_get(xntfs_vol *v, uint64_t ino, const char *name, void *buf, size_t bufsize, size_t *size_out) {
    return XNTFS_LOCKED(v, get_locked(v, ino, name, buf, bufsize, size_out));
}

/// Writes the whole value into an existing stream, resizing it.
static int overwrite(ntfs_inode *ni, ntfschar *uname, int len, const void *value, size_t size) {
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, uname, len);
    if (!na) return err_or(EIO);
    int rc = 0;
    errno = 0;
    if (ntfs_attr_truncate(na, (s64)size)) rc = err_or(EIO);
    s64 done = 0;
    while (!rc && done < (s64)size) {
        s64 n = ntfs_attr_pwrite(na, done, (s64)size - done, (const char *)value + done);
        if (n <= 0) { rc = err_or(EIO); break; }
        done += n;
    }
    ntfs_attr_close(na);
    return rc;
}

static int set_locked(xntfs_vol *v, uint64_t ino, const char *name, const void *value, size_t size, int flags) {
    if (NVolReadOnly(v->vol)) return EROFS;
    ntfs_inode *ni; ntfschar *uname;
    int len = open_named(v, ino, name, &ni, &uname);
    if (len < 0) return -len;
    int rc;
    errno = 0;
    if (ntfs_attr_exist(ni, AT_DATA, uname, len)) {
        rc = (flags & XNTFS_XATTR_CREATE) ? EEXIST : overwrite(ni, uname, len, value, size);
    } else if (flags & XNTFS_XATTR_REPLACE) {
        rc = ENOATTR;
    } else {
        rc = ntfs_attr_add(ni, AT_DATA, uname, (u8)len, (const u8 *)value, (s64)size) ? err_or(EIO) : 0;
    }
    if (!rc) ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
    return close_named(ni, uname, rc);
}

int xntfs_xattr_set(xntfs_vol *v, uint64_t ino, const char *name, const void *value, size_t size, int flags) {
    return XNTFS_LOCKED(v, set_locked(v, ino, name, value, size, flags));
}

static int remove_locked(xntfs_vol *v, uint64_t ino, const char *name) {
    if (NVolReadOnly(v->vol)) return EROFS;
    ntfs_inode *ni; ntfschar *uname;
    int len = open_named(v, ino, name, &ni, &uname);
    if (len < 0) return -len;
    int rc;
    errno = 0;
    if (!ntfs_attr_exist(ni, AT_DATA, uname, len)) rc = ENOATTR;
    else rc = ntfs_attr_remove(ni, AT_DATA, uname, len) ? err_or(EIO) : 0;
    if (!rc) ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
    return close_named(ni, uname, rc);
}

int xntfs_xattr_remove(xntfs_vol *v, uint64_t ino, const char *name) {
    return XNTFS_LOCKED(v, remove_locked(v, ino, name));
}

static int list_locked(xntfs_vol *v, uint64_t ino, xntfs_xattr_cb cb, void *ctx) {
    ntfs_inode *ni = ntfs_inode_open(v->vol, ino);
    if (!ni) return err_or(ENOENT);
    ntfs_attr_search_ctx *actx = ntfs_attr_get_search_ctx(ni, NULL);
    if (!actx) { int err = err_or(ENOMEM); ntfs_inode_close(ni); return err; }
    int rc = 0;
    while (!ntfs_attr_lookup(AT_DATA, NULL, 0, CASE_SENSITIVE, 0, NULL, 0, actx)) {
        const ATTR_RECORD *a = actx->attr;
        // The unnamed stream is the file contents; only named streams are xattrs. A stream
        // split over several attribute records (non-resident, large) appears once per
        // extent, so list it only at its first extent.
        if (!a->name_length || (a->non_resident && sle64_to_cpu(a->lowest_vcn) != 0)) continue;
        char *name = NULL;
        const ntfschar *uname = (const ntfschar *)((const u8 *)a + le16_to_cpu(a->name_offset));
        if (ntfs_ucstombs(uname, a->name_length, &name, 0) < 0) continue;  // undecodable: skip
        int stop = cb(ctx, name);
        free(name);
        if (stop) break;
    }
    if (errno && errno != ENOENT) rc = errno;
    ntfs_attr_put_search_ctx(actx);
    if (ntfs_inode_close(ni) && !rc) rc = err_or(EIO);
    return rc;
}

int xntfs_xattr_list(xntfs_vol *v, uint64_t ino, xntfs_xattr_cb cb, void *ctx) {
    return XNTFS_LOCKED(v, list_locked(v, ino, cb, ctx));
}
