#include "http.h"

#include <SDL3/SDL.h>
#include <curl/curl.h>

static SDL_InitState init_state;
static bool ready;
static char default_agent[64];

/* Each thread keeps one handle, so its connections stay open between
 * requests (no new TLS handshake for every cover); all threads share DNS
 * answers and TLS sessions, which makes a new connection quicker too. */
static SDL_TLSID thread_curl;
static CURLSH *share;
static SDL_Mutex *share_locks[CURL_LOCK_DATA_LAST];

static void lock_share(CURL *curl, curl_lock_data data, curl_lock_access access, void *user)
{
   SDL_LockMutex(share_locks[data]);
}

static void unlock_share(CURL *curl, curl_lock_data data, void *user)
{
   SDL_UnlockMutex(share_locks[data]);
}

/* curl_global_init isn't safe to race: the first thread to get here does
 * it, others wait. */
static bool init(void)
{
   int i;

   if (SDL_ShouldInit(&init_state))
   {
      ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
      SDL_snprintf(default_agent, sizeof(default_agent), "FuseBox/0.1 (%s)", SDL_GetPlatform());
      for (i = 0; i < CURL_LOCK_DATA_LAST; i++)
         share_locks[i] = SDL_CreateMutex();
      if (ready && (share = curl_share_init()))
      {
         curl_share_setopt(share, CURLSHOPT_LOCKFUNC, lock_share);
         curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, unlock_share);
         curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
         curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
      }
      SDL_SetInitialized(&init_state, true);
   }
   return ready;
}

/* Once every thread that used HTTP has ended (their handles go with them). */
void http_quit(void)
{
   CURL *curl;
   int i;

   if (!SDL_ShouldQuit(&init_state))
      return;
   /* The calling thread's own handle, if it made requests. */
   if ((curl = SDL_GetTLS(&thread_curl)))
   {
      SDL_SetTLS(&thread_curl, NULL, NULL);
      curl_easy_cleanup(curl);
   }
   if (share)
      curl_share_cleanup(share);
   share = NULL;
   for (i = 0; i < CURL_LOCK_DATA_LAST; i++)
   {
      SDL_DestroyMutex(share_locks[i]);
      share_locks[i] = NULL;
   }
   if (ready)
      curl_global_cleanup();
   ready = false;
   SDL_SetInitialized(&init_state, false);
}

static void SDLCALL free_thread_curl(void *curl)
{
   curl_easy_cleanup(curl);
}

/* This thread's handle, reset to defaults (its connections kept). */
static CURL *thread_handle(void)
{
   CURL *curl = SDL_GetTLS(&thread_curl);

   if (curl)
   {
      curl_easy_reset(curl);
      return curl;
   }
   if (!(curl = curl_easy_init()))
      return NULL;
   if (!SDL_SetTLS(&thread_curl, curl, free_thread_curl))
   {
      curl_easy_cleanup(curl);
      return NULL;
   }
   return curl;
}

struct buffer
{
   char *data;
   size_t size, alloc;
   SDL_AtomicInt *abort;
};

static bool aborted(const struct buffer *b)
{
   return b->abort && SDL_GetAtomicInt(b->abort);
}

static size_t on_data(char *data, size_t size, size_t count, void *user)
{
   struct buffer *b = user;
   size_t bytes = size * count;

   if (aborted(b))
      return 0;
   if (b->size + bytes + 1 > b->alloc)
   {
      size_t alloc = SDL_max(b->alloc * 2, b->size + bytes + 1);
      char *grown = SDL_realloc(b->data, alloc);
      if (!grown)
         return 0;
      b->data = grown;
      b->alloc = alloc;
   }
   SDL_memcpy(b->data + b->size, data, bytes);
   b->size += bytes;
   b->data[b->size] = '\0';
   return bytes;
}

/* Lets closing FuseBox cut a transfer short. */
static int on_progress(void *user, curl_off_t dl_total, curl_off_t dl_now,
      curl_off_t ul_total, curl_off_t ul_now)
{
   return aborted(user);
}

/* Length of "https://host": failures are logged without path or query,
 * which can carry names or logins. */
static int host_length(const char *url)
{
   const char *p = SDL_strstr(url, "://");

   p = p ? p + 3 : url;
   while (*p && *p != '/' && *p != '?')
      p++;
   return (int)(p - url);
}

int http_request(const char *url, const char *post, const char *header,
      const char *user_agent, SDL_AtomicInt *abort, char **body, size_t *size,
      long timeout_s)
{
   struct buffer b = { NULL, 0, 0, abort };
   struct curl_slist *headers = NULL;
   CURL *curl;
   CURLcode res;
   long status = -1;

   *body = NULL;
   if (size)
      *size = 0;
   if (!init() || aborted(&b) || !(curl = thread_handle()))
      return -1;

   curl_easy_setopt(curl, CURLOPT_URL, url);
   if (share)
      curl_easy_setopt(curl, CURLOPT_SHARE, share);
   curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent ? user_agent : default_agent);
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
   /* JSON compresses well; images just pass through. */
   curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
   curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
   curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
   curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &b);
   curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
