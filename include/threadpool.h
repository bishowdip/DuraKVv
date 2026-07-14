/*
 * threadpool.h - N workers over a bounded ring buffer. one mutex + two
 * condvars (not_empty, not_full) = the textbook producer-consumer.
 * producers block when full (backpressure), workers block when empty.
 */
#ifndef DURAKV_THREADPOOL_H
#define DURAKV_THREADPOOL_H

#include <stddef.h>

typedef void (*job_fn)(void *arg);

typedef struct ThreadPool ThreadPool;

ThreadPool *threadpool_create(int nworkers, int queue_cap);

/* Enqueue a job. Blocks while the queue is full. Returns 0 on success, or -1
 * if the pool is shutting down. */
int  threadpool_submit(ThreadPool *tp, job_fn fn, void *arg);

/* Stop accepting jobs, let workers drain the queue, then join them all. */
void threadpool_shutdown(ThreadPool *tp);

/* Number of jobs completed so far (for stats/tests). */
unsigned long threadpool_completed(ThreadPool *tp);

#endif /* DURAKV_THREADPOOL_H */
