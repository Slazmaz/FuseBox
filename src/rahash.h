#ifndef FUSEBOX_RAHASH_H
#define FUSEBOX_RAHASH_H

#include <stdbool.h>
#include <SDL3/SDL_atomic.h>

#define RA_HASH_SIZE 33

/* RetroAchievements' console number for a game's system (its source) and
 * ROM; 0 for one it doesn't have. */
unsigned ra_console(const char *system, const char *rom);
/* False for what can't be read: .7z and compressed disc images (CHD, RVZ...). */
bool ra_hashable(const char *rom);
/* The ROM's hash as RetroAchievements makes it (rcheevos); one in a .zip is
 * hashed unpacked. False when it can't. Blocking; one at a time. */
bool ra_hash(unsigned console, const char *rom, char hash[RA_HASH_SIZE], SDL_AtomicInt *abort);

#endif
