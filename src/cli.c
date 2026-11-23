/* fusebox-cli: the library for other apps and scripts.
 *
 *   fusebox-cli art <game name or id> <cover|wide|hero|logo|icon> [--no-fetch]
 *      Prints the image's path. A game or art FuseBox doesn't have yet is
 *      looked up on SteamGridDB (its most popular art) and saved, so the
 *      next ask is instant and the window shows it. Exit 1: none found.
 *   fusebox-cli list           id, name and each art path, tab separated
 *   fusebox-cli library        library.json's path
 *   fusebox-cli key <api key>  saves the SteamGridDB API key
 *   fusebox-cli scan           which games a scan finds and why (saves nothing)
 */
#include "disk.h"
#include "http.h"
#include "library.h"
#include "roms.h"
#include "scan.h"
#include "sgdb.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>

static void usage(void)
{
   fprintf(stderr,
         "usage: fusebox-cli art <game name or id> <cover|wide|hero|logo|icon> [--no-fetch]\n"
         "       fusebox-cli list\n"
         "       fusebox-cli library\n"
         "       fusebox-cli key <SteamGridDB API key>\n"
         "       fusebox-cli scan\n");
}

/* A dry run of the window's scan, to see what it finds and why. */
static void scan(void)
{
   Uint64 start = SDL_GetTicks();
   int launchers, disks, roms, i;

   launchers = scan_launchers();
   disks = scan_disks(1);
   roms = scan_roms();
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
   if (!sgdb_has_key())
   {
      fprintf(stderr, "no SteamGridDB API key: fusebox-cli key <key>\n");
      return 1;
   }

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

   if (!data || !library_init(data))
   {
      fprintf(stderr, "can't open FuseBox's folder\n");
      return 1;
   }
   SDL_snprintf(config, sizeof(config), "%sconfig", data);
   sgdb_init(config);

   if (argc >= 4 && SDL_strcmp(argv[1], "art") == 0)
      code = art(argv[2], argv[3], !(argc >= 5 && SDL_strcmp(argv[4], "--no-fetch") == 0));
   else if (argc == 2 && SDL_strcmp(argv[1], "list") == 0)
      list();
   else if (argc == 2 && SDL_strcmp(argv[1], "library") == 0)
      printf("%slibrary.json\n", data);
   else if (argc == 3 && SDL_strcmp(argv[1], "key") == 0)
      sgdb_set_key(argv[2]);
   else if (argc == 2 && SDL_strcmp(argv[1], "scan") == 0)
      scan();
   else
   {
      usage();
      code = 2;
   }

   library_quit();
   http_quit();
   return code;
}
