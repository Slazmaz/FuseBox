#ifndef _WIN32
#define _DEFAULT_SOURCE
#include <sys/stat.h>
#endif

#include "update.h"
#include "crypto.h"
#include "http.h"
#include "json.h"
#include "platform.h"
#include "version.h"

#include <SDL3/SDL.h>
#include <string.h>
#include <zlib.h>

/* Set otherwise only to test. */
#ifndef UPDATE_API
#define UPDATE_API "https://api.github.com/repos/Slazmaz/FuseBox/releases?per_page=30"
#endif
#ifndef UPDATE_FILES
#define UPDATE_FILES "https://github.com/Slazmaz/FuseBox/releases/download/"
#endif

/* Between two checks (s): doubled by each, and halved by each WAIT_LESS without
 * one once it's over. GitHub takes 60 requests an hour. */
#define WAIT_FIRST 60
#define WAIT_MOST 120
#define WAIT_LESS 120

/* The release's file for this platform ends so (tools/make_release.py); other
 * platforms build their own. */
#ifdef _WIN32
#define ASSET_END "-windows-x64.zip"
#elif defined(__linux__) && defined(__x86_64__)
#define ASSET_END "-linux-x64.zip"
#else
#define ASSET_END ""
#endif

#define MAX_FILES 1024
#define MAX_BYTES (512u << 20)

static struct
{
   SDL_Mutex *lock;
   /* With a separator after it, as SDL_GetBasePath gives it. */
   char data[1024], config[1100];
   /* When GitHub was last asked (seconds since 1970), and how long that lasts. */
   Sint64 asked;
   int wait;
   /* The latest release; its file here, if it has one: its address, SHA-256 (hex)
    * and size. */
   char version[32], url[512], digest[65];
   Sint64 size;
   /* What its notes list, and those of the ones between (update_notes). */
   char notes[UPDATE_NOTES];
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

/* Above 0 if a is newer, below if b is. */
static int compare(const int a[3], const int b[3])
{
   int i;

   for (i = 0; i < 3; i++)
      if (a[i] != b[i])
         return a[i] - b[i];
   return 0;
}

static bool is_newer(const char *version)
{
   int a[3], b[3];

   return read_version(version, a) && read_version(FUSEBOX_VERSION, b) && compare(a, b) > 0;
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
   SDL_IOprintf(io, ",\n  \"url\": ");
   json_write_string(io, up.url);
   SDL_IOprintf(io, ",\n  \"sha256\": ");
   json_write_string(io, up.digest);
   SDL_IOprintf(io, ",\n  \"size\": %lld,\n  \"notes\": ", (long long)up.size);
   json_write_string(io, up.notes);
   SDL_IOprintf(io, "\n}\n");
   SDL_CloseIO(io);
}

/* ---- Files ---------------------------------------------------------------- */

static void remove_tree(const char *path);

static SDL_EnumerationResult SDLCALL on_remove(void *user, const char *dir, const char *name)
{
   char path[1400];

   SDL_snprintf(path, sizeof(path), "%s%s", dir, name);
   remove_tree(path);
   return SDL_ENUM_CONTINUE;
}

/* But for what's in use: an older FuseBox that still runs. */
static void remove_tree(const char *path)
{
   SDL_PathInfo info;

   if (!SDL_GetPathInfo(path, &info))
      return;
   if (info.type == SDL_PATHTYPE_DIRECTORY)
      SDL_EnumerateDirectory(path, on_remove, NULL);
   SDL_RemovePath(path);
}

/* The folders of the file at path. */
static bool make_parents(const char *path)
{
   char dir[1400], *sep;

   SDL_strlcpy(dir, path, sizeof(dir));
   if (!(sep = SDL_strrchr(dir, PATH_SEP[0])))
      return true;
   *sep = '\0';
   return SDL_CreateDirectory(dir);
}

/* Everything of an update's, next to the exe: renaming stays on one disk. */
static void work_path(const char *name, char *dst, size_t size)
{
   SDL_snprintf(dst, size, "%supdate%s%s", up.data, name[0] ? PATH_SEP : "", name);
}

static Uint32 le16(const Uint8 *p)
{
   return (Uint32)(p[0] | p[1] << 8);
}

static Uint32 le32(const Uint8 *p)
{
   return le16(p) | le16(p + 2) << 16;
}

/* Where a file of the release's goes may only be FuseBox's own, in its folder. */
static bool allowed(const char *path)
{
   static const char *const kept[] = { "config", "art", "achievements", "history", "update",
      "library.json" };
   const char *p;
   size_t i;

   if (!path[0] || path[0] == '/' || SDL_strstr(path, "..") || SDL_strlen(path) > 250)
      return false;
   for (p = path; *p; p++)
      if ((Uint8)*p < 0x20 || SDL_strchr("\\:*?\"<>|", *p))
         return false;
   for (i = 0; i < SDL_arraysize(kept); i++)
   {
      size_t n = SDL_strlen(kept[i]);
      if (SDL_strncasecmp(path, kept[i], n) == 0 && (!path[n] || path[n] == '/'))
         return false;
   }
   return true;
}

struct entry
{
   char name[300];
   Uint32 method, crc, packed, size, at;
   /* To be run. */
   bool program;
};

/* The zip's files; *count of them. NULL if it isn't one. */
static struct entry *read_zip(const Uint8 *zip, size_t total, int *count)
{
   struct entry *list = NULL;
   Uint32 n, dir, dir_size, i;
   size_t end;

