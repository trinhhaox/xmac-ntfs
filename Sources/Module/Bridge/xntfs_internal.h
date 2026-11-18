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
};

struct ntfs_device *xntfs_device_new(const xntfs_io *io);
/// Only for a device that never got mounted; ntfs_umount frees mounted devices itself.
void xntfs_device_free(struct ntfs_device *dev);

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

#define XNTFS_LOCKED(v, expr) ({ pthread_mutex_lock(&(v)->lock); __typeof__(expr) _r = (expr); \
    pthread_mutex_unlock(&(v)->lock); _r; })

#endif
