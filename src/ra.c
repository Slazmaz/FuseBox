#include "ra.h"
#include "credentials.h"
#include "http.h"
#include "json.h"
#include "library.h"
#include "platform.h"
#include "rahash.h"

#include <SDL3/SDL.h>

#define API "https://retroachievements.org/dorequest.php"
#define KEY_USER "retroachievements_username"
#define KEY_TOKEN "retroachievements_token"
#define TIMEOUT_S 30
/* RetroAchievements' console numbers are below this. */
#define CONSOLES 128
/* Progress asked for less than this ago isn't asked for again. */
#define FRESH_MS (5 * 60 * 1000)
/* A saved list's older percentages are looked up again (seconds). */
#define LIST_AGE_S (7 * 24 * 3600)
/* From here on, ids aren't achievements: RetroAchievements puts a warning
 * in the list of emulators it doesn't know. */
#define FIRST_WARNING_ID 101000000u

static struct
{
   char config[1024];
   /* Under lock. */
   SDL_Mutex *lock;
   char user[128], token[128];
   bool in_steam;
   int port;

   /* One refresh at a time; what it keeps: when each console's progress
    * was last asked for (ms), and whether its games were. */
   SDL_Mutex *run;
   Uint64 asked[CONSOLES];
   bool listed[CONSOLES];
} ra;

static void settings_path(char *dst, size_t size)
{
   SDL_snprintf(dst, size, "%s" PATH_SEP "retroachievements.json", ra.config);
}

void ra_init(const char *config_dir)
{
   char path[1100], *text;
   struct json *root;

   SDL_strlcpy(ra.config, config_dir, sizeof(ra.config));
   if (!ra.lock)
   {
      ra.lock = SDL_CreateMutex();
      ra.run = SDL_CreateMutex();
   }
   SDL_LockMutex(ra.lock);
   if (!credentials_get(ra.config, KEY_USER, ra.user, sizeof(ra.user))
         || !credentials_get(ra.config, KEY_TOKEN, ra.token, sizeof(ra.token)))
      ra.user[0] = ra.token[0] = '\0';
   settings_path(path, sizeof(path));
   text = SDL_LoadFile(path, NULL);
   root = text ? json_parse(text) : NULL;
   ra.in_steam = !root || json_bool(root, "in_steam", true);
   ra.port = root ? (int)json_num(root, "port", 0) : 0;
   SDL_UnlockMutex(ra.lock);
   json_free(root);
   SDL_free(text);
}

/* Under the lock. */
static void save_settings(void)
{
   char path[1100];
   SDL_IOStream *io;

   SDL_CreateDirectory(ra.config);
   settings_path(path, sizeof(path));
   if (!(io = SDL_IOFromFile(path, "wb")))
   {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Can't write %s: %s", path, SDL_GetError());
      return;
   }
   SDL_IOprintf(io, "{\n  \"in_steam\": %s,\n  \"port\": %d\n}\n",
         ra.in_steam ? "true" : "false", ra.port);
   SDL_CloseIO(io);
}

bool ra_in_steam(void)
{
   bool on;

   SDL_LockMutex(ra.lock);
   on = ra.in_steam;
   SDL_UnlockMutex(ra.lock);
   return on;
}

void ra_set_in_steam(bool on)
{
   SDL_LockMutex(ra.lock);
   ra.in_steam = on;
   save_settings();
   SDL_UnlockMutex(ra.lock);
}

bool ra_active(void)
{
   bool active;

   SDL_LockMutex(ra.lock);
   active = ra.in_steam && ra.token[0];
   SDL_UnlockMutex(ra.lock);
   return active;
}

int ra_port(void)
{
   int port;

   SDL_LockMutex(ra.lock);
   port = ra.port;
   SDL_UnlockMutex(ra.lock);
   return port;
}

void ra_set_port(int port)
{
   SDL_LockMutex(ra.lock);
   ra.port = port;
   save_settings();
   SDL_UnlockMutex(ra.lock);
}

bool ra_user(char *user, size_t size)
{
   bool has;

   SDL_LockMutex(ra.lock);
   SDL_strlcpy(user, ra.user, size);
   has = ra.token[0] != '\0';
   SDL_UnlockMutex(ra.lock);
   return has;
}

/* RetroAchievements gives some numbers as text. */
static double number(const struct json *node, const char *key, double fallback)
{
   const struct json *v = json_get(node, key);

   if (v && v->type == JSON_STRING)
      return SDL_atof(v->str);
   return v && v->type == JSON_NUMBER ? v->num : fallback;
}

/* POST, parsed; NULL on a failure. */
static struct json *post(const char *fields, SDL_AtomicInt *abort, int *status_out)
{
   struct json *root = NULL;
   char *body = NULL;
   int status = http_request(API, fields, NULL, NULL, abort, &body, NULL, TIMEOUT_S);

