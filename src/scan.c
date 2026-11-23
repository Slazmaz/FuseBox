#include "scan.h"
#include "disk.h"
#include "json.h"
#include "library.h"
#include "platform.h"

#include <SDL3/SDL.h>

static int found;

/* installed: a file the game can't be without (its exe, or its folder).
 * Launchers' lists outlive uninstalls (Battle.net's especially), so only
 * what's really on disk counts. */
static void add(const char *id, const char *name, const char *source, const char *exe,
      const char *start_dir, const char *args, const char *installed)
{
   struct game g;

   if (!name[0] || !exe[0] || !installed[0] || !platform_exists(installed))
      return;
   SDL_zero(g);
   SDL_strlcpy(g.id, id, sizeof(g.id));
   SDL_strlcpy(g.name, name, sizeof(g.name));
   SDL_strlcpy(g.source, source, sizeof(g.source));
   SDL_strlcpy(g.exe, exe, sizeof(g.exe));
   SDL_strlcpy(g.start_dir, start_dir, sizeof(g.start_dir));
   SDL_strlcpy(g.args, args, sizeof(g.args));
   /* What proves it installed is its own exe, when it's one: its icon. */
   if (SDL_strlen(installed) > 4 && SDL_strcasecmp(installed + SDL_strlen(installed) - 4, ".exe") == 0)
      SDL_strlcpy(g.game_exe, installed, sizeof(g.game_exe));
   g.installed = true;
   /* Steam's own defaults for a new shortcut. */
   g.steam_overlay = g.steam_input = true;
   library_add(&g);
   found++;
}

/* Windows' variables are spelled "ProgramData", "APPDATA"...: SDL_getenv
 * matches case exactly, the C library's getenv (SDL_getenv_unsafe)
 * doesn't, as on Windows itself. */

static struct json *load_json(const char *path)
{
   char *text = SDL_LoadFile(path, NULL);
   struct json *root = json_parse(text);

   SDL_free(text);
   return root;
}

#ifdef _WIN32

/* Through the launcher (its URL), as Epic's own shortcuts do: games
 * started directly miss Epic's login and some won't run. */
static void scan_epic(void)
{
   const char *programdata = SDL_getenv_unsafe("PROGRAMDATA");
   char dir[1024], launcher[1024], command[1100];
   char **files;
   int count = 0, i;

   if (!programdata)
      return;
   SDL_snprintf(dir, sizeof(dir), "%s\\Epic\\EpicGamesLauncher\\Data\\Manifests", programdata);
   if (!(files = SDL_GlobDirectory(dir, "*.item", 0, &count)))
      return;

   /* The launcher, as its URL handler names it: "C:\...\EpicGamesLauncher.exe" %1 */
   launcher[0] = '\0';
   if (platform_reg_string("HKLM", "SOFTWARE\\Classes\\com.epicgames.launcher\\shell\\open\\command",
            NULL, command, sizeof(command)) && command[0] == '"')
   {
      char *end = SDL_strchr(command + 1, '"');
      if (end)
         SDL_strlcpy(launcher, command + 1, SDL_min(sizeof(launcher), (size_t)(end - command)));
   }
   if (!launcher[0])
      SDL_strlcpy(launcher,
            "C:\\Program Files (x86)\\Epic Games\\Launcher\\Portal\\Binaries\\Win32\\EpicGamesLauncher.exe",
            sizeof(launcher));

   for (i = 0; i < count; i++)
   {
      char path[1100], id[300], args[1024];
      struct json *m, *cats, *c;
      const char *app, *main_app;
      bool game = true;

      SDL_snprintf(path, sizeof(path), "%s\\%s", dir, files[i]);
      if (!(m = load_json(path)))
         continue;
      app = json_str(m, "AppName", "");
      main_app = json_str(m, "MainGameAppName", app);
      /* Plugins, apps and DLC aren't games of their own. */
      if ((cats = json_get(m, "AppCategories")) && cats->type == JSON_ARRAY)
      {
         game = false;
         for (c = cats->child; c; c = c->next)
            if (c->type == JSON_STRING && SDL_strcmp(c->str, "games") == 0)
               game = true;
      }
      if (game && app[0] && SDL_strcmp(app, main_app) == 0
            && !json_bool(m, "bIsIncompleteInstall", false))
      {
         SDL_snprintf(id, sizeof(id), "epic:%s", app);
         SDL_snprintf(args, sizeof(args),
               "com.epicgames.launcher://apps/%s%%3A%s%%3A%s?action=launch&silent=true",
               json_str(m, "CatalogNamespace", ""), json_str(m, "CatalogItemId", ""), app);
         char exe[1100];
         SDL_snprintf(exe, sizeof(exe), "%s\\%s", json_str(m, "InstallLocation", ""),
               json_str(m, "LaunchExecutable", ""));
         add(id, json_str(m, "DisplayName", app), "Epic", launcher,
               json_str(m, "InstallLocation", ""), args, exe);
      }
      json_free(m);
   }
   SDL_free(files);
}