   *count = 0;
   if (total < 22 || total > MAX_BYTES)
      return NULL;
   /* The end record, behind a comment. */
   for (end = total - 22; end > 0 && le32(zip + end) != 0x06054b50; end--)
      ;
   if (le32(zip + end) != 0x06054b50)
      return NULL;
   n = le16(zip + end + 10);
   dir_size = le32(zip + end + 12);
   dir = le32(zip + end + 16);
   if (!n || n > MAX_FILES || (Uint64)dir + dir_size > end
         || !(list = SDL_calloc(n, sizeof(*list))))
      return NULL;
   for (i = 0; n; n--)
   {
      const Uint8 *p = zip + dir + i;
      struct entry *e = &list[*count];
      Uint32 len;

      if ((Uint64)i + 46 > dir_size || le32(p) != 0x02014b50)
         break;
      len = le16(p + 28);
      if ((Uint64)i + 46 + len > dir_size || len >= sizeof(e->name))
         break;
      e->method = le16(p + 10);
      e->crc = le32(p + 16);
      e->packed = le32(p + 20);
      e->size = le32(p + 24);
      e->at = le32(p + 42);
      /* Made on a Unix: its modes. */
      e->program = p[5] == 3 && (le16(p + 40) & 0111);
      SDL_memcpy(e->name, p + 46, len);
      e->name[len] = '\0';
      i += 46 + len + le16(p + 30) + le16(p + 32);
      /* Not its folders: they're made for the files. */
      if (len && e->name[len - 1] != '/')
         (*count)++;
   }
   if (!n && *count)
      return list;
   SDL_free(list);
   return NULL;
}

/* The file unpacked into path, checked. */
static bool unpack(const Uint8 *zip, size_t total, const struct entry *e, const char *path)
{
   Uint8 *out = NULL;
   const Uint8 *from;
   z_stream z;
   bool ok = false;
   Uint64 at;

   if ((Uint64)e->at + 30 > total || le32(zip + e->at) != 0x04034b50 || e->size > MAX_BYTES)
      return false;
   at = (Uint64)e->at + 30 + le16(zip + e->at + 26) + le16(zip + e->at + 28);
   if (at + e->packed > total || !(out = SDL_malloc((size_t)e->size + 1)))
      return false;
   from = zip + at;
   SDL_zero(z);
   if (e->method == 0 && e->packed == e->size)
   {
      SDL_memcpy(out, from, e->size);
      ok = true;
   }
   /* Raw deflate: a zip has headers of its own. */
   else if (e->method == 8 && inflateInit2(&z, -MAX_WBITS) == Z_OK)
   {
      z.next_in = (Bytef *)from;
      z.avail_in = e->packed;
      z.next_out = out;
      z.avail_out = e->size + 1;
      ok = inflate(&z, Z_FINISH) == Z_STREAM_END && z.total_out == e->size;
      inflateEnd(&z);
   }
   ok = ok && crc32(0, out, e->size) == e->crc && make_parents(path)
      && SDL_SaveFile(path, out, e->size);
#ifndef _WIN32
   if (ok && e->program)
      ok = chmod(path, 0755) == 0;
#endif
   SDL_free(out);
   return ok;
}

/* ---- Checking ------------------------------------------------------------- */

void update_init(const char *data_dir, const char *config_dir)
{
#ifdef _WIN32
   /* What came along until 0.1.2. */
   static const char *const once[] = { "SDL3.dll", "fuseboxlan.dll", "libbrotlicommon.dll",
      "libbrotlidec.dll", "libcrypto-3-x64.dll", "libcurl-4.dll", "libiconv-2.dll",
      "libidn2-0.dll", "libintl-8.dll", "libnghttp2-14.dll", "libnghttp3-9.dll",
      "libngtcp2-16.dll", "libngtcp2_crypto_ossl-0.dll", "libpsl-5.dll", "libsharpyuv-0.dll",
      "libssh2-1.dll", "libssl-3-x64.dll", "libunistring-5.dll", "libwebp-7.dll",
      "libwebpdemux-2.dll", "libwinpthread-1.dll", "libzstd.dll", "zlib1.dll" };
   size_t i;
#endif
   char path[1200], *text;
   struct json *root;

   up.lock = SDL_CreateMutex();
   SDL_strlcpy(up.data, data_dir, sizeof(up.data));
   SDL_strlcpy(up.config, config_dir, sizeof(up.config));
   work_path("", path, sizeof(path));
   remove_tree(path);
#ifdef _WIN32
   /* Those in use go out of the way, and at the next start: OpenSSL takes zlib1.dll
    * where there is one, so it would be each time. */
   for (i = 0; i < SDL_arraysize(once); i++)
   {
      char aside[1200];

      SDL_snprintf(path, sizeof(path), "%s%s", data_dir, once[i]);
      work_path(once[i], aside, sizeof(aside));
      if (!SDL_RemovePath(path) && make_parents(aside))
         SDL_RenamePath(path, aside);
   }
#endif
   state_path(path, sizeof(path));
   text = SDL_LoadFile(path, NULL);
   if ((root = text ? json_parse(text) : NULL))
   {
      up.asked = (Sint64)json_num(root, "asked", 0);
      up.wait = (int)SDL_clamp(json_num(root, "wait", 0), 0, WAIT_MOST);
      up.size = (Sint64)json_num(root, "size", 0);
      SDL_strlcpy(up.version, json_str(root, "version", ""), sizeof(up.version));
      SDL_strlcpy(up.url, json_str(root, "url", ""), sizeof(up.url));
      SDL_strlcpy(up.digest, json_str(root, "sha256", ""), sizeof(up.digest));
      SDL_utf8strlcpy(up.notes, json_str(root, "notes", ""), sizeof(up.notes));
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

/* The release's file for this platform, if it has one. Under the lock. */
static void read_asset(const struct json *root)
{
   const struct json *assets = json_get(root, "assets"), *a;

   up.url[0] = up.digest[0] = '\0';
   up.size = 0;
   for (a = assets ? assets->child : NULL; a && ASSET_END[0]; a = a->next)
   {
      const char *name = json_str(a, "name", ""), *url = json_str(a, "browser_download_url", "");
      const char *digest = json_str(a, "digest", "");
      size_t n = SDL_strlen(name), end = SDL_strlen(ASSET_END);

      /* Never anything that can't be checked, or from elsewhere. */
      if (n < end || SDL_strcasecmp(name + n - end, ASSET_END) != 0
            || SDL_strncmp(url, UPDATE_FILES, SDL_strlen(UPDATE_FILES)) != 0
            || SDL_strncmp(digest, "sha256:", 7) != 0 || SDL_strlen(digest + 7) != 64)
         continue;
      SDL_strlcpy(up.url, url, sizeof(up.url));
      SDL_strlcpy(up.digest, digest + 7, sizeof(up.digest));
      up.size = (Sint64)json_num(a, "size", 0);
      return;
   }
}

/* Not drafts and pre-releases. */
static bool read_release(const struct json *release, int v[3])
{
   return !json_bool(release, "draft", false) && !json_bool(release, "prerelease", false)
      && read_version(json_str(release, "tag_name", ""), v);
}

/* The list's newest release, its version in v; older than below, if not NULL. */
static const struct json *newest_release(const struct json *list, const int *below, int v[3])
{
   const struct json *r, *best = NULL;
   int other[3];

   for (r = list->child; r; r = r->next)
   {
      if (!read_release(r, other) || (below && compare(other, below) >= 0)
            || (best && compare(other, v) <= 0))
         continue;
      best = r;
      SDL_memcpy(v, other, sizeof(other));
   }
   return best;
}

/* The items of the release notes' lists (Markdown), without what's around them,
 * after the first n of up.notes. Returns how many there are then. Under the lock. */
static size_t add_notes(const char *body, size_t n)
{
   while (*body)
   {
      size_t len = strcspn(body, "\r\n"), at = strspn(body, " \t");
      bool item = at + 2 < len && SDL_strchr("-*+", body[at]) && body[at + 1] == ' ';

      /* Whole ones only. */
      if (item && n + len + 2 < sizeof(up.notes))
      {
         if (n)
            up.notes[n++] = '\n';
         for (at += 2; at < len; at++)
         {
            if (body[at] == '`' || (body[at] == '*' && body[at + 1] == '*'))
               at += body[at] == '*';
            else
               up.notes[n++] = body[at];
         }
      }
      body += len;
      body += strspn(body, "\r\n");
   }
   up.notes[n] = '\0';
   return n;
}

/* Those of all releases newer than this FuseBox, the newest first; each under its
 * version if there are more. Under the lock. */
static void read_notes(const struct json *list)
{
   const struct json *r;
   int v[3], below[3], mine[3], count = 0;
   size_t n = 0;

   up.notes[0] = '\0';
   if (!read_version(FUSEBOX_VERSION, mine))
      return;
   for (r = newest_release(list, NULL, v); r && compare(v, mine) > 0;
         r = newest_release(list, below, v))
   {
      char head[48];
      size_t before = n, len;

      SDL_memcpy(below, v, sizeof(v));
      SDL_snprintf(head, sizeof(head), "%s" UPDATE_HEAD "%d.%d.%d", n ? "\n" : "", v[0], v[1], v[2]);
      len = SDL_strlen(head);
      if (n + len >= sizeof(up.notes))
         break;
      SDL_memcpy(up.notes + n, head, len + 1);
      n = add_notes(json_str(r, "body", ""), n + len);
      if (n == before + len)
         up.notes[n = before] = '\0';
      else
         count++;
   }
   if (count == 1)
   {
      size_t len = strcspn(up.notes, "\n") + 1;

      SDL_memmove(up.notes, up.notes + len, SDL_strlen(up.notes + len) + 1);
   }
}

void update_check(SDL_AtomicInt *abort, char *note, size_t size)
{
   Sint64 asked, idle, now = now_s();
   const struct json *release = NULL;
   struct json *root = NULL;
   char *body = NULL;
   int wait, next, status, v[3];

   /* Counted before it's asked: closing FuseBox meanwhile doesn't undo it. */
   SDL_LockMutex(up.lock);
   asked = up.asked;
   wait = up.wait;
   next = wait * 2;
   for (idle = now - asked - wait; idle >= WAIT_LESS && next > WAIT_FIRST; idle -= WAIT_LESS)
      next /= 2;
   up.wait = SDL_clamp(next, WAIT_FIRST, WAIT_MOST);
   up.asked = now;
   save();
   SDL_UnlockMutex(up.lock);

   status = http_request(UPDATE_API, NULL, "Accept: application/vnd.github+json", NULL, abort,
         &body, NULL, 20);
   if (status == 200)
      root = json_parse(body);
   if (root && root->type != JSON_ARRAY)
   {
      json_free(root);
      root = NULL;
   }

   SDL_LockMutex(up.lock);
   if (root && (release = newest_release(root, NULL, v)))
   {
      SDL_snprintf(up.version, sizeof(up.version), "%d.%d.%d", v[0], v[1], v[2]);
      read_asset(release);
      read_notes(root);
      if (is_newer(up.version))
         SDL_snprintf(note, size, "%s is available", up.version);
      else
         SDL_strlcpy(note, "Up to date", size);
   }
   /* No release yet, or none any more. */
   else if (root || status == 404)
   {
      up.version[0] = up.url[0] = up.digest[0] = '\0';
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

bool update_notes(char *notes, size_t size)
{
   bool any;

   SDL_LockMutex(up.lock);
   any = is_newer(up.version) && up.notes[0];
   SDL_strlcpy(notes, any ? up.notes : "", size);
   SDL_UnlockMutex(up.lock);
   return any;
}

bool update_can_install(void)
{
   bool can;

   SDL_LockMutex(up.lock);
   can = is_newer(up.version) && up.url[0];
   SDL_UnlockMutex(up.lock);
   return can;
}

/* ---- Installing ----------------------------------------------------------- */

/* Moves the new file in, the old one out; or both back. Windows lets files that
 * are in use (this exe, its DLLs) be moved, not replaced. */
struct move
{
   char from[1400], to[1400], old[1400];
   bool out, in;
};

static bool move_in(struct move *m)
{
   if (platform_exists(m->to))
   {
      if (!make_parents(m->old) || !SDL_RenamePath(m->to, m->old))
         return false;
      m->out = true;
   }
   m->in = make_parents(m->to) && SDL_RenamePath(m->from, m->to);
   return m->in;
}

static void move_back(struct move *m)
{
   if (m->in)
      SDL_RenamePath(m->to, m->from);
   if (m->out)
      SDL_RenamePath(m->old, m->to);
}

static bool check_file(const void *data, size_t size, const char *digest)
{
   static const char hex[] = "0123456789abcdef";
   Uint8 sum[32];
   char text[65];
   int i;

   sha256(data, size, sum);
   for (i = 0; i < 32; i++)
   {
      text[i * 2] = hex[sum[i] >> 4];
      text[i * 2 + 1] = hex[sum[i] & 15];
   }
   text[64] = '\0';
   return SDL_strcasecmp(text, digest) == 0;
}

/* Unpacks the zip and puts its files in place. */
static bool install(const Uint8 *zip, size_t size, SDL_AtomicInt *abort, char *error,
      size_t error_size)
{
   struct entry *list;
   struct move *moves = NULL;
   char fresh[1200], old[1200], name[64];
   int count, i, exe = -1, n = 0;
   size_t prefix = 0;
   bool ok = false;

   SDL_strlcpy(error, "The release's file can't be read", error_size);
   if (!(list = read_zip(zip, size, &count)) || !(moves = SDL_calloc((size_t)count, sizeof(*moves))))
      goto done;
   /* All of it may be in a folder: the one the exe is in. */
   for (i = 0; i < count && exe < 0; i++)
   {
      const char *slash = SDL_strrchr(list[i].name, '/');
      const char *file = slash ? slash + 1 : list[i].name;
      if (SDL_strcasecmp(file, FUSEBOX_EXE) == 0)
      {
         exe = i;
         prefix = (size_t)(file - list[i].name);
      }
   }
   if (exe < 0)
      goto done;
   /* The exe last: once it's new, so is the rest. */
   if (exe != count - 1)
   {
      struct entry e = list[exe];
      list[exe] = list[count - 1];
      list[count - 1] = e;
   }
   work_path("new", fresh, sizeof(fresh));
   SDL_snprintf(name, sizeof(name), "old-%lld", (long long)now_s());
   work_path(name, old, sizeof(old));
   for (i = 0; i < count; i++)
   {
      const char *path = list[i].name + prefix;
      struct move *m = &moves[i];
      char *p, local[300];

      if (SDL_strncmp(list[i].name, list[count - 1].name, prefix) != 0 || !allowed(path))
         goto done;
      SDL_strlcpy(local, path, sizeof(local));
      for (p = local; *p; p++)
         if (*p == '/')
            *p = PATH_SEP[0];
      SDL_snprintf(m->from, sizeof(m->from), "%s" PATH_SEP "%s", fresh, local);
      SDL_snprintf(m->old, sizeof(m->old), "%s" PATH_SEP "%s", old, local);
      SDL_snprintf(m->to, sizeof(m->to), "%s%s", up.data, local);
      if (!unpack(zip, size, &list[i], m->from))
         goto done;
   }
   if (abort && SDL_GetAtomicInt(abort))
      goto done;
   SDL_strlcpy(error, "Can't write to FuseBox's folder", error_size);
   for (ok = true; n < count && ok; n++)
      if (!(ok = move_in(&moves[n])))
         SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Update: %s: %s", moves[n].to, SDL_GetError());
   while (!ok && n > 0)
      move_back(&moves[--n]);
done:
   SDL_free(moves);
   SDL_free(list);
   return ok;
}

bool update_install(SDL_AtomicInt *abort, SDL_AtomicInt *percent, char *error, size_t size)
{
   char url[512], digest[65], path[1200];
   Sint64 expected;
   size_t got = 0;
   Uint8 *zip = NULL;
   bool ok = false;
   int status;

   SDL_LockMutex(up.lock);
   SDL_strlcpy(url, up.url, sizeof(url));
   SDL_strlcpy(digest, up.digest, sizeof(digest));
   expected = up.size;
   SDL_UnlockMutex(up.lock);

   /* What an update that didn't work out left. */
   work_path("", path, sizeof(path));
   remove_tree(path);
   if (!SDL_CreateDirectory(path))
   {
      SDL_strlcpy(error, "Can't write to FuseBox's folder", size);
      return false;
   }
   work_path("download.zip", path, sizeof(path));
   status = http_download(url, path, abort, percent);
   if (status != 200)
   {
      if (status < 0)
         SDL_strlcpy(error, "The download failed", size);
      else if (status == 404)
      {
         /* Taken back: asked again at once, for the one there is now. */
         SDL_LockMutex(up.lock);
         up.version[0] = up.url[0] = up.digest[0] = '\0';
         up.size = up.asked = 0;
         save();
         SDL_UnlockMutex(up.lock);
         SDL_strlcpy(error, "That version isn't there any more", size);
      }
      else
         SDL_snprintf(error, size, "The download failed (%d)", status);
   }
   else if (!(zip = SDL_LoadFile(path, &got)) || (Sint64)got != expected
         || !check_file(zip, got, digest))
      SDL_strlcpy(error, "The download isn't what GitHub says it should be", size);
   else
      ok = install(zip, got, abort, error, size);
   SDL_free(zip);
   /* The old files stay until nothing runs from them (update_init). */
   SDL_RemovePath(path);
   if (!ok)
   {
      work_path("new", path, sizeof(path));
      remove_tree(path);
   }
   return ok;
}