#ifdef _WIN32
   /* OpenSSL has no CA bundle next to fusebox.exe: use Windows' store. */
   curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
   if (header)
   {
      headers = curl_slist_append(NULL, header);
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   }
   if (post)
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);

   res = curl_easy_perform(curl);
   if (res == CURLE_OK)
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
   else if (!aborted(&b))
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "HTTP: %.*s: %s", host_length(url), url,
            curl_easy_strerror(res));
   /* The handle stays with the thread; it mustn't keep pointing at these. */
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
   curl_slist_free_all(headers);

   if (status < 0)
   {
      SDL_free(b.data);
      return -1;
   }
   *body = b.data ? b.data : SDL_strdup("");
   if (size)
      *size = b.size;
   return (int)status;
}

struct sink
{
   SDL_IOStream *io;
   SDL_AtomicInt *abort, *percent;
};

static size_t on_file(char *data, size_t size, size_t count, void *user)
{
   struct sink *s = user;

   return SDL_WriteIO(s->io, data, size * count);
}

static int on_file_progress(void *user, curl_off_t dl_total, curl_off_t dl_now,
      curl_off_t ul_total, curl_off_t ul_now)
{
   struct sink *s = user;

   if (s->percent)
      SDL_SetAtomicInt(s->percent, dl_total > 0 ? (int)(dl_now * 100 / dl_total) : -1);
   return s->abort && SDL_GetAtomicInt(s->abort);
}

int http_download(const char *url, const char *path, SDL_AtomicInt *abort,
      SDL_AtomicInt *percent)
{
   struct sink s = { NULL, abort, percent };
   CURL *curl;
   CURLcode res;
   long status = -1;

   if (percent)
      SDL_SetAtomicInt(percent, -1);
   if (!init() || (abort && SDL_GetAtomicInt(abort)) || !(curl = thread_handle()))
      return -1;
   if (!(s.io = SDL_IOFromFile(path, "wb")))
   {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "HTTP: can't write %s: %s", path, SDL_GetError());
      return -1;
   }

   curl_easy_setopt(curl, CURLOPT_URL, url);
   if (share)
      curl_easy_setopt(curl, CURLOPT_SHARE, share);
   curl_easy_setopt(curl, CURLOPT_USERAGENT, default_agent);
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
   /* A big file takes as long as it takes, as long as it keeps coming. */
   curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
   curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
   curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_file);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
   curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_file_progress);
   curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &s);
   curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
#ifdef _WIN32
   curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif

   res = curl_easy_perform(curl);
   if (res == CURLE_OK)
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
   else if (!abort || !SDL_GetAtomicInt(abort))
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "HTTP: %.*s: %s", host_length(url), url,
            curl_easy_strerror(res));
   if (!SDL_CloseIO(s.io))
      status = -1;
   if (status != 200)
      SDL_RemovePath(path);
   return (int)status;
}

char *http_ws_ask(const char *url, const char *text, SDL_AtomicInt *abort, int timeout_s)
{
   struct buffer b = { NULL, 0, 0, abort };
   Uint64 until = SDL_GetTicks() + (Uint64)timeout_s * 1000;
   size_t left = SDL_strlen(text), n;
   Uint64 sent;
   CURL *curl;
   CURLcode res;
   bool done = false;

   if (!init() || aborted(&b) || !(curl = curl_easy_init()))
      return NULL;
   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
   curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_s);
   curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
   res = curl_easy_perform(curl);
   while ((res == CURLE_OK || res == CURLE_AGAIN) && left && !aborted(&b)
         && SDL_GetTicks() < until)
   {
      n = 0;
      res = curl_ws_send(curl, text, left, &n, 0, CURLWS_TEXT);
      text += n;
      left -= n;
      if (res == CURLE_AGAIN)
         SDL_Delay(2);
   }
   /* Its answer, in as many pieces as it comes; one that takes a while is
    * looked for less often. */
   sent = SDL_GetTicks();
   while (res == CURLE_OK && !left && !done && !aborted(&b) && SDL_GetTicks() < until)
   {
      const struct curl_ws_frame *meta = NULL;
      char piece[16384];

      n = 0;
      res = curl_ws_recv(curl, piece, sizeof(piece), &n, &meta);
      if (res == CURLE_AGAIN)
      {
         res = CURLE_OK;
         SDL_Delay(SDL_GetTicks() - sent < 500 ? 2 : 25);
      }
      else if (res == CURLE_OK && meta && (meta->flags & (CURLWS_TEXT | CURLWS_CONT)))
      {
         if (on_data(piece, 1, n, &b) != n)
            break;
         done = !meta->bytesleft && !(meta->flags & CURLWS_CONT);
      }
   }
   if (res != CURLE_OK)
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "HTTP: %.*s: %s", host_length(url), url,
            curl_easy_strerror(res));
   curl_easy_cleanup(curl);
   if (!done)
   {
      SDL_free(b.data);
      return NULL;
   }
   return b.data;
}

char *http_escape(const char *text)
{
   char *escaped, *copy;

   if (!init() || !(escaped = curl_easy_escape(NULL, text, 0)))
      return NULL;
   copy = SDL_strdup(escaped);
   curl_free(escaped);
   return copy;
}
