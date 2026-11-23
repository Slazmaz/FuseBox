#include "library.h"
#include "json.h"
#include "platform.h"

#include <SDL3/SDL.h>

const char *const art_names[ART_TYPES] = { "cover", "wide", "hero", "logo", "icon" };
const char *const art_labels[ART_TYPES] = { "Cover", "Wide", "Hero", "Logo", "Icon" };

static struct
{
   /* SDL mutexes are recursive: functions here lock too, even when the
    * caller already does. */
   SDL_Mutex *lock;
   struct game *games;
   int count, alloc;
   char dir[1024];
   char file[1024];
   bool dirty;
   /* library.json's time when last read or written by this process. */
   SDL_Time seen;
} lib;

enum art_type art_from_name(const char *name)
{
   int i;

   for (i = 0; i < ART_TYPES; i++)
      if (SDL_strcasecmp(name, art_names[i]) == 0)
         return (enum art_type)i;
   return ART_TYPES;
}

Uint32 fb_crc32(const void *data, size_t size)
{
   static Uint32 table[256];
   const Uint8 *p = data;
   Uint32 crc = 0xFFFFFFFFu;
   size_t i;

   if (!table[1])
      for (i = 0; i < 256; i++)
      {
         Uint32 c = (Uint32)i;
         int k;
         for (k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
         table[i] = c;
      }
   for (i = 0; i < size; i++)
      crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
   return crc ^ 0xFFFFFFFFu;
}

void library_launch_options(const struct game *g, char *dst, size_t size)
{
   SDL_snprintf(dst, size, "%s%s%s", g->args, g->args[0] && g->launch_options[0] ? " " : "",
         g->launch_options);
}

void library_normalize(const char *name, char *dst, size_t size)
{
   size_t n = 0;

   for (; *name && n + 1 < size; name++)
   {
      char c = *name;
      if (c >= 'A' && c <= 'Z')
         c = (char)(c - 'A' + 'a');
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (Uint8)c >= 0x80)
         dst[n++] = c;
   }
   dst[n] = '\0';
}

void library_lock(void)
{
   SDL_LockMutex(lib.lock);
}

void library_unlock(void)
{
   SDL_UnlockMutex(lib.lock);
}

int library_count(void)
{
   return lib.count;
}

struct game *library_game(int index)
{
   return index >= 0 && index < lib.count ? &lib.games[index] : NULL;
}

const char *library_dir(void)
{
   return lib.dir;
}

int library_find(const char *id)
{
   int i;

   for (i = 0; i < lib.count; i++)
      if (SDL_strcmp(lib.games[i].id, id) == 0)
         return i;
   return -1;
}

int library_match(const char *name_or_id)
{
   char want[256], have[256];
   int i;

   if ((i = library_find(name_or_id)) >= 0)
      return i;
   library_normalize(name_or_id, want, sizeof(want));
   if (!want[0])
      return -1;
   for (i = 0; i < lib.count; i++)
   {
      library_normalize(lib.games[i].name, have, sizeof(have));
      if (SDL_strcmp(want, have) == 0)
         return i;
   }
   /* Other apps may not know what it was renamed to. */
   for (i = 0; i < lib.count; i++)
   {
      library_normalize(lib.games[i].own_name, have, sizeof(have));
      if (SDL_strcmp(want, have) == 0)
         return i;
   }
   return -1;
}

static int append(const struct game *g)
{
   if (lib.count == lib.alloc)
   {
      int alloc = lib.alloc ? lib.alloc * 2 : 64;
      struct game *grown = SDL_realloc(lib.games, (size_t)alloc * sizeof(*grown));
      if (!grown)
         return -1;
      lib.games = grown;
      lib.alloc = alloc;
   }
   lib.games[lib.count] = *g;
   if (!g->own_name[0])
      SDL_strlcpy(lib.games[lib.count].own_name, g->name, sizeof(g->own_name));
   return lib.count++;
}

