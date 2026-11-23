#ifndef FUSEBOX_STEAMUI_H
#define FUSEBOX_STEAMUI_H

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL_atomic.h>
#include "platform.h"

/* RetroAchievements in Steam's own library: its interface is a web page,
 * and a Steam started with a debugging port (on this computer only) takes
 * scripts for it. steamui_js.h gives that page the achievements of the
 * ROMs' shortcuts, in memory: none of Steam's files are changed, and Steam
 * forgets them when it restarts. */

enum steamui
{
   /* Steam isn't running, or has no port open. */
   STEAMUI_NO_PORT,
   /* It's still starting, or nobody is logged in yet. */
   STEAMUI_WAIT,
   /* Steam changed: the script found nothing to hold on to. */
   STEAMUI_UNSUPPORTED,
   STEAMUI_OK
};

/* Whether Steam has what was given to it: STEAMUI_OK, STEAMUI_WAIT (not
 * yet, or it forgot) or STEAMUI_NO_PORT. */
enum steamui steamui_check(SDL_AtomicInt *abort);
/* Starts Steam: with a port when achievements are to be shown in it. */
void steamui_start(const struct steam_install *steam);
/* Gives Steam every game's achievements and the friends who play it (and
 * takes back those of games that left); only_friends: the friends again,
 * who may have started playing. Blocking. *games: how many have them. */
enum steamui steamui_push(bool only_friends, int *games, SDL_AtomicInt *abort);
/* Takes them all back out, if Steam has them. */
void steamui_clear(SDL_AtomicInt *abort);

#endif