#define GOG_KEY "SOFTWARE\\WOW6432Node\\GOG.com\\Games"

/* GOG's games are DRM-free: started directly, no Galaxy needed. */
static void on_gog_game(const char *subkey, void *user)
{
   char key[512], name[256], exe[1024], dir[1024], args[1024], depends[64], id[300];

   SDL_snprintf(key, sizeof(key), GOG_KEY "\\%s", subkey);
   /* DLC depend on their game. */
   if (platform_reg_string("HKLM", key, "dependsOn", depends, sizeof(depends)))
      return;
   if (!platform_reg_string("HKLM", key, "gameName", name, sizeof(name))
         || !platform_reg_string("HKLM", key, "exe", exe, sizeof(exe)))
      return;
   if (!platform_reg_string("HKLM", key, "workingDir", dir, sizeof(dir)))
      platform_reg_string("HKLM", key, "path", dir, sizeof(dir));
   platform_reg_string("HKLM", key, "launchParam", args, sizeof(args));
   SDL_snprintf(id, sizeof(id), "gog:%s", subkey);
   add(id, name, "GOG", exe, dir, args, exe);
}

static void scan_gog(void)
{
   platform_reg_subkeys("HKLM", GOG_KEY, on_gog_game, NULL);
}

#define UNINSTALL_KEY "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"

/* Battle.net's launch codes for its uninstaller's ids; others start by
 * id (launch_uid), which Battle.net also takes. */
static const char *battlenet_code(const char *uid)
{
   static const char *const codes[][2] = {
      { "wow", "WoW" }, { "wow_classic", "WoWC" },
      { "fenris", "Fen" }, { "hs_beta", "WTCG" }, { "heroes", "Hero" },
      { "prometheus", "Pro" }, { "diablo3", "D3" }, { "s2", "S2" }, { "s1", "S1" },
      { "w3", "W3" }, { "anbs", "ANBS" }, { "osi", "OSI" }, { "rtro", "RTRO" },
   };
   size_t i;

   for (i = 0; i < SDL_arraysize(codes); i++)
      if (SDL_strcmp(uid, codes[i][0]) == 0)
         return codes[i][1];
   return NULL;
}

/* Names for Battle.net's ids, where Windows' uninstall list has none
 * (newer games like WoW Forever get no entry there). */
static const char *battlenet_name(const char *uid)
{
   static const char *const names[][2] = {
      { "wow", "World of Warcraft" }, { "wow_classic", "World of Warcraft Classic" },
      { "wow_classic_era", "World of Warcraft Classic Era" },
      { "wowf", "World of Warcraft Forever" }, { "fenris", "Diablo IV" },
      { "hs_beta", "Hearthstone" }, { "heroes", "Heroes of the Storm" },
      { "prometheus", "Overwatch 2" }, { "diablo3", "Diablo III" }, { "s2", "StarCraft II" },
      { "s1", "StarCraft: Remastered" }, { "w3", "Warcraft III: Reforged" },
      { "osi", "Diablo II: Resurrected" }, { "rtro", "Blizzard Arcade Collection" },
   };
   size_t i;

   for (i = 0; i < SDL_arraysize(names); i++)
      if (SDL_strcmp(uid, names[i][0]) == 0)
         return names[i][1];
   return NULL;
}

#define BN_NAMES 64

struct battlenet
{
   char exe[1024];
   /* Display names from the uninstall list, by uid. */
   struct
   {
      char uid[64], name[256];
   } names[BN_NAMES];
   int name_count;
};