   if (status_out)
      *status_out = status;
   /* A refused login is JSON too, with a 401 or so. */
   if (body && status > 0)
      root = json_parse(body);
   if (body)
      SDL_memset(body, 0, SDL_strlen(body));
   SDL_free(body);
   return root;
}

/* An "r=" request as the user; NULL on a failure or a refusal. */
static struct json *ask(const char *request, SDL_AtomicInt *abort)
{
   char fields[900], *user, *token;
   struct json *root = NULL;
   int status = 0;

   SDL_LockMutex(ra.lock);
   user = ra.token[0] ? http_escape(ra.user) : NULL;
   token = ra.token[0] ? http_escape(ra.token) : NULL;
   SDL_UnlockMutex(ra.lock);
   if (user && token)
   {
      SDL_snprintf(fields, sizeof(fields), "r=%s&u=%s&t=%s", request, user, token);
      root = post(fields, abort, &status);
      SDL_memset(fields, 0, sizeof(fields));
   }
   if (token)
      SDL_memset(token, 0, SDL_strlen(token));
   SDL_free(user);
   SDL_free(token);
   if (root && !json_bool(root, "Success", false))
   {
      json_free(root);
      root = NULL;
   }
   if (!root && status)
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "RetroAchievements: %s failed (%d)", request,
            status);
   return root;
}

bool ra_login(const char *user, const char *password, SDL_AtomicInt *abort, char *error,
      size_t size)
{
   char fields[1200], *u = http_escape(user), *p = http_escape(password);
   struct json *root = NULL;
   const char *name, *token;
   bool ok = false;

   SDL_strlcpy(error, "Couldn't reach RetroAchievements", size);
   if (u && p)
   {
      SDL_snprintf(fields, sizeof(fields), "r=login2&u=%s&p=%s", u, p);
      root = post(fields, abort, NULL);
      SDL_memset(fields, 0, sizeof(fields));
   }
   if (p)
      SDL_memset(p, 0, SDL_strlen(p));
   SDL_free(u);
   SDL_free(p);
   if (!root)
      return false;
   name = json_str(root, "User", user);
   token = json_str(root, "Token", "");
   if (json_bool(root, "Success", false) && token[0])
   {
      SDL_LockMutex(ra.lock);
      SDL_strlcpy(ra.user, name, sizeof(ra.user));
      SDL_strlcpy(ra.token, token, sizeof(ra.token));
      ok = credentials_set(ra.config, KEY_USER, ra.user)
         && credentials_set(ra.config, KEY_TOKEN, ra.token);
      SDL_UnlockMutex(ra.lock);
      SDL_strlcpy(error, ok ? "" : "Can't write credentials.cfg", size);
   }
   else
      SDL_strlcpy(error, json_str(root, "Error", "Wrong username or password"), size);
   json_free(root);
   return ok;
}

void ra_logout(void)
{
   int i;

   SDL_LockMutex(ra.lock);
   SDL_zeroa(ra.user);
   SDL_zeroa(ra.token);
   credentials_set(ra.config, KEY_TOKEN, NULL);
   credentials_set(ra.config, KEY_USER, NULL);
   SDL_UnlockMutex(ra.lock);
   /* The hashes are the ROMs': still good for the next account. */
   library_lock();
   for (i = 0; i < library_count(); i++)
   {
      struct game *g = library_game(i);
      g->ra_total = g->ra_unlocked = 0;
   }
   library_unlock();
   library_changed();
}

/* A ROM on its way through a refresh. */
struct item
{
   int index;
   unsigned console;
   char rom[1024], hash[RA_HASH_SIZE];
   Sint64 size, time;
   unsigned id, total, unlocked;
   /* Hashed just now. */
   bool fresh;
};

/* Each ROM whose hash is missing, or of an older file. */
static void hash_items(struct item *items, int count, SDL_AtomicInt *abort)
{
   int i;

   for (i = 0; i < count && !(abort && SDL_GetAtomicInt(abort)); i++)
   {
      struct item *it = &items[i];
      SDL_PathInfo info;

      if (!SDL_GetPathInfo(it->rom, &info) || info.type != SDL_PATHTYPE_FILE)
         continue;
      /* One without may only have been beyond an older FuseBox. */
      if (it->time == info.modify_time / SDL_NS_PER_SECOND && it->size == (Sint64)info.size
            && (it->hash[0] || !ra_hashable(it->rom)))
         continue;
      ra_hash(it->console, it->rom, it->hash, abort);
      if (abort && SDL_GetAtomicInt(abort))
         break;
      it->size = (Sint64)info.size;
      it->time = info.modify_time / SDL_NS_PER_SECOND;
      it->id = it->total = it->unlocked = 0;
      it->fresh = it->hash[0] != '\0';
   }
}

