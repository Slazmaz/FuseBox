#include "steam.h"
#include "history.h"
#include "json.h"
#include "library.h"
#include "ra.h"
#include "steamui.h"
#include "vdf.h"

#include <SDL3/SDL.h>

#define TAG "FuseBox"
/* Steam writes its shortcuts down within this (ms). */
#define WRITTEN_MS 8000

static struct steam_install steam;
static bool found;

static struct
{
   char config[1024];
   /* Under lock. */
   SDL_Mutex *lock;
   bool shortcuts, menus, autostart, made_file;
   int port;
} set;

/* Under the lock. */
static void save_settings(void)
{
   char path[1100];
   SDL_IOStream *io;

   SDL_CreateDirectory(set.config);
   SDL_snprintf(path, sizeof(path), "%s" PATH_SEP "steam.json", set.config);
   if (!(io = SDL_IOFromFile(path, "wb")))
   {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Can't write %s: %s", path, SDL_GetError());
      return;
   }
   SDL_IOprintf(io, "{\n  \"shortcuts\": %s,\n  \"menus\": %s,\n  \"autostart\": %s,\n"
         "  \"made_file\": %s,\n  \"port\": %d\n}\n", set.shortcuts ? "true" : "false",
         set.menus ? "true" : "false", set.autostart ? "true" : "false",
         set.made_file ? "true" : "false", set.port);
   SDL_CloseIO(io);
}

void steam_read_settings(const char *config_dir)
{
   char path[1100], *text;
   struct json *root;
   bool old;

   SDL_strlcpy(set.config, config_dir, sizeof(set.config));
   if (!set.lock)
      set.lock = SDL_CreateMutex();
   SDL_snprintf(path, sizeof(path), "%s" PATH_SEP "steam.json", config_dir);
   /* They were the achievements', and on with a login; a new FuseBox has
    * neither file. */
   if ((old = !platform_exists(path)))
      SDL_snprintf(path, sizeof(path), "%s" PATH_SEP "retroachievements.json", config_dir);
   text = SDL_LoadFile(path, NULL);
   root = text ? json_parse(text) : NULL;
   SDL_LockMutex(set.lock);
   /* One being written just now can't be read: as they were. */
   if (root || old)
   {
      set.shortcuts = old || json_bool(root, "shortcuts", true);
      set.menus = old && root ? ra_active() : json_bool(root, "menus", true);
      set.autostart = !root || json_bool(root, "autostart", true);
      set.made_file = root && json_bool(root, "made_file", false);
      set.port = root ? (int)json_num(root, "port", 0) : 0;
   }
   /* Now, or a login would change the menus later. */
   if (old)
      save_settings();
   SDL_UnlockMutex(set.lock);
   json_free(root);
   SDL_free(text);
}

static bool get_setting(const bool *value)
{
   bool on;

   SDL_LockMutex(set.lock);
   on = *value;
   SDL_UnlockMutex(set.lock);
   return on;
}

static void change_setting(bool *value, bool on)
{
   SDL_LockMutex(set.lock);
   *value = on;
   save_settings();
   SDL_UnlockMutex(set.lock);
}

bool steam_shortcuts(void)
{
   return get_setting(&set.shortcuts);
}

void steam_set_shortcuts(bool on)
{
   change_setting(&set.shortcuts, on);
}

bool steam_menus(void)
{
   return get_setting(&set.menus);
}

void steam_set_menus(bool on)
{
   change_setting(&set.menus, on);
}

bool steam_autostart(void)
{
   return get_setting(&set.autostart);
}

void steam_set_autostart(bool on)
{
   change_setting(&set.autostart, on);
}

bool steam_made_file(void)
{
   return get_setting(&set.made_file);
}

void steam_set_made_file(bool made)
{
   change_setting(&set.made_file, made);
}

int steam_port(void)
{
   int port;

   SDL_LockMutex(set.lock);
   port = set.port;
   SDL_UnlockMutex(set.lock);
   return port;
}

void steam_set_port(int port)
{
   SDL_LockMutex(set.lock);
   set.port = port;
   save_settings();
   SDL_UnlockMutex(set.lock);
}

bool steam_init(void)
{
   found = platform_find_steam(&steam);
   if (found)
      SDL_Log("Steam: %s (%s)", steam.root, steam.config);
   else
      SDL_Log("Steam: not found");
   return found;
}

void steam_account(void)
{
   struct steam_install now;

   if (!found || !platform_find_steam(&now))
      return;
   SDL_LockMutex(set.lock);
   if (SDL_strcmp(now.config, steam.config) != 0)
   {
      SDL_Log("Steam: now %s", now.config);
      SDL_strlcpy(steam.config, now.config, sizeof(steam.config));
   }
   SDL_UnlockMutex(set.lock);
}

void steam_config(char *dst, size_t size)
{
   SDL_LockMutex(set.lock);
   SDL_strlcpy(dst, found ? steam.config : "", size);
   SDL_UnlockMutex(set.lock);
}

