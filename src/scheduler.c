/*
 * scheduler.c - round robin. each client has its own FIFO, served one
 * request per turn in rotation; the cursor always moves PAST the client it
 * just served, so nobody starves -- any client with work gets service
 * within one sweep of the ring. enqueue O(1), next O(nclients) worst case
 * (skipping empties). one mutex makes it thread safe.
 */
#define _POSIX_C_SOURCE 200809L
#include "scheduler.h"

#include <stdlib.h>
#include <pthread.h>

/* One pending request, held in a singly-linked FIFO node. */
typedef struct Node { void *req; struct Node *next; } Node;

/* One client's private request queue (FIFO: enqueue at tail, serve at head). */
typedef struct {
    Node *head, *tail;   /* this client's pending requests */
    size_t len;          /* number queued (lets scheduler_next skip empties) */
} ClientQ;

struct Scheduler {
    ClientQ        *clients;    /* one queue per client id                    */
    int             nclients;
    int             cursor;     /* index of the client whose turn is next     */
    size_t          pending;    /* total across all queues (fast empty check) */
    pthread_mutex_t mtx;        /* guards every field above                   */
};

Scheduler *scheduler_create(int nclients)
{
    if (nclients < 1) nclients = 1;
    Scheduler *s = calloc(1, sizeof(*s));
    s->nclients = nclients;
    s->clients  = calloc(nclients, sizeof(ClientQ));
    s->cursor   = 0;
    pthread_mutex_init(&s->mtx, NULL);
    return s;
}

void scheduler_destroy(Scheduler *s)
{
    if (!s) return;
    for (int i = 0; i < s->nclients; i++) {
        Node *n = s->clients[i].head;
        while (n) { Node *nx = n->next; free(n); n = nx; }
    }
    pthread_mutex_destroy(&s->mtx);
    free(s->clients);
    free(s);
}

/* append to a client's queue. bad ids ignored, not trusted. */
void scheduler_enqueue(Scheduler *s, int client_id, void *request)
{
    if (client_id < 0 || client_id >= s->nclients) return;
    Node *n = malloc(sizeof(*n));
    n->req = request; n->next = NULL;

    pthread_mutex_lock(&s->mtx);
    ClientQ *q = &s->clients[client_id];
    if (q->tail) q->tail->next = n; else q->head = n;   /* link or start list */
    q->tail = n;
    q->len++;
    s->pending++;
    pthread_mutex_unlock(&s->mtx);
}

/* next request in rr order. 1 = dispatched, 0 = all queues empty.
 * serve one, move cursor past that client = the fairness guarantee. */
int scheduler_next(Scheduler *s, int *client_out, void **req_out)
{
    pthread_mutex_lock(&s->mtx);
    if (s->pending == 0) { pthread_mutex_unlock(&s->mtx); return 0; }

    /* Sweep at most one full lap to find the next client that has work. */
    for (int step = 0; step < s->nclients; step++) {
        int c = (s->cursor + step) % s->nclients;   /* wrap around the ring */
        ClientQ *q = &s->clients[c];
        if (q->len == 0) continue;                  /* skip idle clients    */

        /* Dequeue the head request of client c. */
        Node *n = q->head;
        q->head = n->next;
        if (!q->head) q->tail = NULL;               /* queue became empty   */
        q->len--;
        s->pending--;
        s->cursor = (c + 1) % s->nclients;          /* rotate past the served client */

        if (client_out) *client_out = c;
        if (req_out)    *req_out = n->req;
        free(n);
        pthread_mutex_unlock(&s->mtx);
        return 1;
    }
    pthread_mutex_unlock(&s->mtx);
    return 0;   /* unreachable: pending>0 implies some queue is non-empty */
}

/* Total requests waiting across all clients. */
size_t scheduler_pending(const Scheduler *s) { return s->pending; }
