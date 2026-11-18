// SPDX-License-Identifier: GPL-2.0-or-later
// Volume lifecycle: mount (with read-only fallback for unsafe volumes), statfs, sync, unmount.
#include "xntfs_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/// The first error from closing the kept-open file since the last call.
static int take_deferred(xntfs_vol *v) {
    int rc = v->deferred_err;
    v->deferred_err = 0;
    return rc;
}

int xntfs_mount(const xntfs_io *io, int want_write, xntfs_vol **out, int *ro_reason) {
    *out = NULL;
    if (ro_reason) *ro_reason = XNTFS_MOUNT_WRITABLE;
    // libntfs-3g on macOS turns names into NFD when listing and NFC when storing. Keep names
    // exactly as stored (Windows writes NFC); the Swift side normalises new names to NFC.
    ntfs_macosx_normalize_filenames(0);
    struct ntfs_device *dev = xntfs_device_new(io);
    if (!dev) return ENOMEM;

    // RECOVER resets a journal left by an unsafe eject (standard ntfs-3g behaviour); a volume
    // with Windows hibernated / Fast Startup or cached metadata falls back to read-only.
    ntfs_mount_flags flags = want_write ? (NTFS_MNT_RECOVER | NTFS_MNT_MAY_RDONLY) : NTFS_MNT_RDONLY;
    ntfs_volume *vol = ntfs_device_mount(dev, flags);
    if (!vol) {
        int err = errno ? errno : EIO;
        xntfs_device_free(dev);
        return err;
    }
    if (want_write && NVolReadOnly(vol) && ro_reason) {
        *ro_reason = ntfs_volume_check_hiberfile(vol, 0) < 0
            ? XNTFS_MOUNT_FORCED_RO_HIBERNATED : XNTFS_MOUNT_FORCED_RO_DIRTY;
    }
    // Metafiles ($MFT, $Bitmap...) stay hidden; Windows-hidden files are shown with the hidden flag.
    ntfs_set_shown_files(vol, FALSE, TRUE, FALSE);
    // Windows and macOS both expect case-insensitive, case-preserving names.
    xntfs_vol *v = calloc(1, sizeof(*v));
    if (!v || ntfs_set_ignore_case(vol)) {
        int err = !v ? ENOMEM : (errno ? errno : EIO);
        void *dev_private = dev->d_private;
        ntfs_umount(vol, FALSE);  // frees `dev` but not our callback state
        xntfs_device_private_free(dev_private);
        free(v);
        return err;
    }
    v->vol = vol;
    pthread_mutex_init(&v->lock, NULL);
    *out = v;
    return 0;
}

int xntfs_unmount(xntfs_vol *v) {
    if (!v) return 0;
    pthread_mutex_lock(&v->lock);
    xntfs_close_open_file(v);
    int deferred = take_deferred(v);
    void *dev_private = v->vol->dev->d_private;
    int rc = ntfs_umount(v->vol, FALSE) ? (errno ? errno : EIO) : 0;
    if (!rc) rc = deferred;
    xntfs_device_private_free(dev_private);  // ntfs_umount freed the device but not our callback state
    pthread_mutex_unlock(&v->lock);
    pthread_mutex_destroy(&v->lock);
    free(v);
    return rc;
}

int xntfs_is_readonly(xntfs_vol *v) {
    return NVolReadOnly(v->vol) ? 1 : 0;
}

static int sync_locked(xntfs_vol *v) {
    // XNTFS_LOCKED already closed the kept-open file; report how that went.
    int deferred = take_deferred(v);
    if (deferred) return deferred;
    if (NVolReadOnly(v->vol)) return 0;
    // Flushes cached MFT / bitmap state, then the device.
    if (ntfs_inode_sync(v->vol->mft_ni)) return errno ? errno : EIO;
    struct ntfs_device *dev = v->vol->dev;
    return dev->d_ops->sync(dev) ? (errno ? errno : EIO) : 0;
}

int xntfs_sync(xntfs_vol *v) {
    return XNTFS_LOCKED(v, sync_locked(v));
}

static int statfs_locked(xntfs_vol *v, xntfs_statfs *out) {
    ntfs_volume *vol = v->vol;
    memset(out, 0, sizeof(*out));
    // Counting free clusters reads the whole $Bitmap (tens of MB on a TB drive): do it on the
    // first statfs only (a label-only probe mount never pays). libntfs-3g then keeps
    // free_clusters / free_mft_records current as it allocates and frees.
    if (!v->free_space_known) {
        if (ntfs_volume_get_free_space(vol)) return errno ? errno : EIO;
        v->free_space_known = 1;
    }
    out->cluster_size = vol->cluster_size;
    out->total_bytes = (uint64_t)vol->nr_clusters * vol->cluster_size;
    out->free_bytes = vol->free_clusters > 0 ? (uint64_t)vol->free_clusters * vol->cluster_size : 0;
    // free_mft_records counts free records in the MFT plus room it can still grow into, so the
    // total is what is in use plus that, never less than the free count (df showed -33k files).
    out->free_files = vol->free_mft_records > 0 ? (uint64_t)vol->free_mft_records : 0;
    uint64_t mft_records = (uint64_t)(vol->mft_na->initialized_size / vol->mft_record_size);
    out->total_files = mft_records + out->free_files;
    if (vol->vol_name) strlcpy(out->label, vol->vol_name, sizeof(out->label));
    return 0;
}

int xntfs_get_label(xntfs_vol *v, char *buf, size_t bufsize) {
    pthread_mutex_lock(&v->lock);
    strlcpy(buf, v->vol->vol_name ? v->vol->vol_name : "", bufsize);
    pthread_mutex_unlock(&v->lock);
    return 0;
}

int xntfs_get_statfs(xntfs_vol *v, xntfs_statfs *out) {
    return XNTFS_LOCKED_KEEP(v, statfs_locked(v, out));
}

int xntfs_name_to_ucs(const char *name, ntfschar **out) {
    *out = NULL;
    if (!name || !*name) return -EINVAL;
    int len = ntfs_mbstoucs(name, out);
    if (len < 0) return -(errno ? errno : EILSEQ);
    if (len > NTFS_MAX_NAME_LEN) { free(*out); *out = NULL; return -ENAMETOOLONG; }
    return len;
}
