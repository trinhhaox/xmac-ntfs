// SPDX-License-Identifier: GPL-2.0-or-later
// ntfs_device_operations backed by xntfs_io callbacks (FSKit's FSBlockDeviceResource).
// libntfs-3g issues byte-granular I/O; block devices need sector-aligned I/O, so unaligned
// requests go through a bounce buffer (read-modify-write for writes).
#include "xntfs_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/stat.h>

typedef struct {
    xntfs_io io;
    int64_t pos;
} dev_priv;

static dev_priv *priv(struct ntfs_device *dev) { return (dev_priv *)dev->d_private; }

static int dev_open(struct ntfs_device *dev, int flags) {
    if (NDevOpen(dev)) { errno = EBUSY; return -1; }
    NDevSetBlock(dev);
    if ((flags & O_ACCMODE) == O_RDONLY) NDevSetReadOnly(dev);
    priv(dev)->pos = 0;
    NDevSetOpen(dev);
    return 0;
}

static int dev_sync(struct ntfs_device *dev) {
    if (NDevReadOnly(dev) || !NDevDirty(dev)) return 0;
    xntfs_io *io = &priv(dev)->io;
    if (io->sync && io->sync(io->ctx) != 0) { errno = EIO; return -1; }
    NDevClearDirty(dev);
    return 0;
}

static int dev_close(struct ntfs_device *dev) {
    if (!NDevOpen(dev)) { errno = EBADF; return -1; }
    int rc = dev_sync(dev);
    NDevClearOpen(dev);
    return rc;
}

static s64 dev_seek(struct ntfs_device *dev, s64 offset, int whence) {
    dev_priv *p = priv(dev);
    switch (whence) {
    case SEEK_SET: p->pos = offset; break;
    case SEEK_CUR: p->pos += offset; break;
    case SEEK_END: p->pos = p->io.size_bytes + offset; break;
    default: errno = EINVAL; return -1;
    }
    return p->pos;
}

/// Aligned transfer straight through the callbacks; short counts are treated as errors.
static int raw_io(xntfs_io *io, void *buf, int64_t count, int64_t offset, int write) {
    while (count > 0) {
        int64_t n = write ? io->pwrite(io->ctx, buf, count, offset) : io->pread(io->ctx, buf, count, offset);
        if (n <= 0) { errno = n < 0 ? (int)-n : EIO; return -1; }
        buf = (char *)buf + n; offset += n; count -= n;
    }
    return 0;
}

static s64 dev_xfer(struct ntfs_device *dev, void *buf, s64 count, s64 offset, int write) {
    xntfs_io *io = &priv(dev)->io;
    if (count <= 0) return 0;
    if (offset < 0 || offset >= io->size_bytes) { errno = EINVAL; return -1; }
    if (offset + count > io->size_bytes) count = io->size_bytes - offset;
    if (write) {
        if (NDevReadOnly(dev)) { errno = EROFS; return -1; }
        NDevSetDirty(dev);
    }

    const int64_t sector = io->sector_size ? io->sector_size : 512;
    const int64_t start = offset - offset % sector;
    int64_t end = ((offset + count + sector - 1) / sector) * sector;
    if (end > io->size_bytes) end = io->size_bytes;  // device size is a whole number of sectors
    if (start == offset && end == offset + count && ((uintptr_t)buf % 16) == 0) {
        return raw_io(io, buf, count, offset, write) == 0 ? count : -1;
    }

    const int64_t span = end - start;
    char *bounce = NULL;
    if (posix_memalign((void **)&bounce, 4096, (size_t)span) != 0) { errno = ENOMEM; return -1; }
    s64 result = -1;
    // Writes keep the bytes around the request: read the covering sectors first.
    if (raw_io(io, bounce, span, start, 0) == 0) {
        if (write) {
            memcpy(bounce + (offset - start), buf, (size_t)count);
            if (raw_io(io, bounce, span, start, 1) == 0) result = count;
        } else {
            memcpy(buf, bounce + (offset - start), (size_t)count);
            result = count;
        }
    }
    free(bounce);
    return result;
}

static s64 dev_pread(struct ntfs_device *dev, void *buf, s64 count, s64 offset) {
    return dev_xfer(dev, buf, count, offset, 0);
}

static s64 dev_pwrite(struct ntfs_device *dev, const void *buf, s64 count, s64 offset) {
    return dev_xfer(dev, (void *)buf, count, offset, 1);
}

static s64 dev_read(struct ntfs_device *dev, void *buf, s64 count) {
    s64 n = dev_pread(dev, buf, count, priv(dev)->pos);
    if (n > 0) priv(dev)->pos += n;
    return n;
}

static s64 dev_write(struct ntfs_device *dev, const void *buf, s64 count) {
    s64 n = dev_pwrite(dev, buf, count, priv(dev)->pos);
    if (n > 0) priv(dev)->pos += n;
    return n;
}

static int dev_stat(struct ntfs_device *dev, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFBLK;
    st->st_size = priv(dev)->io.size_bytes;
    return 0;
}

/// Answers the size queries libntfs-3g makes on Darwin.
static int dev_ioctl(struct ntfs_device *dev, unsigned long request, void *argp) {
    xntfs_io *io = &priv(dev)->io;
    const uint32_t sector = io->sector_size ? io->sector_size : 512;
    switch (request) {
    case DKIOCGETBLOCKSIZE: *(uint32_t *)argp = sector; return 0;
    case DKIOCGETBLOCKCOUNT: *(uint64_t *)argp = (uint64_t)io->size_bytes / sector; return 0;
    default: errno = ENOTTY; return -1;
    }
}

static struct ntfs_device_operations xntfs_dev_ops = {
    .open = dev_open, .close = dev_close, .seek = dev_seek, .read = dev_read, .write = dev_write,
    .pread = dev_pread, .pwrite = dev_pwrite, .sync = dev_sync, .stat = dev_stat, .ioctl = dev_ioctl,
};

struct ntfs_device *xntfs_device_new(const xntfs_io *io) {
    dev_priv *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->io = *io;
    struct ntfs_device *dev = ntfs_device_alloc("xmac-ntfs", 0, &xntfs_dev_ops, p);
    if (!dev) free(p);
    return dev;
}

void xntfs_device_free(struct ntfs_device *dev) {
    if (!dev) return;
    free(dev->d_private);
    dev->d_private = NULL;
    ntfs_device_free(dev);
}