/* The console's games by hash. */
static bool identify(struct item *items, int count, unsigned console, SDL_AtomicInt *abort)
{
   char request[64];
   struct json *root, *list, *o;
   int i;

   SDL_snprintf(request, sizeof(request), "hashlibrary&c=%u", console);
   if (!(root = ask(request, abort)))
      return false;
   list = json_get(root, "MD5List");
   for (o = list ? list->child : NULL; o; o = o->next)
      for (i = 0; i < count; i++)
         if (items[i].console == console && o->key && o->type == JSON_NUMBER
               && SDL_strcasecmp(items[i].hash, o->key) == 0)
            items[i].id = (unsigned)o->num;
   json_free(root);
   return true;
}

/* How many achievements the console's games have, and how many of them
 * the user has. */
static bool progress(struct item *items, int count, unsigned console, SDL_AtomicInt *abort)
{
   char request[64];
   struct json *root, *list, *o;
   int i;

   SDL_snprintf(request, sizeof(request), "allprogress&c=%u", console);
   if (!(root = ask(request, abort)))
      return false;
   for (i = 0; i < count; i++)
      if (items[i].console == console)
         items[i].total = items[i].unlocked = 0;
   list = json_get(root, "Response");
   for (o = list ? list->child : NULL; o; o = o->next)
      for (i = 0; i < count; i++)
         if (items[i].id && items[i].console == console && o->key
               && SDL_strtoul(o->key, NULL, 10) == items[i].id)
         {
            items[i].total = (unsigned)number(o, "Achievements", 0);
            items[i].unlocked = (unsigned)number(o, "Unlocked", 0);
         }
   json_free(root);
   return true;
}

/* The ROMs RetroAchievements may know, as the library has them now. */
static struct item *collect(int *count)
{
   struct item *items;
   int i, n = 0;

   library_lock();
   items = SDL_calloc((size_t)SDL_max(1, library_count()), sizeof(*items));
   for (i = 0; items && i < library_count(); i++)
   {
      const struct game *g = library_game(i);
      struct item *it = &items[n];

      /* Steam's, too: what's installed isn't known before a scan. */
      if (!g->rom[0] || (!g->installed && !g->steam_appid)
            || !(it->console = ra_console(g->source, g->rom)) || it->console >= CONSOLES)
         continue;
      it->index = i;
      SDL_strlcpy(it->rom, g->rom, sizeof(it->rom));
      SDL_strlcpy(it->hash, g->ra_hash, sizeof(it->hash));
      it->size = g->ra_size;
      it->time = g->ra_time;
      it->id = g->ra_id;
      it->total = g->ra_total;
      it->unlocked = g->ra_unlocked;
      n++;
   }
   library_unlock();
   *count = n;
   return items;
}

static void store(const struct item *items, int count)
{
   bool changed = false;
   int i;

   library_lock();
   for (i = 0; i < count; i++)
   {
      const struct item *it = &items[i];
      struct game *g = library_game(it->index);

      /* Another file's by now. */
      if (!g || SDL_strcmp(g->rom, it->rom) != 0)
         continue;
      if (SDL_strcmp(g->ra_hash, it->hash) == 0 && g->ra_size == it->size
            && g->ra_time == it->time && g->ra_id == it->id && g->ra_total == it->total
            && g->ra_unlocked == it->unlocked)
         continue;
      SDL_strlcpy(g->ra_hash, it->hash, sizeof(g->ra_hash));
      g->ra_size = it->size;
      g->ra_time = it->time;
      g->ra_id = it->id;
      g->ra_total = it->total;
      g->ra_unlocked = it->unlocked;
      changed = true;
   }
   library_unlock();
   if (changed)
      library_changed();
}

bool ra_refresh(bool force, SDL_AtomicInt *abort)
{
   struct item *items;
   char user[128], now_user[128];
   int count, i, with = 0;
   unsigned c;
   bool ok = true;

   if (!ra_user(user, sizeof(user)))
      return false;
   SDL_LockMutex(ra.run);
   if (!(items = collect(&count)))
   {
      SDL_UnlockMutex(ra.run);
      return false;
   }
   hash_items(items, count, abort);
   for (c = 1; c < CONSOLES && !(abort && SDL_GetAtomicInt(abort)); c++)
   {
      bool any = false, fresh = false, unknown = false;

      for (i = 0; i < count; i++)
         if (items[i].console == c && items[i].hash[0])
         {
            any = true;
            fresh |= items[i].fresh;
            unknown |= !items[i].id;
         }
      if (!any)
         continue;
      if (fresh || (unknown && !ra.listed[c]))
      {
         if (identify(items, count, c, abort))
            ra.listed[c] = true;
         else
            ok = false;
      }
      if (force || fresh || !ra.asked[c] || SDL_GetTicks() - ra.asked[c] > FRESH_MS)
      {
         if (progress(items, count, c, abort))
            ra.asked[c] = SDL_GetTicks();
         else
            ok = false;
      }
   }
   /* Unless it's someone else's by now. */
   if (ra_user(now_user, sizeof(now_user)) && SDL_strcmp(user, now_user) == 0)
      store(items, count);
   for (i = 0; i < count; i++)
      with += items[i].total > 0;
   SDL_Log("RetroAchievements: %d of %d ROMs have achievements", with, count);
   SDL_free(items);
   SDL_UnlockMutex(ra.run);
   return ok && !(abort && SDL_GetAtomicInt(abort));
}