int library_add(const struct game *game)
{
   int i;

   library_lock();
   if ((i = library_find(game->id)) < 0)
      i = append(game);
   else
   {
      struct game *g = &lib.games[i];
      /* What the scan knows is newest; the art and match are the user's. */
      SDL_strlcpy(g->own_name, game->name, sizeof(g->own_name));
      SDL_strlcpy(g->name, g->custom_name[0] ? g->custom_name : g->own_name, sizeof(g->name));
      SDL_strlcpy(g->source, game->source, sizeof(g->source));
      SDL_strlcpy(g->exe, game->exe, sizeof(g->exe));
      if (game->game_exe[0])
         SDL_strlcpy(g->game_exe, game->game_exe, sizeof(g->game_exe));
      SDL_strlcpy(g->start_dir, game->start_dir, sizeof(g->start_dir));
      SDL_strlcpy(g->args, game->args, sizeof(g->args));
      if (game->steam_appid)
         g->steam_appid = game->steam_appid;
      g->installed |= game->installed;
      g->sc_overlay = game->sc_overlay;
      g->sc_input = game->sc_input;
      SDL_strlcpy(g->sc_options, game->sc_options, sizeof(g->sc_options));
      SDL_strlcpy(g->sc_name, game->sc_name, sizeof(g->sc_name));
      g->gone |= game->gone;
      g->selected |= game->selected;
   }
   lib.dirty = true;
   library_unlock();
   return i;
}

void library_rename(int index, const char *name)
{
   char text[256];
   size_t n;
   struct game *g;

   while (*name == ' ')
      name++;
   SDL_strlcpy(text, name, sizeof(text));
   for (n = SDL_strlen(text); n && text[n - 1] == ' '; n--)
      text[n - 1] = '\0';

   library_lock();
   if (!(g = library_game(index)))
   {
      library_unlock();
      return;
   }
   if (!text[0])
      SDL_strlcpy(text, g->own_name, sizeof(text));
   SDL_strlcpy(g->name, text, sizeof(g->name));
   SDL_strlcpy(g->custom_name, SDL_strcmp(text, g->own_name) == 0 ? "" : text,
         sizeof(g->custom_name));
   lib.dirty = true;
   library_unlock();
}

/* art/<id, made safe for a folder name>-<its crc>, unique per id. */
static void game_folder(const struct game *g, char *dst, size_t size)
{
   char safe[64];
   size_t n = 0;
   const char *p;

   for (p = g->id; *p && n + 1 < sizeof(safe); p++)
   {
      char c = *p;
      if (c >= 'A' && c <= 'Z')
         c = (char)(c - 'A' + 'a');
      safe[n++] = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '_';
   }
   safe[n] = '\0';
   SDL_snprintf(dst, size, "%s" "art" PATH_SEP "%s-%08x", lib.dir, safe,
         (unsigned)fb_crc32(g->id, SDL_strlen(g->id)));
}

void library_tmp_path(int index, enum art_type type, char *dst, size_t size)
{
   char folder[1024];

   library_lock();
   game_folder(&lib.games[index], folder, sizeof(folder));
   library_unlock();
   SDL_CreateDirectory(folder);
   /* Per thread: two downloads for the same art never share a file. */
   SDL_snprintf(dst, size, "%s" PATH_SEP "%s.%llx.download", folder, art_names[type],
         (unsigned long long)SDL_GetCurrentThreadID());
}

bool library_set_art(int index, enum art_type type, const char *tmp_path,
      const char *ext, unsigned sgdb_id)
{
   char folder[1024], path[1024];
   struct game *g;
   bool ok;

   library_lock();
   g = &lib.games[index];
   game_folder(g, folder, sizeof(folder));
   SDL_snprintf(path, sizeof(path), "%s" PATH_SEP "%s.%s", folder, art_names[type], ext);
   SDL_CreateDirectory(folder);
   if (g->art[type].path[0] && SDL_strcmp(g->art[type].path, path) != 0)
      SDL_RemovePath(g->art[type].path);
   ok = SDL_RenamePath(tmp_path, path);
   if (ok)
   {
      SDL_strlcpy(g->art[type].path, path, sizeof(g->art[type].path));
      g->art[type].sgdb_id = sgdb_id;
      g->art[type].picked = false;
      g->art_version++;
      lib.dirty = true;
   }
   else
   {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Can't save %s: %s", path, SDL_GetError());
      SDL_RemovePath(tmp_path);
   }
   library_unlock();
   return ok;
}

void library_discard(void)
{
   library_lock();
   lib.dirty = false;
   library_unlock();
}

void library_set_picked(int index, enum art_type type, bool changed)
{
   library_lock();
   if (library_game(index))
   {
      library_game(index)->art[type].picked = true;
      library_game(index)->art_edited |= changed;
   }
   library_unlock();
   library_changed();
}

void library_changed(void)
{
   library_lock();
   lib.dirty = true;
   library_unlock();
}

static void copy_str(char *dst, size_t size, const struct json *obj, const char *key)
{
   SDL_strlcpy(dst, json_str(obj, key, ""), size);
}

