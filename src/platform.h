#ifndef FUSEBOX_PLATFORM_H
#define FUSEBOX_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

/* What differs between Windows and Linux: where Steam is, whether it
 * runs, the registry. Paths are UTF-8 with '/' or '\' as the OS likes. */

#ifdef _WIN32
#define PATH_SEP "\\"
#else
#define PATH_SEP "/"
#endif

struct steam_install
{
   /* Steam's folder (holds userdata), empty if none was found. */
   char root[1024];
   /* The most recently used account's userdata/<id>/config. */
   char config[1024];
   /* How to run Steam (for -shutdown and starting it again). */
   char exe[1024];
   bool flatpak;
};

bool platform_find_steam(struct steam_install *steam);
bool platform_steam_running(const struct steam_install *steam);
/* Asks Steam to quit, or starts it (with its interface's debugging port
 * open on this computer, unless 0); returns at once. */
void platform_steam_shutdown(const struct steam_install *steam);
void platform_steam_start(const struct steam_install *steam, int port);

/* The home folder, without a trailing separator. */
void platform_home(char *dst, size_t size);
bool platform_exists(const char *path);

/* Saves the exe's own icon (its biggest, up to 256) as a PNG at png_path.
 * Windows; false elsewhere or when it has none. */
bool platform_exe_icon(const char *exe, const char *png_path);

/* Gives a frameless window the system's drop shadow (Windows; nothing
 * elsewhere). */
void platform_window_shadow(void *sdl_window);

/* fusebox --background (background.h) at login, or no more. */
void platform_autostart(bool on);
/* Starts it now, unless it runs. by_game: from a game Steam started, so
 * not as part of it: Steam would say the game still runs. */
void platform_start_background(bool by_game);
/* Whether the caller is the only background one; stays so till it ends. */
bool platform_only_background(void);
/* The background one's icon by the taskbar's clock: opens FuseBox, or quits
 * the background one (Windows; nothing elsewhere). */
void platform_tray_start(void);
void platform_tray_stop(void);
/* Waits that long, or until quitting is asked for there: true then. */
bool platform_tray_wait(int ms);
/* A program's window to the front, or the program started; in front, though
 * the caller isn't. False if it can't be. */
bool platform_open(const char *exe);
/* Runs a game and waits for it: exe, with args (as in a command line), and
 * after them this program's own arguments from argv[skip] on (Windows: as they
 * were typed). dir: where it starts, empty for here. False if it can't be
 * started; else its exit code in *code. */
bool platform_run_game(const char *exe, const char *args, const char *dir, char **argv, int skip,
      int *code);

#ifdef _WIN32
/* hive: "HKLM" or "HKCU". value NULL reads the key's default value. */
bool platform_reg_string(const char *hive, const char *key, const char *value,
      char *dst, size_t size);
bool platform_reg_dword(const char *hive, const char *key, const char *value,
      unsigned *dst);
/* Calls fn with each subkey's name. */
void platform_reg_subkeys(const char *hive, const char *key,
      void (*fn)(const char *name, void *user), void *user);
#endif

#endif
