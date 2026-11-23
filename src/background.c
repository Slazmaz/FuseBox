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
/* Progress is looked up again this often (ms). */
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
      platform_start_background();
}

int background_run(const char *data_dir, const char *config_dir)
{
   const struct steam_install *st;
   Uint64 steam_since = 0, pushed = 0, friends = 0, next = 0;

   if (!platform_only_background())
      return 0;
   for (;; SDL_Delay(LOOK_MS))
   {
      Uint64 now = SDL_GetTicks();
      enum steamui state;
      bool only_friends;
      int games = 0;

      /* As the window left them. */
      ra_init(config_dir);
      if (!ra_active() || !ra_autostart())
         return 0;
      if (!(st = steam_install()) && !(steam_init() && (st = steam_install())))
         continue;
      if (!platform_steam_running(st))
      {
         steam_since = pushed = next = 0;
         continue;
      }
      if (!steam_since)
         steam_since = now;
      if (now < next)
         continue;
      state = steamui_check(NULL);
      if (state == STEAMUI_NO_PORT)
      {
         next = now - steam_since > YOUNG_MS ? now + PORT_MS : 0;
         continue;
      }
      only_friends = state == STEAMUI_OK && pushed && now - pushed < REFRESH_MS;
      if (only_friends && now - friends < FRIENDS_MS)
         continue;
      /* Read again each time, and never written: the window's is newer. */
      if (!library_init(data_dir))
         continue;
      if (!only_friends)
         ra_refresh(false, NULL);
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
