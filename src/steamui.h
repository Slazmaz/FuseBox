#ifndef FUSEBOX_STEAMUI_H
#define FUSEBOX_STEAMUI_H

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_stdinc.h>
#include "platform.h"

/* What FuseBox adds to Steam's own library (steam_menus): its interface is a
 * web page, and a Steam started with a debugging port (on this computer only)
 * takes scripts for it. steamui_js.h gives that page the shortcuts' activity
 * and buttons and the ROMs' achievements (ra.h), in memory: none of Steam's
 * files are changed, and Steam forgets them when it restarts. */

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
/* Starts Steam: with a port when content is to be added to its menus. */
void steamui_start(const struct steam_install *steam);
/* Gives Steam every game's button and activity and, where they're shown
 * (ra_active), its achievements and the friends who play it (and takes back
 * those of games that left); only_friends: the friends again, who may have
 * started playing. Blocking. *games: how many have achievements. */
enum steamui steamui_push(bool only_friends, int *games, SDL_AtomicInt *abort);
/* Waits some seconds for a click on what's there only while this is called:
 * "open <app id>" (the button for a game's emulator or launcher). False for
 * none; at once when Steam has no port or nothing to click. */
bool steamui_asked(char *what, size_t size, SDL_AtomicInt *abort);
/* Has Steam run a game: by the shortcut it has, or by one that's there only
 * until steamui_unonce (lan_once). The app id, 0 if Steam can't now. */
Uint32 steamui_run(Uint32 appid, SDL_AtomicInt *abort);
Uint32 steamui_once(const char *name, const char *exe, const char *options, const char *icon,
      bool overlay, bool input, SDL_AtomicInt *abort);
bool steamui_unonce(Uint32 appid, SDL_AtomicInt *abort);
/* Takes them all back out, if Steam has them. */
void steamui_clear(SDL_AtomicInt *abort);

#endif
