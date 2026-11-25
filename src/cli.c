/* fusebox-cli: the library for other apps and scripts.
 *
 *   fusebox-cli art <game name or id> <cover|wide|hero|logo|icon> [--no-fetch]
 *      Prints the image's path. A game or art FuseBox doesn't have yet is
 *      looked up on SteamGridDB (its most popular art) and saved, so the
 *      next ask is instant and the window shows it. Exit 1: none found.
 *   fusebox-cli choices <game name or id> <type> [--animated]
 *      SteamGridDB's art for it, most popular first: id, size, kind and
 *      address, tab separated.
 *   fusebox-cli list           id, name and each art path, tab separated
 *   fusebox-cli library        library.json's path
 *   fusebox-cli version        FuseBox's version
 *   fusebox-cli scan           which games a scan finds and why (saves nothing)
 *   fusebox-cli steam          whether and why Steam offers to invite friends
 */
#include "disk.h"
#include "http.h"
#include "lan.h"
#include "library.h"
#include "platform.h"
#include "ra.h"
#include "roms.h"
#include "scan.h"
#include "sgdb.h"
#include "steam.h"
#include "steamui.h"
#include "version.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>

static void usage(void)
{
   fprintf(stderr,
         "usage: fusebox-cli art <game name or id> <cover|wide|hero|logo|icon> [--no-fetch]\n"
         "       fusebox-cli choices <game name or id> <type> [--animated]\n"
         "       fusebox-cli list\n"
         "       fusebox-cli library\n"
         "       fusebox-cli version\n"
         "       fusebox-cli scan\n"
         "       fusebox-cli steam\n");
}

/* A dry run of the window's scan, to see what it finds and why. */
static void scan(void)
{
   Uint64 start = SDL_GetTicks();
   int launchers, disks, roms, i;

   library_scan_begin();
   launchers = scan_launchers();
   disk_keep_added();
   disks = scan_disks(1);
   roms = scan_roms();
   library_scan_end(false);
   library_lock();
   printf("emulators: %s\n", roms_emulators()[0] ? roms_emulators() : "none");
   for (i = 0; i < roms_folder_count(); i++)
      printf("ROM folder: %s\n", roms_folder(i)->path);
   for (i = 0; i < library_count(); i++)
   {
      const struct game *g = library_game(i);
      if (!g->installed || !g->rom[0])
         continue;
      printf("%s: %s: ", g->source, g->name);
      if (g->no_emulator)
         printf("no emulator (%s)\n", g->rom);
      else
         printf("\"%s\" %s\n", g->exe, g->args);
   }
   if (roms_unknown())
      printf("%d files of no known system\n", roms_unknown());
   library_unlock();
   printf("%d from launchers, %d on disk, %d ROMs, in %llu ms\n", launchers, disks, roms,
         (unsigned long long)(SDL_GetTicks() - start));
   library_discard();
}

/* For a computer that can't be looked at. While a game runs. */
static void steam(const char *config)
{
   static char text[16384];
   char dir[1024];
   int i;

   ra_init(config);
   steam_read_settings(config);
   steam_init();
   steam_config(dir, sizeof(dir));
   printf("FuseBox %s\n", FUSEBOX_VERSION);
   printf("Steam: %s, %s\n", dir[0] ? dir : "not found",
         steam_install() && platform_steam_running(steam_install()) ? "runs" : "doesn't run");
   printf("content in Steam's menus: %s, auto start: %s\n", steam_menus() ? "on" : "off",
         steam_autostart() ? "on" : "off");
   platform_fuseboxes(text, sizeof(text));
   printf("running FuseBoxes:\n%s", text[0] ? text : "none\n");
   printf("a game runs: %s\n", lan_any(config) ? "yes" : "no");
   lan_read_once(config);
   library_lock();
   for (i = 0; i < library_count(); i++)
   {
      const struct game *g = library_game(i);

      if (lan_app(g) && SDL_strcmp(g->source, "Steam") != 0)
         printf("%u: %s (the library says %u)\n", (unsigned)lan_app(g), g->name,
               (unsigned)g->steam_appid);
   }
   library_unlock();
   steamui_state(text, sizeof(text), NULL);
   printf("%s\n", text);
}

static bool has_key(void)
{
   if (sgdb_has_key())
      return true;
   fprintf(stderr, "no SteamGridDB API key: add one in FuseBox, in Settings\n");
   return false;
}

