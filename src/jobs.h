#ifndef FUSEBOX_JOBS_H
#define FUSEBOX_JOBS_H

#include <stdbool.h>
#include <SDL3/SDL_atomic.h>

/* A few worker threads for what blocks: scanning, SteamGridDB, downloads,
 * decoding. Jobs of a level run before any of the ones after it. */

enum job_level
{
   /* What the user just did, and what's on disk already. */
   JOB_NOW,
   /* The icons games have: the list's and the open game's. */
   JOB_ICON,
   /* The open game's art of the kind shown. */
   JOB_SELECTED,
   /* The other art to pick from. */
   JOB_OPTIONS,
   /* Other games' art. Never on every thread, so the rest starts at once. */
   JOB_REST,
   JOB_LEVELS
};

void jobs_init(int threads);
/* Cuts transfers short (jobs_abort) and waits for the threads; jobs not
 * started are dropped (their data freed with SDL_free). */
void jobs_quit(void);
/* fn runs on a worker and owns data (frees it). first: before the others
 * of its level, else after them. */
void jobs_add(void (*fn)(void *data), void *data, enum job_level level, bool first);
/* Set while quitting: pass to HTTP calls so they end early. */
SDL_AtomicInt *jobs_abort(void);

#endif
