#ifndef FUSEBOX_STEAM_H
#define FUSEBOX_STEAM_H

#include <stdbool.h>
#include <stddef.h>
#include "library.h"
#include "platform.h"

/* Steam as one user of the library: its non-Steam shortcuts
 * (userdata/<id>/config/shortcuts.vdf) and their art (config/grid). */

/* Finds Steam; false if it isn't installed. */
bool steam_init(void);
const struct steam_install *steam_install(void);

/* The settings, in <config>/steam.json. After ra_init (older FuseBoxes kept
 * them with the achievements'). Again to read them again. */
void steam_read_settings(const char *config_dir);
/* Whether the window has games to add to Steam (and take out). On until turned
 * off. */
bool steam_shortcuts(void);
void steam_set_shortcuts(bool on);
/* Whether content is added to Steam's menus (steamui.h). Off until turned
 * on: the port it needs is open to every program on the computer. */
bool steam_menus(void);
void steam_set_menus(bool on);
/* Whether FuseBox starts with the computer for that (background.h); on until
 * turned off. */
bool steam_autostart(void);
void steam_set_autostart(bool on);
/* Whether FuseBox made the file that has Steam open its port. */
bool steam_made_file(void);
void steam_set_made_file(bool made);
/* The port Steam was last started with, 0 for none. */
int steam_port(void);
void steam_set_port(int port);

/* What the shortcut FuseBox makes for the game starts: FuseBox, with the
 * game's path (library_path) as its options; how that's started is FuseBox's
 * to know (background_game). Options of the user's with %command% go around. */
void steam_target(const struct game *g, char *exe, size_t exe_size, char *options,
      size_t options_size);

/* What the shortcut starts that's there only while the game runs
 * (steamui_once): fusebox --once "<path>", and more after it. The launch
 * options count where they have %command%; the rest is FuseBox's to add. */
void steam_once_target(const char *path, const char *launch_options, const char *more, char *exe,
      size_t exe_size, char *options, size_t options_size);
/* Where the settings are (steam_read_settings). */
const char *steam_settings_dir(void);

/* Adds Steam's own non-Steam shortcuts (those FuseBox didn't make) to the
 * library, so their art can be picked too. */
int steam_read_shortcuts(void);
/* When the shortcut's art was first put in Steam (seconds since 1970), 0 if
 * that can't be told. */
Sint64 steam_art_time(Uint32 appid);

/* Makes Steam match the library: a shortcut (with its art) for each
 * installed game ticked in_steam, and none for the rest (only ever
 * removing shortcuts FuseBox made). Steam must not be running (it would
 * write its own copy over ours on exit). msg gets what happened. */
bool steam_sync(char *msg, size_t size);

#endif
