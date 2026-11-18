// SPDX-License-Identifier: GPL-2.0-or-later
// Engine tests against a file-backed NTFS image (made by mkntfs). Run via Scripts/test-engine.sh.
#include "xntfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static int64_t file_pread(void *ctx, void *buf, int64_t count, int64_t off) {
    ssize_t n = pread(*(int *)ctx, buf, (size_t)count, off); return n < 0 ? -errno : n;
}
static int64_t file_pwrite(void *ctx, const void *buf, int64_t count, int64_t off) {
    ssize_t n = pwrite(*(int *)ctx, buf, (size_t)count, off); return n < 0 ? -errno : n;
}
static int file_sync(void *ctx) { return fsync(*(int *)ctx); }

/// Sector size 4096 exercises the unaligned bounce path (NTFS boot code reads 512 bytes).
static xntfs_vol *mount_image(int *fd, const char *path, int want_write) {
    *fd = open(path, want_write ? O_RDWR : O_RDONLY);
    struct stat st; fstat(*fd, &st);
    xntfs_io io = { fd, file_pread, file_pwrite, file_sync, st.st_size, 4096 };
    xntfs_vol *v = NULL; int reason = -1;
    int rc = xntfs_mount(&io, want_write, &v, &reason);
    CHECK(rc == 0, "mount rc=%d", rc);
    CHECK(reason == XNTFS_MOUNT_WRITABLE || !want_write, "forced read-only reason=%d", reason);
    return v;
}

typedef struct { int count; int saw_hello; } list_ctx;
static int on_entry(void *ctx, const char *name, uint64_t ino, uint32_t dtype) {
    (void)ino; (void)dtype; list_ctx *l = ctx; l->count++; if (getenv("XNTFS_DEBUG")) printf("entry: %s\n", name);
    if (strcmp(name, "Đổi tên") == 0) l->saw_hello = 1;  // exact case, not lower-cased
    return 0;
}

static void test_write_session(const char *image) {
    int fd; xntfs_vol *v = mount_image(&fd, image, 1);
    if (!v) return;
    xntfs_stat dir, file, st;
    CHECK(xntfs_create(v, XNTFS_ROOT_INO, "Thư mục", S_IFDIR, &dir) == 0, "mkdir");
    CHECK(S_ISDIR(dir.mode), "dir mode %o", dir.mode);
    CHECK(xntfs_create(v, XNTFS_ROOT_INO, "thư MỤC", S_IFDIR, NULL) == EEXIST, "case-insensitive EEXIST");
    CHECK(xntfs_create(v, dir.ino, "a.txt", S_IFREG, &file) == 0, "create");
    CHECK(file.parent == dir.ino && dir.parent == XNTFS_ROOT_INO, "parent %llu/%llu", file.parent, dir.parent);

    // 3 MiB + 7 bytes: non-resident, crosses clusters, odd tail.
    const int64_t size = 3 * 1024 * 1024 + 7;
    char *data = malloc(size), *back = malloc(size);
    for (int64_t i = 0; i < size; i++) data[i] = (char)(i * 31 + 7);
    CHECK(xntfs_write(v, file.ino, data, size, 0) == size, "write");
    CHECK(xntfs_write(v, file.ino, "XYZ", 3, 1000001) == 3, "overwrite middle");
    memcpy(data + 1000001, "XYZ", 3);
    CHECK(xntfs_getattr(v, file.ino, &st) == 0 && st.size == (uint64_t)size, "size %llu", st.size);
    CHECK(xntfs_read(v, file.ino, back, size, 0) == size && memcmp(data, back, size) == 0, "read back");
    CHECK(xntfs_read(v, file.ino, back, 100, size) == 0, "read at EOF");

    CHECK(xntfs_truncate(v, file.ino, 10) == 0, "truncate");
    CHECK(xntfs_getattr(v, file.ino, &st) == 0 && st.size == 10, "truncated size");
    CHECK(xntfs_rename(v, dir.ino, "a.txt", dir.ino, "A.TXT") == 0, "case-only rename");
    CHECK(xntfs_lookup(v, dir.ino, "A.TXT", &st) == 0 && st.ino == file.ino, "lookup renamed");
    CHECK(xntfs_create(v, dir.ino, "b.txt", S_IFREG, NULL) == 0, "create b");
    CHECK(xntfs_rename(v, dir.ino, "b.txt", dir.ino, "A.TXT") == 0, "rename over existing");
    CHECK(xntfs_rename(v, dir.ino, "A.TXT", XNTFS_ROOT_INO, "moved.txt") == 0, "move to root");
    CHECK(xntfs_create(v, dir.ino, "sub", S_IFDIR, &st) == 0, "mkdir sub");
    CHECK(xntfs_remove(v, XNTFS_ROOT_INO, "Thư mục") == ENOTEMPTY, "rmdir non-empty");
    CHECK(xntfs_rename(v, XNTFS_ROOT_INO, "Thư mục", XNTFS_ROOT_INO, "Đổi tên") == 0, "rename non-empty dir");
    CHECK(xntfs_lookup(v, XNTFS_ROOT_INO, "Đổi tên", &st) == 0 && S_ISDIR(st.mode), "dir after rename");
    CHECK(xntfs_create(v, XNTFS_ROOT_INO, "big.bin", S_IFREG, &file) == 0, "create big");
    CHECK(xntfs_write(v, file.ino, data, size, 0) == size, "write big");

    xntfs_statfs fs;
    CHECK(xntfs_get_statfs(v, &fs) == 0 && fs.total_bytes > fs.free_bytes, "statfs");
    CHECK(strcmp(fs.label, "XTEST") == 0, "label '%s'", fs.label);
    CHECK(xntfs_sync(v) == 0, "sync");
    CHECK(xntfs_unmount(v) == 0, "unmount");
    close(fd); free(data); free(back);
}

