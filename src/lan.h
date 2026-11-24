#ifndef FUSEBOX_LAN_H
#define FUSEBOX_LAN_H

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL_stdinc.h>
#include "library.h"

/* The games that run through FuseBox (background_game). */

/* Notes that this process runs the game of that shortcut (0: none) and path
 * (empty: not known), or that it's over. <config>/playing/<process>. */
void lan_playing(const char *config_dir, Uint32 appid, const char *path, bool on);
/* The process that runs the game of that shortcut or that path (0, empty: not
 * asked for), 0 if none does. */
Uint32 lan_game(const char *config_dir, Uint32 appid, const char *path);
bool lan_running(Uint32 pid);

/* Notes the app id of a shortcut that's there only while this process waits for
 * its game (steamui_once), or that it's gone. <config>/once/<app id>. */
void lan_once(const char *config_dir, Uint32 appid, const char *path, bool on);
/* One whose process is gone, 0 for none (lan_once then). */
Uint32 lan_once_left(const char *config_dir);

#endif
