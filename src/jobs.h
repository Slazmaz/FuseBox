#ifndef FUSEBOX_JOBS_H
#define FUSEBOX_JOBS_H

#include <stdbool.h>
#include <SDL3/SDL_atomic.h>

/* A few worker threads for what blocks: scanning, SteamGridDB, downloads,
 * decoding. Jobs run in order, urgent ones (what the user is looking at)
 * before the rest. */

void jobs_init(int threads);
/* Cuts transfers short (jobs_abort) and waits for the threads; jobs not
 * started are dropped (their data freed with SDL_free). */
void jobs_quit(void);
/* fn runs on a worker and owns data (frees it). */
void jobs_add(void (*fn)(void *data), void *data, bool urgent);
/* Set while quitting: pass to HTTP calls so they end early. */
SDL_AtomicInt *jobs_abort(void);

#endif