static void test_reopen(const char *image) {
    int fd; xntfs_vol *v = mount_image(&fd, image, 0);
    if (!v) return;
    CHECK(xntfs_is_readonly(v), "ro mount");
    xntfs_stat st;
    CHECK(xntfs_lookup(v, XNTFS_ROOT_INO, "moved.txt", &st) == 0 && st.size == 0 && st.parent == XNTFS_ROOT_INO, "b.txt survived as moved.txt in root");
    CHECK(xntfs_lookup(v, XNTFS_ROOT_INO, "big.bin", &st) == 0 && st.size == 3 * 1024 * 1024 + 7, "big.bin size");
    CHECK(xntfs_create(v, XNTFS_ROOT_INO, "x", S_IFREG, NULL) == EROFS, "EROFS on ro");
    list_ctx l = {0};
    CHECK(xntfs_readdir(v, XNTFS_ROOT_INO, on_entry, &l) == 0, "readdir");
    CHECK(l.count == 3, "root entries %d (expected Đổi tên, moved.txt, big.bin)", l.count);
    CHECK(l.saw_hello, "listing keeps the saved case of \"Đổi tên\"");
    CHECK(xntfs_unmount(v) == 0, "unmount ro");
    close(fd);
}

/// Regressions found in review: subtree moves, stale ids, rename-over a non-empty directory.
static void test_rename_safety(const char *image) {
    int fd; xntfs_vol *v = mount_image(&fd, image, 1);
    if (!v) return;
    xntfs_stat a, b, c, f, g, st;
    CHECK(xntfs_create(v, XNTFS_ROOT_INO, "A", S_IFDIR, &a) == 0, "mkdir A");
    CHECK(xntfs_create(v, a.ino, "B", S_IFDIR, &b) == 0, "mkdir A/B");
    CHECK(xntfs_create(v, b.ino, "C", S_IFDIR, &c) == 0, "mkdir A/B/C");
    CHECK(xntfs_rename(v, XNTFS_ROOT_INO, "A", c.ino, "A2") == EINVAL, "move A into A/B/C refused");
    CHECK(xntfs_rename(v, XNTFS_ROOT_INO, "A", a.ino, "A2") == EINVAL, "move A into itself refused");
    CHECK(xntfs_lookup(v, XNTFS_ROOT_INO, "A", &st) == 0 && st.ino == a.ino, "A still in root");
    CHECK(xntfs_rename(v, a.ino, "B", XNTFS_ROOT_INO, "B") == 0, "move A/B up to root");
    CHECK(xntfs_lookup(v, XNTFS_ROOT_INO, "B", &st) == 0 && st.parent == XNTFS_ROOT_INO, "B parent is root");

    // A deleted file's id must not reach a new file that reuses its MFT record.
    CHECK(xntfs_create(v, XNTFS_ROOT_INO, "old.txt", S_IFREG, &f) == 0, "create old");
    CHECK(xntfs_remove(v, XNTFS_ROOT_INO, "old.txt") == 0, "remove old");
    CHECK(xntfs_create(v, XNTFS_ROOT_INO, "new.txt", S_IFREG, &g) == 0, "create new");
    if ((f.ino & 0xffffffffffffULL) == (g.ino & 0xffffffffffffULL)) {
        CHECK(f.ino != g.ino, "reused record gets a new sequence");
        CHECK(xntfs_write(v, f.ino, "x", 1, 0) < 0, "stale id refused");
    }
    CHECK(xntfs_getattr(v, g.ino, &st) == 0 && st.size == 0, "new file untouched");

    // Rename over a non-empty directory fails and leaves both in place.
    CHECK(xntfs_create(v, XNTFS_ROOT_INO, "D", S_IFDIR, &st) == 0, "mkdir D");
    CHECK(xntfs_create(v, st.ino, "inside", S_IFREG, NULL) == 0, "D/inside");
    CHECK(xntfs_rename(v, XNTFS_ROOT_INO, "A", XNTFS_ROOT_INO, "D") == ENOTEMPTY, "rename over full dir");
    CHECK(xntfs_lookup(v, XNTFS_ROOT_INO, "A", &st) == 0, "A survives");
    CHECK(xntfs_lookup(v, XNTFS_ROOT_INO, "D", &st) == 0, "D survives");
    list_ctx l = {0};
    CHECK(xntfs_readdir(v, XNTFS_ROOT_INO, on_entry, &l) == 0 && l.count == 7, "root has 7 entries (3 earlier + A B new.txt D), got %d", l.count);
    CHECK(xntfs_rename(v, XNTFS_ROOT_INO, "new.txt", XNTFS_ROOT_INO, "new.txt") == 0, "rename to itself");
    CHECK(xntfs_unmount(v) == 0, "unmount");
    close(fd);
}