const struct steam_install *steam_install(void)
{
   return found ? &steam : NULL;
}

static void shortcuts_path(char *dst, size_t size)
{
   char config[1024];

   steam_config(config, sizeof(config));
   SDL_snprintf(dst, size, "%s" PATH_SEP "shortcuts.vdf", config);
}

static bool tagged(const struct vdf *shortcut)
{
   const struct vdf *tags = vdf_find(shortcut, "tags"), *t;

   for (t = tags ? tags->child : NULL; t; t = t->next)
      if (t->type == VDF_STRING && SDL_strcmp(t->str, TAG) == 0)
         return true;
   return false;
}

/* Steam's id for a shortcut: from its target and name, as Steam itself
 * makes it; the same for the grid's file names. */
static Uint32 shortcut_appid(const char *exe, const char *name)
{
   char buf[1500];
   int len = SDL_snprintf(buf, sizeof(buf), "%s%s", exe, name);
   return fb_crc32(buf, (size_t)SDL_min(len, (int)sizeof(buf) - 1)) | 0x80000000u;
}

/* Steam keeps paths quoted. */
static void unquote(const char *src, char *dst, size_t size)
{
   size_t len;

   if (*src == '"')
      src++;
   SDL_strlcpy(dst, src, size);
   len = SDL_strlen(dst);
   if (len && dst[len - 1] == '"')
      dst[len - 1] = '\0';
}

static bool is_path(const char *p)
{
   return (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':'
            && (p[2] == '\\' || p[2] == '/'))
      || p[0] == '/' || (p[0] == '\\' && p[1] == '\\');
}

#ifdef _WIN32
#define OWN_EXE "fusebox.exe"
#else
#define OWN_EXE "fusebox"
#endif
/* What shortcuts had before their game's path alone. */
#define RUN "--run "
/* One that's there only while its game runs. */
#define ONCE "--once "
#define COMMAND "%command%"

/* A quoted argument: on Linux what a shell would take for its own is escaped
 * (Steam starts shortcuts through one). */
static void quote_arg(const char *text, char *dst, size_t size)
{
   size_t n = 0, len = SDL_strlen(text);

   /* A folder's separator would escape the quote. */
   while (len > 1 && (text[len - 1] == '/' || text[len - 1] == '\\'))
      len--;
   dst[n++] = '"';
   for (; len && n + 4 < size; text++, len--)
   {
#ifndef _WIN32
      if (*text == '"' || *text == '\\' || *text == '$' || *text == '`')
         dst[n++] = '\\';
#endif
      dst[n++] = *text;
   }
   dst[n++] = '"';
   dst[n] = '\0';
}

/* Back again: src is at the quote; returns what's after, NULL if it's no such
 * argument. */
static const char *read_arg(const char *src, char *dst, size_t size)
{
   size_t n = 0;

   if (*src++ != '"')
      return NULL;
   for (; *src && *src != '"'; src++)
   {
#ifndef _WIN32
      if (*src == '\\' && src[1])
         src++;
#endif
      if (n + 1 < size)
         dst[n++] = *src;
   }
   dst[n] = '\0';
   return *src ? src + 1 : NULL;
}

static const char *own_exe(void)
{
   static char own[1024];

   if (!own[0])
      SDL_snprintf(own, sizeof(own), "%s%s", SDL_GetBasePath(), OWN_EXE);
   return own;
}

/* FuseBox's arguments, where the launch options have them go. */
static void around_options(const char *lo, const char *args, char *options, size_t size)
{
   const char *command = SDL_strstr(lo, COMMAND);

   /* Steam puts FuseBox there. */
   if (command)
      SDL_snprintf(options, size, "%.*s%s %s%s", (int)(command - lo), lo, COMMAND, args,
            command + SDL_strlen(COMMAND));
   else
      SDL_strlcpy(options, args, size);
}

void steam_target(const struct game *g, char *exe, size_t exe_size, char *options,
      size_t options_size)
{
   char path[1100];

   SDL_strlcpy(exe, own_exe(), exe_size);
   quote_arg(library_path(g), path, sizeof(path));
   around_options(g->launch_options, path, options, options_size);
}

void steam_once_target(const char *path, const char *emulator, const char *launch_options,
      const char *more, char *exe, size_t exe_size, char *options, size_t options_size)
{
   char quoted[1100], with[1100], args[4400];

   SDL_strlcpy(exe, own_exe(), exe_size);
   quote_arg(path, quoted, sizeof(quoted));
   with[0] = '\0';
   if (emulator[0])
      quote_arg(emulator, with, sizeof(with));
   SDL_snprintf(args, sizeof(args), ONCE "%s%s%s%s%s", quoted, with[0] ? " --emulator " : "", with,
         more[0] ? " " : "", more);
   around_options(launch_options, args, options, options_size);
}

const char *steam_settings_dir(void)
{
   return set.config;
}

