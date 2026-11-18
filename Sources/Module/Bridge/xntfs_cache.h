// SPDX-License-Identifier: GPL-2.0-or-later
// Write-back cache for small device writes (see xntfs_cache.c).
#ifndef XNTFS_CACHE_H
#define XNTFS_CACHE_H

#include <stdint.h>

/// Aligned device transfer; returns 0 or -1 with errno set.
typedef int (*xntfs_raw_io)(void *ctx, void *buf, int64_t count, int64_t offset, int write);

typedef struct xntfs_cache xntfs_cache;

xntfs_cache *xntfs_cache_new(xntfs_raw_io raw, void *ctx, int64_t size_bytes);
void xntfs_cache_free(xntfs_cache *c);
/// Whether a write of `count` bytes should be cached (small) or sent to the device (large).
int xntfs_cache_takes(int64_t count);
/// Keeps a small write in memory. 0 or -1 with errno set.
int xntfs_cache_write(xntfs_cache *c, const void *buf, int64_t count, int64_t offset);
/// Copies cached bytes over `buf` (after a device read), or `buf` into the cache.
void xntfs_cache_overlay(xntfs_cache *c, void *buf, int64_t count, int64_t offset, int into_cache);
/// Writes every cached block to the device. 0 or -1 with errno set (blocks kept on failure).
int xntfs_cache_flush(xntfs_cache *c);

#endif