static bool game_from_json(const struct json *o, struct game *g)
{
   struct json *art = json_get(o, "art");
   int t;

   SDL_zerop(g);
   copy_str(g->id, sizeof(g->id), o, "id");
   if (!g->id[0])
      return false;
   copy_str(g->name, sizeof(g->name), o, "name");
   copy_str(g->custom_name, sizeof(g->custom_name), o, "custom_name");
   SDL_strlcpy(g->own_name, json_str(o, "original_name", g->name), sizeof(g->own_name));
   copy_str(g->source, sizeof(g->source), o, "source");
   copy_str(g->exe, sizeof(g->exe), o, "exe");
   copy_str(g->game_exe, sizeof(g->game_exe), o, "game_exe");
   copy_str(g->start_dir, sizeof(g->start_dir), o, "start_dir");
   copy_str(g->args, sizeof(g->args), o, "args");
   copy_str(g->sgdb_name, sizeof(g->sgdb_name), o, "sgdb_name");
   g->steam_appid = (Uint32)json_num(o, "steam_appid", 0);
   g->shortcut_id = (Uint32)json_num(o, "shortcut_id", g->steam_appid);
   g->sgdb_id = (unsigned)json_num(o, "sgdb_id", 0);
   /* Steam's own defaults where not set. */
   g->steam_overlay = json_bool(o, "steam_overlay", true);
   g->steam_input = json_bool(o, "steam_input", true);
   g->art_edited = json_bool(o, "art_edited", false);
   copy_str(g->launch_options, sizeof(g->launch_options), o, "launch_options");
   for (t = 0; t < ART_TYPES; t++)
   {
      struct json *a = json_get(art, art_names[t]);
      char path[1024];

      if (!a)
         continue;
      /* "file" is relative to FuseBox's folder, so moving it keeps the
       * art; "path" (absolute) is for other apps. Only art that's there. */
      if (json_str(a, "file", NULL))
         SDL_snprintf(path, sizeof(path), "%s%s", lib.dir, json_str(a, "file", ""));
      else
         SDL_strlcpy(path, json_str(a, "path", ""), sizeof(path));
      if (platform_exists(path))
      {
         SDL_strlcpy(g->art[t].path, path, sizeof(g->art[t].path));
         g->art[t].sgdb_id = (unsigned)json_num(a, "sgdb_id", 0);
         g->art[t].picked = json_bool(a, "picked", false);
      }
   }
   return true;
}

/* Merges library.json into what's in memory: games it has that memory
 * doesn't, and art it has where memory has none. */
static bool merge_file(void)
{
   struct json *root, *games, *o;
   SDL_PathInfo info;
   char *text;
   bool found = false;

   if (!SDL_GetPathInfo(lib.file, &info))
      return false;
   lib.seen = info.modify_time;
   if (!(text = SDL_LoadFile(lib.file, NULL)))
      return false;
   root = json_parse(text);
   SDL_free(text);
   if (!root)
   {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s isn't valid JSON", lib.file);
      return false;
   }
   games = json_get(root, "games");
   for (o = games ? games->child : NULL; o; o = o->next)
   {
      struct game g;
      int i, t;

      if (!game_from_json(o, &g))
         continue;
      if ((i = library_find(g.id)) < 0)
      {
         append(&g);
         found = true;
         continue;
      }
      for (t = 0; t < ART_TYPES; t++)
         if (!lib.games[i].art[t].path[0] && g.art[t].path[0])
         {
            lib.games[i].art[t] = g.art[t];
            lib.games[i].art_version++;
            found = true;
         }
      if (!lib.games[i].sgdb_id && g.sgdb_id)
      {
         lib.games[i].sgdb_id = g.sgdb_id;
         SDL_strlcpy(lib.games[i].sgdb_name, g.sgdb_name, sizeof(g.sgdb_name));
      }
   }
   json_free(root);
   return found;
}

bool library_init(const char *data_dir)
{
   size_t len;

   SDL_zero(lib);
   SDL_strlcpy(lib.dir, data_dir, sizeof(lib.dir));
   len = SDL_strlen(lib.dir);
   if (len && lib.dir[len - 1] != '/' && lib.dir[len - 1] != '\\')
      SDL_strlcat(lib.dir, PATH_SEP, sizeof(lib.dir));
   SDL_snprintf(lib.file, sizeof(lib.file), "%slibrary.json", lib.dir);
   SDL_CreateDirectory(lib.dir);
   if (!(lib.lock = SDL_CreateMutex()))
      return false;
   library_lock();
   merge_file();
   library_unlock();
   return true;
}

