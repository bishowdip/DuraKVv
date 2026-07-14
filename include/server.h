/*
 * server.h - af_unix server over the store. server_run listens on the
 * socket path, hands each connection to the thread pool, returns after a
 * SIGINT/SIGTERM once the workers drain.
 */
#ifndef DURAKV_SERVER_H
#define DURAKV_SERVER_H

#include "storage.h"

int server_run(const char *sock_path, DB *db, int nworkers);

#endif /* DURAKV_SERVER_H */