/* One of steam_once_target's. */
static bool is_once(const struct vdf *s)
{
   char exe[1024];
   size_t len, own = SDL_strlen(OWN_EXE);

   unquote(vdf_str(s, "Exe"), exe, sizeof(exe));
   len = SDL_strlen(exe);
   return len > own && SDL_strcasecmp(exe + len - own, OWN_EXE) == 0
      && SDL_strstr(vdf_str(s, "LaunchOptions"), ONCE "\"");
}

/* What the shortcut starts (into exe), and with which options. One of
 * FuseBox's (*through): the game's path, or what older ones started. */
static const char *target(const struct vdf *s, char *exe, size_t size, bool *through)
{
   const char *options = vdf_str(s, "LaunchOptions"), *p, *end;
   size_t len, own = SDL_strlen(OWN_EXE);
   char path[1024];

   *through = false;
   unquote(vdf_str(s, "Exe"), exe, size);
   len = SDL_strlen(exe);
   if (len <= own || SDL_strcasecmp(exe + len - own, OWN_EXE) != 0
         || (exe[len - own - 1] != '/' && exe[len - own - 1] != '\\'))
      return options;
   p = options;
   if (SDL_strncmp(p, RUN, SDL_strlen(RUN)) == 0)
      p += SDL_strlen(RUN);
   else if (SDL_strstr(p, COMMAND " \""))
      p = SDL_strstr(p, COMMAND " \"") + SDL_strlen(COMMAND) + 1;
   if (!(end = read_arg(p, path, sizeof(path))))
      return options;
   *through = true;
   SDL_strlcpy(exe, path, size);
   while (*end == ' ')
      end++;
   return end;
}

/* Whether the shortcut's game is still there: its target and its start
 * folder both. One that's a URL or a bare command can't be checked. */
static bool shortcut_present(const struct vdf *s)
{
   char dir[1024], exe[1024];
   bool through;

   unquote(vdf_str(s, "StartDir"), dir, sizeof(dir));
   target(s, exe, sizeof(exe), &through);
   /* One of FuseBox's starts in its folder, which may have been moved. */
   return (!is_path(exe) || platform_exists(exe))
      && (through || !is_path(dir) || platform_exists(dir));
}

/* What the shortcut has of the settings FuseBox sets (to tell when they
 * differ from the game's). A missing one is Steam's default, on. */
static void read_settings(struct game *g, const struct vdf *s)
{
   g->sc_overlay = !vdf_find(s, "AllowOverlay") || vdf_int(s, "AllowOverlay");
   g->sc_input = !vdf_find(s, "AllowDesktopConfig") || vdf_int(s, "AllowDesktopConfig");
   unquote(vdf_str(s, "Exe"), g->sc_exe, sizeof(g->sc_exe));
   SDL_strlcpy(g->sc_options, vdf_str(s, "LaunchOptions"), sizeof(g->sc_options));
   SDL_strlcpy(g->sc_name, vdf_str(s, "AppName"), sizeof(g->sc_name));
}

/* The id the shortcut FuseBox makes for a game has. */
static Uint32 game_appid(const struct game *g)
{
   char exe[1100];

   SDL_snprintf(exe, sizeof(exe), "\"%s\"", g->exe);
   return shortcut_appid(exe, g->name);
}

/* Whether a shortcut's options start this ROM: whichever emulator it's
 * for, it's that game's. */
static bool starts_rom(const char *options, const struct game *g)
{
   size_t n = SDL_strlen(g->rom), args = SDL_strlen(g->args);
   const char *p = options;

   if (!n)
      return false;
   if (args && SDL_strncmp(options, g->args, args) == 0 && (!options[args] || options[args] == ' '))
      return true;
   while ((p = SDL_strstr(p, g->rom)))
   {
      if ((p == options || p[-1] == '"') && (!p[n] || p[n] == '"'))
         return true;
      p += n;
   }
   return false;
}

/* The library's game a shortcut is of, -1 if none: by the id it's known
 * to have or would be given, an installed one first (a game found again
 * under another id leaves the old one behind); else the one that one of
 * FuseBox's own starts. */
static int shortcut_game(Uint32 appid, const struct vdf *s)
{
   char exe[1024];
   bool through;
   const char *options = target(s, exe, sizeof(exe), &through);
   int i, known, old = -1;

   for (known = 1; known >= 0; known--)
      for (i = 0; i < library_count(); i++)
      {
         const struct game *g = library_game(i);
         if (SDL_strcmp(g->source, "Steam") == 0 || !g->exe[0]
               || (known ? g->shortcut_id : game_appid(g)) != appid)
            continue;
         if (g->scan.installed)
            return i;
         if (old < 0)
            old = i;
      }
   /* One added while Steam ran has no tag. */
   if (old < 0 && through)
      old = library_by_path(exe, 0);
   if (old >= 0 && library_game(old)->scan.installed)
      return old;
   for (i = 0; (through || tagged(s)) && i < library_count(); i++)
   {
      if (!starts_rom(options, library_game(i)))
         continue;
      if (library_game(i)->scan.installed)
         return i;
      if (old < 0)
         old = i;
   }
   return old;
}

