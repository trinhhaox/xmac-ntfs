// SPDX-License-Identifier: GPL-2.0-or-later
// Write-back cache for small device writes. libntfs-3g updates $Bitmap and MFT records with
// many small writes per allocation; sent straight to a hard drive, each one is a seek, and
// file data trickled out at ~60% of the drive's speed. ntfs-3g on FUSE gets this coalescing
// from the kernel buffer cache; FSKit's direct I/O has none, so the engine keeps its own.
// Large writes (file data) still go straight to the device.
//
// Coherence: every device access of the volume goes through this file (one process owns the
// device), so reads overlay the cached blocks and large writes patch them.
#include "xntfs_cache.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum { BLOCK = 4096, SMALL_WRITE = 64 * 1024, MAX_BLOCKS = 8192 };  // 32 MB at most

typedef struct { int64_t index; unsigned char *data; } block_t;

struct xntfs_cache {
    xntfs_raw_io raw;
    void *ctx;
    int64_t size_bytes;
    block_t *blocks;  // sorted by index
    size_t count, capacity;
};

xntfs_cache *xntfs_cache_new(xntfs_raw_io raw, void *ctx, int64_t size_bytes) {
    xntfs_cache *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->raw = raw; c->ctx = ctx; c->size_bytes = size_bytes;
    return c;
}

/// Bytes of block `index` that exist on the device (the last block may be short).
static int64_t block_len(const xntfs_cache *c, int64_t index) {
    int64_t start = index * BLOCK;
    return c->size_bytes - start < BLOCK ? c->size_bytes - start : BLOCK;
}

/// Position of the first block with index >= `index`.
static size_t lower_bound(const xntfs_cache *c, int64_t index) {
    size_t lo = 0, hi = c->count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c->blocks[mid].index < index) lo = mid + 1; else hi = mid;
    }
    return lo;
}

/// The cached block `index`, loading it from the device when absent (unless `whole` says
/// the caller overwrites all of it). NULL with errno set on failure.
static unsigned char *block_for_write(xntfs_cache *c, int64_t index, int whole) {
    size_t at = lower_bound(c, index);
    if (at < c->count && c->blocks[at].index == index) return c->blocks[at].data;
    if (c->count == c->capacity) {
        size_t cap = c->capacity ? c->capacity * 2 : 64;
        block_t *grown = realloc(c->blocks, cap * sizeof(*grown));
        if (!grown) { errno = ENOMEM; return NULL; }
        c->blocks = grown; c->capacity = cap;
    }
    unsigned char *data = NULL;
    if (posix_memalign((void **)&data, BLOCK, BLOCK) != 0) { errno = ENOMEM; return NULL; }
    if (!whole && c->raw(c->ctx, data, block_len(c, index), index * BLOCK, 0) != 0) {
        free(data);
        return NULL;
    }
    memmove(&c->blocks[at + 1], &c->blocks[at], (c->count - at) * sizeof(*c->blocks));
    c->blocks[at] = (block_t){ index, data };
    c->count++;
    return data;
}

int xntfs_cache_flush(xntfs_cache *c) {
    size_t i = 0;
    int rc = 0;
    while (i < c->count && !rc) {
        // Merge consecutive blocks into one device write.
        size_t j = i + 1;
        while (j < c->count && c->blocks[j].index == c->blocks[j - 1].index + 1) j++;
        int64_t len = 0;
        for (size_t k = i; k < j; k++) len += block_len(c, c->blocks[k].index);
        unsigned char *run = NULL;
        if (posix_memalign((void **)&run, BLOCK, (size_t)len) != 0) { errno = ENOMEM; return -1; }
        int64_t at = 0;
        for (size_t k = i; k < j; k++) {
            memcpy(run + at, c->blocks[k].data, (size_t)block_len(c, c->blocks[k].index));
            at += block_len(c, c->blocks[k].index);
        }
        rc = c->raw(c->ctx, run, len, c->blocks[i].index * BLOCK, 1);
        free(run);
        i = j;
    }
    if (rc) return -1;  // keep the blocks: a later flush can retry them
    for (size_t k = 0; k < c->count; k++) free(c->blocks[k].data);
    c->count = 0;
    return 0;
}

int xntfs_cache_takes(int64_t count) { return count < SMALL_WRITE; }

int xntfs_cache_write(xntfs_cache *c, const void *buf, int64_t count, int64_t offset) {
    for (int64_t done = 0; done < count;) {
        int64_t pos = offset + done, index = pos / BLOCK, in = pos % BLOCK;
        int64_t n = BLOCK - in < count - done ? BLOCK - in : count - done;
        unsigned char *block = block_for_write(c, index, in == 0 && n == block_len(c, index));
        if (!block) return -1;
        memcpy(block + in, (const char *)buf + done, (size_t)n);
        done += n;
    }
    return c->count > MAX_BLOCKS ? xntfs_cache_flush(c) : 0;
}

void xntfs_cache_overlay(xntfs_cache *c, void *buf, int64_t count, int64_t offset, int into_cache) {
    int64_t first = offset / BLOCK, last = (offset + count - 1) / BLOCK;
    for (size_t i = lower_bound(c, first); i < c->count && c->blocks[i].index <= last; i++) {
        int64_t bstart = c->blocks[i].index * BLOCK;
        int64_t from = offset > bstart ? offset : bstart;
        int64_t bend = bstart + block_len(c, c->blocks[i].index);
        int64_t to = offset + count < bend ? offset + count : bend;
        if (into_cache) memcpy(c->blocks[i].data + (from - bstart), (const char *)buf + (from - offset), (size_t)(to - from));
        else memcpy((char *)buf + (from - offset), c->blocks[i].data + (from - bstart), (size_t)(to - from));
    }
}

void xntfs_cache_free(xntfs_cache *c) {
    if (!c) return;
    for (size_t k = 0; k < c->count; k++) free(c->blocks[k].data);
    free(c->blocks);
    free(c);
}
