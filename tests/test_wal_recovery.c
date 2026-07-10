/*
 * test_wal_recovery.c - honest power-loss model for the redo pass.
 * 1. snapshot a fresh data.db  2. SET 500 keys (WAL fsync'd every commit)
 * 3. clobber data.db back to the snapshot = power loss ate the data pages
 * 4. reopen -> the keys can ONLY come back via redo from the WAL. check all.
 */
#define _POSIX_C_SOURCE 200809L
#include "storage.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define DATA "/tmp/durakv_recovery.db"
#define SNAP "/tmp/durakv_recovery.db.snap"
#define WAL  "/tmp/durakv_recovery.log"
#define NKEYS 500

static void copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb"), *out = fopen(dst, "wb");
    assert(in && out);
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) assert(fwrite(buf, 1, n, out) == n);
    fclose(in); fclose(out);
}

static int get_eq(DB *db, const char *k, const char *expect)
{
    char buf[256]; uint32_t vl = 0;
    int rc = db_get(db, k, buf, sizeof(buf), &vl);
    return rc == DK_OK && vl == strlen(expect) && memcmp(buf, expect, vl) == 0;
}

int main(void)
{
    unlink(DATA); unlink(WAL); unlink(SNAP);

    /* 1. fresh store, snapshot the fsync'd (header-only) data file */
    DB *db = db_open(DATA, WAL);
    assert(db);
    db_close(db);
    copy_file(DATA, SNAP);

    /* 2. write many keys normally (data pages + fsync'd WAL) */
    db = db_open(DATA, WAL);
    assert(db);
    for (int i = 0; i < NKEYS; i++) {
        char k[32], v[48];
        snprintf(k, sizeof(k), "rk%d", i);
        snprintf(v, sizeof(v), "recovered-value-%d", i);
        assert(db_set(db, k, v, (uint32_t)strlen(v)) == DK_OK);
    }
    db_close(db);

    /* 3. simulate power loss: throw away every data-page write; keep the WAL */
    copy_file(SNAP, DATA);

    /* 4. reopen -> recovery must redo everything from the WAL */
    db = db_open(DATA, WAL);
    assert(db);
    assert(db->page_count > 1);          /* redo extended the data file */
    for (int i = 0; i < NKEYS; i++) {
        char k[32], v[48];
        snprintf(k, sizeof(k), "rk%d", i);
        snprintf(v, sizeof(v), "recovered-value-%d", i);
        assert(get_eq(db, k, v));
    }
    db_close(db);

    printf("test_wal_recovery: PASS (%d keys redone from WAL after data loss)\n", NKEYS);
    return 0;
}
