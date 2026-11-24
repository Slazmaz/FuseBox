#ifndef FUSEBOX_BACKGROUND_H
#define FUSEBOX_BACKGROUND_H

/* The FuseBox without a window (fusebox --background): while achievements
 * are to be shown in Steam it waits for Steam, however that's started, and
 * gives it them (steamui.h), again whenever Steam forgot them or a game
 * ended. Started with the computer, or by a game (background_game). */

/* Makes the computer match the settings: the file that has Steam open its
 * port, starting with the computer, and the background one running now. Run
 * it when they or the login change, and at the start. */
void background_apply(void);

/* fusebox --background: until it's no longer wanted. data_dir: FuseBox's
 * folder; config_dir: its config. */
int background_run(const char *data_dir, const char *config_dir);

/* fusebox "<game's path>" [arguments], as Steam's shortcuts have it (older
 * ones: --run "<exe>" <arguments>): starts the background one if it's wanted,
 * then the game as the library says to (a ROM with its emulator); returns when
 * that ends, with its exit code. argv: main's. */
int background_game(const char *data_dir, const char *config_dir, char **argv);

/* fusebox --open <app id>: opens the emulator or launcher of the game whose
 * shortcut that is (its button in Steam). */
int background_open(const char *data_dir, const char *appid);

#endif
