#include "background.h"
#include "library.h"
#include "platform.h"
#include "ra.h"
#include "steam.h"
#include "steamui.h"

#include <SDL3/SDL.h>

/* How often it looks (ms): whether Steam runs; for a port while Steam is
 * young, and later (it won't open one any more). */
#define LOOK_MS 5000
#define PORT_MS 60000
#define YOUNG_MS 120000
/* Progress is looked up again this often, games' ends aside (ms). */
#define REFRESH_MS (30 * 60 * 1000)
/* And which friends play, this often (ms). */
#define FRIENDS_MS (3 * 60 * 1000)
/* A Steam it can't work with is tried again after this (ms). */
#define UNSUPPORTED_MS (10 * 60 * 1000)

/* Steam opens its port by itself when this is in its folder. */
static void port_file(const struct steam_install *st, char *dst, size_t size)
{
   SDL_snprintf(dst, size, "%s" PATH_SEP ".cef-enable-remote-debugging", st->root);
}

/* Touched when a game ends. */
static void played_file(const char *config_dir, char *dst, size_t size)
{
   SDL_snprintf(dst, size, "%s" PATH_SEP "played", config_dir);
}

void background_apply(void)
{
   const struct steam_install *st = steam_install();
   char path[1100];
   bool active = ra_active();

   if (st)
   {
      port_file(st, path, sizeof(path));
      if (active && !platform_exists(path))
      {
         if (SDL_SaveFile(path, "", 0))
            ra_set_made_file(true);
         else
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Can't write %s: %s", path, SDL_GetError());
      }
      /* Another program's stays. */
      else if (!active && ra_made_file())
      {
         SDL_RemovePath(path);
         ra_set_made_file(false);
      }
   }
   platform_autostart(active && ra_autostart());
   if (active && ra_autostart())
      platform_start_background(false);
}

/* Until it's no longer wanted, or told to quit by its icon. */
static void look_after_steam(const char *data_dir, const char *config_dir)
{
   char played[1100];
   const struct steam_install *st;
   SDL_PathInfo info;
   SDL_Time seen = 0;
   Uint64 steam_since = 0, pushed = 0, friends = 0, next = 0;
   bool quit;

   played_file(config_dir, played, sizeof(played));
   if (SDL_GetPathInfo(played, &info))
      seen = info.modify_time;
   for (quit = false; !quit; quit = platform_tray_wait(LOOK_MS))
   {
      Uint64 now = SDL_GetTicks();
      enum steamui state;
      bool ended = false, only_friends;
      int games = 0;

      /* As the window left them. */
      ra_init(config_dir);
      if (!ra_active())
         return;
      if (!(st = steam_install()) && !(steam_init() && (st = steam_install())))
         continue;
      if (!platform_steam_running(st))
      {
         /* Started by a game: gone with Steam. */
         if (!ra_autostart())
            return;
         steam_since = pushed = next = 0;
         continue;
      }
      if (!steam_since)
         steam_since = now;
      if (SDL_GetPathInfo(played, &info) && info.modify_time != seen)
      {
         seen = info.modify_time;
         ended = true;
         next = 0;
      }
      if (now < next)
         continue;
      state = steamui_check(NULL);
      if (state == STEAMUI_NO_PORT)
      {
         next = now - steam_since > YOUNG_MS ? now + PORT_MS : 0;
         continue;
      }
      only_friends = state == STEAMUI_OK && pushed && !ended && now - pushed < REFRESH_MS;
      if (only_friends && now - friends < FRIENDS_MS)
         continue;
      /* Read again each time, and never written: the window's is newer. */
      if (!library_init(data_dir))
         continue;
      if (!only_friends)
         ra_refresh(ended, NULL);
      state = steamui_push(only_friends, &games, NULL);
      library_discard();
      library_quit();
      if (state == STEAMUI_OK)
      {
         friends = SDL_GetTicks();
         if (!only_friends)
            pushed = friends;
      }
      else if (state == STEAMUI_UNSUPPORTED)
         next = SDL_GetTicks() + UNSUPPORTED_MS;
   }
}

int background_run(const char *data_dir, const char *config_dir)
{
   if (!platform_only_background())
      return 0;
   /* No icon just to take it away again. */
   ra_init(config_dir);
   if (!ra_active())
      return 0;
   platform_tray_start();
   look_after_steam(data_dir, config_dir);
   platform_tray_stop();
   return 0;
}

int background_game(const char *config_dir, char **argv)
{
   char played[1100];
   int code;

   ra_init(config_dir);
   if (ra_active())
      platform_start_background(true);
   code = platform_run_game(argv + 2);
   played_file(config_dir, played, sizeof(played));
   SDL_SaveFile(played, "1", 1);
   return code;
}
