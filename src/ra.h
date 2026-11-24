#ifndef FUSEBOX_RA_H
#define FUSEBOX_RA_H

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_stdinc.h>

/* RetroAchievements: which ROMs have achievements and which of them the
 * user has unlocked. Only ever read: earning them is the emulator's job.
 * The login's token is in <config>/credentials.cfg (the password is never
 * kept); <config>/retroachievements.json has the settings here. */

/* Again to read the login and settings again. */
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
/* Whether FuseBox starts with the computer for it (background.h); on until
 * turned off. */
bool ra_autostart(void);
void ra_set_autostart(bool on);
/* Whether FuseBox made the file that has Steam open its port. */
bool ra_made_file(void);
void ra_set_made_file(bool made);
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

/* The user's friends who played a game, as a JSON list of [name, avatar's
 * address, playing it now, what they're doing in it, the game they played
 * last, when that was (seconds since 1970), playing that now, its icon's address]; SDL_free it. It's
 * told by who has the game's most common achievements, looked up once a
 * day, and by what each friend played last. list: ra_list's. Blocking; NULL
 * on a failure. */
char *ra_friends(unsigned id, const char *list, SDL_AtomicInt *abort);

/* Who unlocked a game's achievements when, as a JSON list of [achievement,
 * friend's name (empty: the user), seconds since 1970]; SDL_free it. Friends'
 * are all of them, the user's those from when FuseBox first looked on, and
 * before that those lately enough to be among an achievement's last winners:
 * RetroAchievements doesn't tell this login more. Only some are looked up a
 * call, the rest in later ones. list: ra_list's. Blocking; NULL on a failure. */
char *ra_activity(unsigned id, const char *list, SDL_AtomicInt *abort);

/* The game's leaderboards the user or a friend who plays it is on, as a JSON
 * list of [id, title, description, how many are on it, [[friend's name (empty:
 * the user), score as it's shown, rank], ...]]; SDL_free it. Looked up again
 * when the game was played since (last: when that was, seconds since 1970), or
 * after a day; like ra_activity's, some a call. list: ra_list's. Blocking; NULL
 * on a failure. */
char *ra_boards(unsigned id, const char *list, Sint64 last, SDL_AtomicInt *abort);

#endif
