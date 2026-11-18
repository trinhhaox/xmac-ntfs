// SPDX-License-Identifier: GPL-2.0-or-later
// Randomised engine test: thousands of create / write / truncate / rename / remove operations
// mirrored in an in-memory model, with remounts in between; every file's bytes are compared.
#include "xntfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SLOTS 24
#define MAX_SIZE (512 * 1024)

typedef struct { int used; int dir; char name[32]; uint64_t ino; size_t size; unsigned char *data; } model_file;

/// Files live in the root or one of two subdirectories; renames move them around and over
/// each other.
#define DIRS 3
static uint64_t dirs[DIRS];

static model_file files[SLOTS];
static unsigned seed_state;
static unsigned rnd(unsigned n) { seed_state = seed_state * 1103515245u + 12345u; return (seed_state >> 8) % n; }

static int64_t f_pread(void *c, void *b, int64_t n, int64_t o) { ssize_t r = pread(*(int *)c, b, n, o); return r < 0 ? -errno : r; }
static int64_t f_pwrite(void *c, const void *b, int64_t n, int64_t o) { ssize_t r = pwrite(*(int *)c, b, n, o); return r < 0 ? -errno : r; }
static int f_sync(void *c) { return fsync(*(int *)c); }

static xntfs_vol *mount_rw(int fd) {
    struct stat st; fstat(fd, &st);
    static int fdbox; fdbox = fd;
    xntfs_io io = { &fdbox, f_pread, f_pwrite, f_sync, st.st_size, 4096 };
    xntfs_vol *v = NULL; int reason = 0;
    if (xntfs_mount(&io, 1, &v, &reason) || reason) { printf("mount failed reason=%d\n", reason); exit(1); }
    return v;
}

static int fail(const char *what, int slot, long long a, long long b) {
    printf("FAIL %s slot=%d (%lld vs %lld)\n", what, slot, a, b);
    return 1;
}

static int verify_all(xntfs_vol *v) {
    unsigned char *buf = malloc(MAX_SIZE + 16);
    int bad = 0;
    for (int i = 0; i < SLOTS && !bad; i++) {
        if (!files[i].used) continue;
        xntfs_stat st;
        if (xntfs_lookup(v, dirs[files[i].dir], files[i].name, &st)) { bad = fail("lookup", i, files[i].dir, 0); break; }
        if (st.size != files[i].size) { bad = fail("size", i, st.size, files[i].size); break; }
        int64_t n = xntfs_read(v, st.ino, buf, MAX_SIZE + 16, 0);
        if (n != (int64_t)files[i].size || memcmp(buf, files[i].data, files[i].size))
            bad = fail("content", i, n, files[i].size);
    }
    free(buf);
    return bad;
}

static int step(xntfs_vol *v) {
    int i = (int)rnd(SLOTS);
    model_file *f = &files[i];
    switch (rnd(6)) {
    case 0:  // create
        if (f->used) return 0;
        snprintf(f->name, sizeof(f->name), "tệp-%d-%u.bin", i, rnd(1000));
        f->dir = (int)rnd(DIRS);
        xntfs_stat st;
        if (xntfs_create(v, dirs[f->dir], f->name, S_IFREG, &st)) return fail("create", i, errno, 0);
        f->used = 1; f->ino = st.ino; f->size = 0; f->data = calloc(1, MAX_SIZE);
        return 0;
    case 1: case 2: {  // write at random offset (may extend, may leave a hole)
        if (!f->used) return 0;
        size_t off = rnd(MAX_SIZE / 2), len = 1 + rnd(MAX_SIZE / 2);
        unsigned char *chunk = malloc(len);
        for (size_t k = 0; k < len; k++) chunk[k] = (unsigned char)rnd(256);
        int64_t n = xntfs_write(v, f->ino, chunk, (int64_t)len, (int64_t)off);
        if (n != (int64_t)len) { free(chunk); return fail("write", i, n, len); }
        memcpy(f->data + off, chunk, len);
        if (off + len > f->size) f->size = off + len;
        free(chunk);
        return 0;
    }
    case 3: {  // truncate
        if (!f->used) return 0;
        size_t size = rnd(MAX_SIZE);
        if (xntfs_truncate(v, f->ino, size)) return fail("truncate", i, size, 0);
        if (size > f->size) memset(f->data + f->size, 0, size - f->size);
        else memset(f->data + size, 0, f->size - size);
        f->size = size;
        return 0;
    }
    case 4: {  // rename: case-only, to a new name in any directory, or over another file
        if (!f->used) return 0;
        char next[32];
        int to_dir = (int)rnd(DIRS);
        int victim = -1;
        if (rnd(3) == 0 && (f->name[0] == 't' || f->name[0] == 'T')) {
            strcpy(next, f->name); next[0] = next[0] == 't' ? 'T' : 't'; to_dir = f->dir;
        } else if (rnd(3) == 0) {
            victim = (int)rnd(SLOTS);
            if (victim == i || !files[victim].used) return 0;
            strcpy(next, files[victim].name); to_dir = files[victim].dir;
        } else {
            snprintf(next, sizeof(next), "đổi-%d-%u.bin", i, rnd(1000));
        }
        int rc = xntfs_rename(v, dirs[f->dir], f->name, dirs[to_dir], next);
        if (rc) return fail("rename", i, rc, victim);
        if (victim >= 0) { free(files[victim].data); memset(&files[victim], 0, sizeof(files[victim])); }
        strcpy(f->name, next); f->dir = to_dir;
        return 0;
    }
    default:  // remove
        if (!f->used) return 0;
        if (xntfs_remove(v, dirs[f->dir], f->name)) return fail("remove", i, errno, 0);
        free(f->data); memset(f, 0, sizeof(*f));
        return 0;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    seed_state = argc > 2 ? (unsigned)atoi(argv[2]) : 42;
    int fd = open(argv[1], O_RDWR);
    xntfs_vol *v = mount_rw(fd);
    dirs[0] = XNTFS_ROOT_INO;
    for (int d = 1; d < DIRS; d++) {
        char name[16]; snprintf(name, sizeof(name), "thư-mục-%d", d);
        xntfs_stat st;
        if (xntfs_create(v, XNTFS_ROOT_INO, name, S_IFDIR, &st)) { printf("FAIL mkdir\n"); return 1; }
        dirs[d] = st.ino;
    }
    for (int round = 0; round < 6; round++) {
        for (int k = 0; k < 400; k++) if (step(v)) return 1;
        if (verify_all(v)) return 1;
        if (xntfs_unmount(v)) { printf("FAIL unmount\n"); return 1; }
        v = mount_rw(fd);  // remount: everything must come back from disk
        if (verify_all(v)) return 1;
    }
    xntfs_unmount(v);
    printf("FUZZ PASSED (seed %u)\n", argc > 2 ? (unsigned)atoi(argv[2]) : 42);
    return 0;
}