/* After the launchers' scan: each shortcut in Steam, whoever made it,
 * either belongs to a launcher game (which is then in Steam) or is listed
 * as Steam's own. One whose game's folder is gone marks it gone. */
int steam_read_shortcuts(void)
{
   char path[1100];
   struct vdf *root, *list, *s;
   int n = 0;

   if (!found)
      return 0;
   steam_account();
   shortcuts_path(path, sizeof(path));
   if (!(root = vdf_load(path)))
      return 0;
   list = vdf_map(root, "shortcuts");
   for (s = list->child; s; s = s->next)
   {
      struct game g;
      Uint32 appid;
      bool present, matched = false;
      int i;

      if (s->type != VDF_MAP || !vdf_str(s, "AppName")[0] || is_once(s))
         continue;
      appid = vdf_int(s, "appid");
      if (!appid)
         appid = shortcut_appid(vdf_str(s, "Exe"), vdf_str(s, "AppName"));
      present = shortcut_present(s);
      n++;

      library_lock();
      if ((i = shortcut_game(appid, s)) >= 0)
      {
         struct game *lg = library_game(i);
         bool moved;
         char starts[1024];

         lg->scan.steam_appid = lg->shortcut_id = appid;
         read_settings(lg, s);
         target(s, starts, sizeof(starts), &moved);
         moved |= lg->rom[0] && SDL_strcmp(starts, lg->exe) != 0;
         /* Ticked from the start: only what Steam already has. */
         lg->scan.selected = true;
         /* Its launcher knows best: uninstalling often leaves the folder
          * (settings, addons) behind. A ROM whose shortcut is for another
          * emulator is only updated, as is one that's found by FuseBox. */
         /* Gone: kept until ticked for removal. */
         if ((!present && !moved) || !lg->scan.installed)
         {
            lg->scan.gone = true;
            lg->scan.selected = false;
         }
         matched = true;
      }
      library_unlock();
      if (matched)
         continue;

      SDL_zero(g);
      SDL_snprintf(g.id, sizeof(g.id), "steam:%u", (unsigned)appid);
      SDL_strlcpy(g.name, vdf_str(s, "AppName"), sizeof(g.name));
      SDL_strlcpy(g.source, "Steam", sizeof(g.source));
      {
         bool through;
         SDL_strlcpy(g.args, target(s, g.exe, sizeof(g.exe), &through), sizeof(g.args));
      }
      if (SDL_strlen(g.exe) > 4 && SDL_strcasecmp(g.exe + SDL_strlen(g.exe) - 4, ".exe") == 0)
         SDL_strlcpy(g.game_exe, g.exe, sizeof(g.game_exe));
      unquote(vdf_str(s, "StartDir"), g.start_dir, sizeof(g.start_dir));
      g.scan.steam_appid = appid;
      read_settings(&g, s);
      /* First seen: its settings are what Steam has. */
      g.steam_overlay = g.sc_overlay;
      g.steam_input = g.sc_input;
      g.scan.installed = present;
      g.scan.gone = !present;
      library_add(&g);
   }
   vdf_free(root);
   return n;
}

/* The grid's file name for each type, without extension. */
static void grid_name(Uint32 appid, enum art_type type, char *dst, size_t size)
{
   static const char *const suffix[ART_TYPES] = { "p", "", "_hero", "_logo", "_icon" };
   char config[1024];

   steam_config(config, sizeof(config));
   SDL_snprintf(dst, size, "%s" PATH_SEP "grid" PATH_SEP "%u%s", config, (unsigned)appid,
         suffix[type]);
}

Sint64 steam_art_time(Uint32 appid)
{
   Sint64 first = 0;

   /* Windows keeps a file's creation time when it's replaced right away, as
    * copy_art does. Elsewhere there's none to go by. */
#ifdef _WIN32
   static const char *const exts[] = { "png", "jpg" };
   char base[1100], path[1200];
   SDL_PathInfo info;
   int t;

   for (t = 0; found && t < ART_TYPES * 2; t++)
   {
      grid_name(appid, (enum art_type)(t / 2), base, sizeof(base));
      SDL_snprintf(path, sizeof(path), "%s.%s", base, exts[t % 2]);
      if (SDL_GetPathInfo(path, &info) && info.create_time > 0
            && (!first || info.create_time / SDL_NS_PER_SECOND < first))
         first = info.create_time / SDL_NS_PER_SECOND;
   }
#endif
   return first;
}

/* The grid's file for that art. Steam only looks for .png and .jpg, but goes
 * by what's in them: a .webp named .png shows, and moves. */
static void grid_file(const struct game *g, Uint32 appid, enum art_type type, char *dst, size_t size)
{
   const char *ext = SDL_strrchr(g->art[type].path, '.');
   char base[1100];

   grid_name(appid, type, base, sizeof(base));
   SDL_snprintf(dst, size, "%s%s", base, !ext || SDL_strcasecmp(ext, ".webp") == 0 ? ".png" : ext);
}

