#include "steamui.h"
#include "history.h"
#include "http.h"
#include "json.h"
#include "lan.h"
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
/* Bigger images (animated ones) aren't sent to Steam's page. */
#define IMAGE_MAX (3 * 1024 * 1024)

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
      Uint32 appid = lan_app(g);
      if (!appid || !g->ra_id || !g->ra_total || !g->rom[0])
         continue;
      games[n].appid = appid;
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

      Uint32 appid = lan_app(g);

      if (!appid || !scan_program(g, exe, sizeof(exe), name, sizeof(name)))
         continue;
      SDL_IOprintf(io, "%s\"%u\":", n++ ? "," : "", (unsigned)appid);
      json_write_string(io, name);
   }
   library_unlock();
   SDL_IOprintf(io, "})");
   SDL_WriteU8(io, '\0');
   evaluate(url, SDL_GetPointerProperty(SDL_GetIOProperties(io),
            SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, ""), result, sizeof(result), abort);
   SDL_CloseIO(io);
}

/* What a game that isn't in Steam goes by there. */
static Uint32 other_id(const struct game *g)
{
   return fb_crc32(g->id, SDL_strlen(g->id));
}

/* The games friends can be invited to: those that start through FuseBox. And
 * those that are here, but not in Steam. */
static void push_lan(const char *url, SDL_AtomicInt *abort)
{
   SDL_IOStream *io = SDL_IOFromDynamicMem();
   char result[64];
   int i, n = 0;

   if (!io)
      return;
   SDL_IOprintf(io, "__fusebox.lan([");
   library_lock();
   for (i = 0; i < library_count(); i++)
   {
      const struct game *g = library_game(i);

      if (lan_app(g) && SDL_strcmp(g->source, "Steam") != 0)
      {
         SDL_IOprintf(io, "%s[%u,", n++ ? "," : "", (unsigned)lan_app(g));
         json_write_string(io, g->name);
         SDL_IOprintf(io, ",");
         json_write_string(io, g->own_name);
         SDL_IOprintf(io, "]");
      }
   }
   library_unlock();
   SDL_IOprintf(io, "],[");
   library_lock();
   for (i = n = 0; i < library_count(); i++)
   {
      const struct game *g = library_game(i);

      /* Uninstalled ones stay in the library. */
      if (lan_app(g) || !platform_exists(library_path(g)))
         continue;
      SDL_IOprintf(io, "%s[%u,", n++ ? "," : "", (unsigned)other_id(g));
      json_write_string(io, g->name);
      SDL_IOprintf(io, ",");
      json_write_string(io, g->own_name);
      SDL_IOprintf(io, "]");
   }
   library_unlock();
   SDL_IOprintf(io, "])");
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

      Uint32 appid = lan_app(g);

      for (k = 0; k < count && list[k].appid != appid; k++)
         ;
      if (appid && k == count && SDL_strcmp(g->source, "Steam") != 0)
         apps[n++] = appid;
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
   lan_read_once(steam_settings_dir());
   push_opens(url, abort);
   push_lan(url, abort);
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
   return evaluate(url, "window.__fusebox && __fusebox.version === " STEAMUI_VERSION
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

enum steamui steamui_ready(SDL_AtomicInt *abort)
{
   char url[512], result[64];

   if (!find_target(url, sizeof(url), abort))
      return STEAMUI_NO_PORT;
   if (!evaluate(url, STEAMUI_JS, result, sizeof(result), abort)
         || SDL_strcmp(result, "wait") == 0)
      return STEAMUI_WAIT;
   return SDL_strcmp(result, "ready") == 0 ? STEAMUI_OK : STEAMUI_UNSUPPORTED;
}

/* Runs what starts a game there; the app id it's run as, 0 if Steam can't. */
static Uint32 run_in_steam(const char *script, SDL_AtomicInt *abort)
{
   char url[512], result[64];

   if (!find_target(url, sizeof(url), abort)
         || !evaluate(url, STEAMUI_JS, result, sizeof(result), abort)
         || SDL_strcmp(result, "ready") != 0
         || !evaluate(url, script, result, sizeof(result), abort))
      return 0;
   return (Uint32)SDL_strtoul(result, NULL, 10);
}

Uint32 steamui_run(Uint32 appid, SDL_AtomicInt *abort)
{
   char script[64];

   SDL_snprintf(script, sizeof(script), "__fusebox.run(%u)", (unsigned)appid);
   return run_in_steam(script, abort);
}

Uint32 steamui_once(const char *name, const char *exe, const char *options, const char *icon,
      bool overlay, bool input, SDL_AtomicInt *abort)
{
   SDL_IOStream *io = SDL_IOFromDynamicMem();
   Uint32 appid;

   if (!io)
      return 0;
   SDL_IOprintf(io, "__fusebox.once(");
   json_write_string(io, name);
   SDL_IOprintf(io, ",");
   json_write_string(io, exe);
   SDL_IOprintf(io, ",");
   json_write_string(io, SDL_GetBasePath());
   SDL_IOprintf(io, ",");
   json_write_string(io, options);
   SDL_IOprintf(io, ",");
   json_write_string(io, icon);
   SDL_IOprintf(io, ",%s,%s)", overlay ? "true" : "false", input ? "true" : "false");
   SDL_WriteU8(io, '\0');
   appid = run_in_steam(SDL_GetPointerProperty(SDL_GetIOProperties(io),
            SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, ""), abort);
   SDL_CloseIO(io);
   return appid;
}

bool steamui_unonce(Uint32 appid, SDL_AtomicInt *abort)
{
   char url[512], script[128], result[64];

   /* Without the script in it (Steam was restarted) too. */
   SDL_snprintf(script, sizeof(script), "window.__fusebox && __fusebox.unonce ? __fusebox.unonce(%u)"
         " : (SteamClient.Apps.RemoveShortcut(%u), 'ok')", (unsigned)appid, (unsigned)appid);
   return find_target(url, sizeof(url), abort)
      && evaluate(url, script, result, sizeof(result), abort);
}

/* ---- Shortcuts ----------------------------------------------------------- */

/* What the scripts start with. */
#define APPS "(async () => { const a = SteamClient.Apps, wait = ms => new Promise(f => setTimeout(f, ms));" \
   " const has = app => appStore.GetAppOverviewByAppID(app); "

bool steamui_can_put(SDL_AtomicInt *abort)
{
   char url[512], result[64];

   return find_target(url, sizeof(url), abort)
      && evaluate(url, "['AddShortcut', 'RemoveShortcut', 'SetShortcutName', 'SetShortcutExe', 'SetShortcutIcon',"
            " 'SetShortcutStartDir', 'SetShortcutLaunchOptions', 'SetCustomArtworkForApp', 'RequestIconDataForApp',"
            " 'ToggleAllowDesktopConfiguration', 'RegisterForAppDetails']"
            ".every(f => typeof SteamClient.Apps[f] === 'function') && appStore.GetAppOverviewByAppID"
            " && App.m_CurrentUser.strSteamID ? 'yes' : ''", result, sizeof(result), abort)
      && result[0];
}

/* Runs it, and frees it. */
static bool run_script(SDL_IOStream *io, char *result, size_t size, SDL_AtomicInt *abort)
{
   char url[512];
   bool ok;

   result[0] = '\0';
   SDL_WriteU8(io, '\0');
   ok = find_target(url, sizeof(url), abort)
      && evaluate(url, SDL_GetPointerProperty(SDL_GetIOProperties(io),
               SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, ""), result, size, abort) && result[0];
   SDL_CloseIO(io);
   return ok;
}

Uint32 steamui_put(Uint32 appid, const char *name, const char *exe, const char *dir,
      const char *options, SDL_AtomicInt *abort)
{
   SDL_IOStream *io = SDL_IOFromDynamicMem();
   char result[64];

   if (!io)
      return 0;
   SDL_IOprintf(io, APPS "let app = %u; const name = ", (unsigned)appid);
   json_write_string(io, name);
   SDL_IOprintf(io, ", exe = ");
   json_write_string(io, exe);
   SDL_IOprintf(io, ", dir = ");
   json_write_string(io, dir);
   SDL_IOprintf(io, ", options = ");
   json_write_string(io, options);
   SDL_IOprintf(io, "; if (app) { if (!has(app)) return ''; }"
         " else { app = await a.AddShortcut(name, exe, '', ''); if (!app) return '';"
         " for (let i = 0; i < 100 && !has(app); i++) await wait(50); }"
         " a.SetShortcutName(app, name); a.SetShortcutExe(app, '\"' + exe + '\"');"
         " a.SetShortcutStartDir(app, '\"' + dir + '\"'); a.SetShortcutLaunchOptions(app, options);"
         " return String(app); })()");
   return run_script(io, result, sizeof(result), abort) ? (Uint32)SDL_strtoul(result, NULL, 10) : 0;
}

bool steamui_put_input(Uint32 appid, bool on, SDL_AtomicInt *abort)
{
   SDL_IOStream *io = SDL_IOFromDynamicMem();
   char result[64];

   if (!io)
      return false;
   /* It can only be turned around: first, which way it is. */
   SDL_IOprintf(io, APPS "const app = %u, d = await new Promise((f, no) => { setTimeout(() => f(null), 5000);"
         " const h = a.RegisterForAppDetails(app, x => { setTimeout(() => h.unregister(), 0); f(x); }); });"
         " if (!d || typeof d.bEnableAllowDesktopConfiguration !== 'boolean') return '';"
         " if (d.bEnableAllowDesktopConfiguration !== %s) a.ToggleAllowDesktopConfiguration(app);"
         " return 'ok'; })()", (unsigned)appid, on ? "true" : "false");
   return run_script(io, result, sizeof(result), abort);
}

bool steamui_put_icon(Uint32 appid, const char *path, SDL_AtomicInt *abort)
{
   SDL_IOStream *io = SDL_IOFromDynamicMem();
   char result[64];

   if (!io)
      return false;
   SDL_IOprintf(io, APPS "const app = %u; a.SetShortcutIcon(app, ", (unsigned)appid);
   json_write_string(io, path);
   /* The one it has is kept until it's asked for again. */
   SDL_IOprintf(io, "); await wait(200); const o = has(app); if (o) { o.icon_data = undefined;"
         " a.RequestIconDataForApp(app); } return 'ok'; })()");
   return run_script(io, result, sizeof(result), abort);
}

bool steamui_put_art(Uint32 appid, enum art_type type, const char *file, SDL_AtomicInt *abort)
{
   static const int kinds[ART_TYPES] = { 0, 3, 1, 2, -1 };
   SDL_IOStream *io = SDL_IOFromDynamicMem();
   const char *ext = SDL_strrchr(file, '.');
   char result[64];

   if (!io || !ext || kinds[type] < 0)
   {
      SDL_CloseIO(io);
      return false;
   }
   /* Steam's page has the grid's files: megabytes needn't be sent there. */
   SDL_IOprintf(io, APPS "const r = await fetch('/customimages/%s?v=' + Date.now()); if (!r.ok) return '';"
         " const b = new Uint8Array(await r.arrayBuffer()); let s = '';"
         " for (let i = 0; i < b.length; i += 32768) s += String.fromCharCode.apply(null, b.subarray(i, i + 32768));"
         " await a.SetCustomArtworkForApp(%u, btoa(s), '%s', %d); return 'ok'; })()", file, (unsigned)appid,
         ext + 1, kinds[type]);
   return run_script(io, result, sizeof(result), abort);
}

bool steamui_take_out(Uint32 appid, SDL_AtomicInt *abort)
{
   SDL_IOStream *io = SDL_IOFromDynamicMem();
   char result[64];

   if (!io)
      return false;
   SDL_IOprintf(io, APPS "a.RemoveShortcut(%u); return 'ok'; })()", (unsigned)appid);
   return run_script(io, result, sizeof(result), abort);
}

/* An image as an address that holds it; empty without one, or one that's too
 * big for that. */
static void write_image(SDL_IOStream *io, const char *path)
{
   static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   const char *type = NULL;
   size_t size = 0, i, n = 0;
   Uint8 *data = path[0] ? SDL_LoadFile(path, &size) : NULL;
   char out[4096];

   SDL_IOprintf(io, "'");
   if (data && size > 12 && size <= IMAGE_MAX)
   {
      if (SDL_memcmp(data, "\x89PNG", 4) == 0)
         type = "png";
      else if (SDL_memcmp(data, "\xFF\xD8\xFF", 3) == 0)
         type = "jpeg";
      else if (SDL_memcmp(data, "RIFF", 4) == 0 && SDL_memcmp(data + 8, "WEBP", 4) == 0)
         type = "webp";
   }
   if (type)
   {
      SDL_IOprintf(io, "data:image/%s;base64,", type);
      for (i = 0; i < size; i += 3)
      {
         Uint32 v = (Uint32)data[i] << 16 | (i + 1 < size ? (Uint32)data[i + 1] << 8 : 0)
            | (i + 2 < size ? data[i + 2] : 0);

         out[n++] = digits[v >> 18];
         out[n++] = digits[v >> 12 & 63];
         out[n++] = i + 1 < size ? digits[v >> 6 & 63] : '=';
         out[n++] = i + 2 < size ? digits[v & 63] : '=';
         if (n == sizeof(out) || i + 3 >= size)
         {
            SDL_WriteIO(io, out, n);
            n = 0;
         }
      }
   }
   SDL_IOprintf(io, "'");
   SDL_free(data);
}

bool steamui_art(Uint32 id, SDL_AtomicInt *abort)
{
   SDL_IOStream *io = SDL_IOFromDynamicMem();
   char url[512], result[64];
   bool found = false;
   int i;

   if (!io)
      return false;
   library_lock();
   for (i = 0; i < library_count() && !found; i++)
   {
      const struct game *g = library_game(i);

      if (g->steam_appid || other_id(g) != id)
         continue;
      found = true;
      SDL_IOprintf(io, "window.__fusebox && __fusebox.art ? __fusebox.art(%u,", (unsigned)id);
      write_image(io, g->art[ART_ICON].path);
      SDL_IOprintf(io, ",");
      write_image(io, g->art[ART_WIDE].path[0] ? g->art[ART_WIDE].path : g->art[ART_HERO].path);
      SDL_IOprintf(io, ") : ''");
      SDL_WriteU8(io, '\0');
   }
   library_unlock();
   found = found && find_target(url, sizeof(url), abort)
      && evaluate(url, SDL_GetPointerProperty(SDL_GetIOProperties(io),
               SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, ""), result, sizeof(result), abort);
   SDL_CloseIO(io);
   return found;
}

bool steamui_unlocks(Uint32 appid, SDL_AtomicInt *abort)
{
   char url[512], result[64], *script = NULL, *unlocks;
   unsigned id = 0;
   bool ok = false;
   int i;

   library_lock();
   for (i = 0; i < library_count() && !id; i++)
      if (lan_app(library_game(i)) == appid)
         id = library_game(i)->ra_id;
   library_unlock();
   if (!id || !(unlocks = ra_unlocks(id, abort)))
      return false;
   if (SDL_asprintf(&script, "window.__fusebox && __fusebox.unlocked ? __fusebox.unlocked(%u,%s) : ''",
            (unsigned)appid, unlocks) > 0)
      ok = find_target(url, sizeof(url), abort)
         && evaluate(url, script, result, sizeof(result), abort) && result[0];
   SDL_free(script);
   SDL_free(unlocks);
   return ok;
}

void steamui_invited(Uint64 who, const char *name, SDL_AtomicInt *abort)
{
   SDL_IOStream *io = SDL_IOFromDynamicMem();
   char url[512], clean[128], result[64];
   size_t i, n = 0;

   if (!io)
      return;
   /* One line, and nothing a chat would take for markup. */
   for (i = 0; name[i] && n + 1 < sizeof(clean); i++)
      if ((Uint8)name[i] >= ' ')
         clean[n++] = name[i] == '[' ? '(' : name[i] == ']' ? ')' : name[i];
   /* Not in the middle of a letter. */
   while (name[i] && n && ((Uint8)clean[n - 1] & 0x80))
      n--;
   clean[n] = '\0';
   SDL_IOprintf(io, "window.__fusebox ? __fusebox.invited('%llu',", (unsigned long long)who);
   json_write_string(io, clean);
   SDL_IOprintf(io, ") : ''");
   SDL_WriteU8(io, '\0');
   if (clean[0] && find_target(url, sizeof(url), abort))
      evaluate(url, SDL_GetPointerProperty(SDL_GetIOProperties(io),
               SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, ""), result, sizeof(result), abort);
   SDL_CloseIO(io);
}

void steamui_pass(Uint64 lobby, Uint64 host, SDL_AtomicInt *abort)
{
   char url[512], script[200], result[64];

   SDL_snprintf(script, sizeof(script), "window.__fusebox ? __fusebox.pass('%llu','%llu') : ''",
         (unsigned long long)lobby, (unsigned long long)host);
   if (find_target(url, sizeof(url), abort))
      evaluate(url, script, result, sizeof(result), abort);
}

void steamui_clear(SDL_AtomicInt *abort)
{
   char url[512], result[64];

   if (find_target(url, sizeof(url), abort))
      evaluate(url, "window.__fusebox ? __fusebox.undo() : ''", result, sizeof(result), abort);
}
