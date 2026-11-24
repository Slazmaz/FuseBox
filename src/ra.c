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
/* The friends' list asked for less than this ago isn't asked for again. */
#define FRIENDS_MS (2 * 60 * 1000)
/* Who played a game is looked up again after this (seconds), by this many
 * of its most common achievements. */
#define PLAYED_AGE_S (24 * 3600)
#define COMMON 3
/* Seen playing a game this long ago (seconds), a friend still is. */
#define NOW_S (5 * 60)
/* An achievement's winners: so many are looked through for the user, so many
 * achievements a call, with a pause before each (RetroAchievements refuses
 * bursts). Friends' are looked up again after a day when one has
 * played since, else after a week (seconds). */
#define WINNERS 500
#define WON_ASKS 25
#define WON_PAUSE_MS 300
#define WON_AGE_S (24 * 3600)
#define WON_OLD_S (7 * 24 * 3600)

static struct
{
   char config[1024];
   /* Under lock. */
   SDL_Mutex *lock;
   char user[128], token[128];
   bool in_steam, autostart, made_file;
   int port;

   /* One refresh at a time; what it keeps: when each console's progress
    * was last asked for (ms), and whether its games were. */
   SDL_Mutex *run;
   Uint64 asked[CONSOLES];
   bool listed[CONSOLES];

   /* Under friends_lock: RetroAchievements' answer, whose, and when (ms). */
   SDL_Mutex *friends_lock;
   struct json *friends;
   char friends_of[128];
   Uint64 friends_at;
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
      ra.friends_lock = SDL_CreateMutex();
   }
   SDL_LockMutex(ra.lock);
   if (!credentials_get(ra.config, KEY_USER, ra.user, sizeof(ra.user))
         || !credentials_get(ra.config, KEY_TOKEN, ra.token, sizeof(ra.token)))
      ra.user[0] = ra.token[0] = '\0';
   settings_path(path, sizeof(path));
   text = SDL_LoadFile(path, NULL);
   root = text ? json_parse(text) : NULL;
   ra.in_steam = !root || json_bool(root, "in_steam", true);
   ra.autostart = !root || json_bool(root, "autostart", true);
   ra.made_file = root && json_bool(root, "made_file", false);
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
   SDL_IOprintf(io, "{\n  \"in_steam\": %s,\n  \"autostart\": %s,\n  \"made_file\": %s,\n"
         "  \"port\": %d\n}\n", ra.in_steam ? "true" : "false", ra.autostart ? "true" : "false",
         ra.made_file ? "true" : "false", ra.port);
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

bool ra_autostart(void)
{
   bool on;

   SDL_LockMutex(ra.lock);
   on = ra.autostart;
   SDL_UnlockMutex(ra.lock);
   return on;
}

void ra_set_autostart(bool on)
{
   SDL_LockMutex(ra.lock);
   ra.autostart = on;
   save_settings();
   SDL_UnlockMutex(ra.lock);
}

bool ra_made_file(void)
{
   bool made;

   SDL_LockMutex(ra.lock);
   made = ra.made_file;
   SDL_UnlockMutex(ra.lock);
   return made;
}

