/*
 * wal.c -- the write-ahead log (WAL): the source of DuraKV's crash safety.
 *
 * The write-ahead PRINCIPLE: before any change is applied to the data file, a
 * record describing that change is appended here and forced to stable storage.
 * On COMMIT the WAL is fsync'd BEFORE the client is told "OK", so every
 * acknowledged write is already durable. Data pages themselves may still be
 * only in RAM/OS cache at that moment -- that is safe, because recovery can
 * reconstruct them from the log. This is the write-ahead invariant:
 * log-then-data, never data-then-log.
 *
 * Record layout (little-endian), all framed by a length prefix:
 *   [u32 reclen] [u64 lsn][u64 prev_lsn][u64 txn_id][u8 type][u64 page_id]
 *   [u32 before_len][before...][u32 after_len][after...] [u32 crc]
 * Each record carries BOTH the before-image (for undo) and after-image (for
 * redo) of a page, which also lets recovery repair a torn page write.
 *
 * Two integrity mechanisms make a half-written tail (the normal result of a
 * crash mid-append) detectable and harmless: the length prefix bounds the read,
 * and a CRC32 over the body is verified on scan -- a partial or corrupt final
 * record fails one of these checks and is simply discarded, so recovery replays
 * only whole, intact records. See include/wal.h for the public API.
 */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE   /* expose F_FULLFSYNC from <fcntl.h> */
#endif
#include "wal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

/* ---- CRC32 (IEEE 802.3, reflected, poly 0xEDB88420) -------------------- */
/* Detects torn/corrupt records. Bit-at-a-time (no lookup table) -- the WAL is
 * fsync-bound, so CRC speed is not the bottleneck. */
uint32_t wal_crc32(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88420u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFFu;
}
