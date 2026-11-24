#include "steamui.h"
#include "history.h"
#include "http.h"
#include "json.h"
#include "library.h"
#include "ra.h"
#include "scan.h"
#include "steam.h"
#include "steamui_js.h"

#include <SDL3/SDL.h>

/* On this computer: quick, or not there. */
#define TIMEOUT_S 15
/* The port Steam opens when nothing says which (background.c). */
#define STEAM_PORT 8080

void steamui_start(const struct steam_install *steam)
{
   int port = 0;

   /* Another one each time: not to be found by what looks for a usual one. */
   if (steam_menus())
      port = 20000 + (int)SDL_rand(40000);
   if (port || steam_port())
      steam_set_port(port);
   platform_steam_start(steam, port);
}

/* The address of Steam's interface to send scripts to, at that port. */
static bool find_at(int port, char *url, size_t size, SDL_AtomicInt *abort)
{
   char list[64], prefix[64];
   struct json *root, *o;
   char *body = NULL;

   url[0] = '\0';
   if (!port)
      return false;
   SDL_snprintf(list, sizeof(list), "http://127.0.0.1:%d/json", port);
   SDL_snprintf(prefix, sizeof(prefix), "ws://127.0.0.1:%d/", port);
   if (http_request(list, NULL, NULL, NULL, abort, &body, NULL, TIMEOUT_S) != 200)
   {
      SDL_free(body);
      return false;
   }
   root = json_parse(body);
   SDL_free(body);
   for (o = root && root->type == JSON_ARRAY ? root->child : NULL; o; o = o->next)
      /* By its address too: 8080 is many programs' port. */
      if (SDL_strcmp(json_str(o, "title", ""), "SharedJSContext") == 0
            && SDL_strncmp(json_str(o, "url", ""), "https://steamloopback.host/", 27) == 0)
      {
         const char *ws = json_str(o, "webSocketDebuggerUrl", "");
         /* Never elsewhere, whatever answered. */
         if (SDL_strncmp(ws, prefix, SDL_strlen(prefix)) == 0)
            SDL_strlcpy(url, ws, size);
      }
   json_free(root);
   return url[0] != '\0';
}

static bool find_target(char *url, size_t size, SDL_AtomicInt *abort)
{
   return find_at(steam_port(), url, size, abort) || find_at(STEAM_PORT, url, size, abort);
}

/* Runs the script there; what it returned (text) in result. */
static bool evaluate(const char *url, const char *script, char *result, size_t size,
      SDL_AtomicInt *abort)
{
   SDL_IOStream *io = SDL_IOFromDynamicMem();
   struct json *root, *value;
   char *answer = NULL;
   bool ok = false;

   result[0] = '\0';
   if (!io)
      return false;
   SDL_IOprintf(io, "{\"id\":1,\"method\":\"Runtime.evaluate\",\"params\":{\"returnByValue\":true,"
         "\"awaitPromise\":true,\"expression\":");
   json_write_string(io, script);
   SDL_IOprintf(io, "}}");
   SDL_WriteU8(io, '\0');
   answer = http_ws_ask(url, SDL_GetPointerProperty(SDL_GetIOProperties(io),
            SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, ""), abort, TIMEOUT_S);
   SDL_CloseIO(io);
   if (!answer)
      return false;
   root = json_parse(answer);
   SDL_free(answer);
   value = json_get(json_get(root, "result"), "result");
   if (value && !json_get(json_get(root, "result"), "exceptionDetails"))
   {
      SDL_strlcpy(result, json_str(value, "value", ""), size);
      ok = true;
   }
   else
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Steam's interface refused a script");
   json_free(root);
   return ok;
}

/* A shortcut with achievements to show. */
struct shown
{
   Uint32 appid;
   unsigned id, total, unlocked;
};

static struct shown *collect(int *count)
{
   struct shown *games;
   bool shown = ra_active();
   int i, n = 0;

   library_lock();
   games = SDL_calloc((size_t)SDL_max(1, library_count()), sizeof(*games));
   for (i = 0; games && shown && i < library_count(); i++)
   {
      const struct game *g = library_game(i);
      if (!g->steam_appid || !g->ra_id || !g->ra_total || !g->rom[0])
         continue;
      games[n].appid = g->steam_appid;
      games[n].id = g->ra_id;
      games[n].total = g->ra_total;
      games[n++].unlocked = g->ra_unlocked;
   }
   library_unlock();
   *count = n;
   return games;
}

