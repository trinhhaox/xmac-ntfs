// SPDX-License-Identifier: GPL-2.0-or-later
// Shared between the engine's C files; not visible to Swift.
#ifndef XNTFS_INTERNAL_H
#define XNTFS_INTERNAL_H

#include "config.h"
#include "xntfs.h"

#include <pthread.h>
#include <ntfs-3g/types.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/ntfstime.h>
#include <ntfs-3g/reparse.h>

struct xntfs_vol {
    ntfs_volume *vol;
    pthread_mutex_t lock;
    int free_space_known;
    // The file being written stays open between writes: closing it after every write flushed
    // its MFT record, bitmap and log each time (4-5 seeks per chunk on a hard drive). Any
    // operation that could open the same inode again closes it first (XNTFS_LOCKED).
    ntfs_inode *open_ni;
    ntfs_attr *open_na;
    uint64_t open_ino;
    int deferred_err;  // a failed close of that file, reported by the next sync/unmount
};

struct ntfs_device *xntfs_device_new(const xntfs_io *io);
/// Only for a device that never got mounted; ntfs_umount frees mounted devices itself.
void xntfs_device_free(struct ntfs_device *dev);
/// Frees the callback state (and write cache) ntfs_umount leaves behind in d_private.
void xntfs_device_private_free(void *private_data);

/// Converts a UTF-8 name to NTFS UTF-16; returns length or -errno. Caller frees *out.
int xntfs_name_to_ucs(const char *name, ntfschar **out);
/// Namespace primitives (volume lock held). Each opens and closes its own inodes.
u64 xntfs_lookup_in(ntfs_volume *vol, uint64_t dir, const ntfschar *uname, int len);
int xntfs_link_name(ntfs_volume *vol, uint64_t ino, uint64_t dir, const ntfschar *uname, int len);
int xntfs_unlink_name(ntfs_volume *vol, uint64_t dir, const ntfschar *uname, int len);
/// xntfs_getattr for callers already holding the volume lock.
int xntfs_getattr_unlocked(xntfs_vol *v, uint64_t ino, xntfs_stat *out);
/// Fills `out` from an open inode.
int xntfs_fill_stat(ntfs_inode *ni, xntfs_stat *out);

/// Public node id: the full MFT reference (record number + sequence number), so a handle to
/// a deleted file can never reach a new file that reuses its record. The root stays 5.
static inline uint64_t xntfs_ref(u64 mft_no, u16 seq) {
    return mft_no == FILE_root ? FILE_root : MK_MREF(mft_no, seq);
}
static inline uint64_t xntfs_ref_of(ntfs_inode *ni) {
    return xntfs_ref(ni->mft_no, le16_to_cpu(ni->mrec->sequence_number));
}
static inline uint64_t xntfs_ref_from_mref(MFT_REF mref) {
    return xntfs_ref(MREF(mref), MSEQNO(mref));
}

/// The kept-open inode when it is `ino`, else NULL (volume lock held).
ntfs_inode *xntfs_kept_inode(xntfs_vol *v, uint64_t ino);
/// Closes the file kept open for writing (volume lock held). Errors go to deferred_err.
void xntfs_close_open_file(xntfs_vol *v);

/// Runs `expr` under the volume lock, after closing the file kept open for writing.
#define XNTFS_LOCKED(v, expr) ({ pthread_mutex_lock(&(v)->lock); xntfs_close_open_file(v); \
    __typeof__(expr) _r = (expr); pthread_mutex_unlock(&(v)->lock); _r; })
/// For operations that never open the kept-open file a second time (they reuse it or touch
/// other inodes only): read, write, attributes, lookup, listing, statfs.
#define XNTFS_LOCKED_KEEP(v, expr) ({ pthread_mutex_lock(&(v)->lock); \
    __typeof__(expr) _r = (expr); pthread_mutex_unlock(&(v)->lock); _r; })

#endif
