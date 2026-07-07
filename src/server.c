/*
 * server.c -- AF_UNIX server: parse, validate, dispatch commands to the store,
 * one worker per connection. See include/server.h.
 *
 * The accept loop hands each connection to the thread pool, so many clients are
 * served concurrently. Every message is a length-prefixed frame carrying one
 * text command (PING/SET/GET/DEL/STATS/QUIT). (Authentication, per-key
 * permissions and the audit log are layered on top in a later phase.)
 */
#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include "protocol.h"
#include "threadpool.h"
#include "bufferpool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>

#define MAX_KEY 256

/* Set by SIGINT/SIGTERM to request a clean shutdown of the accept loop.
 * `volatile sig_atomic_t` is the only type the C standard allows a handler to
 * touch safely: volatile stops the compiler caching it, sig_atomic_t guarantees
 * the write is atomic w.r.t. interruption. The handler does the minimum -- just
 * flips the flag -- and the main loop notices it. */
static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ---- per-connection session state ------------------------------------- */

typedef struct {
    int  fd;
    DB  *db;
} Conn;

/* ---- command dispatch -------------------------------------------------- */

/*
 * Parse and execute one text command, writing the reply into `resp` and
 * returning its length. This is where the protocol's verbs live (PING/SET/GET/
 * DEL/STATS/QUIT). Every recognised command produces exactly one response line,
 * so the protocol stays a simple request/response.
 */
static size_t handle_command(Conn *c, const char *payload, uint32_t plen,
                             char *resp, size_t respcap)
{
    char line[512 + 16];
    char *big = NULL, *buf = line;
    if (plen + 1 > sizeof(line)) { big = malloc(plen + 1); buf = big; }
    memcpy(buf, payload, plen);
    buf[plen] = '\0';

    DB *db = c->db;
    char *save = NULL;
    char *cmd = strtok_r(buf, " ", &save);
    int n = 0;

    if (!cmd) { n = snprintf(resp, respcap, "ERR empty"); goto done; }

    if (strcmp(cmd, "PING") == 0) { n = snprintf(resp, respcap, "PONG"); goto done; }
    if (strcmp(cmd, "QUIT") == 0) { n = snprintf(resp, respcap, "BYE");  goto done; }

    if (strcmp(cmd, "STATS") == 0) {
        BPStats s = bp_stats(db->bp);
        n = snprintf(resp, respcap,
                     "policy=%s frames=%zu accesses=%llu hits=%llu hit_ratio=%.1f%%",
                     bp_policy_name(db->bp), bp_nframes(db->bp),
                     (unsigned long long)s.accesses, (unsigned long long)s.hits,
                     100.0 * bp_hit_ratio(db->bp));
        goto done;
    }

    if (strcmp(cmd, "SET") == 0 || strcmp(cmd, "GET") == 0 || strcmp(cmd, "DEL") == 0) {
        char *key = strtok_r(NULL, " ", &save);
        char *val = strtok_r(NULL, "", &save);           /* SET only */

        if (!key) { n = snprintf(resp, respcap, "ERR usage %s <key> ...", cmd); goto done; }
        if (strlen(key) > MAX_KEY) { n = snprintf(resp, respcap, "ERR key too long"); goto done; }

        if (cmd[0] == 'S') {                              /* SET */
            if (!val) { n = snprintf(resp, respcap, "ERR usage SET <key> <value>"); goto done; }
            int rc = db_set(db, key, val, (uint32_t)strlen(val));
            n = snprintf(resp, respcap, "%s", rc == DK_OK ? "OK" :
                         rc == DK_TOOBIG ? "ERR value too large" : "ERR io");
        } else if (cmd[0] == 'G') {                       /* GET */
            static __thread char vbuf[PROTO_MAX_PAYLOAD];
            uint32_t vl = 0;
            int rc = db_get(db, key, vbuf, sizeof(vbuf), &vl);
            if (rc == DK_OK) n = snprintf(resp, respcap, "OK %.*s", (int)vl, vbuf);
            else             n = snprintf(resp, respcap, "ERR notfound");
        } else {                                          /* DEL */
            int rc = db_del(db, key);
            n = snprintf(resp, respcap, "%s", rc == DK_OK ? "OK" : "ERR notfound");
        }
        goto done;
    }

    n = snprintf(resp, respcap, "ERR unknown command");

done:
    free(big);
    return n < 0 ? 0 : (size_t)n;
}

