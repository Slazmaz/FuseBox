#include "update.h"
#include "http.h"
#include "json.h"
#include "platform.h"
#include "version.h"

#include <SDL3/SDL.h>

/* Set otherwise only to test. */
#ifndef UPDATE_API
#define UPDATE_API "https://api.github.com/repos/Slazmaz/FuseBox/releases/latest"
#endif

/* Between two checks (s): doubled by each, up to the hour GitHub counts
 * requests by, and short again once there were none for a while. */
#define WAIT_FIRST 60
#define WAIT_MOST 3600
#define WAIT_FORGET 7200

static struct
{
   SDL_Mutex *lock;
   char config[1100];
   /* When GitHub was last asked (seconds since 1970), and how long that lasts. */
   Sint64 asked;
   int wait;
   /* The latest release. */
   char version[32];
} up;

/* "v1.2.3", what's after it aside. */
static bool read_version(const char *text, int v[3])
{
   int i;

   if (*text == 'v' || *text == 'V')
      text++;
   for (i = 0; i < 3; i++)
   {
      char *end;

      v[i] = 0;
      if (*text < '0' || *text > '9')
         return i > 0;
      v[i] = (int)SDL_clamp(SDL_strtol(text, &end, 10), 0, 999999);
      text = *end == '.' ? end + 1 : "";
   }
   return true;
}

static bool is_newer(const char *version)
{
   int a[3], b[3], i;

   if (!read_version(version, a) || !read_version(FUSEBOX_VERSION, b))
      return false;
   for (i = 0; i < 3; i++)
      if (a[i] != b[i])
         return a[i] > b[i];
   return false;
}

static Sint64 now_s(void)
{
   SDL_Time now = 0;

   SDL_GetCurrentTime(&now);
   return SDL_NS_TO_SECONDS(now);
}

static void state_path(char *dst, size_t size)
{
   SDL_snprintf(dst, size, "%s" PATH_SEP "update.json", up.config);
}

/* Under the lock. */
static void save(void)
{
   char path[1200];
   SDL_IOStream *io;

   SDL_CreateDirectory(up.config);
   state_path(path, sizeof(path));
   if (!(io = SDL_IOFromFile(path, "wb")))
   {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Can't write %s: %s", path, SDL_GetError());
      return;
   }
   SDL_IOprintf(io, "{\n  \"asked\": %lld,\n  \"wait\": %d,\n  \"version\": ", (long long)up.asked,
         up.wait);
   json_write_string(io, up.version);
   SDL_IOprintf(io, "\n}\n");
   SDL_CloseIO(io);
}

void update_init(const char *config_dir)
{
   char path[1200], *text;
   struct json *root;

   up.lock = SDL_CreateMutex();
   SDL_strlcpy(up.config, config_dir, sizeof(up.config));
   state_path(path, sizeof(path));
   text = SDL_LoadFile(path, NULL);
   if ((root = text ? json_parse(text) : NULL))
   {
      up.asked = (Sint64)json_num(root, "asked", 0);
      up.wait = (int)SDL_clamp(json_num(root, "wait", 0), 0, WAIT_MOST);
      SDL_strlcpy(up.version, json_str(root, "version", ""), sizeof(up.version));
   }
   json_free(root);
   SDL_free(text);
}

int update_wait(float *done)
{
   SDL_Time now = 0;
   Sint64 gone;
   float part = 1;
   int left = 0;

   SDL_GetCurrentTime(&now);
   SDL_LockMutex(up.lock);
   gone = now - SDL_SECONDS_TO_NS(up.asked);
   /* Less than none: a clock that was set back. */
   if (gone >= 0 && gone < (Sint64)SDL_SECONDS_TO_NS(up.wait))
   {
      left = up.wait - (int)SDL_NS_TO_SECONDS(gone);
      part = (float)((double)gone / (double)SDL_SECONDS_TO_NS(up.wait));
   }
   SDL_UnlockMutex(up.lock);
   if (done)
      *done = part;
   return left;
}

void update_check(SDL_AtomicInt *abort, char *note, size_t size)
{
   Sint64 asked, now = now_s();
   struct json *root = NULL;
   char *body = NULL;
   int wait, status, v[3];

   /* Counted before it's asked: closing FuseBox meanwhile doesn't undo it. */
   SDL_LockMutex(up.lock);
   asked = up.asked;
   wait = up.wait;
   up.wait = !wait || now - asked >= WAIT_FORGET ? WAIT_FIRST : SDL_min(wait * 2, WAIT_MOST);
   up.asked = now;
   save();
   SDL_UnlockMutex(up.lock);

   status = http_request(UPDATE_API, NULL, "Accept: application/vnd.github+json", NULL, abort,
         &body, NULL, 20);
   if (status == 200)
      root = json_parse(body);

   SDL_LockMutex(up.lock);
   if (root && read_version(json_str(root, "tag_name", ""), v))
   {
      SDL_snprintf(up.version, sizeof(up.version), "%d.%d.%d", v[0], v[1], v[2]);
      if (is_newer(up.version))
         SDL_snprintf(note, size, "%s is available", up.version);
      else
         SDL_strlcpy(note, "Up to date", size);
   }
   /* No release yet, or none any more. */
   else if (status == 404)
   {
      up.version[0] = '\0';
      SDL_strlcpy(note, "Up to date", size);
   }
   else if (status == 403 || status == 429)
   {
      /* Asked too often, maybe by others on this network. */
      up.wait = WAIT_MOST;
      SDL_strlcpy(note, "GitHub has had too many requests", size);
   }
   else if (status < 0 && !(abort && SDL_GetAtomicInt(abort)))
   {
      /* It didn't get there: not counted. */
      up.asked = asked;
      up.wait = wait;
      SDL_strlcpy(note, "Can't reach GitHub", size);
   }
   else
      SDL_snprintf(note, size, "GitHub couldn't say (%d)", status);
   save();
   SDL_UnlockMutex(up.lock);
   json_free(root);
   SDL_free(body);
}

bool update_newer(char *version, size_t size)
{
   bool newer;

   SDL_LockMutex(up.lock);
   newer = is_newer(up.version);
   SDL_strlcpy(version, newer ? up.version : "", size);
   SDL_UnlockMutex(up.lock);
   return newer;
}