static bool same_file(const char *a, const char *b)
{
   SDL_PathInfo x, y;
   size_t n = 0, m = 0;
   void *p, *q;
   bool same;

   if (!SDL_GetPathInfo(a, &x) || !SDL_GetPathInfo(b, &y) || x.size != y.size)
      return false;
   p = SDL_LoadFile(a, &n);
   q = SDL_LoadFile(b, &m);
   same = p && q && n == m && SDL_memcmp(p, q, n) == 0;
   SDL_free(p);
   SDL_free(q);
   return same;
}

/* Copies the game's art into Steam's grid; the icon's path for the
 * shortcut in icon (empty if none). */
static void copy_art(const struct game *g, Uint32 appid, char *icon, size_t size)
{
   char config[1024], grid[1100], base[1100], dst[1200];
   int t;

   icon[0] = '\0';
   steam_config(config, sizeof(config));
   SDL_snprintf(grid, sizeof(grid), "%s" PATH_SEP "grid", config);
   SDL_CreateDirectory(grid);
   for (t = 0; t < ART_TYPES; t++)
   {
      const char *src = g->art[t].path;

      if (!src[0])
         continue;
      grid_name(appid, (enum art_type)t, base, sizeof(base));
      /* An old one in the other format would win or confuse Steam. */
      SDL_snprintf(dst, sizeof(dst), "%s.png", base);
      SDL_RemovePath(dst);
      SDL_snprintf(dst, sizeof(dst), "%s.jpg", base);
      SDL_RemovePath(dst);
      grid_file(g, appid, (enum art_type)t, dst, sizeof(dst));
      if (!SDL_CopyFile(src, dst))
         SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Can't copy art to %s: %s", dst, SDL_GetError());
      else if (t == ART_ICON)
         SDL_strlcpy(icon, dst, size);
   }
}

static struct vdf *find_appid(struct vdf *list, Uint32 appid)
{
   struct vdf *s;

   /* Old shortcuts have no appid of their own: Steam's is from the target
    * and name. */
   for (s = list->child; s; s = s->next)
      if (s->type == VDF_MAP && (vdf_int(s, "appid") == appid || (!vdf_int(s, "appid")
               && shortcut_appid(vdf_str(s, "Exe"), vdf_str(s, "AppName")) == appid)))
         return s;
   return NULL;
}

static void quote(const char *text, char *dst, size_t size)
{
   SDL_snprintf(dst, size, "\"%s\"", text);
}

/* Steam numbers the shortcuts "0", "1"...; a new one goes after the
 * highest. */
static struct vdf *new_shortcut(struct vdf *list)
{
   struct vdf *s;
   char key[16];
   long next = 0;

   for (s = list->child; s; s = s->next)
      next = SDL_max(next, SDL_strtol(s->key, NULL, 10) + 1);
   SDL_snprintf(key, sizeof(key), "%ld", next);
   return vdf_map(list, key);
}

static void fill_shortcut(struct vdf *s, const struct game *g, Uint32 appid, const char *icon)
{
   char own[1024], exe[1100], options[2048], dir[1100];
   struct vdf *tags;

   steam_target(g, own, sizeof(own), options, sizeof(options));
   quote(own, exe, sizeof(exe));
   /* The game's own is FuseBox's to start it in. */
   quote(SDL_GetBasePath(), dir, sizeof(dir));
   vdf_set_int(s, "appid", appid);
   vdf_set_str(s, "AppName", g->name);
   vdf_set_str(s, "Exe", exe);
   vdf_set_str(s, "StartDir", dir);
   vdf_set_str(s, "LaunchOptions", options);
   vdf_set_str(s, "icon", icon);
   if (!vdf_find(s, "ShortcutPath"))
      vdf_set_str(s, "ShortcutPath", "");
   /* The game's own settings (FuseBox's game settings). */
   vdf_set_int(s, "AllowDesktopConfig", g->steam_input);
   vdf_set_int(s, "AllowOverlay", g->steam_overlay);
   /* Steam's own defaults for the rest, only where missing. */
   if (!vdf_find(s, "IsHidden"))
      vdf_set_int(s, "IsHidden", 0);
   if (!vdf_find(s, "OpenVR"))
      vdf_set_int(s, "OpenVR", 0);
   if (!vdf_find(s, "Devkit"))
      vdf_set_int(s, "Devkit", 0);
   if (!vdf_find(s, "DevkitGameID"))
      vdf_set_str(s, "DevkitGameID", "");
   if (!vdf_find(s, "DevkitOverrideAppID"))
      vdf_set_int(s, "DevkitOverrideAppID", 0);
   if (!vdf_find(s, "LastPlayTime"))
      vdf_set_int(s, "LastPlayTime", 0);
   if (!vdf_find(s, "FlatpakAppID"))
      vdf_set_str(s, "FlatpakAppID", "");
   tags = vdf_map(s, "tags");
   if (!tagged(s))
   {
      char key[16];
      int n = 0;
      struct vdf *t;
      for (t = tags->child; t; t = t->next)
         n++;
      SDL_snprintf(key, sizeof(key), "%d", n);
      vdf_set_str(tags, key, TAG);
   }
}

