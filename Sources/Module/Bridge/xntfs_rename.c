// SPDX-License-Identifier: GPL-2.0-or-later
// rename(2) on NTFS, built from link and unlink so that a failure part-way never loses data:
// at every step the source stays reachable by at least one name.
#include "xntfs_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static int err_or(int fallback) { return errno ? errno : fallback; }

/// Whether `dir` is `ancestor` or lies below it. Walks $FILE_NAME parents (NTFS indexes have
/// no ".." entry) and fails closed: any unreadable step counts as "inside".
static int inside(xntfs_vol *v, uint64_t ancestor, uint64_t dir) {
    for (int depth = 0; depth < 4096; depth++) {
        if (MREF(dir) == MREF(ancestor)) return 1;
        if (MREF(dir) == FILE_root) return 0;
        xntfs_stat st;
        if (xntfs_getattr_unlocked(v, dir, &st)) return 1;
        dir = st.parent;
    }
    return 1;
}

/// Temporary name for `ino` used while swapping names.
static int temp_name(uint64_t ino, const char *tag, ntfschar **out) {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), ".xmac-%s-%llx", tag, (unsigned long long)MREF(ino));
    return xntfs_name_to_ucs(tmp, out);
}

/// Same directory, same file, names differing only in case: hop through a temporary name,
/// because NTFS compares names case-insensitively and the new name collides with the old one.
static int rename_case(ntfs_volume *vol, u64 ino, uint64_t dir, const ntfschar *sname, int slen,
                       const ntfschar *dname, int dlen) {
    ntfschar *tname = NULL;
    int tlen = temp_name(ino, "case", &tname);
    if (tlen < 0) return -tlen;
    int rc;
    if (!(rc = xntfs_link_name(vol, ino, dir, tname, tlen))
        && !(rc = xntfs_unlink_name(vol, dir, sname, slen))
        && !(rc = xntfs_link_name(vol, ino, dir, dname, dlen)))
        rc = xntfs_unlink_name(vol, dir, tname, tlen);
    free(tname);
    return rc;
}

/// Replaces an existing destination: the source gets a temporary name in the destination
/// directory before the destination is removed, so running out of index space cannot leave
/// the caller with neither the old nor the new file.
static int rename_over(xntfs_vol *v, u64 ino, u64 existing, uint64_t src_dir, const ntfschar *sname, int slen,
                       uint64_t dst_dir, const ntfschar *dname, int dlen) {
    xntfs_stat s, d;
    if (xntfs_getattr_unlocked(v, ino, &s) || xntfs_getattr_unlocked(v, existing, &d)) return err_or(EIO);
    if (S_ISDIR(s.mode) && !S_ISDIR(d.mode)) return ENOTDIR;
    if (!S_ISDIR(s.mode) && S_ISDIR(d.mode)) return EISDIR;

    ntfschar *tname = NULL;
    int tlen = temp_name(ino, "move", &tname);
    if (tlen < 0) return -tlen;
    int rc = xntfs_link_name(v->vol, ino, dst_dir, tname, tlen);
    if (!rc) {
        rc = xntfs_unlink_name(v->vol, dst_dir, dname, dlen);  // ENOTEMPTY for a full directory
        if (rc) {
            xntfs_unlink_name(v->vol, dst_dir, tname, tlen);
        } else if (!(rc = xntfs_link_name(v->vol, ino, dst_dir, dname, dlen))) {
            if (!(rc = xntfs_unlink_name(v->vol, dst_dir, tname, tlen)))
                rc = xntfs_unlink_name(v->vol, src_dir, sname, slen);
        }
    }
    free(tname);
    return rc;
}

static int rename_names(xntfs_vol *v, uint64_t src_dir, const ntfschar *sname, int slen,
                        uint64_t dst_dir, const ntfschar *dname, int dlen) {
    ntfs_volume *vol = v->vol;
    u64 ino = xntfs_lookup_in(vol, src_dir, sname, slen);
    if (!ino) return err_or(ENOENT);
    // A directory cannot move into itself or below itself.
    if (inside(v, ino, dst_dir)) {
        xntfs_stat st;
        if (xntfs_getattr_unlocked(v, ino, &st) || S_ISDIR(st.mode)) return EINVAL;
    }
    u64 existing = xntfs_lookup_in(vol, dst_dir, dname, dlen);
    if (existing == ino) {
        int same_name = MREF(src_dir) == MREF(dst_dir)
            && ntfs_names_are_equal(sname, slen, dname, dlen, IGNORE_CASE, vol->upcase, vol->upcase_len);
        if (!same_name) return 0;  // another hard link of the same file: rename(2) does nothing
        if (ntfs_names_are_equal(sname, slen, dname, dlen, CASE_SENSITIVE, vol->upcase, vol->upcase_len))
            return 0;  // identical name
        return rename_case(vol, ino, src_dir, sname, slen, dname, dlen);
    }
    if (existing) return rename_over(v, ino, existing, src_dir, sname, slen, dst_dir, dname, dlen);
    // Link first, then unlink: a failure in between leaves the file reachable by both names.
    int rc = xntfs_link_name(vol, ino, dst_dir, dname, dlen);
    return rc ? rc : xntfs_unlink_name(vol, src_dir, sname, slen);
}

static int rename_locked(xntfs_vol *v, uint64_t src_dir, const char *src_name, uint64_t dst_dir, const char *dst_name) {
    if (NVolReadOnly(v->vol)) return EROFS;
    ntfschar *sname = NULL, *dname = NULL;
    int slen = xntfs_name_to_ucs(src_name, &sname);
    int dlen = slen < 0 ? slen : xntfs_name_to_ucs(dst_name, &dname);
    int rc = (slen < 0 || dlen < 0) ? -(slen < 0 ? slen : dlen)
        : rename_names(v, src_dir, sname, slen, dst_dir, dname, dlen);
    free(sname); free(dname);
    return rc;
}

int xntfs_rename(xntfs_vol *v, uint64_t src_dir, const char *src_name, uint64_t dst_dir, const char *dst_name) {
    return XNTFS_LOCKED(v, rename_locked(v, src_dir, src_name, dst_dir, dst_name));
}