typedef struct { int count; int saw_finder; } xattr_list_ctx;
static int on_xattr(void *ctx, const char *name) {
    xattr_list_ctx *l = ctx; l->count++;
    if (strcmp(name, "com.apple.FinderInfo") == 0) l->saw_finder = 1;
    return 0;
}

/// Extended attributes round-trip through named streams, small (resident) and large.
static void test_xattrs(const char *image) {
    int fd; xntfs_vol *v = mount_image(&fd, image, 1);
    if (!v) return;
    xntfs_stat f;
    CHECK(xntfs_create(v, XNTFS_ROOT_INO, "xattr.txt", S_IFREG, &f) == 0, "create");
    CHECK(xntfs_write(v, f.ino, "body", 4, 0) == 4, "write body");
    char info[32] = "TEXTttxt";
    size_t size = 0;
    CHECK(xntfs_xattr_get(v, f.ino, "com.apple.FinderInfo", NULL, 0, &size) == ENOATTR, "missing -> ENOATTR");
    CHECK(xntfs_xattr_set(v, f.ino, "com.apple.FinderInfo", info, sizeof(info), XNTFS_XATTR_REPLACE) == ENOATTR, "replace missing");
    CHECK(xntfs_xattr_set(v, f.ino, "com.apple.FinderInfo", info, sizeof(info), XNTFS_XATTR_CREATE) == 0, "create xattr");
    CHECK(xntfs_xattr_set(v, f.ino, "com.apple.FinderInfo", info, sizeof(info), XNTFS_XATTR_CREATE) == EEXIST, "create twice");
    const size_t big = 300 * 1024;  // non-resident stream (resource-fork sized)
    char *fork = malloc(big), *back = malloc(big);
    for (size_t i = 0; i < big; i++) fork[i] = (char)(i * 7);
    CHECK(xntfs_xattr_set(v, f.ino, "com.apple.ResourceFork", fork, big, 0) == 0, "big xattr");
    CHECK(xntfs_xattr_set(v, f.ino, "com.apple.ResourceFork", fork, big / 2, 0) == 0, "shrink xattr");
    CHECK(xntfs_unmount(v) == 0, "unmount");
    close(fd);

    v = mount_image(&fd, image, 1);
    CHECK(xntfs_xattr_get(v, f.ino, "com.apple.ResourceFork", NULL, 0, &size) == 0 && size == big / 2, "size %zu", size);
    CHECK(xntfs_xattr_get(v, f.ino, "com.apple.ResourceFork", back, 10, &size) == ERANGE, "ERANGE");
    CHECK(xntfs_xattr_get(v, f.ino, "com.apple.ResourceFork", back, big, &size) == 0 && memcmp(back, fork, big / 2) == 0, "big content");
    char got[32] = {0};
    CHECK(xntfs_xattr_get(v, f.ino, "com.apple.FinderInfo", got, sizeof(got), &size) == 0 && memcmp(got, info, 32) == 0, "small content");
    xattr_list_ctx l = {0};
    CHECK(xntfs_xattr_list(v, f.ino, on_xattr, &l) == 0 && l.count == 2 && l.saw_finder, "list %d", l.count);
    char body[8] = {0};
    CHECK(xntfs_read(v, f.ino, body, 8, 0) == 4 && memcmp(body, "body", 4) == 0, "file body untouched");
    CHECK(xntfs_xattr_remove(v, f.ino, "com.apple.ResourceFork") == 0, "remove");
    CHECK(xntfs_xattr_remove(v, f.ino, "com.apple.ResourceFork") == ENOATTR, "remove twice");
    CHECK(xntfs_remove(v, XNTFS_ROOT_INO, "xattr.txt") == 0, "delete file with streams");
    CHECK(xntfs_unmount(v) == 0, "unmount 2");
    close(fd); free(fork); free(back);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s image\n", argv[0]); return 2; }
    test_write_session(argv[1]);
    test_reopen(argv[1]);
    test_rename_safety(argv[1]);
    test_xattrs(argv[1]);
    printf(failures ? "%d FAILED\n" : "ALL PASSED\n", failures);
    return failures ? 1 : 0;
}
