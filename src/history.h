#ifndef FUSEBOX_HISTORY_H
#define FUSEBOX_HISTORY_H

#include <SDL3/SDL_stdinc.h>

/* When a game was added to Steam by FuseBox, and when it was first played
 * from there since: for its activity in Steam (steamui.h). One small file a
 * shortcut, <data>/history/<app id>.json: written by whichever FuseBox adds
 * or starts it, so it's not in library.json, which is the window's. */

/* Times are seconds since 1970, 0 for unknown; played is -1 for a game that
 * may have been played before anyone looked. */
void history_added(const char *data_dir, Uint32 appid);
/* One added before that was noted: when that was, as far as can be told. */
void history_found(const char *data_dir, Uint32 appid, Sint64 added);
void history_played(const char *data_dir, Uint32 appid, Sint64 played);
void history_removed(const char *data_dir, Uint32 appid);
/* A game Steam starts (SteamGameId says which): its first time is noted, for
 * one whose adding was. Others may have been played before. */
void history_playing(const char *data_dir);
void history_get(const char *data_dir, Uint32 appid, Sint64 *added, Sint64 *played);

#endif
