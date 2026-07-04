/*
 * durakv.c -- command-line front end for the durable KV store.
 *
 * Usage:
 *   durakv <data.db> <wal.log>                 interactive use
 *   durakv <data.db> <wal.log> stress N START  commit N sequential keys
 *
 * Raw commands (one per line): set <k> <v...> | get <k> | del <k> | list |
 * checkpoint | quit. Piped/scripted input is driven through this parser, so
 * crashtest.sh and the demos behave predictably.
 */
#define _POSIX_C_SOURCE 200809L
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Raw line-command interpreter for piped/scripted input (set/get/del/list/
 * checkpoint/quit), one command per line. Kept deliberately simple and stable
 * because crashtest.sh and the demos drive the CLI through this path. */
static void run_interactive(DB *db)
{
    char line[1 << 20];          /* generous: keys + values on one line */
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = '\0';
        char *cmd = strtok(line, " ");
        if (!cmd) continue;

        if (strcmp(cmd, "set") == 0) {
            char *key = strtok(NULL, " ");
            char *val = strtok(NULL, "");          /* rest of line = value */
            if (!key || !val) { printf("ERR usage: set <key> <value>\n"); continue; }
            int rc = db_set(db, key, val, (uint32_t)strlen(val));
            printf(rc == DK_OK ? "OK\n" :
                   rc == DK_TOOBIG ? "ERR size\n" : "ERR io\n");
        } else if (strcmp(cmd, "get") == 0) {
            char *key = strtok(NULL, " ");
            if (!key) { printf("ERR usage: get <key>\n"); continue; }
            static char buf[1 << 20];
            uint32_t vlen = 0;
            int rc = db_get(db, key, buf, sizeof(buf), &vlen);
            if (rc == DK_OK) printf("VALUE %.*s\n", (int)vlen, buf);
            else             printf("NOTFOUND\n");
        } else if (strcmp(cmd, "del") == 0) {
            char *key = strtok(NULL, " ");
            if (!key) { printf("ERR usage: del <key>\n"); continue; }
            printf(db_del(db, key) == DK_OK ? "OK\n" : "NOTFOUND\n");
        } else if (strcmp(cmd, "list") == 0) {
            for (size_t b = 0; b < db->dir.nbuckets; b++)
                for (DirEntry *e = db->dir.buckets[b]; e; e = e->next)
                    printf("%s\n", e->key);
            printf("END\n");
        } else if (strcmp(cmd, "checkpoint") == 0) {
            db_checkpoint(db);
            printf("OK\n");
        } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            break;
        } else {
            printf("ERR unknown command: %s\n", cmd);
        }
        fflush(stdout);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <data.db> <wal.log>\n", argv[0]);
        return 2;
    }

    DB *db = db_open(argv[1], argv[2]);
    if (!db) { fprintf(stderr, "failed to open store\n"); return 1; }

    run_interactive(db);         /* raw line commands for pipes/scripts */

    db_close(db);
    return 0;
}
