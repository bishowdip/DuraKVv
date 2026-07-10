/*
 * recovery.h - crash recovery (simplified ARIES). three passes on open:
 * analysis (who committed since the last checkpoint), redo (re-apply
 * committed after-images, only where page_lsn < record lsn -> idempotent),
 * undo (roll losers back with before-images). also restores next_lsn/txn.
 */
#ifndef DURAKV_RECOVERY_H
#define DURAKV_RECOVERY_H

#include "storage.h"

/* Run analysis/redo/undo against db's WAL and data file. Returns DK_OK. */
int recovery_run(DB *db);

#endif /* DURAKV_RECOVERY_H */
