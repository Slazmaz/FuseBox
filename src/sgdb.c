#include "sgdb.h"
#include "credentials.h"
#include "http.h"
#include "json.h"
#include "platform.h"

#include <SDL3/SDL.h>

#define API "https://www.steamgriddb.com/api/v2"

static char config[1024];
static char key[128];
static SDL_Mutex *key_lock;

void sgdb_init(const char *config_dir)
{
   SDL_strlcpy(config, config_dir, sizeof(config));
   if (!key_lock)
      key_lock = SDL_CreateMutex();
   credentials_get(config, "steamgriddb_api_key", key, sizeof(key));
}

bool sgdb_has_key(void)
{
   bool has;

   SDL_LockMutex(key_lock);
   has = key[0] != '\0';
   SDL_UnlockMutex(key_lock);
   return has;
}

void sgdb_set_key(const char *value)
{
   SDL_LockMutex(key_lock);
   SDL_strlcpy(key, value, sizeof(key));
   credentials_set(config, "steamgriddb_api_key", key);
   SDL_UnlockMutex(key_lock);
}

/* GET API + path, parsed; NULL on a failure. */
static struct json *get(const char *path, SDL_AtomicInt *abort, int *status_out)
{
   char url[1024], header[200], *body = NULL;
   struct json *root;
   int status;

   SDL_LockMutex(key_lock);
   SDL_snprintf(header, sizeof(header), "Authorization: Bearer %s", key);
   SDL_UnlockMutex(key_lock);
   if (status_out)
      *status_out = 0;
   if (SDL_strlen(header) <= 22)
      return NULL;
   SDL_snprintf(url, sizeof(url), API "%s", path);
   status = http_request(url, NULL, header, NULL, abort, &body, NULL, 20);
   SDL_memset(header, 0, sizeof(header));
   if (status_out)
      *status_out = status;
   if (status != 200)
   {
      if (status > 0 && status != 404)
         SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SteamGridDB: HTTP %d", status);
      SDL_free(body);
      return NULL;
   }
   root = json_parse(body);
   SDL_free(body);
   if (root && !json_bool(root, "success", false))
   {
      json_free(root);
      return NULL;
   }
   return root;
}

int sgdb_search(const char *name, struct sgdb_game *games, int max, SDL_AtomicInt *abort,
      int *status)
{
   char path[600], *term;
   struct json *root, *item;
   int n = 0;

   /* SteamGridDB's search stumbles on trademark signs and the like. */
   {
      char clean[256];
      size_t i = 0;
      const char *p = name;
      while (*p && i + 1 < sizeof(clean))
      {
         if (SDL_strncmp(p, "\xE2\x84\xA2", 3) == 0 || SDL_strncmp(p, "\xC2\xAE", 2) == 0
               || SDL_strncmp(p, "\xC2\xA9", 2) == 0)
         {
            p += *p == '\xE2' ? 3 : 2;
            continue;
         }
         clean[i++] = *p++;
      }
      clean[i] = '\0';
      if (!(term = http_escape(clean)))
         return -1;
   }
   SDL_snprintf(path, sizeof(path), "/search/autocomplete/%s", term);
   SDL_free(term);
   if (!(root = get(path, abort, status)))
      return -1;
   for (item = json_get(root, "data") ? json_get(root, "data")->child : NULL; item && n < max;
         item = item->next)
   {
      games[n].id = (unsigned)json_num(item, "id", 0);
      SDL_strlcpy(games[n].name, json_str(item, "name", ""), sizeof(games[n].name));
      if (games[n].id)
         n++;
   }
   json_free(root);
   return n;
}

