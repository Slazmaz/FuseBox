#include "ra.h"
#include "credentials.h"
#include "http.h"
#include "json.h"

#include <SDL3/SDL.h>

#define API "https://retroachievements.org/dorequest.php"
#define KEY_USER "retroachievements_username"
#define KEY_TOKEN "retroachievements_token"
#define TIMEOUT_S 30

static struct
{
   char config[1024];
   /* Under lock. */
   SDL_Mutex *lock;
   char user[128], token[128];
} ra;

void ra_init(const char *config_dir)
{
   SDL_strlcpy(ra.config, config_dir, sizeof(ra.config));
   if (!ra.lock)
      ra.lock = SDL_CreateMutex();
   SDL_LockMutex(ra.lock);
   if (!credentials_get(ra.config, KEY_USER, ra.user, sizeof(ra.user))
         || !credentials_get(ra.config, KEY_TOKEN, ra.token, sizeof(ra.token)))
      ra.user[0] = ra.token[0] = '\0';
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

/* POST, parsed; NULL on a failure. */
static struct json *post(const char *fields, SDL_AtomicInt *abort)
{
   struct json *root = NULL;
   char *body = NULL;
   int status = http_request(API, fields, NULL, NULL, abort, &body, NULL, TIMEOUT_S);

   /* A refused login is JSON too, with a 401 or so. */
   if (body && status > 0)
      root = json_parse(body);
   if (body)
      SDL_memset(body, 0, SDL_strlen(body));
   SDL_free(body);
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
      root = post(fields, abort);
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
   SDL_LockMutex(ra.lock);
   SDL_zeroa(ra.user);
   SDL_zeroa(ra.token);
   credentials_set(ra.config, KEY_TOKEN, NULL);
   credentials_set(ra.config, KEY_USER, NULL);
   SDL_UnlockMutex(ra.lock);
}