/* Blizzard's uninstall entries: "... Blizzard Uninstaller.exe" --uid=<id>.
 * Only their names are used: the list outlives uninstalls. */
static void on_uninstall_entry(const char *subkey, void *user)
{
   struct battlenet *bn = user;
   char key[512], uninstall[1024];
   const char *p;
   size_t n = 0;

   if (bn->name_count >= BN_NAMES)
      return;
   SDL_snprintf(key, sizeof(key), UNINSTALL_KEY "\\%s", subkey);
   if (!platform_reg_string("HKLM", key, "UninstallString", uninstall, sizeof(uninstall))
         || !SDL_strstr(uninstall, "Blizzard Uninstaller.exe")
         || !(p = SDL_strstr(uninstall, "--uid=")))
      return;
   for (p += 6; *p && *p != ' ' && *p != '"' && n + 1 < sizeof(bn->names[0].uid); p++)
      bn->names[bn->name_count].uid[n++] = *p;
   bn->names[bn->name_count].uid[n] = '\0';
   if (platform_reg_string("HKLM", key, "DisplayName", bn->names[bn->name_count].name,
            sizeof(bn->names[0].name)))
      bn->name_count++;
}

/* product.db is protobuf: just enough of it to walk fields. */
struct pb
{
   const Uint8 *p, *end;
};

static bool pb_varint(struct pb *b, Uint64 *v)
{
   int shift = 0;

   *v = 0;
   while (b->p < b->end && shift < 64)
   {
      Uint8 c = *b->p++;
      *v |= (Uint64)(c & 0x7F) << shift;
      if (!(c & 0x80))
         return true;
      shift += 7;
   }
   return false;
}

/* The next field: its number, and for a length-delimited one its bytes
 * in *sub. False at the end or on garbage. */
static bool pb_next(struct pb *b, Uint32 *field, struct pb *sub)
{
   Uint64 key, v;

   while (b->p < b->end)
   {
      if (!pb_varint(b, &key))
         return false;
      *field = (Uint32)(key >> 3);
      switch (key & 7)
      {
         case 0:
            if (!pb_varint(b, &v))
               return false;
            break;
         case 1:
            if (b->end - b->p < 8)
               return false;
            b->p += 8;
            break;
         case 5:
            if (b->end - b->p < 4)
               return false;
            b->p += 4;
            break;
         case 2:
            if (!pb_varint(b, &v) || v > (Uint64)(b->end - b->p))
               return false;
            sub->p = b->p;
            sub->end = b->p + v;
            b->p += v;
            return true;
         default:
            return false;
      }
   }
   return false;
}

static void pb_string(const struct pb *s, char *dst, size_t size)
{
   SDL_strlcpy(dst, (const char *)s->p, SDL_min(size, (size_t)(s->end - s->p) + 1));
}

struct flavor
{
   const char *uid;
   char dir[1024];
};

/* A WoW-style install holds several games, one per _<flavor>_ folder,
 * each naming its id in .flavor.info. */
static SDL_EnumerationResult SDLCALL on_flavor_dir(void *user, const char *dir, const char *name)
{
   struct flavor *f = user;
   char path[1100], *text, *line, *p;

   if (name[0] != '_')
      return SDL_ENUM_CONTINUE;
   SDL_snprintf(path, sizeof(path), "%s%s\\.flavor.info", dir, name);
   if (!(text = SDL_LoadFile(path, NULL)))
      return SDL_ENUM_CONTINUE;
   /* "Product Flavor!STRING:0" then the id. */
   if ((line = SDL_strchr(text, '\n')))
   {
      line++;
      for (p = line; *p && *p != '\r' && *p != '\n'; p++)
         ;
      *p = '\0';
      if (SDL_strcmp(line, f->uid) == 0)
         SDL_snprintf(f->dir, sizeof(f->dir), "%s%s", dir, name);
   }
   SDL_free(text);
   return f->dir[0] ? SDL_ENUM_SUCCESS : SDL_ENUM_CONTINUE;
}