void ra_set_made_file(bool made)
{
   SDL_LockMutex(ra.lock);
   ra.made_file = made;
   save_settings();
   SDL_UnlockMutex(ra.lock);
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

/* --- Friends ------------------------------------------------------------ */

/* Under friends_lock. */
static struct json *friend_list(const char *user, SDL_AtomicInt *abort)
{
   struct json *root;

   if (ra.friends && SDL_strcmp(ra.friends_of, user) == 0
         && SDL_GetTicks() - ra.friends_at < FRIENDS_MS)
      return json_get(ra.friends, "Friends");
   if (!(root = ask("getfriendlist", abort)))
      return NULL;
   json_free(ra.friends);
   ra.friends = root;
   SDL_strlcpy(ra.friends_of, user, sizeof(ra.friends_of));
   ra.friends_at = SDL_GetTicks();
   return json_get(root, "Friends");
}

/* The ids of the list's most common achievements. */
static int most_common(const char *list, unsigned ids[COMMON])
{
   struct json *root = json_parse(list), *o;
   double percent[COMMON];
   int n = 0, i;

   for (o = root ? root->child : NULL; o; o = o->next)
   {
      const struct json *v = o->child;
      unsigned id = v ? (unsigned)v->num : 0;
      double p;

      for (i = 0; v && i < 4; i++)
         v = v->next;
      if (!v)
         continue;
      p = v->num;
      for (i = n; i > 0 && percent[i - 1] < p; i--)
         if (i < COMMON)
         {
            percent[i] = percent[i - 1];
            ids[i] = ids[i - 1];
         }
      if (i < COMMON)
      {
         percent[i] = p;
         ids[i] = id;
         n = SDL_min(n + 1, COMMON);
      }
   }
   json_free(root);
   return n;
}

static bool has_name(const struct json *names, const char *name)
{
   const struct json *o;

   for (o = names ? names->child : NULL; o; o = o->next)
      if (o->type == JSON_STRING && SDL_strcasecmp(o->str, name) == 0)
         return true;
   return false;
}

/* The names of those who played the game, {"names": [...]}: from its file
 * while that's new. */
static struct json *who_played(unsigned id, const char *list, const char *user,
      SDL_AtomicInt *abort)
{
   char path[1100], request[96];
   unsigned ids[COMMON];
   SDL_Time now = 0;
   SDL_IOStream *io;
   struct json *root = NULL;
   char *text;
   int n, i;
   bool first = true, ok = true;

   SDL_GetCurrentTime(&now);
   SDL_snprintf(path, sizeof(path), "%sachievements" PATH_SEP "%u-friends.json", library_dir(), id);
   if ((text = SDL_LoadFile(path, NULL)))
   {
      root = json_parse(text);
      SDL_free(text);
      if (root && SDL_strcmp(json_str(root, "user", ""), user) == 0
            && (double)(now / SDL_NS_PER_SECOND) - json_num(root, "time", 0) < PLAYED_AGE_S)
         return root;
      json_free(root);
      root = NULL;
   }
   if (!(io = SDL_IOFromDynamicMem()))
      return NULL;
   SDL_IOprintf(io, "{\"user\": ");
   json_write_string(io, user);
   SDL_IOprintf(io, ", \"time\": %lld, \"names\": [", (long long)(now / SDL_NS_PER_SECOND));
   n = most_common(list, ids);
   for (i = 0; i < n && ok; i++)
   {
      struct json *won, *o;

      SDL_snprintf(request, sizeof(request), "achievementwondata&a=%u&f=1&o=0&c=100", ids[i]);
      if (!(won = ask(request, abort)))
      {
         ok = false;
         break;
      }
      o = json_get(json_get(won, "Response"), "RecentWinner");
      for (o = o ? o->child : NULL; o; o = o->next)
      {
         if (!first)
            SDL_WriteU8(io, ',');
         json_write_string(io, json_str(o, "User", ""));
         first = false;
      }
      json_free(won);
   }
   SDL_IOprintf(io, "]}");
   SDL_WriteU8(io, '\0');
   if (ok)
   {
      text = SDL_GetPointerProperty(SDL_GetIOProperties(io),
            SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, NULL);
      SDL_SaveFile(path, text, SDL_strlen(text));
      root = json_parse(text);
   }
   SDL_CloseIO(io);
   return root;
}

/* --- Who unlocked what when ---------------------------------------------- */

/* Where an achievement of the user's stands. */
enum
{
   /* Not seen unlocked yet; unlocked before FuseBox looked, and not looked
    * up yet. Else when, 0 if that can't be known. */
   MINE_NEW = -2,
   MINE_OLD = -1
};

struct won
{
   unsigned id;
   bool unlocked;
   Sint64 mine;
   /* When friends' was looked up. */
   Sint64 asked;
};

struct win
{
   unsigned id;
   Sint64 time;
   char who[64];
};

static void activity_path(unsigned id, char *dst, size_t size)
{
   SDL_snprintf(dst, size, "%sachievements" PATH_SEP "%u-activity.json", library_dir(), id);
}

/* The file, if it's the user's; NULL. */
static struct json *saved_activity(unsigned id, const char *user)
{
   char path[1100], *text;
   struct json *root;

   activity_path(id, path, sizeof(path));
   if (!platform_exists(path) || !(text = SDL_LoadFile(path, NULL)))
      return NULL;
   root = json_parse(text);
   SDL_free(text);
   if (root && SDL_strcmp(json_str(root, "user", ""), user) != 0)
   {
      json_free(root);
      root = NULL;
   }
   return root;
}

static bool in_activity(const struct json *saved, const char *name)
{
   const struct json *o = json_get(saved, "friends");

   for (o = o ? o->child : NULL; o; o = o->next)
      if (o->child && o->child->next && o->child->next->type == JSON_STRING
            && SDL_strcasecmp(o->child->next->str, name) == 0)
         return true;
   return false;
}

static struct won *won_of(struct won *won, int count, const struct json *pair)
{
   int i;

   for (i = 0; pair->child && pair->child->next && i < count; i++)
      if (won[i].id == (unsigned)pair->child->num)
         return &won[i];
   return NULL;
}

/* An achievement's winners, the last first; friends: only those. */
static struct json *winners(unsigned id, bool friends, SDL_AtomicInt *abort)
{
   char request[96];

   SDL_Delay(WON_PAUSE_MS);
   SDL_snprintf(request, sizeof(request), "achievementwondata&a=%u&f=%d&o=0&c=%d", id, friends,
         friends ? 100 : WINNERS);
   return ask(request, abort);
}

static const struct json *first_winner(const struct json *root)
{
   const struct json *list = json_get(json_get(root, "Response"), "RecentWinner");
   return list ? list->child : NULL;
}

/* The user's, of those that have none yet. False when RetroAchievements
 * couldn't be asked. */
static bool find_mine(struct won *won, int count, const char *user, int *asks, SDL_AtomicInt *abort)
{
   int pass, i;

   /* The new ones first: they're the last winners now. */
   for (pass = MINE_NEW; pass <= MINE_OLD; pass++)
      for (i = 0; i < count && *asks < WON_ASKS; i++)
      {
         const struct json *o;
         struct json *root;
         SDL_Time now = 0;
         Sint64 time = 0;

         if (!won[i].unlocked || won[i].mine != pass)
            continue;
         ++*asks;
         if (!(root = winners(won[i].id, false, abort)))
            return false;
         for (o = first_winner(root); o && !time; o = o->next)
            if (SDL_strcasecmp(json_str(o, "User", ""), user) == 0)
               time = (Sint64)number(o, "DateAwarded", 0);
         json_free(root);
         SDL_GetCurrentTime(&now);
         won[i].mine = time || pass == MINE_OLD ? time : now / SDL_NS_PER_SECOND;
      }
   return true;
}

static void add_win(struct win **wins, int *count, unsigned id, const char *who, Sint64 time)
{
   struct win *grown;

   if (!who[0] || time <= 0)
      return;
   if ((*count & (*count - 1)) == 0)
   {
      if (!(grown = SDL_realloc(*wins, sizeof(*grown) * (size_t)(*count ? *count * 2 : 1))))
         return;
      *wins = grown;
   }
   (*wins)[*count].id = id;
   (*wins)[*count].time = time;
   SDL_strlcpy((*wins)[(*count)++].who, who, sizeof((*wins)->who));
}

static void drop_wins(struct win *wins, int *count, unsigned id)
{
   int i, n = 0;

   for (i = 0; i < *count; i++)
      if (wins[i].id != id)
         wins[n++] = wins[i];
   *count = n;
}

/* Whether any friend played the game, and when one last did. */
static bool friends_play(unsigned id, const char *list, const char *user, Sint64 *last,
      SDL_AtomicInt *abort)
{
   struct json *friends, *played, *o;
   bool any = false;

   *last = 0;
   SDL_LockMutex(ra.friends_lock);
   friends = friend_list(user, abort);
   for (o = friends ? friends->child : NULL; o; o = o->next)
      if ((unsigned)number(o, "LastGameId", 0) == id)
      {
         *last = SDL_max(*last, (Sint64)number(o, "LastSeenTime", 0));
         any = true;
      }
   if (friends && friends->child && !any && (played = who_played(id, list, user, abort)))
   {
      o = json_get(played, "names");
      any = o && o->child;
      json_free(played);
   }
   SDL_UnlockMutex(ra.friends_lock);
   return any;
}

static void save_activity(unsigned id, const char *user, const struct won *won, int count,
      const struct win *wins, int win_count)
{
   char path[1100], tmp[1120];
   SDL_IOStream *io;
   bool first = true;
   int i;

   SDL_snprintf(path, sizeof(path), "%sachievements", library_dir());
   SDL_CreateDirectory(path);
   activity_path(id, path, sizeof(path));
   SDL_snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   if (!(io = SDL_IOFromFile(tmp, "wb")))
      return;
   SDL_IOprintf(io, "{\"user\": ");
   json_write_string(io, user);
   SDL_IOprintf(io, ", \"mine\": [");
   for (i = 0; i < count; i++)
      if (won[i].mine != MINE_NEW)
      {
         SDL_IOprintf(io, "%s[%u,%lld]", first ? "" : ",", won[i].id, (long long)won[i].mine);
         first = false;
      }
   SDL_IOprintf(io, "],\n\"asked\": [");
   for (i = 0, first = true; i < count; i++)
      if (won[i].asked)
      {
         SDL_IOprintf(io, "%s[%u,%lld]", first ? "" : ",", won[i].id, (long long)won[i].asked);
         first = false;
      }
   SDL_IOprintf(io, "],\n\"friends\": [");
   for (i = 0; i < win_count; i++)
   {
      SDL_IOprintf(io, "%s[%u,", i ? "," : "", wins[i].id);
      json_write_string(io, wins[i].who);
      SDL_IOprintf(io, ",%lld]", (long long)wins[i].time);
   }
   SDL_IOprintf(io, "]}\n");
   if (SDL_CloseIO(io))
      SDL_RenamePath(tmp, path);
}

char *ra_activity(unsigned id, const char *list, SDL_AtomicInt *abort)
{
   struct json *root = json_parse(list), *saved, *o;
   struct won *won = NULL, *w;
   struct win *wins = NULL;
   char user[128], *out = NULL;
   int i, count = 0, win_count = 0, asks = 0;
   SDL_IOStream *io;
   SDL_Time now = 0;
   Sint64 played;
   bool first = true, ok;

   for (o = root ? root->child : NULL; o; o = o->next)
      count++;
   if (!ra_user(user, sizeof(user)) || !count || !(won = SDL_calloc((size_t)count, sizeof(*won))))
   {
      json_free(root);
      return NULL;
   }
   for (o = root->child, i = 0; o; o = o->next, i++)
   {
      const struct json *v = o->child;
      int k;

      won[i].id = v ? (unsigned)v->num : 0;
      for (k = 0; v && k < 5; k++)
         v = v->next;
      won[i].unlocked = v && v->num != 0;
      won[i].mine = MINE_NEW;
   }
   json_free(root);

   if ((saved = saved_activity(id, user)))
   {
      o = json_get(saved, "mine");
      for (o = o ? o->child : NULL; o; o = o->next)
         if ((w = won_of(won, count, o)))
            w->mine = (Sint64)o->child->next->num;
      o = json_get(saved, "asked");
      for (o = o ? o->child : NULL; o; o = o->next)
         if ((w = won_of(won, count, o)))
            w->asked = (Sint64)o->child->next->num;
      o = json_get(saved, "friends");
      for (o = o ? o->child : NULL; o; o = o->next)
         if (won_of(won, count, o) && o->child->next->type == JSON_STRING && o->child->next->next)
            add_win(&wins, &win_count, (unsigned)o->child->num, o->child->next->str,
                  (Sint64)o->child->next->next->num);
   }
   else
      for (i = 0; i < count; i++)
         if (won[i].unlocked)
            won[i].mine = MINE_OLD;

   ok = find_mine(won, count, user, &asks, abort);
   SDL_GetCurrentTime(&now);
   now /= SDL_NS_PER_SECOND;
   if (ok && friends_play(id, list, user, &played, abort))
      for (i = 0; i < count && asks < WON_ASKS; i++)
      {
         Sint64 age = now - won[i].asked;
         const struct json *f;

         if (!won[i].id || (won[i].asked && age < WON_OLD_S
                  && (age < WON_AGE_S || played <= won[i].asked)))
            continue;
         asks++;
         if (!(root = winners(won[i].id, true, abort)))
            break;
         drop_wins(wins, &win_count, won[i].id);
         for (f = first_winner(root); f; f = f->next)
            add_win(&wins, &win_count, won[i].id, json_str(f, "User", ""),
                  (Sint64)number(f, "DateAwarded", 0));
         json_free(root);
         won[i].asked = now;
      }
   if (asks || !saved)
      save_activity(id, user, won, count, wins, win_count);
   json_free(saved);

   if ((io = SDL_IOFromDynamicMem()))
   {
      SDL_WriteU8(io, '[');
      for (i = 0; i < count; i++)
         if (won[i].unlocked && won[i].mine > 0)
         {
            SDL_IOprintf(io, "%s[%u,\"\",%lld]", first ? "" : ",", won[i].id, (long long)won[i].mine);
            first = false;
         }
      for (i = 0; i < win_count; i++)
      {
         SDL_IOprintf(io, "%s[%u,", first ? "" : ",", wins[i].id);
         json_write_string(io, wins[i].who);
         SDL_IOprintf(io, ",%lld]", (long long)wins[i].time);
         first = false;
      }
      SDL_WriteU8(io, ']');
      SDL_WriteU8(io, '\0');
      out = SDL_strdup(SDL_GetPointerProperty(SDL_GetIOProperties(io),
               SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, "[]"));
      SDL_CloseIO(io);
   }
   SDL_free(won);
   SDL_free(wins);
   return out;
}

char *ra_friends(unsigned id, const char *list, SDL_AtomicInt *abort)
{
   struct json *friends, *played, *won, *o;
   char user[128], *out = NULL;
   SDL_Time now = 0;
   SDL_IOStream *io;
   bool first = true;

   if (!ra_user(user, sizeof(user)))
      return NULL;
   SDL_LockMutex(ra.friends_lock);
   friends = friend_list(user, abort);
   if (friends && !friends->child)
      out = SDL_strdup("[]");
   else if (friends && (io = SDL_IOFromDynamicMem()))
   {
      /* Without it, those seen playing are still known. */
      played = who_played(id, list, user, abort);
      won = saved_activity(id, user);
      SDL_GetCurrentTime(&now);
      SDL_WriteU8(io, '[');
      for (o = friends->child; o; o = o->next)
      {
         const char *name = json_str(o, "Friend", "");
         bool last = (unsigned)number(o, "LastGameId", 0) == id;
         bool on = (double)(now / SDL_NS_PER_SECOND) - number(o, "LastSeenTime", 0) < NOW_S;

         if (!name[0] || (!last && !has_name(json_get(played, "names"), name)
                  && !in_activity(won, name)))
            continue;
         SDL_IOprintf(io, "%s[", first ? "" : ",");
         json_write_string(io, name);
         SDL_WriteU8(io, ',');
         json_write_string(io, json_str(o, "AvatarUrl", ""));
         SDL_IOprintf(io, ",%d,", on && last);
         json_write_string(io, on && last ? json_str(o, "LastSeen", "") : "");
         SDL_WriteU8(io, ',');
         json_write_string(io, json_str(o, "LastGameTitle", ""));
         SDL_IOprintf(io, ",%.0f,%d,", number(o, "LastSeenTime", 0), on);
         json_write_string(io, json_str(o, "LastGameIconUrl", ""));
         SDL_WriteU8(io, ']');
         first = false;
      }
      SDL_WriteU8(io, ']');
      SDL_WriteU8(io, '\0');
      out = SDL_strdup(SDL_GetPointerProperty(SDL_GetIOProperties(io),
               SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, "[]"));
      SDL_CloseIO(io);
      json_free(played);
      json_free(won);
   }
   SDL_UnlockMutex(ra.friends_lock);
   return out;
}

/* --- Leaderboards -------------------------------------------------------- */

/* Friends looked up on each; lookups a call. */
#define BOARD_FRIENDS 8
#define BOARD_ASKS 25

struct board
{
   unsigned id, total;
   char title[128], desc[256], format[16];
   Sint64 asked;
};

struct place
{
   unsigned board, rank;
   double score;
   /* Empty: the user. */
   char who[64];
};

static void *grown(void *items, int count, size_t size)
{
   return (count & (count - 1)) ? items : SDL_realloc(items, size * (size_t)(count ? count * 2 : 1));
}

/* A score as its board shows it. */
static void format_score(const char *format, double score, char *dst, size_t size)
{
   Sint64 v = (Sint64)score, t;

   /* Frames, at 60 a second, and hundredths: as hundredths. */
   if (SDL_strcmp(format, "TIME") == 0 || SDL_strcmp(format, "FRAMES") == 0
         || SDL_strcmp(format, "MILLISECS") == 0)
   {
      t = SDL_strcmp(format, "MILLISECS") == 0 ? v : v * 100 / 60;
      if (t >= 360000)
         SDL_snprintf(dst, size, "%d:%02d:%02d.%02d", (int)(t / 360000), (int)(t / 6000 % 60),
               (int)(t / 100 % 60), (int)(t % 100));
      else
         SDL_snprintf(dst, size, "%d:%02d.%02d", (int)(t / 6000), (int)(t / 100 % 60), (int)(t % 100));
   }
   else if (SDL_strcmp(format, "SECS") == 0 || SDL_strcmp(format, "TIMESECS") == 0)
   {
      if (v >= 3600)
         SDL_snprintf(dst, size, "%d:%02d:%02d", (int)(v / 3600), (int)(v / 60 % 60), (int)(v % 60));
      else
         SDL_snprintf(dst, size, "%d:%02d", (int)(v / 60), (int)(v % 60));
   }
   else if (SDL_strcmp(format, "MINUTES") == 0 || SDL_strcmp(format, "SECS_AS_MINS") == 0)
   {
      t = SDL_strcmp(format, "MINUTES") == 0 ? v : v / 60;
      SDL_snprintf(dst, size, "%dh%02d", (int)(t / 60), (int)(t % 60));
   }
   else if (SDL_strncmp(format, "FIXED", 5) == 0 && format[5] >= '1' && format[5] <= '6')
   {
      double by = SDL_pow(10, format[5] - '0');
      SDL_snprintf(dst, size, "%.*f", format[5] - '0', score / by);
   }
   else
   {
      if (SDL_strcmp(format, "TENS") == 0)
         v *= 10;
      else if (SDL_strcmp(format, "HUNDREDS") == 0)
         v *= 100;
      else if (SDL_strcmp(format, "THOUSANDS") == 0)
         v *= 1000;
      SDL_snprintf(dst, size, "%lld", (long long)v);
   }
}

static int load_boards(const char *path, const char *user, struct board **boards,
      struct place **places, int *place_count, Sint64 *listed)
{
   char *text = platform_exists(path) ? SDL_LoadFile(path, NULL) : NULL;
   struct json *root = text ? json_parse(text) : NULL, *o;
   int n = 0;

   SDL_free(text);
   if (root && SDL_strcmp(json_str(root, "user", ""), user) == 0)
   {
      *listed = (Sint64)json_num(root, "time", 0);
      o = json_get(root, "boards");
      for (o = o ? o->child : NULL; o; o = o->next)
      {
         struct board *b = grown(*boards, n, sizeof(**boards));

         if (!b)
            break;
         *boards = b;
         b += n++;
         SDL_zerop(b);
         b->id = (unsigned)json_num(o, "id", 0);
         b->total = (unsigned)json_num(o, "total", 0);
         b->asked = (Sint64)json_num(o, "asked", 0);
         SDL_strlcpy(b->title, json_str(o, "title", ""), sizeof(b->title));
         SDL_strlcpy(b->desc, json_str(o, "desc", ""), sizeof(b->desc));
         SDL_strlcpy(b->format, json_str(o, "format", ""), sizeof(b->format));
      }
      o = json_get(root, "places");
      for (o = o ? o->child : NULL; o; o = o->next)
      {
         struct place *p = grown(*places, *place_count, sizeof(**places));

         if (!p)
            break;
         *places = p;
         p += (*place_count)++;
         SDL_zerop(p);
         p->board = (unsigned)json_num(o, "board", 0);
         p->rank = (unsigned)json_num(o, "rank", 0);
         p->score = json_num(o, "score", 0);
         SDL_strlcpy(p->who, json_str(o, "who", ""), sizeof(p->who));
      }
   }
   json_free(root);
   return n;
}

static void save_boards(const char *path, const char *user, Sint64 listed, const struct board *boards,
      int count, const struct place *places, int place_count)
{
   char tmp[1120];
   SDL_IOStream *io;
   int i;

   SDL_snprintf(tmp, sizeof(tmp), "%sachievements", library_dir());
   SDL_CreateDirectory(tmp);
   SDL_snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   if (!(io = SDL_IOFromFile(tmp, "wb")))
      return;
   SDL_IOprintf(io, "{\"user\": ");
   json_write_string(io, user);
   SDL_IOprintf(io, ", \"time\": %lld, \"boards\": [", (long long)listed);
   for (i = 0; i < count; i++)
   {
      SDL_IOprintf(io, "%s\n{\"id\": %u, \"total\": %u, \"asked\": %lld, \"title\": ", i ? "," : "",
            boards[i].id, boards[i].total, (long long)boards[i].asked);
      json_write_string(io, boards[i].title);
      SDL_IOprintf(io, ", \"desc\": ");
      json_write_string(io, boards[i].desc);
      SDL_IOprintf(io, ", \"format\": ");
      json_write_string(io, boards[i].format);
      SDL_IOprintf(io, "}");
   }
   SDL_IOprintf(io, "],\n\"places\": [");
   for (i = 0; i < place_count; i++)
   {
      SDL_IOprintf(io, "%s\n{\"board\": %u, \"rank\": %u, \"score\": %.0f, \"who\": ", i ? "," : "",
            places[i].board, places[i].rank, places[i].score);
      json_write_string(io, places[i].who);
      SDL_IOprintf(io, "}");
   }
   SDL_IOprintf(io, "]}\n");
   if (SDL_CloseIO(io))
      SDL_RenamePath(tmp, path);
}

/* The game's boards as RetroAchievements has them now, with what's known of
 * them; how many. -1 when it can't be asked. */
static int list_boards(unsigned id, struct board **boards, int count, SDL_AtomicInt *abort)
{
   char request[64];
   struct board *now = NULL;
   struct json *root, *o;
   int i, n = 0;

   SDL_snprintf(request, sizeof(request), "patch&g=%u", id);
   if (!(root = ask(request, abort)))
      return -1;
   o = json_get(json_get(root, "PatchData"), "Leaderboards");
   for (o = o ? o->child : NULL; o; o = o->next)
   {
      struct board *b;

      if (json_bool(o, "Hidden", false) || !(b = grown(now, n, sizeof(*now))))
         continue;
      now = b;
      b += n++;
      SDL_zerop(b);
      b->id = (unsigned)number(o, "ID", 0);
      for (i = 0; i < count; i++)
         if ((*boards)[i].id == b->id)
            *b = (*boards)[i];
      SDL_strlcpy(b->title, json_str(o, "Title", ""), sizeof(b->title));
      SDL_strlcpy(b->desc, json_str(o, "Description", ""), sizeof(b->desc));
      SDL_strlcpy(b->format, json_str(o, "Format", ""), sizeof(b->format));
   }
   json_free(root);
   SDL_free(*boards);
   *boards = now;
   return n;
}

/* The names of the friends who play the game. */
static int playing_friends(unsigned id, const char *list, const char *user,
      char names[BOARD_FRIENDS][64], SDL_AtomicInt *abort)
{
   struct json *friends, *played = NULL, *won = NULL, *o;
   int n = 0;

   SDL_LockMutex(ra.friends_lock);
   if ((friends = friend_list(user, abort)) && friends->child)
   {
      played = who_played(id, list, user, abort);
      won = saved_activity(id, user);
   }
   for (o = friends ? friends->child : NULL; o && n < BOARD_FRIENDS; o = o->next)
   {
      const char *name = json_str(o, "Friend", "");

      if (name[0] && ((unsigned)number(o, "LastGameId", 0) == id
               || has_name(json_get(played, "names"), name) || in_activity(won, name)))
         SDL_strlcpy(names[n++], name, 64);
   }
   SDL_UnlockMutex(ra.friends_lock);
   json_free(played);
   json_free(won);
   return n;
}

/* Where someone is on a board (who: a friend; empty for the user), into
 * places. False when RetroAchievements couldn't be asked. */
static bool find_place(struct board *b, const char *who, const char *user, struct place **places,
      int *count, SDL_AtomicInt *abort)
{
   char request[256], *escaped;
   const char *name = who[0] ? who : user;
   struct json *root = NULL, *o;

   SDL_Delay(WON_PAUSE_MS);
   /* Around whoever's asked for: a friend needs no login. */
   if (!who[0])
   {
      SDL_snprintf(request, sizeof(request), "lbinfo&i=%u&o=0&c=5", b->id);
      root = ask(request, abort);
   }
   else if ((escaped = http_escape(who)))
   {
      SDL_snprintf(request, sizeof(request), "r=lbinfo&i=%u&o=0&c=5&u=%s", b->id, escaped);
      SDL_free(escaped);
      root = post(request, abort, NULL);
   }
   if (!root || !(o = json_get(root, "LeaderboardData")))
   {
      json_free(root);
      return false;
   }
   b->total = (unsigned)number(o, "TotalEntries", 0);
   o = json_get(o, "Entries");
   for (o = o ? o->child : NULL; o; o = o->next)
   {
      struct place *p;

      if (SDL_strcasecmp(json_str(o, "User", ""), name) != 0)
         continue;
      if ((p = grown(*places, *count, sizeof(**places))))
      {
         *places = p;
         p += (*count)++;
         SDL_zerop(p);
         p->board = b->id;
         p->rank = (unsigned)number(o, "Rank", 0);
         p->score = number(o, "Score", 0);
         SDL_strlcpy(p->who, who, sizeof(p->who));
      }
      break;
   }
   json_free(root);
   return true;
}

char *ra_boards(unsigned id, const char *list, Sint64 last, SDL_AtomicInt *abort)
{
   char path[1100], user[128], names[BOARD_FRIENDS][64], *out = NULL;
   struct board *boards = NULL;
   struct place *places = NULL;
   int i, k, n, count, place_count = 0, friends = 0, asks = 0;
   SDL_IOStream *io;
   SDL_Time now = 0;
   Sint64 listed = 0;
   bool changed = false, ok = true, due = false, first = true;

   if (!ra_user(user, sizeof(user)))
      return NULL;
   SDL_snprintf(path, sizeof(path), "%sachievements" PATH_SEP "%u-boards.json", library_dir(), id);
   count = load_boards(path, user, &boards, &places, &place_count, &listed);
   SDL_GetCurrentTime(&now);
   now /= SDL_NS_PER_SECOND;
   if (now - listed > LIST_AGE_S)
   {
      if ((n = list_boards(id, &boards, count, abort)) < 0)
      {
         SDL_free(boards);
         SDL_free(places);
         return NULL;
      }
      count = n;
      listed = now;
      changed = true;
   }
#define DUE(b) (!(b).asked || last > (b).asked || now - (b).asked > WON_AGE_S)
   for (i = 0; i < count; i++)
      due |= DUE(boards[i]);
   if (due)
      friends = playing_friends(id, list, user, names, abort);
   for (i = 0; i < count && ok && asks < BOARD_ASKS && !(abort && SDL_GetAtomicInt(abort)); i++)
   {
      struct place *fresh = NULL;
      int fresh_count = 0;

      if (!DUE(boards[i]))
         continue;
      asks++;
      ok = find_place(&boards[i], "", user, &fresh, &fresh_count, abort);
      for (k = 0; k < friends && ok && boards[i].total; k++, asks++)
         ok = find_place(&boards[i], names[k], user, &fresh, &fresh_count, abort);
      if (ok)
      {
         boards[i].asked = now;
         for (k = n = 0; k < place_count; k++)
            if (places[k].board != boards[i].id)
               places[n++] = places[k];
         place_count = n;
         for (k = 0; k < fresh_count; k++)
         {
            struct place *p = SDL_realloc(places, sizeof(*p) * (size_t)(place_count + 1));
            if (!p)
               break;
            places = p;
            places[place_count++] = fresh[k];
         }
         changed = true;
      }
      SDL_free(fresh);
   }
#undef DUE
   if (changed)
      save_boards(path, user, listed, boards, count, places, place_count);

   if ((io = SDL_IOFromDynamicMem()))
   {
      SDL_WriteU8(io, '[');
      for (i = 0; i < count; i++)
      {
         bool any = false;

         for (k = 0; k < place_count; k++)
         {
            char score[64];

            if (places[k].board != boards[i].id)
               continue;
            if (!any)
            {
               SDL_IOprintf(io, "%s[%u,", first ? "" : ",", boards[i].id);
               json_write_string(io, boards[i].title);
               SDL_WriteU8(io, ',');
               json_write_string(io, boards[i].desc);
               SDL_IOprintf(io, ",%u,[", boards[i].total);
            }
            format_score(boards[i].format, places[k].score, score, sizeof(score));
            SDL_IOprintf(io, "%s[", any ? "," : "");
            json_write_string(io, places[k].who);
            SDL_WriteU8(io, ',');
            json_write_string(io, score);
            SDL_IOprintf(io, ",%u]", places[k].rank);
            any = true;
            first = false;
         }
         if (any)
            SDL_IOprintf(io, "]]");
      }
      SDL_WriteU8(io, ']');
      SDL_WriteU8(io, '\0');
      out = SDL_strdup(SDL_GetPointerProperty(SDL_GetIOProperties(io),
               SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, "[]"));
      SDL_CloseIO(io);
   }
   SDL_free(boards);
   SDL_free(places);
   return out;
}
