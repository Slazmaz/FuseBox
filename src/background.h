#ifndef FUSEBOX_BACKGROUND_H
#define FUSEBOX_BACKGROUND_H

/* The FuseBox without a window (fusebox --background): while content is
 * to be added to Steam's menus it waits for Steam, however that's started, and
 * gives it that (steamui.h), again whenever Steam forgot it or a game
 * ended. Started with the computer, or by a game (background_game). */

/* Makes the computer match the settings: the file that has Steam open its
 * port, starting with the computer, and the background one running now. Run
 * it when they change, and at the start. */
void background_apply(void);

/* fusebox --background: until it's no longer wanted. data_dir: FuseBox's
 * folder; config_dir: its config. */
int background_run(const char *data_dir, const char *config_dir);

/* fusebox "<game's path>" [--emulator "<path>"] [arguments], as Steam's shortcuts
 * have it (older ones: --run "<exe>" <arguments>): starts the background one if
 * it's wanted, then the game as the library says to (a ROM with its emulator), or
 * with the emulator that's given, this time only; returns when that ends, with its exit code. Not started by Steam, it has Steam run the game
 * (its code is 0 then), for the overlay and the rest, whether it has a shortcut
 * there or not: --once "<path>" is one that's there only meanwhile. argv: main's. */
int background_game(const char *data_dir, const char *config_dir, char **argv);

/* fusebox --open <app id>: opens the emulator or launcher of the game whose
 * shortcut that is (its button in Steam). */
int background_open(const char *data_dir, const char *config_dir, const char *appid);

/* fusebox --art <id>: the pictures of a game that isn't in Steam, to Steam
 * (steamui_art). */
int background_art(const char *data_dir, const char *config_dir, const char *id);

/* fusebox --unlocks <app id>: which achievements of the game whose shortcut that
 * is the user has, to Steam (steamui_unlocks). */
int background_unlocks(const char *data_dir, const char *config_dir, const char *appid);

#endif
