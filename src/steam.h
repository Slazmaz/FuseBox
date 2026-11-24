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

/* What the shortcut FuseBox makes for the game starts: FuseBox, with the
 * game's path (library_path) as its options; how that's started is FuseBox's
 * to know (background_game). One with %command% in its options starts by itself. */
void steam_target(const struct game *g, char *exe, size_t exe_size, char *options,
      size_t options_size);

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
