#include "steam.h"
#include "history.h"
#include "library.h"
#include "ra.h"
#include "vdf.h"

#include <SDL3/SDL.h>

#define TAG "FuseBox"

static struct steam_install steam;
static bool found;

bool steam_init(void)
{
   found = platform_find_steam(&steam);
   if (found)
      SDL_Log("Steam: %s (%s)", steam.root, steam.config);
   else
      SDL_Log("Steam: not found");
   return found;
}

const struct steam_install *steam_install(void)
{
   return found ? &steam : NULL;
}

static void shortcuts_path(char *dst, size_t size)
{
   SDL_snprintf(dst, size, "%s" PATH_SEP "shortcuts.vdf", steam.config);
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

void steam_target(const struct game *g, char *exe, size_t exe_size, char *options,
      size_t options_size)
{
   const char *lo = g->launch_options, *command = SDL_strstr(lo, COMMAND);
   char path[1100];

   SDL_strlcpy(exe, own_exe(), exe_size);
   quote_arg(library_path(g), path, sizeof(path));
   /* Steam puts FuseBox there. */
   if (command)
      SDL_snprintf(options, options_size, "%.*s%s %s%s", (int)(command - lo), lo, COMMAND, path,
            command + SDL_strlen(COMMAND));
   else
      SDL_strlcpy(options, path, options_size);
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
   if (old < 0 && through && tagged(s))
      old = library_by_path(exe, 0);
   if (old >= 0 && library_game(old)->scan.installed)
      return old;
   for (i = 0; tagged(s) && i < library_count(); i++)
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

      if (s->type != VDF_MAP || !vdf_str(s, "AppName")[0])
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
   SDL_snprintf(dst, size, "%s" PATH_SEP "grid" PATH_SEP "%u%s", steam.config, (unsigned)appid,
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

/* Copies the game's art into Steam's grid; the icon's path for the
 * shortcut in icon (empty if none). */
static void copy_art(const struct game *g, Uint32 appid, char *icon, size_t size)
{
   char grid[1100], base[1100], dst[1200];
   int t;

   icon[0] = '\0';
   SDL_snprintf(grid, sizeof(grid), "%s" PATH_SEP "grid", steam.config);
   SDL_CreateDirectory(grid);
   for (t = 0; t < ART_TYPES; t++)
   {
      const char *src = g->art[t].path, *ext;

      if (!src[0])
         continue;
      ext = SDL_strrchr(src, '.');
      grid_name(appid, (enum art_type)t, base, sizeof(base));
      /* An old one in the other format would win or confuse Steam. */
      SDL_snprintf(dst, sizeof(dst), "%s.png", base);
      SDL_RemovePath(dst);
      SDL_snprintf(dst, sizeof(dst), "%s.jpg", base);
      SDL_RemovePath(dst);
      /* Steam only looks for .png and .jpg, but goes by what's in them: a
       * .webp named .png shows, and moves. */
      if (!ext || SDL_strcasecmp(ext, ".webp") == 0)
         ext = ".png";
      SDL_snprintf(dst, sizeof(dst), "%s%s", base, ext);
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

bool steam_sync(char *msg, size_t size)
{
   char path[1100], icon[1200], exe[1100];
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

   library_lock();
   for (i = 0; i < library_count(); i++)
   {
      struct game *g = library_game(i);
      struct vdf *s;
      Uint32 appid;

      bool own = SDL_strcmp(g->source, "Steam") == 0;

      /* Out of Steam: a gone game ticked for it (whoever made its
       * shortcut), or an installed launcher game unticked. */
      if (g->gone ? g->selected : (g->installed && !own && !g->selected))
      {
         if (g->steam_appid && (s = find_appid(list, g->steam_appid)))
         {
            vdf_remove(list, s);
            remove_art(g->steam_appid);
            history_removed(library_dir(), g->steam_appid);
            removed++;
         }
         g->steam_appid = g->shortcut_id = 0;
         g->gone = false;
         if (own)
            g->installed = false;
         continue;
      }
      if (!g->installed)
         continue;
      /* Steam's own shortcuts: only their art (and icon) change. */
      if (own)
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
      /* A ROM without an emulator: nothing to start it with. */
      if (g->no_emulator)
         continue;
      quote(g->exe, exe, sizeof(exe));
      appid = shortcut_appid(exe, g->name);
      /* A shortcut keeps its id, whatever its game is renamed to or
       * whichever emulator it's for: Steam keeps play time and collections
       * by it. A new one doesn't take one that's kept. */
      if (g->steam_appid && find_appid(list, g->steam_appid))
         appid = g->steam_appid;
      else
      {
         int other;
         for (other = 0; other < library_count(); other++)
            if (other != i && library_game(other)->steam_appid == appid)
            {
               appid = shortcut_appid(exe, g->id);
               break;
            }
      }
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
   if (ok)
      SDL_snprintf(msg, size, "Steam shortcuts: %d added, %d updated, %d removed", added,
            updated + art_only, removed);
   else
      SDL_snprintf(msg, size, "Can't write %s", path);
   return ok;
}