/* --- A game's list ------------------------------------------------------ */

/* The list saved for the game, if it's for this account and these numbers.
 * Its file is a line about it, then the list. */
static char *saved_list(const char *path, const char *user, unsigned total, unsigned unlocked)
{
   char *text = SDL_LoadFile(path, NULL), *line, *list = NULL;
   struct json *about;
   SDL_Time now = 0;

   if (!text || !(line = SDL_strchr(text, '\n')))
   {
      SDL_free(text);
      return NULL;
   }
   *line++ = '\0';
   SDL_GetCurrentTime(&now);
   about = json_parse(text);
   if (about && SDL_strcmp(json_str(about, "user", ""), user) == 0
         && json_num(about, "total", -1) == total && json_num(about, "unlocked", -1) == unlocked
         && (double)(now / SDL_NS_PER_SECOND) - json_num(about, "time", 0) < LIST_AGE_S)
      list = SDL_strdup(line);
   json_free(about);
   SDL_free(text);
   return list;
}

static bool is_unlocked(const struct json *unlocks, unsigned id)
{
   const struct json *list = json_get(unlocks, "UserUnlocks"), *o;

   for (o = list ? list->child : NULL; o; o = o->next)
      if (o->type == JSON_NUMBER && (unsigned)o->num == id)
         return true;
   return false;
}

char *ra_list(unsigned id, unsigned total, unsigned unlocked, SDL_AtomicInt *abort)
{
   char path[1100], request[64], user[128], *list;
   struct json *game, *unlocks = NULL, *achievements, *o;
   SDL_IOStream *io;
   SDL_Time now = 0;
   bool first = true;

   if (!ra_user(user, sizeof(user)))
      return NULL;
   SDL_snprintf(path, sizeof(path), "%sachievements" PATH_SEP "%u.json", library_dir(), id);
   if ((list = saved_list(path, user, total, unlocked)))
      return list;

   SDL_snprintf(request, sizeof(request), "patch&g=%u", id);
   game = ask(request, abort);
   SDL_snprintf(request, sizeof(request), "unlocks&g=%u&h=0", id);
   if (!game || !(unlocks = ask(request, abort)) || !(io = SDL_IOFromDynamicMem()))
   {
      json_free(game);
      json_free(unlocks);
      return NULL;
   }
   SDL_WriteU8(io, '[');
   achievements = json_get(json_get(game, "PatchData"), "Achievements");
   for (o = achievements ? achievements->child : NULL; o; o = o->next)
   {
      unsigned aid = (unsigned)number(o, "ID", 0);

      /* 3: its own set, not those still being made. */
      if ((int)number(o, "Flags", 0) != 3 || !aid || aid >= FIRST_WARNING_ID)
         continue;
      SDL_IOprintf(io, "%s[%u,", first ? "" : ",", aid);
      json_write_string(io, json_str(o, "Title", ""));
      SDL_WriteU8(io, ',');
      json_write_string(io, json_str(o, "Description", ""));
      SDL_WriteU8(io, ',');
      json_write_string(io, json_str(o, "BadgeName", ""));
      SDL_IOprintf(io, ",%.2f,%d]", number(o, "Rarity", 0), is_unlocked(unlocks, aid));
      first = false;
   }
   SDL_WriteU8(io, ']');
   SDL_WriteU8(io, '\0');
   json_free(game);
   json_free(unlocks);
   list = SDL_strdup(SDL_GetPointerProperty(SDL_GetIOProperties(io),
            SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, ""));
   SDL_CloseIO(io);

   {
      char dir[1100];
      SDL_snprintf(dir, sizeof(dir), "%sachievements", library_dir());
      SDL_CreateDirectory(dir);
   }
   if (list && (io = SDL_IOFromFile(path, "wb")))
   {
      SDL_GetCurrentTime(&now);
      SDL_IOprintf(io, "{\"user\": ");
      json_write_string(io, user);
      SDL_IOprintf(io, ", \"total\": %u, \"unlocked\": %u, \"time\": %lld}\n", total, unlocked,
            (long long)(now / SDL_NS_PER_SECOND));
      SDL_WriteIO(io, list, SDL_strlen(list));
      SDL_CloseIO(io);
   }
   return list;
}
