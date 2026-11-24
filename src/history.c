#include "history.h"
#include "json.h"
#include "platform.h"

#include <SDL3/SDL.h>

/* data_dir: with or without a separator at its end. */
static void history_path(const char *data_dir, Uint32 appid, char *dst, size_t size)
{
   size_t n = SDL_strlen(data_dir);
   const char *sep = n && (data_dir[n - 1] == '/' || data_dir[n - 1] == '\\') ? "" : PATH_SEP;

   if (appid)
      SDL_snprintf(dst, size, "%s%shistory" PATH_SEP "%u.json", data_dir, sep, (unsigned)appid);
   else
      SDL_snprintf(dst, size, "%s%shistory", data_dir, sep);
}

static Sint64 now(void)
{
   SDL_Time t = 0;

   SDL_GetCurrentTime(&t);
   return t / SDL_NS_PER_SECOND;
}

static void save(const char *data_dir, Uint32 appid, Sint64 added, Sint64 played)
{
   char path[1100], text[96];

   history_path(data_dir, 0, path, sizeof(path));
   SDL_CreateDirectory(path);
   history_path(data_dir, appid, path, sizeof(path));
   SDL_snprintf(text, sizeof(text), "{\"added\": %lld, \"played\": %lld}\n", (long long)added,
         (long long)played);
   if (!SDL_SaveFile(path, text, SDL_strlen(text)))
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Can't write %s: %s", path, SDL_GetError());
}

void history_added(const char *data_dir, Uint32 appid)
{
   if (appid)
      save(data_dir, appid, now(), 0);
}

void history_found(const char *data_dir, Uint32 appid, Sint64 added)
{
   if (appid && added > 0)
      save(data_dir, appid, added, -1);
}

void history_played(const char *data_dir, Uint32 appid, Sint64 played)
{
   Sint64 added, before;

   history_get(data_dir, appid, &added, &before);
   if (added && before <= 0)
      save(data_dir, appid, added, played);
}

void history_removed(const char *data_dir, Uint32 appid)
{
   char path[1100];

   if (!appid)
      return;
   history_path(data_dir, appid, path, sizeof(path));
   SDL_RemovePath(path);
}

Uint32 history_appid(void)
{
   const char *id = SDL_getenv("SteamGameId");

   /* A shortcut's: its app id above, its kind below. */
   return id ? (Uint32)(SDL_strtoull(id, NULL, 10) >> 32) : 0;
}

void history_playing(const char *data_dir)
{
   Uint32 appid = history_appid();
   Sint64 added, played;

   if (!appid)
      return;
   history_get(data_dir, appid, &added, &played);
   if (added && !played)
      save(data_dir, appid, added, now());
}

void history_get(const char *data_dir, Uint32 appid, Sint64 *added, Sint64 *played)
{
   char path[1100], *text;
   struct json *root;

   *added = *played = 0;
   if (!appid)
      return;
   history_path(data_dir, appid, path, sizeof(path));
   if (!platform_exists(path) || !(text = SDL_LoadFile(path, NULL)))
      return;
   root = json_parse(text);
   SDL_free(text);
   *added = (Sint64)json_num(root, "added", 0);
   *played = (Sint64)json_num(root, "played", 0);
   json_free(root);
}
