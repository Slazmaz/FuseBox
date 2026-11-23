#ifndef FUSEBOX_RA_H
#define FUSEBOX_RA_H

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL_atomic.h>

/* RetroAchievements: which ROMs have achievements and which of them the
 * user has unlocked. Only ever read: earning them is the emulator's job.
 * The login's token is in <config>/credentials.cfg (the password is never
 * kept); <config>/retroachievements.json has the settings here. */

void ra_init(const char *config_dir);

/* The account logged in to; false (and an empty name) for none. */
bool ra_user(char *user, size_t size);
/* Blocking. False when refused or offline, with why in error. */
bool ra_login(const char *user, const char *password, SDL_AtomicInt *abort, char *error,
      size_t size);
/* Forgets the account, and the achievements known of games. */
void ra_logout(void);

/* Whether Steam shows them (steamui.h); on until turned off. */
bool ra_in_steam(void);
void ra_set_in_steam(bool on);
/* Logged in, with that on. */
bool ra_active(void);
/* The port Steam was last started with for it, 0 for none. */
int ra_port(void);
void ra_set_port(int port);

/* Finds the achievements of the library's ROMs (the ra_ fields of game).
 * What was asked minutes ago is only asked again with force. Blocking;
 * false when not logged in or RetroAchievements couldn't be reached. */
bool ra_refresh(bool force, SDL_AtomicInt *abort);

/* A game's achievements as a JSON list of [id, title, description, badge,
 * percent of players, unlocked]; SDL_free it. From disk while the numbers
 * are the same. Blocking; NULL on a failure. */
char *ra_list(unsigned id, unsigned total, unsigned unlocked, SDL_AtomicInt *abort);

#endif