/* The grid art of a shortcut FuseBox took out. */
static void remove_art(Uint32 appid)
{
   char base[1100], path[1200];
   int t;

   for (t = 0; t < ART_TYPES; t++)
   {
      grid_name(appid, (enum art_type)t, base, sizeof(base));
      SDL_snprintf(path, sizeof(path), "%s.png", base);
      SDL_RemovePath(path);
      SDL_snprintf(path, sizeof(path), "%s.jpg", base);
      SDL_RemovePath(path);
   }
}

enum todo
{
   TODO_NOTHING,
   TODO_REMOVE,
   /* Steam's own shortcut: only its art (and icon) and settings change. */
   TODO_OWN,
   TODO_PUT
};

static enum todo todo(const struct game *g)
{
   bool own = SDL_strcmp(g->source, "Steam") == 0;

   /* Out of Steam: a gone game ticked for it (whoever made its
    * shortcut), or an installed launcher game unticked. */
   if (g->gone ? g->selected : (g->installed && !own && !g->selected))
      return TODO_REMOVE;
   if (!g->installed)
      return TODO_NOTHING;
   if (own)
      return TODO_OWN;
   /* A ROM without an emulator: nothing to start it with. */
   return g->no_emulator ? TODO_NOTHING : TODO_PUT;
}

/* Its shortcut was taken out; none: it had none. */
static void left_steam(struct game *g, bool none)
{
   if (!none)
   {
      remove_art(g->steam_appid);
      history_removed(library_dir(), g->steam_appid);
   }
   g->steam_appid = g->shortcut_id = 0;
   g->gone = false;
   if (SDL_strcmp(g->source, "Steam") == 0)
      g->installed = false;
}

/* The app id of the game's shortcut: the one it has in the list, else a new
 * one's. Under the library's lock. */
static Uint32 choose_appid(struct vdf *list, int index)
{
   const struct game *g = library_game(index);
   Uint32 appid = game_appid(g);
   char exe[1100];
   int other;

   /* A shortcut keeps its id, whatever its game is renamed to or
    * whichever emulator it's for: Steam keeps play time and collections
    * by it. A new one doesn't take one that's kept. */
   if (g->steam_appid && find_appid(list, g->steam_appid))
      return g->steam_appid;
   for (other = 0; other < library_count(); other++)
      if (other != index && library_game(other)->steam_appid == appid)
      {
         quote(g->exe, exe, sizeof(exe));
         return shortcut_appid(exe, g->id);
      }
   return appid;
}

bool steam_sync(char *msg, size_t size)
{
   char path[1100], icon[1200];
   struct vdf *root, *list;
   int i, added = 0, updated = 0, removed = 0, art_only = 0;
   bool ok;

   if (!found)
   {
      SDL_strlcpy(msg, "Steam wasn't found", size);
      return false;
   }
   shortcuts_path(path, sizeof(path));
   if (!(root = vdf_load(path)))
   {
      SDL_strlcpy(msg, "Steam's shortcuts.vdf can't be read", size);
      return false;
   }
   list = vdf_map(root, "shortcuts");
   /* Left behind by a Steam that ended before its game. */
   {
      struct vdf *s, *next;
      for (s = list->child; s; s = next)
      {
         next = s->next;
         if (s->type == VDF_MAP && is_once(s))
            vdf_remove(list, s);
      }
   }

   library_lock();
   for (i = 0; i < library_count(); i++)
   {
      struct game *g = library_game(i);
      struct vdf *s;
      Uint32 appid;
      enum todo what = todo(g);

      if (what == TODO_REMOVE)
      {
         if ((s = g->steam_appid ? find_appid(list, g->steam_appid) : NULL))
         {
            vdf_remove(list, s);
            removed++;
         }
         left_steam(g, !s);
         continue;
      }
      if (what == TODO_OWN)
      {
         copy_art(g, g->steam_appid, icon, sizeof(icon));
         if ((s = find_appid(list, g->steam_appid)))
         {
            if (icon[0])
               vdf_set_str(s, "icon", icon);
            /* Overlay and Steam Input from its game settings; its launch
             * options are the user's own, in Steam. */
            vdf_set_int(s, "AllowDesktopConfig", g->steam_input);
            vdf_set_int(s, "AllowOverlay", g->steam_overlay);
         }
         art_only++;
         continue;
      }
      if (what != TODO_PUT)
         continue;
      appid = choose_appid(list, i);
      /* Ours from before, perhaps under an older id (renamed, moved). */
      s = find_appid(list, appid);
      if (!s && g->steam_appid)
         s = find_appid(list, g->steam_appid);
      if (s)
         updated++;
      else
      {
         s = new_shortcut(list);
         history_added(library_dir(), appid);
         added++;
      }
      copy_art(g, appid, icon, sizeof(icon));
      fill_shortcut(s, g, appid, icon);
      g->steam_appid = g->shortcut_id = appid;
   }

   /* Steam numbers its shortcuts 0, 1, 2...: no gaps after removing. */
   {
      struct vdf *s;
      int n = 0;
      for (s = list->child; s; s = s->next)
      {
         char key[16];
         SDL_snprintf(key, sizeof(key), "%d", n++);
         SDL_free(s->key);
         s->key = SDL_strdup(key);
      }
   }

   ok = vdf_save(root, path);
   vdf_free(root);
   /* Their art is in Steam now (still locked: none was picked since). */
   for (i = 0; ok && i < library_count(); i++)
   {
      struct game *g = library_game(i);
      if (g->installed && g->steam_appid && !g->no_emulator)
         g->art_edited = false;
   }
   library_unlock();
   library_changed();
   msg[0] = '\0';
   if (!ok)
      SDL_snprintf(msg, size, "Can't write %s", path);
   SDL_Log("Steam shortcuts: %d added, %d updated, %d removed (%s)", added, updated + art_only,
         removed, ok ? path : msg);
   return ok;
}

