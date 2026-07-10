/*
 * scheduler.h - round robin over per-client queues. one request per client
 * per turn, cursor rotates past whoever was just served -- so a heavy
 * client cannot starve the light ones (unlike a single FCFS queue).
 */
#ifndef DURAKV_SCHEDULER_H
#define DURAKV_SCHEDULER_H

#include <stddef.h>

typedef struct Scheduler Scheduler;

Scheduler *scheduler_create(int nclients);
void       scheduler_destroy(Scheduler *s);

/* Append a request (an opaque pointer) to a client's queue. */
void scheduler_enqueue(Scheduler *s, int client_id, void *request);

/* Pop the next request in round-robin order. Returns 1 and fills *client_out
 * / *req_out on success, or 0 when every queue is empty. */
int  scheduler_next(Scheduler *s, int *client_out, void **req_out);

size_t scheduler_pending(const Scheduler *s);   /* total queued across clients */

#endif /* DURAKV_SCHEDULER_H */
