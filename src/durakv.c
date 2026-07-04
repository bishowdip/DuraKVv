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
 *
 * In stress mode each committed key is printed as "COMMIT <key>" and flushed
 * *after* its WAL fsync, so anything the harness sees on stdout is guaranteed
 * durable -- exactly the contract crashtest.sh relies on.
 */
#define _POSIX_C_SOURCE 200809L
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Commit `count` keys starting at `start`. Each committed key is announced on
 * stdout only after db_set has fsync'd its WAL, so the crashtest can trust that
 * every "COMMIT" line it reads is already durable. */
static void run_stress(DB *db, long count, long start)
{
    char key[64], val[64];
    for (long i = start; i < start + count; i++) {
        snprintf(key, sizeof(key), "key%ld", i);
        snprintf(val, sizeof(val), "val%ld", i);
        if (db_set(db, key, val, (uint32_t)strlen(val)) == DK_OK) {
            printf("COMMIT %s\n", key);
            fflush(stdout);
        }
    }
}

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

/* ====================================================================== */
/* Friendly guided menu (shown only when a human is at the terminal)      */
/* ====================================================================== */

#define C_RESET "\033[0m"
#define C_BOLD  "\033[1m"
#define C_DIM   "\033[2m"
#define C_GREEN "\033[32m"
#define C_RED   "\033[31m"
#define C_CYAN  "\033[36m"
#define C_YEL   "\033[33m"

/* Print a prompt and read one trimmed line. Returns 0 on EOF (Ctrl-D). */
static int ask(const char *prompt, char *buf, size_t cap)
{
    fputs(prompt, stdout);
    fflush(stdout);
    if (!fgets(buf, (int)cap, stdin)) return 0;
    buf[strcspn(buf, "\n")] = '\0';
    return 1;
}

static void menu_banner(void)
{
    printf("\n" C_CYAN C_BOLD
           "  ____                  _  ____   __\n"
           " |  _ \\ _   _ _ __ __ _| |/ /\\ \\ / /\n"
           " | | | | | | | '__/ _` | ' /  \\ V / \n"
           " | |_| | |_| | | | (_| | . \\   | |  \n"
           " |____/ \\__,_|_|  \\__,_|_|\\_\\  |_|  \n" C_RESET);
    printf(C_DIM "  crash-safe key/value store" C_RESET "\n");
}

static void menu_show(void)
{
    printf("\n" C_BOLD "  What would you like to do?" C_RESET "\n");
    printf("    " C_GREEN "1" C_RESET ") Set a value      "
           "    " C_GREEN "4" C_RESET ") List all keys\n");
    printf("    " C_GREEN "2" C_RESET ") Get a value      "
           "    " C_GREEN "5" C_RESET ") Save to disk (checkpoint)\n");
    printf("    " C_GREEN "3" C_RESET ") Delete a key     "
           "    " C_GREEN "0" C_RESET ") Quit\n");
}

static void run_menu(DB *db)
{
    char choice[64], key[1024];
    static char val[1 << 20];

    menu_banner();

    for (;;) {
        menu_show();
        if (!ask("\n  choose [0-5]: ", choice, sizeof(choice))) { printf("\n"); break; }
        if (choice[0] == '\0') continue;

        if (!strcmp(choice, "1")) {
            if (!ask("    key:   ", key, sizeof(key)) || !*key) {
                printf(C_RED "    cancelled (empty key)\n" C_RESET); continue;
            }
            if (!ask("    value: ", val, sizeof(val))) break;
            int rc = db_set(db, key, val, (uint32_t)strlen(val));
            if (rc == DK_OK)
                printf(C_GREEN "    \xE2\x9C\x93 stored  %s = \"%s\"\n" C_RESET, key, val);
            else
                printf(C_RED "    \xE2\x9C\x97 failed (%s)\n" C_RESET,
                       rc == DK_TOOBIG ? "value too large" : "I/O error");

        } else if (!strcmp(choice, "2")) {
            if (!ask("    key: ", key, sizeof(key)) || !*key) continue;
            uint32_t vl = 0;
            int rc = db_get(db, key, val, sizeof(val), &vl);
            if (rc == DK_OK)
                printf(C_GREEN "    \xE2\x9C\x93 %s = \"%.*s\"\n" C_RESET, key, (int)vl, val);
            else
                printf(C_YEL "    \xE2\x80\x94 \"%s\" not found\n" C_RESET, key);

        } else if (!strcmp(choice, "3")) {
            if (!ask("    key to delete: ", key, sizeof(key)) || !*key) continue;
            if (db_del(db, key) == DK_OK)
                printf(C_GREEN "    \xE2\x9C\x93 deleted \"%s\"\n" C_RESET, key);
            else
                printf(C_YEL "    \xE2\x80\x94 \"%s\" not found\n" C_RESET, key);

        } else if (!strcmp(choice, "4")) {
            size_t count = 0;
            printf(C_BOLD "    stored keys:\n" C_RESET);
            for (size_t b = 0; b < db->dir.nbuckets; b++)
                for (DirEntry *e = db->dir.buckets[b]; e; e = e->next) {
                    printf("      \xE2\x80\xA2 %s\n", e->key);
                    count++;
                }
            if (count == 0) printf(C_DIM "      (none yet)\n" C_RESET);
            else            printf(C_DIM "    %zu key(s) total\n" C_RESET, count);

        } else if (!strcmp(choice, "5")) {
            db_checkpoint(db);
            printf(C_GREEN "    \xE2\x9C\x93 all data flushed safely to disk\n" C_RESET);

        } else if (!strcmp(choice, "0")) {
            printf(C_CYAN "  goodbye \xE2\x80\x94 your data is saved.\n" C_RESET);
            break;
        } else {
            printf(C_RED "    please choose a number from 0 to 5\n" C_RESET);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <data.db> <wal.log> [stress N START]\n", argv[0]);
        return 2;
    }

    DB *db = db_open(argv[1], argv[2]);
    if (!db) { fprintf(stderr, "failed to open store\n"); return 1; }

    /* Three modes: batch stress (for the crashtest), a guided menu for a human
     * at a terminal, or the raw parser for pipes/scripts. */
    if (argc >= 5 && strcmp(argv[3], "stress") == 0) {
        long count = strtol(argv[4], NULL, 10);
        long start = argc >= 6 ? strtol(argv[5], NULL, 10) : 0;
        run_stress(db, count, start);
    } else if (isatty(STDIN_FILENO)) {
        run_menu(db);            /* friendly menu for a human at the TTY */
    } else {
        run_interactive(db);     /* raw line commands for pipes/scripts */
    }

    db_close(db);
    return 0;
}
