/*
 * audit.h - append-only hash-chained log. each entry stores
 * hash = sha256(prev_hash || fields), so editing/deleting any past entry
 * breaks the chain from there on and audit_verify catches it.
 * line format (tab separated): seq ts user op key result prev_hex hash_hex
 */
#ifndef DURAKV_AUDIT_H
#define DURAKV_AUDIT_H

typedef struct Audit Audit;

/* Open (creating if needed) an append-only audit log, recovering the chain
 * tip from any existing entries. */
Audit *audit_open(const char *path);
void   audit_close(Audit *a);

/* Append one event. Returns 0 on success, -1 on error. */
int audit_append(Audit *a, const char *user, const char *op,
                 const char *key, const char *result);

/* Re-read the whole log and recompute the chain. Returns 0 if intact; -1 if
 * broken, with *bad_seq set to the first inconsistent entry's sequence number
 * (or to the entry count if the file is unreadable). */
int audit_verify(const char *path, long *bad_seq);

#endif /* DURAKV_AUDIT_H */