static void battlenet_game(const struct battlenet *bn, const char *uid, char *install)
{
   char id[128], args[160], name[256], check[1100];
   struct flavor f;
   char *p;
   int i;

   /* Battle.net itself, its updater and dev kit, and test realms. */
   if (SDL_strcmp(uid, "battle.net") == 0 || SDL_strcmp(uid, "agent") == 0
         || SDL_strcmp(uid, "bdk") == 0
         || SDL_strstr(uid, "_ptr") || SDL_strstr(uid, "wowt") == uid
         || (SDL_strstr(uid, "_beta") && SDL_strcmp(uid, "hs_beta") != 0))
      return;
   for (p = install; *p; p++)
      if (*p == '/')
         *p = '\\';

   /* WoW's editions share a folder: the edition's own is where it's
    * installed, its exe what proves it. */
   SDL_zero(f);
   f.uid = uid;
   SDL_EnumerateDirectory(install, on_flavor_dir, &f);
   SDL_strlcpy(check, install, sizeof(check));
   if (f.dir[0])
   {
      int count = 0;
      char **exes = SDL_GlobDirectory(f.dir, "Wow*.exe", SDL_GLOB_CASEINSENSITIVE, &count);
      if (!exes || count < 1)
      {
         SDL_free(exes);
         return;
      }
      SDL_snprintf(check, sizeof(check), "%s\\%s", f.dir, exes[0]);
      SDL_free(exes);
   }
   /* Others: the folder's own exe (for its icon), if one stands out;
    * else the folder proves it installed. */
   else
   {
      char exe[1100];
      if (disk_main_exe(install, exe, sizeof(exe)))
         SDL_strlcpy(check, exe, sizeof(check));
   }

   SDL_strlcpy(name, battlenet_name(uid) ? battlenet_name(uid) : uid, sizeof(name));
   for (i = 0; i < bn->name_count; i++)
      if (SDL_strcmp(bn->names[i].uid, uid) == 0)
         SDL_strlcpy(name, bn->names[i].name, sizeof(name));
   if (battlenet_code(uid))
      SDL_snprintf(args, sizeof(args), "--exec=\"launch %s\"", battlenet_code(uid));
   else
      SDL_snprintf(args, sizeof(args), "--exec=\"launch_uid %s\"", uid);
   SDL_snprintf(id, sizeof(id), "battlenet:%s", uid);
   add(id, name, "Battle.net", bn->exe, f.dir[0] ? f.dir : install, args, check);
}

/* Battle.net's own list of what's installed (product.db: installs, each
 * with its id and folder). Games start through Battle.net (its login,
 * updates). */
static void scan_battlenet(void)
{
   const char *programdata = SDL_getenv_unsafe("PROGRAMDATA");
   struct battlenet *bn;
   char path[1024];
   size_t size = 0;
   Uint8 *data;
   struct pb db, install;
   Uint32 field;

   if (!programdata)
      return;
   SDL_snprintf(path, sizeof(path), "%s\\Battle.net\\Agent\\product.db", programdata);
   /* Battle.net replaces the file now and then: a moment without it
    * isn't every game gone. */
   for (int tries = 0; !(data = SDL_LoadFile(path, &size)) && tries < 10; tries++)
      SDL_Delay(200);
   if (!data)
      return;
   if (!(bn = SDL_calloc(1, sizeof(*bn))))
   {
      SDL_free(data);
      return;
   }
   if (platform_reg_string("HKLM", UNINSTALL_KEY "\\Battle.net", "InstallLocation", path,
            sizeof(path)))
      SDL_snprintf(bn->exe, sizeof(bn->exe), "%s\\Battle.net.exe", path);
   else
      SDL_strlcpy(bn->exe, "C:\\Program Files (x86)\\Battle.net\\Battle.net.exe", sizeof(bn->exe));
   platform_reg_subkeys("HKLM", UNINSTALL_KEY, on_uninstall_entry, bn);

   /* Database: 1 = install { 1 = uid, 3 = settings { 1 = install path } }. */
   db.p = data;
   db.end = data + size;
   while (pb_next(&db, &field, &install))
   {
      char uid[64] = "", dir[1024] = "";
      struct pb sub, settings;
      Uint32 f2, f3;

      if (field != 1)
         continue;
      while (pb_next(&install, &f2, &sub))
      {
         if (f2 == 1)
            pb_string(&sub, uid, sizeof(uid));
         else if (f2 == 3)
            while (pb_next(&sub, &f3, &settings))
               if (f3 == 1)
                  pb_string(&settings, dir, sizeof(dir));
      }
      if (uid[0] && dir[0])
         battlenet_game(bn, uid, dir);
   }
   SDL_free(bn);
   SDL_free(data);
}

