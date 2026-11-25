#include "background.h"
#include "history.h"
#include "inject.h"
#include "lan.h"
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

static void own_exe(char *dst, size_t size)
{
   SDL_snprintf(dst, size, "%sfusebox%s", SDL_GetBasePath(), PATH_SEP[0] == '\\' ? ".exe" : "");
}

/* Touched when a game ends (its shortcut's app id) or starts. */
static void played_file(const char *config_dir, char *dst, size_t size)
{
   SDL_snprintf(dst, size, "%s" PATH_SEP "played", config_dir);
}

void background_apply(void)
{
   const struct steam_install *st = steam_install();
   char path[1100];
   bool active = steam_menus();

   if (st)
   {
      port_file(st, path, sizeof(path));
      if (active && !platform_exists(path))
      {
         if (SDL_SaveFile(path, "", 0))
            steam_set_made_file(true);
         else
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Can't write %s: %s", path, SDL_GetError());
      }
      /* Another program's stays. */
      else if (!active && steam_made_file())
      {
         SDL_RemovePath(path);
         steam_set_made_file(false);
      }
   }
   platform_autostart(active && steam_autostart());
   if (active && steam_autostart())
      platform_start_background(false);
}

/* Until it's no longer wanted, or told to quit by its icon. */
static void look_after_steam(const char *data_dir, const char *config_dir)
{
   char played[1100];
   const struct steam_install *st;
   SDL_PathInfo info;
   SDL_Time seen = 0, built = 0;
   char exe[1100];
   Uint64 steam_since = 0, pushed = 0, friends = 0, next = 0;
   bool quit;

   played_file(config_dir, played, sizeof(played));
   if (SDL_GetPathInfo(played, &info))
      seen = info.modify_time;
   own_exe(exe, sizeof(exe));
   if (SDL_GetPathInfo(exe, &info))
      built = info.modify_time;
   for (quit = false; !quit; quit = platform_tray_wait(LOOK_MS))
   {
      Uint64 now = SDL_GetTicks();
      enum steamui state;
      bool ended = false, only_friends;
      int games = 0, left;
      Uint32 appid;

      /* FuseBox was updated: the new one's turn. A game that runs may be played
       * with friends, which this one sees to. */
      if (built && SDL_GetPathInfo(exe, &info) && info.modify_time != built
            && !lan_any(config_dir) && platform_again_background())
         return;
      /* As the window left them. */
      ra_init(config_dir);
      steam_read_settings(config_dir);
      if (!steam_menus())
         return;
      if (!(st = steam_install()) && !(steam_init() && (st = steam_install())))
         continue;
      if (!platform_steam_running(st))
      {
         /* Started by a game: gone with Steam. */
         if (!steam_autostart())
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
      if (only_friends && (!ra_active() || now - friends < FRIENDS_MS))
         continue;
      /* What a game that didn't get to end left in Steam. */
      for (left = 0; left < 16 && (appid = lan_once_left(config_dir))
            && steamui_unonce(appid, NULL); left++)
         lan_once(config_dir, appid, "", false);
      steam_account();
      /* Read again each time, and never written: the window's is newer. */
      if (!library_init(data_dir))
         continue;
      if (!only_friends && ra_active())
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

/* Tells the session (session.h), started if there's none. *session: the one
 * started last. */
static void tell_session(SDL_Process **session, const char *line)
{
   SDL_IOStream *io;
   char exe[1100];
   int tries;

   for (tries = 0; tries < 2; tries++)
   {
      if (*session && SDL_WaitProcess(*session, false, NULL))
      {
         SDL_DestroyProcess(*session);
         *session = NULL;
      }
      if (!*session)
      {
         const char *args[] = { exe, "--session", NULL };
         SDL_PropertiesID p = SDL_CreateProperties();

         own_exe(exe, sizeof(exe));
         SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);
         SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
         SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
         *session = platform_process(args, p);
         SDL_DestroyProperties(p);
      }
      if (!*session || !(io = SDL_GetProcessInput(*session)))
         return;
      if (SDL_IOprintf(io, "%s\n", line) && SDL_FlushIO(io))
         return;
      /* It's just ending: another. */
      SDL_WaitProcess(*session, true, NULL);
   }
}

/* Clicks in Steam, taken by a thread of their own: the other is busy with the
 * network. */
static int SDLCALL take_clicks(void *user)
{
   SDL_AtomicInt *stop = user;
   SDL_Process *session = NULL;

   while (!SDL_GetAtomicInt(stop))
   {
      Uint64 since = SDL_GetTicks();
      unsigned long long a, b;
      char what[128];
      const char *flag;
      int i;

      if (steamui_asked(what, sizeof(what), stop))
      {
         char line[128];

         /* Only what these are: it's a web page that says it. */
         if (SDL_sscanf(what, "invite %llu %llu", &a, &b) == 2 && a <= 0xFFFFFFFFu)
         {
            SDL_snprintf(line, sizeof(line), "invite %llu %llu", a, b);
            tell_session(&session, line);
            continue;
         }
         if (SDL_sscanf(what, "join %llu %llu", &a, &b) == 2)
         {
            SDL_snprintf(line, sizeof(line), "join %llu %llu", a, b);
            tell_session(&session, line);
            continue;
         }
         if (SDL_sscanf(what, "open %llu", &a) == 1)
            flag = "--open";
         else if (SDL_sscanf(what, "art %llu", &a) == 1)
            flag = "--art";
         else if (SDL_sscanf(what, "unlocks %llu", &a) == 1)
            flag = "--unlocks";
         else
            continue;
         if (a > 0xFFFFFFFFu)
            continue;
         /* A process of its own: it reads the library, which is the other
          * thread's here. */
         const char *args[] = { NULL, flag, NULL, NULL };
         char exe[1100], id[16];
         SDL_Process *p;

         own_exe(exe, sizeof(exe));
         SDL_snprintf(id, sizeof(id), "%u", (unsigned)a);
         args[0] = exe;
         args[2] = id;
         if ((p = platform_process(args, 0)))
            SDL_DestroyProcess(p);
      }
      /* No Steam, or none with buttons. */
      else if (SDL_GetTicks() - since < 2000)
         for (i = 0; i < LOOK_MS / 100 && !SDL_GetAtomicInt(stop); i++)
            SDL_Delay(100);
   }
   /* Its input ends: so does it. */
   SDL_DestroyProcess(session);
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
   steam_read_settings(config_dir);
   if (!steam_menus())
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

int background_open(const char *data_dir, const char *config_dir, const char *appid)
{
   Uint32 id = (Uint32)SDL_strtoul(appid, NULL, 10);
   char exe[1024], name[96];
   bool found = false;
   int i;

   if (!id || !library_init(data_dir))
      return 1;
   lan_read_once(config_dir);
   library_lock();
   for (i = 0; i < library_count() && !found; i++)
      found = lan_app(library_game(i)) == id
         && scan_program(library_game(i), exe, sizeof(exe), name, sizeof(name));
   library_unlock();
   library_discard();
   library_quit();
   return found && platform_open(exe) ? 0 : 1;
}

int background_art(const char *data_dir, const char *config_dir, const char *id)
{
   bool ok;

   ra_init(config_dir);
   steam_read_settings(config_dir);
   if (!steam_menus() || !library_init(data_dir))
      return 1;
   ok = steamui_art((Uint32)SDL_strtoul(id, NULL, 10), NULL);
   library_discard();
   library_quit();
   return ok ? 0 : 1;
}

int background_unlocks(const char *data_dir, const char *config_dir, const char *appid)
{
   Uint32 id = (Uint32)SDL_strtoul(appid, NULL, 10);
   bool ok;

   ra_init(config_dir);
   steam_read_settings(config_dir);
   if (!id || !steam_menus() || !ra_active() || !library_init(data_dir))
      return 1;
   lan_read_once(config_dir);
   ok = steamui_unlocks(id, NULL);
   library_discard();
   library_quit();
   return ok ? 0 : 1;
}

/* How a game is started, and what Steam is to know of it. */
struct start
{
   char exe[1024], args[1600], dir[1024];
   char name[256], icon[1024], launch_options[512];
   Uint32 appid;
   bool overlay, input;
};

static bool is_program(const char *path)
{
   size_t n = SDL_strlen(path);

   return n > 4 && SDL_strcasecmp(path + n - 4, ".exe") == 0;
}

/* A path from the command line, from where FuseBox was run: Steam runs it
 * elsewhere. */
static void full_path(const char *path, char *dst, size_t size)
{
   char *here = NULL;
   bool full = path[0] == '/';

#ifdef _WIN32
   full |= path[0] == '\\' || (path[0] && path[1] == ':');
#endif
   if (!full && platform_exists(path))
      here = SDL_GetCurrentDirectory();
   while (here && path[0] == '.' && path[1] == PATH_SEP[0])
      path += 2;
   SDL_snprintf(dst, size, "%s%s", here ? here : "", path);
   SDL_free(here);
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
            SDL_strlcpy(s->name, g->name, sizeof(s->name));
            SDL_strlcpy(s->icon, g->art[ART_ICON].path, sizeof(s->icon));
            SDL_strlcpy(s->launch_options, g->launch_options, sizeof(s->launch_options));
            s->appid = g->steam_appid;
            s->overlay = g->steam_overlay;
            s->input = g->steam_input;
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
      SDL_strlcpy(s->name, slash ? slash + 1 : path, sizeof(s->name));
      if ((slash = SDL_strrchr(s->name, '.')))
         *slash = '\0';
      s->overlay = s->input = true;
      slash = SDL_strrchr(s->dir, PATH_SEP[0]);
      if (slash)
         *slash = '\0';
      if (!slash || !platform_exists(s->dir))
         s->dir[0] = '\0';
      return true;
   }
   return false;
}

/* How long Steam may take to start a game (ms). */
#define START_MS 60000

/* Has Steam run the game, with its overlay and the rest; waits until it's over.
 * False if Steam can't now: it's started without. more: arguments after it. */
static bool through_steam(const char *config_dir, const char *path, const struct start *s,
      const char *more)
{
   char exe[1024], options[4096], asked[1100], played[1100], *text;
   SDL_PathInfo info;
   SDL_Time now = 0;
   Uint32 appid = 0, pid;
   Uint64 until;
   bool once = !s->appid || more[0], over = false;

   if (!steam_menus() || !steam_init() || !platform_steam_running(steam_install()))
      return false;
   /* One that comes back here didn't come from Steam as it looks: not again. */
   SDL_snprintf(asked, sizeof(asked), "%s" PATH_SEP "asked", config_dir);
   SDL_GetCurrentTime(&now);
   if (SDL_GetPathInfo(asked, &info) && now - info.modify_time < (SDL_Time)SDL_SECONDS_TO_NS(5)
         && (text = SDL_LoadFile(asked, NULL)))
   {
      bool same = library_same_path(text, path);
      SDL_free(text);
      if (same)
         return false;
   }
   SDL_SaveFile(asked, path, SDL_strlen(path));
   played_file(config_dir, played, sizeof(played));
   if (!once)
      appid = steamui_run(s->appid, NULL);
   /* Also where Steam doesn't have the shortcut any more. */
   if (!appid)
   {
      once = true;
      steam_once_target(path, s->launch_options, more, exe, sizeof(exe), options, sizeof(options));
      if (!(appid = steamui_once(s->name, exe, options, s->icon, s->overlay, s->input, NULL)))
         return false;
      lan_once(config_dir, appid, path, true);
   }
   for (until = SDL_GetTicks() + START_MS; !(pid = lan_game(config_dir, appid, "")) && !over
         && SDL_GetTicks() < until; SDL_Delay(250))
      /* Over before it was seen. */
      if (SDL_GetPathInfo(played, &info) && info.modify_time > now
            && (text = SDL_LoadFile(played, NULL)))
      {
         over = (Uint32)SDL_strtoul(text, NULL, 10) == appid;
         SDL_free(text);
      }
   /* Its achievements and the rest, for the background one to give. */
   if (pid && once)
      SDL_SaveFile(played, "0", 1);
   while (pid && lan_running(pid))
      SDL_Delay(500);
   if (once)
   {
      /* Steam still notes that it ended. */
      SDL_Delay(3000);
      if (steamui_unonce(appid, NULL))
         lan_once(config_dir, appid, "", false);
   }
   return true;
}

int background_game(const char *data_dir, const char *config_dir, char **argv)
{
   char path[1024], played[1100], error[1200], more[2048], ended[16];
   /* Older shortcuts say how. */
   bool old = SDL_strcmp(argv[1], "--run") == 0, once = SDL_strcmp(argv[1], "--once") == 0;
   const char *in_steam = SDL_getenv("SteamGameId");
   int code = 1, skip = old || once ? 3 : 2;
   struct start s;

   ra_init(config_dir);
   steam_read_settings(config_dir);
   SDL_zero(s);
   full_path(once ? argv[2] : argv[1], path, sizeof(path));
   if (old)
      SDL_strlcpy(s.exe, argv[2], sizeof(s.exe));
   else if (!find_start(data_dir, config_dir, path, &s, error, sizeof(error)))
   {
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FuseBox", error, NULL);
      return 1;
   }
   if (steam_menus())
      platform_start_background(true);
   /* Not by Steam: by it now. */
   if (!old && !once && !(in_steam && in_steam[0]))
   {
      platform_arguments(argv, skip, more, sizeof(more));
      if (through_steam(config_dir, path, &s, more))
         return 0;
   }
   history_playing(data_dir);
   lan_playing(config_dir, history_appid(), old ? "" : path, true);
#ifndef _WIN32
   if (steam_menus() && !inject_prepare())
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The game can't be put on a LAN");
#endif
   if (!platform_run_game(s.exe, s.args, s.dir, argv, skip, &code))
   {
      SDL_snprintf(error, sizeof(error), "Can't start this game:\n%s", s.exe);
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FuseBox", error, NULL);
   }
   lan_playing(config_dir, 0, "", false);
#ifndef _WIN32
   inject_over();
#endif
   played_file(config_dir, played, sizeof(played));
   SDL_snprintf(ended, sizeof(ended), "%u", (unsigned)history_appid());
   SDL_SaveFile(played, ended, SDL_strlen(ended));
   return code;
}