/* ---- In the Steam that runs ---------------------------------------------- */

/* What's to do for a game, copied: Steam's answers take a while. */
struct change
{
   char id[256];
   enum todo what;
   /* 0: a new shortcut. */
   Uint32 appid;
   /* Its name, target and options; its icon; Steam Input. Types of art, as
    * bits. */
   bool fields, icon, input;
   unsigned art;
   bool on;
   char name[256], exe[1100], options[2048];
};

/* Whether a path of the shortcut's is that one; Steam quotes them. */
static bool is_quoted(const struct vdf *s, const char *key, const char *path)
{
   char own[1100];

   unquote(vdf_str(s, key), own, sizeof(own));
   return SDL_strcmp(own, path) == 0;
}

/* How the shortcut (NULL: none yet) differs from the game. False: in what only
 * steam_sync changes. Under the library's lock. */
static bool compare(const struct game *g, const struct vdf *s, struct change *c)
{
   char file[1200];
   int t;

   /* Turned off there, Steam keeps the overlay elsewhere. */
   if (g->steam_overlay != (!s || !vdf_find(s, "AllowOverlay") || vdf_int(s, "AllowOverlay")))
      return false;
   c->on = g->steam_input;
   c->input = g->steam_input != (!s || !vdf_find(s, "AllowDesktopConfig")
         || vdf_int(s, "AllowDesktopConfig"));
   if (c->what == TODO_PUT)
   {
      SDL_strlcpy(c->name, g->name, sizeof(c->name));
      steam_target(g, c->exe, sizeof(c->exe), c->options, sizeof(c->options));
      c->fields = !s || SDL_strcmp(vdf_str(s, "AppName"), c->name) != 0
         || !is_quoted(s, "Exe", c->exe) || !is_quoted(s, "StartDir", SDL_GetBasePath())
         || SDL_strcmp(vdf_str(s, "LaunchOptions"), c->options) != 0;
   }
   for (t = 0; t < ART_TYPES; t++)
   {
      if (!g->art[t].path[0])
         continue;
      if (s)
         grid_file(g, c->appid, (enum art_type)t, file, sizeof(file));
      if (s && same_file(g->art[t].path, file)
            && (t != ART_ICON || SDL_strcmp(vdf_str(s, "icon"), file) == 0))
         continue;
      if (t == ART_ICON)
         c->icon = true;
      else
         c->art |= 1u << t;
   }
   return true;
}

/* What's to do; NULL: not this way. */
static struct change *find_changes(int *count)
{
   struct change *list = NULL, *more;
   struct vdf *root, *shortcuts;
   char path[1100], backup[1200];
   bool ok = true;
   int i, n = 0;

   shortcuts_path(path, sizeof(path));
   if (!(root = vdf_load(path)))
      return NULL;
   SDL_snprintf(backup, sizeof(backup), "%s.fusebox-backup", path);
   if (platform_exists(path) && !platform_exists(backup))
      SDL_CopyFile(path, backup);
   shortcuts = vdf_map(root, "shortcuts");
   list = SDL_calloc(1, sizeof(*list));
   library_lock();
   for (i = 0; ok && list && i < library_count(); i++)
   {
      const struct game *g = library_game(i);
      struct change c = { 0 };
      const struct vdf *s = NULL;

      if ((c.what = todo(g)) == TODO_NOTHING)
         continue;
      SDL_strlcpy(c.id, g->id, sizeof(c.id));
      if (c.what == TODO_PUT)
         s = find_appid(shortcuts, choose_appid(shortcuts, i));
      if (!s && g->steam_appid)
         s = find_appid(shortcuts, g->steam_appid);
      if (s)
         c.appid = vdf_int(s, "appid") ? vdf_int(s, "appid")
            : shortcut_appid(vdf_str(s, "Exe"), vdf_str(s, "AppName"));
      if (c.what == TODO_OWN && !s)
         continue;
      if (c.what != TODO_REMOVE)
      {
         if (!compare(g, s, &c))
            ok = false;
         /* As it is, but the library may not know the id. */
         else if (s && !c.fields && !c.icon && !c.input && !c.art && g->steam_appid == c.appid
               && !g->art_edited)
            continue;
      }
      else if (!g->steam_appid && !g->gone)
         continue;
      if (!(more = SDL_realloc(list, sizeof(*list) * (size_t)(n + 1))))
         ok = false;
      else
      {
         list = more;
         list[n++] = c;
      }
   }
   library_unlock();
   vdf_free(root);
   if (!ok)
   {
      SDL_free(list);
      return NULL;
   }
   *count = n;
   return list;
}