#endif

/* Heroic's games start through its URL, so it sets up Wine, the login
 * and cloud saves as when started from Heroic. */
static void heroic_add(const char *runner, const char *app, const char *title,
      const char *install)
{
   char id[300], args[600];

#ifdef _WIN32
   const char *exe = "C:\\Windows\\explorer.exe";
#else
   /* Steam wants a path, not a command to look up. */
   const char *exe = "/usr/bin/xdg-open";
#endif

   SDL_snprintf(id, sizeof(id), "heroic-%s:%s", runner, app);
   SDL_snprintf(args, sizeof(args), "heroic://launch?appName=%s&runner=%s", app, runner);
   add(id, title, "Heroic", exe, install, args, install);
}

/* The title of a GOG game Heroic has installed: from its library cache,
 * else the install folder's name. */
static void gog_title(const struct json *library, const char *app, const char *install,
      char *dst, size_t size)
{
   const struct json *games = json_get(library, "games"), *g;
   const char *slash;

   for (g = games ? games->child : NULL; g; g = g->next)
      if (SDL_strcmp(json_str(g, "app_name", ""), app) == 0)
      {
         SDL_strlcpy(dst, json_str(g, "title", app), size);
         return;
      }
   slash = SDL_strrchr(install, '/');
   if (!slash)
      slash = SDL_strrchr(install, '\\');
   SDL_strlcpy(dst, slash ? slash + 1 : app, size);
}

static void scan_heroic_dir(const char *base)
{
   char path[1100];
   struct json *root, *g, *library;

   SDL_snprintf(path, sizeof(path), "%s%slegendaryConfig%slegendary%sinstalled.json",
         base, PATH_SEP, PATH_SEP, PATH_SEP);
   if ((root = load_json(path)))
   {
      for (g = root->child; g; g = g->next)
         if (!json_bool(g, "is_dlc", false))
            heroic_add("legendary", json_str(g, "app_name", g->key ? g->key : ""),
                  json_str(g, "title", ""), json_str(g, "install_path", ""));
      json_free(root);
   }

   SDL_snprintf(path, sizeof(path), "%s%sstore_cache%sgog_library.json", base, PATH_SEP, PATH_SEP);
   if (!(library = load_json(path)))
   {
      SDL_snprintf(path, sizeof(path), "%s%sgog_store%slibrary.json", base, PATH_SEP, PATH_SEP);
      library = load_json(path);
   }
   SDL_snprintf(path, sizeof(path), "%s%sgog_store%sinstalled.json", base, PATH_SEP, PATH_SEP);
   if ((root = load_json(path)))
   {
      struct json *list = json_get(root, "installed");
      for (g = list ? list->child : NULL; g; g = g->next)
      {
         char title[256];
         const char *app = json_str(g, "appName", "");
         const char *install = json_str(g, "install_path", "");
         if (!app[0] || json_bool(g, "is_dlc", false))
            continue;
         gog_title(library, app, install, title, sizeof(title));
         heroic_add("gog", app, title, install);
      }
      json_free(root);
   }
   json_free(library);
}

static void scan_heroic(void)
{
   char base[1024];

#ifdef _WIN32
   const char *appdata = SDL_getenv_unsafe("APPDATA");
   if (!appdata)
      return;
   SDL_snprintf(base, sizeof(base), "%s\\heroic", appdata);
   scan_heroic_dir(base);
#else
   char home[1024];
   const char *xdg = SDL_getenv("XDG_CONFIG_HOME");

   platform_home(home, sizeof(home));
   if (xdg && xdg[0])
      SDL_snprintf(base, sizeof(base), "%s/heroic", xdg);
   else
      SDL_snprintf(base, sizeof(base), "%s/.config/heroic", home);
   scan_heroic_dir(base);
   SDL_snprintf(base, sizeof(base), "%s/.var/app/com.heroicgameslauncher.hgl/config/heroic", home);
   scan_heroic_dir(base);
#endif
}

int scan_launchers(void)
{
   found = 0;
#ifdef _WIN32
   scan_epic();
   scan_gog();
   scan_battlenet();
#endif
   scan_heroic();
   return found;
}
