#ifndef FUSEBOX_STEAMUI_H
#define FUSEBOX_STEAMUI_H

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_stdinc.h>
#include "library.h"
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

#define STEAMUI_PLAYERS 24

enum steamui_player
{
   STEAMUI_INVITED,
   STEAMUI_CONNECTING,
   STEAMUI_CONNECTED
};

struct steamui_party
{
   Uint64 lobby, host, left;
   char name[256];
   Uint64 ids[STEAMUI_PLAYERS];
   enum steamui_player states[STEAMUI_PLAYERS];
   int count;
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
 * "open <app id>" (the button for a game's emulator or launcher), "invite <app
 * id> <friend's Steam id>" or "join <lobby> <host's Steam id>" (session.h); or
 * what's missing there (steamui_art), or "unlocks <app id>" (steamui_unlocks).
 * False for none; at once when Steam has no port or nothing to click. */
bool steamui_asked(char *what, size_t size, SDL_AtomicInt *abort);
/* Whether Steam can be asked to run a game: STEAMUI_OK if so. */
enum steamui steamui_ready(SDL_AtomicInt *abort);
/* Has Steam run a game: by the shortcut it has, or by one that's there only
 * until steamui_unonce (lan_once). The app id, 0 if Steam can't now. */
Uint32 steamui_run(Uint32 appid, SDL_AtomicInt *abort);
Uint32 steamui_once(const char *name, const char *exe, const char *options, const char *icon,
      bool overlay, bool input, SDL_AtomicInt *abort);
bool steamui_unonce(Uint32 appid, SDL_AtomicInt *abort);
/* The shortcuts of the Steam that runs, changed there (it writes them down
 * itself). steamui_put: adds one (appid 0) or changes it; its app id, 0 if
 * Steam can't now. exe and dir without quotes. Art is what's in the grid's file
 * of that name (<app id>p.png, say). Blocking. */
bool steamui_can_put(SDL_AtomicInt *abort);
Uint32 steamui_put(Uint32 appid, const char *name, const char *exe, const char *dir,
      const char *options, SDL_AtomicInt *abort);
bool steamui_put_input(Uint32 appid, bool on, SDL_AtomicInt *abort);
bool steamui_put_icon(Uint32 appid, const char *path, SDL_AtomicInt *abort);
bool steamui_put_art(Uint32 appid, enum art_type type, const char *file, SDL_AtomicInt *abort);
bool steamui_take_out(Uint32 appid, SDL_AtomicInt *abort);
/* Gives Steam the pictures of a game that isn't in Steam, for an invite to it:
 * "art <id>", which steamui_asked also tells of. */
bool steamui_art(Uint32 id, SDL_AtomicInt *abort);
/* Gives Steam which of the achievements it shows of that shortcut's game the user
 * has by now: "unlocks <app id>", told of when the overlay is opened. */
bool steamui_unlocks(Uint32 appid, SDL_AtomicInt *abort);
/* Sends the friend a chat message: invited to that game. */
void steamui_invited(Uint64 who, const char *name, SDL_AtomicInt *abort);
/* Who is there while playing with friends: Steam shows them in the friends' list,
 * and says who comes and leaves. Lobby 0: it's over; left: by whom, 0 if not by
 * one who left. Steam takes it as over too if it isn't told again in a minute. */
void steamui_party(const struct steamui_party *p, SDL_AtomicInt *abort);
/* Has Steam join a lobby after all that steamui_asked told of. */
void steamui_pass(Uint64 lobby, Uint64 host, SDL_AtomicInt *abort);
/* Takes them all back out, if Steam has them. */
void steamui_clear(SDL_AtomicInt *abort);

#endif