/* Gives Steam the names of the programs its games' buttons open. */
static void push_opens(const char *url, SDL_AtomicInt *abort)
{
   SDL_IOStream *io = SDL_IOFromDynamicMem();
   char exe[1024], name[96], result[64];
   int i, n = 0;

   if (!io)
      return;
   SDL_IOprintf(io, "__fusebox.opens({");
   library_lock();
   for (i = 0; i < library_count(); i++)
   {
      const struct game *g = library_game(i);

      if (!g->steam_appid || !scan_program(g, exe, sizeof(exe), name, sizeof(name)))
         continue;
      SDL_IOprintf(io, "%s\"%u\":", n++ ? "," : "", (unsigned)g->steam_appid);
      json_write_string(io, name);
   }
   library_unlock();
   SDL_IOprintf(io, "})");
   SDL_WriteU8(io, '\0');
   evaluate(url, SDL_GetPointerProperty(SDL_GetIOProperties(io),
            SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, ""), result, sizeof(result), abort);
   SDL_CloseIO(io);
}

/* Both on the same day here. */
static bool same_day(Sint64 a, Sint64 b)
{
   SDL_DateTime x, y;

   return SDL_TimeToDateTime(SDL_SECONDS_TO_NS(a), &x, true)
      && SDL_TimeToDateTime(SDL_SECONDS_TO_NS(b), &y, true)
      && x.year == y.year && x.month == y.month && x.day == y.day;
}

/* Gives Steam a game's activity: who unlocked what when (wins: ra_activity's,
 * of RetroAchievements' game id), and when it was added and first played.
 * Returns when Steam says it was last played (seconds since 1970). */
static Sint64 push_activity(const char *url, Uint32 appid, unsigned id, const char *wins,
      SDL_AtomicInt *abort)
{
   char result[64], *script = NULL;
   Sint64 added, played, last = 0;
   int again;

   history_get(library_dir(), appid, &added, &played);
   if (!added && (added = steam_art_time(appid)))
   {
      history_found(library_dir(), appid, added);
      played = -1;
   }
   for (again = 0; again < 2; again++)
   {
      result[0] = '\0';
      if (SDL_asprintf(&script, "__fusebox.activity(%u,%u,%s,%lld,%lld)", (unsigned)appid, id,
               wins ? wins : "[]", (long long)added, (long long)SDL_max(played, 0)) > 0)
         evaluate(url, script, result, sizeof(result), abort);
      SDL_free(script);
      script = NULL;
      last = (Sint64)SDL_strtoll(result, NULL, 10);
      /* Last played the day it was added: first played then too. */
      if (!added || played > 0 || last < added || !same_day(last, added))
         break;
      played = last;
      history_played(library_dir(), appid, played);
   }
   return last;
}

/* Those without achievements: what FuseBox knows of them itself. list: the
 * others. Their app ids into acts. */
static void push_histories(const char *url, const struct shown *list, int count, SDL_IOStream *acts,
      bool *first, SDL_AtomicInt *abort)
{
   Uint32 *apps = NULL;
   int i, k, n = 0;

   library_lock();
   apps = SDL_calloc((size_t)SDL_max(1, library_count()), sizeof(*apps));
   for (i = 0; apps && i < library_count(); i++)
   {
      const struct game *g = library_game(i);

      for (k = 0; k < count && list[k].appid != g->steam_appid; k++)
         ;
      if (g->steam_appid && k == count && SDL_strcmp(g->source, "Steam") != 0)
         apps[n++] = g->steam_appid;
   }
   library_unlock();
   for (i = 0; i < n && !(abort && SDL_GetAtomicInt(abort)); i++)
   {
      push_activity(url, apps[i], 0, NULL, abort);
      SDL_IOprintf(acts, "%s%u", *first ? "" : ",", (unsigned)apps[i]);
      *first = false;
   }
   SDL_free(apps);
}

enum steamui steamui_push(bool only_friends, int *games, SDL_AtomicInt *abort)
{
   char url[512], result[64];
   struct shown *list;
   SDL_IOStream *keep, *acts;
   bool first = true;
   int i, count, done = 0;