/* The file has them all, as they are now. */
static bool written(const struct change *list, int count)
{
   char path[1100];
   struct vdf *root, *shortcuts;
   bool ok = true;
   int i;

   shortcuts_path(path, sizeof(path));
   if (!(root = vdf_load(path)))
      return false;
   shortcuts = vdf_map(root, "shortcuts");
   for (i = 0; i < count && ok; i++)
   {
      const struct vdf *s = list[i].appid ? find_appid(shortcuts, list[i].appid) : NULL;

      if (list[i].what == TODO_REMOVE)
         ok = !s;
      else
         ok = s && (list[i].what == TODO_OWN
               || (SDL_strcmp(vdf_str(s, "AppName"), list[i].name) == 0
                  && SDL_strcmp(vdf_str(s, "LaunchOptions"), list[i].options) == 0))
            && (!list[i].input || (vdf_int(s, "AllowDesktopConfig") != 0) == list[i].on);
   }
   vdf_free(root);
   return ok;
}

/* The game's art, where Steam has it. */
static bool put_art(struct change *c, SDL_AtomicInt *abort)
{
   char icon[1200] = "", files[ART_TYPES][1200];
   int t, i;

   library_lock();
   if ((i = library_find(c->id)) >= 0)
   {
      if (c->icon || c->art)
         copy_art(library_game(i), c->appid, icon, sizeof(icon));
      for (t = 0; t < ART_TYPES; t++)
         grid_file(library_game(i), c->appid, (enum art_type)t, files[t], sizeof(files[t]));
   }
   library_unlock();
   if (i < 0)
      return false;
   if (c->icon && icon[0] && !steamui_put_icon(c->appid, icon, abort))
      return false;
   for (t = 0; t < ART_TYPES; t++)
   {
      const char *name = SDL_strrchr(files[t], PATH_SEP[0]);

      if ((c->art & 1u << t) && (!name
               || !steamui_put_art(c->appid, (enum art_type)t, name + 1, abort)))
         return false;
   }
   return true;
}

bool steam_sync_live(SDL_AtomicInt *abort)
{
   struct change *list;
   int i, k, count, added = 0, updated = 0, removed = 0;
   Uint64 until;
   bool ok = true;

   if (!found || !steamui_can_put(abort) || !(list = find_changes(&count)))
      return false;
   for (i = 0; i < count && ok && !SDL_GetAtomicInt(abort); i++)
   {
      struct change *c = &list[i];
      bool fresh = !c->appid;

      if (c->what == TODO_REMOVE)
         ok = fresh || steamui_take_out(c->appid, abort);
      else if (c->fields)
         ok = (c->appid = steamui_put(c->appid, c->name, c->exe, SDL_GetBasePath(), c->options,
                  abort)) != 0;
      if (ok && c->what != TODO_REMOVE)
      {
         /* At once: what's left undone isn't added again. */
         library_lock();
         if ((k = library_find(c->id)) >= 0)
            library_game(k)->steam_appid = library_game(k)->shortcut_id = c->appid;
         library_unlock();
         if (fresh)
            history_added(library_dir(), c->appid);
         ok = (!c->input || steamui_put_input(c->appid, c->on, abort)) && put_art(c, abort);
      }
      if (!ok)
         break;
      if (c->what == TODO_REMOVE)
         removed += !fresh;
      else if (fresh)
         added++;
      else if (c->fields || c->icon || c->input || c->art)
         updated++;
   }
   ok = ok && i == count;
   for (until = SDL_GetTicks() + WRITTEN_MS; ok && !written(list, count);)
   {
      if (SDL_GetTicks() >= until || SDL_GetAtomicInt(abort))
         ok = false;
      SDL_Delay(200);
   }
   library_lock();
   for (i = 0; i < count && ok; i++)
      if ((k = library_find(list[i].id)) >= 0)
      {
         if (list[i].what == TODO_REMOVE)
            left_steam(library_game(k), !list[i].appid);
         else
            library_game(k)->art_edited = false;
      }
   library_unlock();
   library_changed();
   SDL_free(list);
   SDL_Log("Steam shortcuts: %d added, %d updated, %d removed (%s)", added, updated, removed,
         ok ? "while Steam runs" : "not all of it: Steam has to restart");
   return ok;
}