/* ---- per-connection session (runs on a worker thread) ------------------ */

/*
 * Handle one client for its whole lifetime. Runs as a thread-pool job, so many
 * connections are served concurrently. Loops read-a-frame -> execute ->
 * write-a-reply until the client QUITs, disconnects, or sends a bad frame; then
 * closes the socket and frees its state. The req/resp buffers are `__thread`
 * (thread-local) so each worker has its own -- sharing one static buffer across
 * concurrent connections would be a data race.
 */
static void serve_connection(void *arg)
{
    Conn *c = arg;
    static __thread char req[PROTO_MAX_PAYLOAD];
    static __thread char resp[PROTO_MAX_PAYLOAD];

    for (;;) {
        uint32_t len = 0;
        int rc = frame_read(c->fd, req, sizeof(req), &len);
        if (rc == -2) { frame_write(c->fd, "ERR frame too large", 19); break; }
        if (rc != 0) break;                       /* peer closed or read error */

        size_t rlen = handle_command(c, req, len, resp, sizeof(resp));
        if (frame_write(c->fd, resp, (uint32_t)rlen) != 0) break;
        if (len >= 4 && strncmp(req, "QUIT", 4) == 0) break;
    }
    close(c->fd);
    free(c);
}

/* ---- accept loop ------------------------------------------------------- */

/*
 * The server main loop: set up signals, open the socket and thread pool, then
 * accept connections forever, handing each to a worker. Returns 0 after a clean
 * shutdown (triggered by SIGINT/SIGTERM).
 */
int server_run(const char *sock_path, DB *db, int nworkers)
{
    /* Ignore SIGPIPE so a client that vanishes mid-reply kills the write with
     * an EPIPE error we can handle, not a signal that would kill the server. */
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int lfd = unix_listen(sock_path, 64);
    if (lfd < 0) { perror("unix_listen"); return -1; }

    ThreadPool *tp = threadpool_create(nworkers, 128);
    fprintf(stderr,
            "\n  DuraKV server is ready.\n"
            "    socket : %s\n"
            "    workers: %d\n"
            "    connect with:  ./durakv-client %s\n"
            "    (press Ctrl-C to stop)\n\n",
            sock_path, nworkers, sock_path);

    while (!g_stop) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (g_stop) break;
            continue;
        }
        /* One heap Conn per client (freed by the worker); enqueue it so a pool
         * thread serves it while we go back to accepting the next connection. */
        Conn *c = calloc(1, sizeof(*c));
        c->fd = cfd; c->db = db;
        threadpool_submit(tp, serve_connection, c);
    }

    fprintf(stderr, "\n  DuraKV server stopped.\n");
    close(lfd);
    unlink(sock_path);
    threadpool_shutdown(tp);
    return 0;
}

#ifndef DURAKV_SERVER_NO_MAIN
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s <socket-path> [data.db] [wal.log] [workers]\n"
            "  env: DURAKV_FRAMES=<n>  DURAKV_POLICY=fifo|lru\n", argv[0]);
        return 2;
    }
    const char *sock = argv[1];
    const char *data = argc >= 3 ? argv[2] : "data.db";
    const char *wal  = argc >= 4 ? argv[3] : "wal.log";
    int workers      = argc >= 5 ? atoi(argv[4]) : 4;

    const char *fenv = getenv("DURAKV_FRAMES");
    const char *penv = getenv("DURAKV_POLICY");
    size_t frames = fenv ? (size_t)strtoul(fenv, NULL, 10) : 64;
    PolicyKind pol = (penv && strcmp(penv, "fifo") == 0) ? POLICY_FIFO : POLICY_LRU;

    DB *db = db_open_ex(data, wal, frames ? frames : 64, pol);
    if (!db) { fprintf(stderr, "failed to open store\n"); return 1; }

    int rc = server_run(sock, db, workers);
    db_close(db);
    return rc == 0 ? 0 : 1;
}
#endif
