// SPDX-License-Identifier: GPL-2.0-or-later
// Read-side node operations: attributes, lookup, directory listing, symlink targets.
#include "xntfs_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int err_or(int fallback) { return errno ? errno : fallback; }

/// Parent directory from the first $FILE_NAME attribute (root is its own parent). Every name
/// of a file created through this engine sits in one directory; for Windows hard links in
/// several directories this is the first one.
static uint64_t parent_of(ntfs_inode *ni) {
    if (ni->mft_no == FILE_root) return FILE_root;
    uint64_t parent = FILE_root;
    ntfs_attr_search_ctx *ctx = ntfs_attr_get_search_ctx(ni, NULL);
    if (!ctx) return parent;
    if (!ntfs_attr_lookup(AT_FILE_NAME, AT_UNNAMED, 0, CASE_SENSITIVE, 0, NULL, 0, ctx)) {
        const FILE_NAME_ATTR *fn = (const FILE_NAME_ATTR *)((const u8 *)ctx->attr
            + le16_to_cpu(ctx->attr->value_offset));
        parent = xntfs_ref_from_mref(le64_to_cpu(fn->parent_directory));
    }
    ntfs_attr_put_search_ctx(ctx);
    return parent;
}

int xntfs_fill_stat(ntfs_inode *ni, xntfs_stat *out) {
    memset(out, 0, sizeof(*out));
    out->ino = xntfs_ref_of(ni);
    out->parent = parent_of(ni);
    out->nlink = le16_to_cpu(ni->mrec->link_count);
    out->ntfs_flags = le32_to_cpu(ni->flags);
    out->birth = ntfs2timespec(ni->creation_time);
    out->mtime = ntfs2timespec(ni->last_data_change_time);
    out->ctime = ntfs2timespec(ni->last_mft_change_time);
    out->atime = ntfs2timespec(ni->last_access_time);

    if ((ni->flags & FILE_ATTR_REPARSE_POINT) && ntfs_possible_symlink(ni)) {
        char *target = ntfs_make_symlink(ni, "");
        if (target) {
            out->mode = S_IFLNK | 0777;
            out->size = strlen(target);
            free(target);
            return 0;
        }
    }
    if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
        out->mode = S_IFDIR | 0755;
        out->nlink = 2;  // NTFS link counts do not count subdirectories
        return 0;
    }
    out->mode = S_IFREG | 0644;
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (na) {
        out->size = (uint64_t)na->data_size;
        out->alloc_size = (uint64_t)((na->data_flags & (ATTR_COMPRESSION_MASK | ATTR_IS_SPARSE))
            ? na->compressed_size : na->allocated_size);
        ntfs_attr_close(na);
    } else if (errno != ENOENT) {
        return err_or(EIO);
    }
    return 0;
}

int xntfs_getattr_unlocked(xntfs_vol *v, uint64_t ino, xntfs_stat *out) {
    ntfs_inode *ni = ntfs_inode_open(v->vol, ino);
    if (!ni) return err_or(ENOENT);
    int rc = xntfs_fill_stat(ni, out);
    ntfs_inode_close(ni);
    return rc;
}

int xntfs_getattr(xntfs_vol *v, uint64_t ino, xntfs_stat *out) {
    return XNTFS_LOCKED(v, xntfs_getattr_unlocked(v, ino, out));
}

static int lookup_locked(xntfs_vol *v, uint64_t dir, const char *name, xntfs_stat *out) {
    ntfschar *uname = NULL;
    int len = xntfs_name_to_ucs(name, &uname);
    if (len < 0) return -len;
    ntfs_inode *dir_ni = ntfs_inode_open(v->vol, dir);
    if (!dir_ni) { free(uname); return err_or(ENOENT); }
    u64 mref = ntfs_inode_lookup_by_name(dir_ni, uname, len);
    int rc = mref == (u64)-1 ? err_or(ENOENT) : xntfs_getattr_unlocked(v, xntfs_ref_from_mref(mref), out);
    ntfs_inode_close(dir_ni);
    free(uname);
    return rc;
}

int xntfs_lookup(xntfs_vol *v, uint64_t dir, const char *name, xntfs_stat *out) {
    return XNTFS_LOCKED(v, lookup_locked(v, dir, name, out));
}

typedef struct {
    xntfs_dirent_cb cb;
    void *ctx;
    int stopped;
} readdir_state;

static int filldir(void *opaque, const ntfschar *name, const int name_len, const int name_type,
                   const s64 pos, const MFT_REF mref, const unsigned dt_type) {
    (void)pos;
    readdir_state *st = opaque;
    if (name_type == FILE_NAME_DOS) return 0;            // 8.3 alias of a long name
    if (MREF(mref) < FILE_first_user && MREF(mref) != FILE_root) return 0;  // metafiles
    char *utf8 = NULL;
    if (ntfs_ucstombs(name, name_len, &utf8, 0) < 0) return 0;  // undecodable name: skip
    int skip = strcmp(utf8, ".") == 0 || strcmp(utf8, "..") == 0;
    int rc = skip ? 0 : st->cb(st->ctx, utf8, xntfs_ref_from_mref(mref), dt_type);
    free(utf8);
    if (rc) st->stopped = 1;
    return rc;
}

static int readdir_locked(xntfs_vol *v, uint64_t dir, xntfs_dirent_cb cb, void *ctx) {
    ntfs_inode *dir_ni = ntfs_inode_open(v->vol, dir);
    if (!dir_ni) return err_or(ENOENT);
    if (!(dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)) { ntfs_inode_close(dir_ni); return ENOTDIR; }
    readdir_state st = { cb, ctx, 0 };
    s64 pos = 0;
    // The volume runs case-insensitive (lookups, deletes), but in that mode libntfs-3g lists
    // every name in lower case. List case-sensitively so names keep the case they were saved in.
    NVolSetCaseSensitive(v->vol);
    int rc = ntfs_readdir(dir_ni, &pos, &st, filldir);
    NVolClearCaseSensitive(v->vol);
    int err = (rc && !st.stopped) ? err_or(EIO) : 0;
    ntfs_inode_close(dir_ni);
    return err;
}

int xntfs_readdir(xntfs_vol *v, uint64_t dir, xntfs_dirent_cb cb, void *ctx) {
    return XNTFS_LOCKED(v, readdir_locked(v, dir, cb, ctx));
}

static int readlink_locked(xntfs_vol *v, uint64_t ino, char *buf, size_t bufsize) {
    ntfs_inode *ni = ntfs_inode_open(v->vol, ino);
    if (!ni) return err_or(ENOENT);
    char *target = ntfs_possible_symlink(ni) ? ntfs_make_symlink(ni, "") : NULL;
    ntfs_inode_close(ni);
    if (!target) return EINVAL;
    size_t n = strlcpy(buf, target, bufsize);
    free(target);
    return n >= bufsize ? ENAMETOOLONG : 0;
}

int xntfs_readlink(xntfs_vol *v, uint64_t ino, char *buf, size_t bufsize) {
    return XNTFS_LOCKED(v, readlink_locked(v, ino, buf, bufsize));
}