int sgdb_list(unsigned game_id, enum art_type type, struct sgdb_art **out,
      SDL_AtomicInt *abort)
{
   static const char *const queries[ART_TYPES] = {
      "/grids/game/%u?dimensions=600x900,342x482,660x930&types=static&mimes=image/png,image/jpeg",
      "/grids/game/%u?dimensions=920x430,460x215&types=static&mimes=image/png,image/jpeg",
      "/heroes/game/%u?types=static&mimes=image/png,image/jpeg",
      "/logos/game/%u?types=static&mimes=image/png",
      "/icons/game/%u?types=static&mimes=image/png",
   };
   char path[256];
   struct json *root, *data, *item;
   struct sgdb_art *arts;
   int n = 0, count = 0;

   *out = NULL;
   SDL_snprintf(path, sizeof(path), queries[type], game_id);
   if (!(root = get(path, abort, NULL)))
      return -1;
   data = json_get(root, "data");
   for (item = data ? data->child : NULL; item; item = item->next)
      count++;
   if (!(arts = SDL_calloc((size_t)SDL_max(count, 1), sizeof(*arts))))
   {
      json_free(root);
      return -1;
   }
   for (item = data ? data->child : NULL; item; item = item->next)
   {
      struct sgdb_art *a = &arts[n];
      const char *mime = json_str(item, "mime", "");

      a->id = (unsigned)json_num(item, "id", 0);
      SDL_strlcpy(a->url, json_str(item, "url", ""), sizeof(a->url));
      SDL_strlcpy(a->thumb, json_str(item, "thumb", a->url), sizeof(a->thumb));
      a->width = (int)json_num(item, "width", 0);
      a->height = (int)json_num(item, "height", 0);
      SDL_strlcpy(a->ext, SDL_strcmp(mime, "image/jpeg") == 0 ? "jpg" : "png", sizeof(a->ext));
      if (a->url[0])
         n++;
   }
   json_free(root);
   *out = arts;
   return n;
}

bool sgdb_download(const char *url, const char *path, SDL_AtomicInt *abort)
{
   return http_download(url, path, abort, NULL) == 200;
}

/* The game's icon from its own exe, unless the user picked one (FuseBox's
 * own fill from SteamGridDB gives way to it). True if it saved one. */
static bool exe_icon(int index)
{
   char exe[1024], tmp[1100];
   bool want;

   library_lock();
   {
      const struct game *g = library_game(index);
      const struct art_choice *a = &g->art[ART_ICON];
      SDL_strlcpy(exe, g->game_exe, sizeof(exe));
      want = exe[0] && !a->picked && (!a->path[0] || a->sgdb_id != 0);
   }
   library_unlock();
   if (!want)
      return false;
   library_tmp_path(index, ART_ICON, tmp, sizeof(tmp));
   if (!platform_exe_icon(exe, tmp))
   {
      SDL_RemovePath(tmp);
      return false;
   }
   /* Not over what the user picked meanwhile. */
   library_lock();
   want = !library_game(index)->art[ART_ICON].picked
      && library_set_art(index, ART_ICON, tmp, "png", 0);
   library_unlock();
   if (!want)
      SDL_RemovePath(tmp);
   return want;
}

int sgdb_fill(int index, bool only_type_set, enum art_type only_type, SDL_AtomicInt *abort)
{
   char name[256];
   unsigned game_id;
   int t, saved = 0;

   if ((!only_type_set || only_type == ART_ICON) && exe_icon(index))
      saved++;
   library_lock();
   SDL_strlcpy(name, library_game(index)->name, sizeof(name));
   game_id = library_game(index)->sgdb_id;
   library_unlock();

   if (!game_id)
   {
      struct sgdb_game match;
      if (sgdb_search(name, &match, 1, abort, NULL) < 1)
         return 0;
      game_id = match.id;
      library_lock();
      library_game(index)->sgdb_id = match.id;
      SDL_strlcpy(library_game(index)->sgdb_name, match.name,
            sizeof(library_game(index)->sgdb_name));
      library_unlock();
      library_changed();
   }

   for (t = 0; t < ART_TYPES; t++)
   {
      struct sgdb_art *arts;
      char tmp[1100];
      bool has;
      int n;

      if (only_type_set && t != (int)only_type)
         continue;
      library_lock();
      has = library_game(index)->art[t].path[0] != '\0';
      library_unlock();
      if (has || (abort && SDL_GetAtomicInt(abort)))
         continue;
      if ((n = sgdb_list(game_id, (enum art_type)t, &arts, abort)) > 0)
      {
         library_tmp_path(index, (enum art_type)t, tmp, sizeof(tmp));
         if (sgdb_download(arts[0].url, tmp, abort))
         {
            /* Unless it has some by now (the user picked one). */
            library_lock();
            if (!library_game(index)->art[t].path[0])
               saved += library_set_art(index, (enum art_type)t, tmp, arts[0].ext, arts[0].id);
            else
               SDL_RemovePath(tmp);
            library_unlock();
         }
      }
      SDL_free(arts);
   }
   return saved;
}
