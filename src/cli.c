/* fusebox-cli: the library for other apps and scripts.
 *
 *   fusebox-cli art <game name or id> <cover|wide|hero|logo|icon> [--no-fetch]
 *      Prints the image's path. A game or art FuseBox doesn't have yet is
 *      looked up on SteamGridDB (its most popular art) and saved, so the
 *      next ask is instant and the window shows it. Exit 1: none found.
 *   fusebox-cli setart <game name or id> <type> <image>
 *      Makes it the game's art, as if picked in the window, and prints its
 *      path: a file, an address, or an id from listart. Steam gets
 *      it through the window. Exit 1: not done.
 *   fusebox-cli listart <game name or id> <type> [--animated]
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
#include <string.h>

static void usage(void)
{
   fprintf(stderr,
         "usage: fusebox-cli art <game name or id> <cover|wide|hero|logo|icon> [--no-fetch]\n"
         "       fusebox-cli setart <game name or id> <type> <image file, address or id from listart>\n"
         "       fusebox-cli listart <game name or id> <type> [--animated]\n"
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

static int list_art(const char *name, const char *type_name, bool animated)
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

/* Which kind of image the file is, by how it starts; NULL for none of art's. */
static const char *image_ext(const char *path)
{
   static const Uint8 png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
   static const Uint8 jpg[] = { 0xff, 0xd8, 0xff };
   SDL_IOStream *io = SDL_IOFromFile(path, "rb");
   Uint8 head[12];
   size_t n = io ? SDL_ReadIO(io, head, sizeof(head)) : 0;

   if (io)
      SDL_CloseIO(io);
   if (n < sizeof(head))
      return NULL;
   if (SDL_memcmp(head, png, sizeof(png)) == 0)
      return "png";
   if (SDL_memcmp(head, jpg, sizeof(jpg)) == 0)
      return "jpg";
   return SDL_memcmp(head, "RIFF", 4) == 0 && SDL_memcmp(head + 8, "WEBP", 4) == 0 ? "webp" : NULL;
}

/* The address of SteamGridDB's art for the game with that id. */
static bool find_art(const char *name, enum art_type type, unsigned id, char *url, size_t size)
{
   unsigned game = sgdb_game(name);
   bool found = false;
   int i, n, kind;

   for (kind = 0; kind < 2 && game && !found; kind++)
   {
      struct sgdb_art *arts = NULL;

      n = sgdb_list(game, type, kind == 1, &arts, NULL);
      for (i = 0; i < n && !found; i++)
         if ((found = arts[i].id == id))
            SDL_strlcpy(url, arts[i].url, size);
      SDL_free(arts);
   }
   return found;
}

static int set_art(const char *data, const char *name, const char *type_name, const char *image)
{
   enum art_type type = art_from_name(type_name);
   char id[256], tmp[1100], url[1024];
   const char *ext = NULL;
   unsigned chosen = 0;
   bool ok, changed;
   int index;

   if (type == ART_TYPES)
   {
      fprintf(stderr, "unknown art type: %s\n", type_name);
      return 2;
   }
   library_lock();
   if ((index = library_match(name)) >= 0)
      SDL_strlcpy(id, library_game(index)->id, sizeof(id));
   library_unlock();
   if (index < 0)
   {
      fprintf(stderr, "no such game: %s\n", name);
      return 1;
   }
   SDL_strlcpy(url, image, sizeof(url));
   if (image[0] && !image[strspn(image, "0123456789")] && !platform_exists(image))
   {
      chosen = (unsigned)SDL_strtoul(image, NULL, 10);
      if (!has_key())
         return 1;
      if (!find_art(id, type, chosen, url, sizeof(url)))
      {
         fprintf(stderr, "no such %s on SteamGridDB for this game: %s\n", art_names[type], image);
         return 1;
      }
   }
   library_tmp_path(index, type, tmp, sizeof(tmp));
   if (SDL_strncasecmp(url, "https://", 8) == 0 || SDL_strncasecmp(url, "http://", 7) == 0)
      ok = sgdb_download(url, tmp, NULL);
   else
      ok = SDL_CopyFile(url, tmp);
   if (!ok)
      fprintf(stderr, "can't get %s\n", url);
   else if (!(ext = image_ext(tmp)))
      fprintf(stderr, "not a PNG, JPEG or WebP image: %s\n", image);
   /* Windows and Steam show no others. */
   else if (type == ART_ICON && SDL_strcmp(ext, "png") != 0)
   {
      fprintf(stderr, "an icon has to be a PNG image: %s\n", image);
      ext = NULL;
   }
   /* What the window wrote while it came in is to stay: read again. */
   if (ext)
   {
      library_quit();
      if (!library_init(data) || (index = library_find(id)) < 0)
      {
         fprintf(stderr, "can't read the library\n");
         ext = NULL;
      }
   }
   if (!ext)
   {
      SDL_RemovePath(tmp);
      return 1;
   }
   library_lock();
   changed = !chosen || !library_game(index)->art[type].path[0]
      || library_game(index)->art[type].sgdb_id != chosen;
   /* As in the window: FuseBox leaves it as it is from now on. */
   if ((ok = library_set_art(index, type, tmp, ext, chosen)))
   {
      library_set_picked(index, type, changed);
      SDL_strlcpy(url, library_game(index)->art[type].path, sizeof(url));
   }
   library_unlock();
   if (!ok || !library_save())
   {
      fprintf(stderr, "can't save it\n");
      return 1;
   }
   printf("%s\n", url);
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
   else if (argc == 5 && SDL_strcmp(argv[1], "setart") == 0)
      code = set_art(data, argv[2], argv[3], argv[4]);
   else if ((argc == 4 || (argc == 5 && SDL_strcmp(argv[4], "--animated") == 0))
         && SDL_strcmp(argv[1], "listart") == 0)
      code = list_art(argv[2], argv[3], argc == 5);
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