   *games = 0;
   if (!find_target(url, sizeof(url), abort))
      return STEAMUI_NO_PORT;
   if (!evaluate(url, STEAMUI_JS, result, sizeof(result), abort)
         || SDL_strcmp(result, "wait") == 0)
      return STEAMUI_WAIT;
   if (SDL_strcmp(result, "ready") != 0)
      return STEAMUI_UNSUPPORTED;
   push_opens(url, abort);
   keep = SDL_IOFromDynamicMem();
   acts = SDL_IOFromDynamicMem();
   if (!(list = collect(&count)) || !keep || !acts)
   {
      SDL_CloseIO(keep);
      SDL_CloseIO(acts);
      SDL_free(list);
      return STEAMUI_OK;
   }
   SDL_IOprintf(keep, "__fusebox.keep([");
   for (i = 0; i < count && !(abort && SDL_GetAtomicInt(abort)); i++)
   {
      char *achievements = ra_list(list[i].id, list[i].total, list[i].unlocked, abort);
      char *script = NULL, *friends, *wins;

      /* One whose list can't be had now keeps what Steam has. */
      SDL_IOprintf(keep, "%s%u", i ? "," : "", (unsigned)list[i].appid);
      if (achievements && (only_friends || (SDL_asprintf(&script, "__fusebox.set(%u,%s)",
                     (unsigned)list[i].appid, achievements) > 0
                  && evaluate(url, script, result, sizeof(result), abort))))
         done++;
      SDL_free(script);
      script = NULL;
      /* Before the friends: it tells more of them. */
      wins = achievements ? ra_activity(list[i].id, achievements, abort) : NULL;
      if (achievements && (friends = ra_friends(list[i].id, achievements, abort)))
      {
         if (SDL_asprintf(&script, "__fusebox.friends(%u,%u,%s)", (unsigned)list[i].appid,
                  list[i].id, friends) > 0)
            evaluate(url, script, result, sizeof(result), abort);
         SDL_free(script);
         script = NULL;
         SDL_free(friends);
      }
      /* What can't be had now stays as Steam has it. */
      SDL_IOprintf(acts, "%s%u", first ? "" : ",", (unsigned)list[i].appid);
      first = false;
      if (wins)
      {
         Sint64 last = push_activity(url, list[i].appid, list[i].id, wins, abort);
         char *boards = ra_boards(list[i].id, achievements, last, abort);

         if (boards && SDL_asprintf(&script, "__fusebox.boards(%u,%s)", (unsigned)list[i].appid,
                  boards) > 0)
            evaluate(url, script, result, sizeof(result), abort);
         SDL_free(script);
         SDL_free(boards);
      }
      SDL_free(wins);
      SDL_free(achievements);
   }
   push_histories(url, list, count, acts, &first, abort);
   SDL_WriteU8(acts, '\0');
   SDL_IOprintf(keep, "],[%s])", (const char *)SDL_GetPointerProperty(SDL_GetIOProperties(acts),
            SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, ""));
   SDL_CloseIO(acts);
   SDL_WriteU8(keep, '\0');
   if (!(abort && SDL_GetAtomicInt(abort)))
      evaluate(url, SDL_GetPointerProperty(SDL_GetIOProperties(keep),
               SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, ""), result, sizeof(result), abort);
   SDL_CloseIO(keep);
   SDL_free(list);
   *games = done;
   SDL_Log("Steam shows the achievements of %d games", done);
   return STEAMUI_OK;
}

enum steamui steamui_check(SDL_AtomicInt *abort)
{
   char url[512], result[64];

   if (!find_target(url, sizeof(url), abort))
      return STEAMUI_NO_PORT;
   return evaluate(url, "window.__fusebox && __fusebox.version === 28"
         " && __fusebox.me === App.m_CurrentUser.strSteamID"
         " && __fusebox.whole() ? 'shown' : ''", result, sizeof(result), abort)
      && result[0] ? STEAMUI_OK : STEAMUI_WAIT;
}

bool steamui_asked(char *what, size_t size, SDL_AtomicInt *abort)
{
   char url[512];

   /* Less long than an answer may take. */
   return find_target(url, sizeof(url), abort)
      && evaluate(url, "window.__fusebox && __fusebox.asked ? __fusebox.asked(10000) : ''", what, size,
            abort)
      && what[0];
}

void steamui_clear(SDL_AtomicInt *abort)
{
   char url[512], result[64];

   if (find_target(url, sizeof(url), abort))
      evaluate(url, "window.__fusebox ? __fusebox.undo() : ''", result, sizeof(result), abort);
}