void library_quit(void)
{
   library_save();
   SDL_free(lib.games);
   SDL_DestroyMutex(lib.lock);
   SDL_zero(lib);
}

bool library_refresh(void)
{
   SDL_PathInfo info;
   bool found;

   if (!SDL_GetPathInfo(lib.file, &info) || info.modify_time == lib.seen)
      return false;
   library_lock();
   found = merge_file();
   library_unlock();
   return found;
}

static void write_game(SDL_IOStream *io, const struct game *g)
{
   int t;
   bool first = true;

   SDL_IOprintf(io, "    {\n      \"id\": ");
   json_write_string(io, g->id);
   SDL_IOprintf(io, ",\n      \"name\": ");
   json_write_string(io, g->name);
   SDL_IOprintf(io, ",\n      \"original_name\": ");
   json_write_string(io, g->own_name);
   SDL_IOprintf(io, ",\n      \"custom_name\": ");
   json_write_string(io, g->custom_name);
   SDL_IOprintf(io, ",\n      \"source\": ");
   json_write_string(io, g->source);
   SDL_IOprintf(io, ",\n      \"exe\": ");
   json_write_string(io, g->exe);
   SDL_IOprintf(io, ",\n      \"game_exe\": ");
   json_write_string(io, g->game_exe);
   SDL_IOprintf(io, ",\n      \"start_dir\": ");
   json_write_string(io, g->start_dir);
   SDL_IOprintf(io, ",\n      \"args\": ");
   json_write_string(io, g->args);
   SDL_IOprintf(io, ",\n      \"steam_overlay\": %s,\n      \"steam_input\": %s,\n      \"art_edited\": %s,\n      \"launch_options\": ",
         g->steam_overlay ? "true" : "false", g->steam_input ? "true" : "false",
         g->art_edited ? "true" : "false");
   json_write_string(io, g->launch_options);
   SDL_IOprintf(io, ",\n      \"steam_appid\": %u,\n      \"shortcut_id\": %u,\n      \"sgdb_id\": %u,\n      \"sgdb_name\": ",
         (unsigned)g->steam_appid, (unsigned)g->shortcut_id, g->sgdb_id);
   json_write_string(io, g->sgdb_name);
   SDL_IOprintf(io, ",\n      \"art\": {");
   for (t = 0; t < ART_TYPES; t++)
   {
      if (!g->art[t].path[0])
         continue;
      size_t dir_len = SDL_strlen(lib.dir);

      SDL_IOprintf(io, "%s\n        \"%s\": { ", first ? "" : ",", art_names[t]);
      if (SDL_strncmp(g->art[t].path, lib.dir, dir_len) == 0)
      {
         SDL_IOprintf(io, "\"file\": ");
         json_write_string(io, g->art[t].path + dir_len);
         SDL_IOprintf(io, ", ");
      }
      SDL_IOprintf(io, "\"path\": ");
      json_write_string(io, g->art[t].path);
      SDL_IOprintf(io, ", \"sgdb_id\": %u, \"picked\": %s }", g->art[t].sgdb_id,
            g->art[t].picked ? "true" : "false");
      first = false;
   }
   SDL_IOprintf(io, "%s}\n    }", first ? "" : "\n      ");
}

bool library_save(void)
{
   char tmp[1100];
   SDL_IOStream *io;
   SDL_PathInfo info;
   bool ok;
   int i;

   library_lock();
   if (!lib.dirty)
   {
      library_unlock();
      return true;
   }
   /* What fusebox-cli added since we last looked mustn't be lost. */
   if (SDL_GetPathInfo(lib.file, &info) && info.modify_time != lib.seen)
      merge_file();
   SDL_snprintf(tmp, sizeof(tmp), "%s.tmp", lib.file);
   if (!(io = SDL_IOFromFile(tmp, "wb")))
   {
      library_unlock();
      return false;
   }
   SDL_IOprintf(io, "{\n  \"version\": 1,\n  \"games\": [\n");
   for (i = 0; i < lib.count; i++)
   {
      write_game(io, &lib.games[i]);
      SDL_IOprintf(io, "%s\n", i + 1 < lib.count ? "," : "");
   }
   SDL_IOprintf(io, "  ]\n}\n");
   ok = SDL_CloseIO(io) && SDL_RenamePath(tmp, lib.file);
   if (ok)
   {
      lib.dirty = false;
      if (SDL_GetPathInfo(lib.file, &info))
         lib.seen = info.modify_time;
   }
   else
      SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Can't write %s: %s", lib.file, SDL_GetError());
   library_unlock();
   return ok;
}