static int art(const char *name, const char *type_name, bool fetch)
{
   enum art_type type = art_from_name(type_name);
   char path[1024];
   int index;

   if (type == ART_TYPES)
   {
      fprintf(stderr, "unknown art type: %s\n", type_name);
      return 2;
   }
   library_lock();
   index = library_match(name);
   path[0] = '\0';
   if (index >= 0)
      SDL_strlcpy(path, library_game(index)->art[type].path, sizeof(path));
   library_unlock();
   if (path[0] || !fetch)
   {
      if (!path[0])
         return 1;
      printf("%s\n", path);
      return 0;
   }
   if (!has_key())
      return 1;

   /* One FuseBox doesn't know: kept as a lookup, so its art is saved and
    * can be changed in the window like any other. */
   if (index < 0)
   {
      struct game g;
      char norm[200];

      SDL_zero(g);
      library_normalize(name, norm, sizeof(norm));
      SDL_snprintf(g.id, sizeof(g.id), "lookup:%s", norm[0] ? norm : name);
      SDL_strlcpy(g.name, name, sizeof(g.name));
      SDL_strlcpy(g.source, "Lookup", sizeof(g.source));
      index = library_add(&g);
   }
   sgdb_fill(index, true, type, NULL);
   library_save();

   library_lock();
   SDL_strlcpy(path, library_game(index)->art[type].path, sizeof(path));
   library_unlock();
   if (!path[0])
      return 1;
   printf("%s\n", path);
   return 0;
}

/* The SteamGridDB game a name or id is of, 0 if none. */
static unsigned sgdb_game(const char *name)
{
   struct sgdb_game match;
   char search[256];
   unsigned id = 0;
   int index;

   SDL_strlcpy(search, name, sizeof(search));
   library_lock();
   if ((index = library_match(name)) >= 0)
   {
      id = library_game(index)->sgdb_id;
      SDL_strlcpy(search, library_game(index)->name, sizeof(search));
   }
   library_unlock();
   if (!id && sgdb_search(search, &match, 1, NULL, NULL) > 0)
      id = match.id;
   return id;
}

static int choices(const char *name, const char *type_name, bool animated)
{
   enum art_type type = art_from_name(type_name);
   struct sgdb_art *arts = NULL;
   unsigned id;
   int i, n;

   if (type == ART_TYPES)
   {
      fprintf(stderr, "unknown art type: %s\n", type_name);
      return 2;
   }
   if (!has_key())
      return 1;
   if (!(id = sgdb_game(name)))
   {
      fprintf(stderr, "not found on SteamGridDB: %s\n", name);
      return 1;
   }
   if ((n = sgdb_list(id, type, animated, &arts, NULL)) < 0)
      fprintf(stderr, "can't ask SteamGridDB\n");
   for (i = 0; i < n; i++)
      printf("%u\t%dx%d\t%s\t%s\n", arts[i].id, arts[i].width, arts[i].height, arts[i].ext,
            arts[i].url);
   SDL_free(arts);
   return n > 0 ? 0 : 1;
}

static void list(void)
{
   int i, t;

   library_lock();
   for (i = 0; i < library_count(); i++)
   {
      const struct game *g = library_game(i);
      printf("%s\t%s", g->id, g->name);
      for (t = 0; t < ART_TYPES; t++)
         printf("\t%s", g->art[t].path);
      printf("\n");
   }
   library_unlock();
}

int main(int argc, char **argv)
{
   /* Everything is next to the exes (fusebox-cli sits beside fusebox). */
   const char *data = SDL_GetBasePath();
   char config[1100];
   int code = 0;

   if (argc == 2 && SDL_strcmp(argv[1], "version") == 0)
   {
      printf("%s\n", FUSEBOX_VERSION);
      return 0;
   }
   if (!data || !library_init(data))
   {
      fprintf(stderr, "can't open FuseBox's folder\n");
      return 1;
   }
   SDL_snprintf(config, sizeof(config), "%sconfig", data);
   sgdb_init(config);
   roms_init(config);

   if (argc >= 4 && SDL_strcmp(argv[1], "art") == 0)
      code = art(argv[2], argv[3], !(argc >= 5 && SDL_strcmp(argv[4], "--no-fetch") == 0));
   else if ((argc == 4 || (argc == 5 && SDL_strcmp(argv[4], "--animated") == 0))
         && SDL_strcmp(argv[1], "choices") == 0)
      code = choices(argv[2], argv[3], argc == 5);
   else if (argc == 2 && SDL_strcmp(argv[1], "list") == 0)
      list();
   else if (argc == 2 && SDL_strcmp(argv[1], "library") == 0)
      printf("%slibrary.json\n", data);
   else if (argc == 2 && SDL_strcmp(argv[1], "scan") == 0)
      scan();
   else if (argc == 2 && SDL_strcmp(argv[1], "steam") == 0)
   {
      steam(config);
      library_discard();
   }
   else
   {
      usage();
      code = 2;
   }

   library_quit();
   http_quit();
   return code;
}
