#include "steam.h"
#include "library.h"
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

/* Whether the shortcut's game is still there: its target and its start
 * folder both. One that's a URL or a bare command can't be checked. */
static bool shortcut_present(const struct vdf *s)
{
   char dir[1024], exe[1024];

   unquote(vdf_str(s, "StartDir"), dir, sizeof(dir));
   unquote(vdf_str(s, "Exe"), exe, sizeof(exe));
   return (!is_path(exe) || platform_exists(exe)) && (!is_path(dir) || platform_exists(dir));
}

/* What the shortcut has of the settings FuseBox sets (to tell when they
 * differ from the game's). A missing one is Steam's default, on. */
static void read_settings(struct game *g, const struct vdf *s)
{
   g->sc_overlay = !vdf_find(s, "AllowOverlay") || vdf_int(s, "AllowOverlay");
   g->sc_input = !vdf_find(s, "AllowDesktopConfig") || vdf_int(s, "AllowDesktopConfig");
   SDL_strlcpy(g->sc_options, vdf_str(s, "LaunchOptions"), sizeof(g->sc_options));
}

/* The id the shortcut FuseBox makes for a game has. */
static Uint32 game_appid(const struct game *g)
{
   char exe[1100];

   SDL_snprintf(exe, sizeof(exe), "\"%s\"", g->exe);
   return shortcut_appid(exe, g->name);
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
      for (i = 0; i < library_count() && !matched; i++)
      {
         struct game *lg = library_game(i);
         if (SDL_strcmp(lg->source, "Steam") == 0 || !lg->exe[0])
            continue;
         if (game_appid(lg) == appid)
         {
            lg->steam_appid = appid;
            read_settings(lg, s);
            /* Ticked from the start: only what Steam already has. */
            lg->selected = true;
            /* Its launcher knows best: uninstalling often leaves the
             * folder (settings, addons) behind. */
            /* Gone: kept until ticked for removal. */
            if (!present || !lg->installed)
            {
               lg->gone = true;
               lg->selected = false;
            }
            matched = true;
         }
      }
      library_unlock();
      if (matched)
         continue;

      SDL_zero(g);
      SDL_snprintf(g.id, sizeof(g.id), "steam:%u", (unsigned)appid);
      SDL_strlcpy(g.name, vdf_str(s, "AppName"), sizeof(g.name));
      SDL_strlcpy(g.source, "Steam", sizeof(g.source));
      unquote(vdf_str(s, "Exe"), g.exe, sizeof(g.exe));
      if (SDL_strlen(g.exe) > 4 && SDL_strcasecmp(g.exe + SDL_strlen(g.exe) - 4, ".exe") == 0)
         SDL_strlcpy(g.game_exe, g.exe, sizeof(g.game_exe));
      unquote(vdf_str(s, "StartDir"), g.start_dir, sizeof(g.start_dir));
      SDL_strlcpy(g.args, vdf_str(s, "LaunchOptions"), sizeof(g.args));
      g.steam_appid = appid;
      read_settings(&g, s);
      /* First seen: its settings are what Steam has. */
      g.steam_overlay = g.sc_overlay;
      g.steam_input = g.sc_input;
      g.installed = present;
      g.gone = !present;
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
      SDL_snprintf(dst, sizeof(dst), "%s%s", base, ext ? ext : ".png");
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
   char exe[1100], dir[1100];
   struct vdf *tags;

   quote(g->exe, exe, sizeof(exe));
   quote(g->start_dir, dir, sizeof(dir));
   vdf_set_int(s, "appid", appid);
   vdf_set_str(s, "AppName", g->name);
   vdf_set_str(s, "Exe", exe);
   vdf_set_str(s, "StartDir", g->start_dir[0] ? dir : "");
   vdf_set_str(s, "icon", icon);
   if (!vdf_find(s, "ShortcutPath"))
      vdf_set_str(s, "ShortcutPath", "");
   {
      char options[1100];
      library_launch_options(g, options, sizeof(options));
      vdf_set_str(s, "LaunchOptions", options);
   }
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
            removed++;
         }
         g->steam_appid = 0;
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
      quote(g->exe, exe, sizeof(exe));
      appid = shortcut_appid(exe, g->name);
      /* Ours from before, perhaps under an older id (renamed, moved). */
      s = find_appid(list, appid);
      if (!s && g->steam_appid)
         s = find_appid(list, g->steam_appid);
      if (s)
         updated++;
      else
      {
         s = new_shortcut(list);
         added++;
      }
      copy_art(g, appid, icon, sizeof(icon));
      fill_shortcut(s, g, appid, icon);
      g->steam_appid = appid;
   }
   library_unlock();
   library_changed();

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
   if (ok)
      SDL_snprintf(msg, size, "Steam shortcuts: %d added, %d updated, %d removed", added,
            updated + art_only, removed);
   else
      SDL_snprintf(msg, size, "Can't write %s", path);
   return ok;
}
