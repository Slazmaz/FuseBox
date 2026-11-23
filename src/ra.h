#ifndef FUSEBOX_RA_H
#define FUSEBOX_RA_H

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL_atomic.h>

/* RetroAchievements: which ROMs have achievements and which of them the
 * user has unlocked. Only ever read: earning them is the emulator's job.
 * The login's token is in <config>/credentials.cfg (the password is never
 * kept). */

void ra_init(const char *config_dir);

/* The account logged in to; false (and an empty name) for none. */
bool ra_user(char *user, size_t size);
/* Blocking. False when refused or offline, with why in error. */
bool ra_login(const char *user, const char *password, SDL_AtomicInt *abort, char *error,
      size_t size);
/* Forgets the account, and the achievements known of games. */
void ra_logout(void);

/* Finds the achievements of the library's ROMs (the ra_ fields of game).
 * What was asked minutes ago is only asked again with force. Blocking;
 * false when not logged in or RetroAchievements couldn't be reached. */
bool ra_refresh(bool force, SDL_AtomicInt *abort);

#endif
