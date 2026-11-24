#ifndef FUSEBOX_UPDATE_H
#define FUSEBOX_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL_atomic.h>

/* Newer FuseBoxes: GitHub's latest release (no drafts or pre-releases) against
 * this one's version (CMakeLists.txt). What was found last, and when, is in
 * <config>/update.json, so it's known without asking again. */

/* All releases, for a browser. */
#define UPDATE_PAGE "https://github.com/Slazmaz/FuseBox/releases/latest"

/* Takes away what an earlier update left, too. */
void update_init(const char *data_dir, const char *config_dir);
/* Seconds until GitHub may be asked again, 0 if it may now: it takes only so
 * many requests an hour, so each one soon after the last makes it longer. *done:
 * how much of the wait is over (0-1), may be NULL. */
int update_wait(float *done);
/* Asks GitHub; note gets what came of it, also where it can't be reached. */
void update_check(SDL_AtomicInt *abort, char *note, size_t size);
/* The newer version there is, false if none is known of. */
bool update_newer(char *version, size_t size);
/* Whether it can be installed from here: else it's in UPDATE_PAGE only. */
bool update_can_install(void);
/* Downloads it and puts it in this one's place; this one is to end then. *percent:
 * see http_download. False with why in error, all as it was. */
bool update_install(SDL_AtomicInt *abort, SDL_AtomicInt *percent, char *error, size_t size);

#endif
