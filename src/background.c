#include "background.h"
#include "history.h"
#include "library.h"
#include "platform.h"
#include "ra.h"
#include "roms.h"
#include "scan.h"
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

/* Clicks on the buttons in Steam, taken by a thread of their own: the other
 * is busy with the network. */
static int SDLCALL take_clicks(void *user)
{
   SDL_AtomicInt *stop = user;

   while (!SDL_GetAtomicInt(stop))
   {
      Uint64 since = SDL_GetTicks();
      Uint32 appid;
      int i;

      if (steamui_asked(&appid, stop))
      {
         /* A process of its own: it reads the library, which is the other
          * thread's here. */
         const char *args[] = { NULL, "--open", NULL, NULL };
         char exe[1100], id[16];
         SDL_Process *p;

         SDL_snprintf(exe, sizeof(exe), "%sfusebox%s", SDL_GetBasePath(), PATH_SEP[0] == '\\' ? ".exe" : "");
         SDL_snprintf(id, sizeof(id), "%u", (unsigned)appid);
         args[0] = exe;
         args[2] = id;
         if ((p = SDL_CreateProcess(args, false)))
            SDL_DestroyProcess(p);
      }
      /* No Steam, or none with buttons. */
      else if (SDL_GetTicks() - since < 2000)
         for (i = 0; i < LOOK_MS / 100 && !SDL_GetAtomicInt(stop); i++)
            SDL_Delay(100);
   }
   return 0;
}

int background_run(const char *data_dir, const char *config_dir)
{
   SDL_Thread *clicks;
   SDL_AtomicInt stop;

   if (!platform_only_background())
      return 0;
   /* No icon just to take it away again. */
   ra_init(config_dir);
   if (!ra_active())
      return 0;
   platform_tray_start();
   SDL_SetAtomicInt(&stop, 0);
   clicks = SDL_CreateThread(take_clicks, "clicks", &stop);
   look_after_steam(data_dir, config_dir);
   SDL_SetAtomicInt(&stop, 1);
   SDL_WaitThread(clicks, NULL);
   platform_tray_stop();
   return 0;
}

int background_open(const char *data_dir, const char *appid)
{
   Uint32 id = (Uint32)SDL_strtoul(appid, NULL, 10);
   char exe[1024], name[96];
   bool found = false;
   int i;

   if (!id || !library_init(data_dir))
      return 1;
   library_lock();
   for (i = 0; i < library_count() && !found; i++)
      found = library_game(i)->steam_appid == id
         && scan_program(library_game(i), exe, sizeof(exe), name, sizeof(name));
   library_unlock();
   library_discard();
   library_quit();
   return found && platform_open(exe) ? 0 : 1;
}

/* How a game is started. */
struct start
{
   char exe[1024], args[1600], dir[1024];
};

static bool is_program(const char *path)
{
   size_t n = SDL_strlen(path);

   return n > 4 && SDL_strcasecmp(path + n - 4, ".exe") == 0;
}

/* By the library; what that has no emulator for is looked for again. False
 * with why in error. */
static bool find_start(const char *data_dir, const char *config_dir, const char *path,
      struct start *s, char *error, size_t size)
{
   bool found = false, rom = false, scanned = false;
   const struct game *g;
   char *slash;

   SDL_zerop(s);
   if (library_init(data_dir))
   {
      for (;;)
      {
         library_lock();
         g = library_game(library_by_path(path, history_appid()));
         rom = g && g->rom[0];
         found = g && g->exe[0] && !(rom && (g->no_emulator || !platform_exists(g->exe)));
         if (found)
         {
            SDL_strlcpy(s->exe, g->exe, sizeof(s->exe));
            library_launch_options(g, s->args, sizeof(s->args));
            if (platform_exists(g->start_dir))
               SDL_strlcpy(s->dir, g->start_dir, sizeof(s->dir));
         }
         library_unlock();
         if (found || scanned || (g && !rom) || is_program(path) || !platform_exists(path))
            break;
         roms_init(config_dir);
         scan_roms();
         scanned = true;
      }
      /* The window's to write. */
      library_discard();
      library_quit();
   }
   if ((rom || !found) && !platform_exists(path))
      SDL_snprintf(error, size, "This game isn't there any more:\n%s", path);
   else if (found)
      return true;
   else if (rom)
      SDL_snprintf(error, size, "No emulator was found for this game:\n%s", path);
   else
   {
      /* One the library doesn't have: as it is. */
      SDL_strlcpy(s->exe, path, sizeof(s->exe));
      SDL_strlcpy(s->dir, path, sizeof(s->dir));
      slash = SDL_strrchr(s->dir, PATH_SEP[0]);
      if (slash)
         *slash = '\0';
      if (!slash || !platform_exists(s->dir))
         s->dir[0] = '\0';
      return true;
   }
   return false;
}

int background_game(const char *data_dir, const char *config_dir, char **argv)
{
   char played[1100], error[1200];
   /* Older shortcuts say how. */
   bool old = SDL_strcmp(argv[1], "--run") == 0;
   struct start s;
   int code = 1;

   ra_init(config_dir);
   if (old)
   {
      SDL_zero(s);
      SDL_strlcpy(s.exe, argv[2], sizeof(s.exe));
   }
   else if (!find_start(data_dir, config_dir, argv[1], &s, error, sizeof(error)))
   {
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FuseBox", error, NULL);
      return 1;
   }
   if (ra_active())
      platform_start_background(true);
   history_playing(data_dir);
   if (!platform_run_game(s.exe, s.args, s.dir, argv, old ? 3 : 2, &code))
   {
      SDL_snprintf(error, sizeof(error), "Can't start this game:\n%s", s.exe);
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FuseBox", error, NULL);
   }
   played_file(config_dir, played, sizeof(played));
   SDL_SaveFile(played, "1", 1);
   return code;
}
