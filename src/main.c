/* FuseBox: finds games in other launchers, lets you pick their art from
 * SteamGridDB, keeps it in a library other apps can read (library.json,
 * fusebox-cli) and adds the games to Steam with that art.
 *
 * The window is drawn immediate-mode: a click is remembered, then the
 * frame's widgets each check whether it landed on them. Nothing is drawn
 * while nothing changes (events or finished jobs wake it). */
#include "anim.h"
#include "gl.h"
#include "http.h"
#include "disk.h"
#include "jobs.h"
#include "library.h"
#include "ra.h"
#include "roms.h"
#include "scan.h"
#include "sgdb.h"
#include "steam.h"
#include "steamui.h"
#include "ui.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stb_image.h>
#include <stb_truetype.h>

/* ---- Messages from jobs to the window ---------------------------------- */

enum msg_kind
{
   MSG_SCANNED,
   MSG_ART_LIST,
   MSG_THUMB,
   MSG_COVER,
   MSG_ART_SAVED,
   MSG_MATCHES,
   MSG_STATUS,
   MSG_STEAM_DONE,
   MSG_ADD_EXE,
   MSG_ROM_FOLDER,
   MSG_EMULATOR,
   MSG_LOGIN,
   MSG_ACHIEVEMENTS
};

struct msg
{
   enum msg_kind kind;
   int gen, index;
   unsigned version;
   void *ptr;
   int count;
   Uint8 *pixels;
   int w, h;
   char text[256];
   struct msg *next;
};

static SDL_Mutex *msg_lock;
static struct msg *msg_head, *msg_tail;
static Uint32 wake_event;

static void post(struct msg *m)
{
   SDL_Event e;

   SDL_LockMutex(msg_lock);
   m->next = NULL;
   if (msg_tail)
      msg_tail->next = m;
   else
      msg_head = m;
   msg_tail = m;
   SDL_UnlockMutex(msg_lock);
   SDL_zero(e);
   e.type = wake_event;
   SDL_PushEvent(&e);
}

static struct msg *new_msg(enum msg_kind kind)
{
   struct msg *m = SDL_calloc(1, sizeof(*m));
   if (m)
      m->kind = kind;
   return m;
}

static void post_status(const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);
static void post_status(const char *fmt, ...)
{
   struct msg *m = new_msg(MSG_STATUS);
   va_list ap;

   if (!m)
      return;
   va_start(ap, fmt);
   SDL_vsnprintf(m->text, sizeof(m->text), fmt, ap);
   va_end(ap);
   post(m);
}

static struct msg *take_msg(void)
{
   struct msg *m;

   SDL_LockMutex(msg_lock);
   if ((m = msg_head))
   {
      msg_head = m->next;
      if (!msg_head)
         msg_tail = NULL;
   }
   SDL_UnlockMutex(msg_lock);
   return m;
}

/* ---- Images ------------------------------------------------------------ */

/* Area-average downscale to fit max_w x max_h (never up): the list's
 * covers are small, and a full 600x900 each would waste memory. */
static Uint8 *shrink(Uint8 *src, int w, int h, int max_w, int max_h, int *ow, int *oh)
{
   float f = SDL_min((float)max_w / w, (float)max_h / h);
   int nw, nh, x, y;
   Uint8 *dst;

   if (f >= 1.0f)
   {
      *ow = w;
      *oh = h;
      return src;
   }
   nw = SDL_max(1, (int)(w * f));
   nh = SDL_max(1, (int)(h * f));
   if (!(dst = SDL_malloc((size_t)nw * nh * 4)))
   {
      *ow = w;
      *oh = h;
      return src;
   }
   for (y = 0; y < nh; y++)
   {
      int y0 = y * h / nh, y1 = SDL_max(y0 + 1, (y + 1) * h / nh);
      for (x = 0; x < nw; x++)
      {
         int x0 = x * w / nw, x1 = SDL_max(x0 + 1, (x + 1) * w / nw);
         Uint32 sum[4] = { 0, 0, 0, 0 }, n = 0;
         int sx, sy, c;
         for (sy = y0; sy < y1; sy++)
            for (sx = x0; sx < x1; sx++, n++)
               for (c = 0; c < 4; c++)
                  sum[c] += src[((size_t)sy * w + sx) * 4 + c];
         for (c = 0; c < 4; c++)
            dst[((size_t)y * nw + x) * 4 + c] = (Uint8)(sum[c] / n);
      }
   }
   SDL_free(src);
   *ow = nw;
   *oh = nh;
   return dst;
}

static Uint8 *decode(const void *data, size_t size, int max_w, int max_h, int *w, int *h)
{
   int n, iw, ih;
   Uint8 *px = stbi_load_from_memory(data, (int)size, &iw, &ih, &n, 4);

   if (!px && !(px = anim_first(data, size, &iw, &ih)))
      return NULL;
   return shrink(px, iw, ih, max_w, max_h, w, h);
}

static GLuint make_texture(const Uint8 *pixels, int w, int h)
{
   GLuint tex;

   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   gl.GenerateMipmap(GL_TEXTURE_2D);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glBindTexture(GL_TEXTURE_2D, 0);
   return tex;
}

/* ---- Jobs -------------------------------------------------------------- */

/* data not NULL: ticks the user changed since the last scan are kept. */
static void job_scan(void *data)
{
   struct msg *m = new_msg(MSG_SCANNED);
   /* What each game's changed tick is (1 off, 2 on), 0 if it's as Steam has it. */
   Uint8 *ticks = NULL;
   int steam_n, found, i, count;

   /* What's installed and what Steam has is found again each time: the
    * launchers first, then Steam's shortcuts matched to their games. */
   library_lock();
   count = library_count();
   if (data)
      ticks = SDL_calloc((size_t)SDL_max(1, count), 1);
   for (i = 0; i < count; i++)
   {
      struct game *g = library_game(i);
      if (ticks && (g->gone ? g->selected : g->installed && g->selected != (g->steam_appid != 0)))
         ticks[i] = g->selected ? 2 : 1;
      g->installed = g->gone = g->selected = false;
      g->steam_appid = 0;
   }
   library_unlock();
   found = scan_launchers();
   /* Then what no launcher knows: Windows' record of games, folders that
    * look like games. */
   found += scan_disks(0);
   /* ROMs last: what their emulators' folders hold is for the others. */
   found += scan_roms();
   steam_n = steam_read_shortcuts();
   if (ticks)
   {
      library_lock();
      for (i = 0; i < count; i++)
         if (ticks[i] && (library_game(i)->installed || library_game(i)->gone))
            library_game(i)->selected = ticks[i] == 2;
      library_unlock();
      SDL_free(ticks);
   }
   SDL_free(data);

   library_save();
   if (m)
   {
      SDL_snprintf(m->text, sizeof(m->text), "Found %d game%s, %d Steam shortcut%s", found,
            found == 1 ? "" : "s", steam_n, steam_n == 1 ? "" : "s");
      post(m);
   }
}

/* What's looked up of a game's art, as bits: its icon (the list's picture),
 * all that's missing, or one type (1 << t of FILL_TYPE). */
#define FILL_ICON 1
#define FILL_ALL 2
#define FILL_TYPE 4

struct fill_job
{
   int index;
   Uint8 what;
   /* Counted in list_fills. */
   bool listed;
};

/* Icons the list's rows wait for: only so many at once, so rows scrolled
 * past don't hold up the ones shown. */
#define LIST_FILLS 16
static SDL_AtomicInt list_fills;

/* The most popular art where the game has none, so nothing is left blank;
 * picking in the window replaces it. */
static void job_fill(void *data)
{
   struct fill_job *j = data;
   int t;

   if (j->what == FILL_ICON)
   {
      bool cover;
      sgdb_fill(j->index, true, ART_ICON, jobs_abort());
      /* Without one, the list shows its cover. */
      library_lock();
      cover = library_game(j->index)->rom[0] && !library_game(j->index)->art[ART_ICON].path[0]
         && library_game(j->index)->sgdb_id;
      library_unlock();
      if (cover)
         sgdb_fill(j->index, true, ART_COVER, jobs_abort());
   }
   else if (j->what == FILL_ALL)
      sgdb_fill(j->index, false, ART_COVER, jobs_abort());
   else
      for (t = 0; t < ART_TYPES; t++)
         if (j->what == FILL_TYPE << t)
            sgdb_fill(j->index, true, (enum art_type)t, jobs_abort());
   library_lock();
   library_game(j->index)->filling &= (Uint8)~j->what;
   library_unlock();
   if (j->listed)
      SDL_AddAtomicInt(&list_fills, -1);
   /* Quietly: the status line keeps what the scan found. An empty status
    * only wakes the window to show the new art (and ask for more). */
   post_status("%s", "");
   SDL_free(j);
}

/* Looks art up, unless that's already waiting. Under the library's lock. */
static void queue_fill(int index, Uint8 what, enum job_level level, bool first, bool listed)
{
   struct game *g = library_game(index);
   struct fill_job *j;

   if (!g || (g->filling & what) || !(j = SDL_calloc(1, sizeof(*j))))
      return;
   g->filling |= what;
   j->index = index;
   j->what = what;
   j->listed = listed;
   if (listed)
      SDL_AddAtomicInt(&list_fills, 1);
   jobs_add(job_fill, j, level, first);
}

/* Every installed game's missing art: the icons first, the rest after all
 * else. */
static void start_fill(void)
{
   int i, t;

   if (!sgdb_has_key())
      return;
   library_lock();
   for (i = 0; i < library_count(); i++)
   {
      const struct game *g = library_game(i);
      bool missing = false;

      /* ROM collections are big: theirs is looked up as they're shown
       * (fill_shown), or ticked. */
      if (!g->installed || (g->rom[0] && !g->selected))
         continue;
      for (t = 0; t < ART_TYPES; t++)
         missing |= !g->art[t].path[0];
      /* An icon FuseBox filled in, where the game's own exe has one. */
      if (!g->rom[0] && (!g->art[ART_ICON].path[0]
            || (g->game_exe[0] && !g->art[ART_ICON].picked && g->art[ART_ICON].sgdb_id != 0)))
         queue_fill(i, FILL_ICON, JOB_ICON, false, false);
      if (missing)
         queue_fill(i, FILL_ALL, JOB_REST, false, false);
   }
   library_unlock();
}

struct cover_job
{
   int index;
   unsigned version;
   /* -1, -2: the list's cover or icon (smaller); else the art type for the
    * game page. */
   int slot;
   int max_w, max_h;
   char path[1024];
};

/* A saved image of a game's, decoded at the size it's shown. */
static void job_cover(void *data)
{
   struct cover_job *j = data;
   struct msg *m = new_msg(MSG_COVER);
   size_t size = 0;
   void *file = SDL_LoadFile(j->path, &size);

   if (m)
   {
      m->index = j->index;
      m->version = j->version;
      m->count = j->slot;
      if (file)
         m->pixels = decode(file, size, j->max_w, j->max_h, &m->w, &m->h);
      post(m);
   }
   SDL_free(file);
   SDL_free(j);
}

struct list_job
{
   int gen, index;
   enum art_type type;
};

/* The picker's current generation: lists and thumbnails for an older one
 * are skipped before they're fetched. */
static SDL_AtomicInt picker_gen;

/* A game's art of one type on SteamGridDB, matching the game first if it
 * isn't yet. */
static void job_list(void *data)
{
   struct list_job *j = data;
   struct msg *m;
   char name[256];
   unsigned id;

   if (SDL_GetAtomicInt(&picker_gen) != j->gen || !(m = new_msg(MSG_ART_LIST)))
   {
      SDL_free(j);
      return;
   }
   m->gen = j->gen;
   m->index = j->index;
   library_lock();
   id = library_game(j->index)->sgdb_id;
   SDL_strlcpy(name, library_game(j->index)->name, sizeof(name));
   library_unlock();
   if (!id)
   {
      struct sgdb_game match;
      int status = 0;
      int n = sgdb_search(name, &match, 1, jobs_abort(), &status);
      if (n == 1)
      {
         id = match.id;
         library_lock();
         library_game(j->index)->sgdb_id = match.id;
         SDL_strlcpy(library_game(j->index)->sgdb_name, match.name, sizeof(match.name));
         library_unlock();
         library_changed();
      }
      else
      {
         m->count = n < 0 ? -1 : 0;
         SDL_strlcpy(m->text, status == 401 || status == 403 ? "SteamGridDB refused the API key"
               : n < 0 ? "Can't reach SteamGridDB" : "Not found on SteamGridDB", sizeof(m->text));
         post(m);
         SDL_free(j);
         return;
      }
   }
   m->count = sgdb_list(id, j->type, sgdb_animated(), (struct sgdb_art **)&m->ptr,
         jobs_abort());
   if (m->count < 0)
      SDL_strlcpy(m->text, "Can't reach SteamGridDB", sizeof(m->text));
   else if (m->count == 0)
      SDL_strlcpy(m->text, sgdb_animated() && j->type != ART_ICON
            ? "No animated art of this kind on SteamGridDB" : "None of this kind on SteamGridDB",
            sizeof(m->text));
   /* Icons: the game's own exe's first (id 0, a local file). */
   if (j->type == ART_ICON)
   {
      char exe[1024], path[1100], *sep;
      library_lock();
      SDL_strlcpy(exe, library_game(j->index)->game_exe, sizeof(exe));
      library_unlock();
      library_tmp_path(j->index, ART_ICON, path, sizeof(path));
      if ((sep = SDL_strrchr(path, '\\')) || (sep = SDL_strrchr(path, '/')))
         SDL_strlcpy(sep + 1, "exe-icon.png", sizeof(path) - (size_t)(sep + 1 - path));
      if (exe[0] && platform_exe_icon(exe, path))
      {
         int n = SDL_max(m->count, 0);
         struct sgdb_art *arts = SDL_realloc(m->ptr, sizeof(*arts) * (size_t)(n + 1));
         if (arts)
         {
            SDL_memmove(arts + 1, arts, sizeof(*arts) * (size_t)n);
            SDL_zero(arts[0]);
            SDL_snprintf(arts[0].url, sizeof(arts[0].url), "file:%s", path);
            SDL_strlcpy(arts[0].thumb, arts[0].url, sizeof(arts[0].thumb));
            SDL_strlcpy(arts[0].ext, "png", sizeof(arts[0].ext));
            m->ptr = arts;
            m->count = n + 1;
         }
      }
   }
   post(m);
   SDL_free(j);
}

struct thumb_job
{
   int gen, item;
   /* "file:" and a path for one on disk. */
   char url[1100];
   bool animated;
};

/* What animations to pick from may take: each, and all a list's (the ones
 * past it are shown small). */
#define ANIM_BYTES (24u << 20)
#define ANIM_SMALL (1u << 20)
#define ANIM_TOTAL (512 << 20)
static SDL_AtomicInt anim_total;

static void job_thumb(void *data)
{
   struct thumb_job *j = data;
   struct msg *m;
   char *body = NULL;
   size_t size = 0;

   if (SDL_GetAtomicInt(&picker_gen) != j->gen || !(m = new_msg(MSG_THUMB)))
   {
      SDL_free(j);
      return;
   }
   m->gen = j->gen;
   m->index = j->item;
   /* "file:": already on disk (the game's own icon, the art in use). */
   if (SDL_strncmp(j->url, "file:", 5) == 0)
      body = SDL_LoadFile(j->url + 5, &size);
   else if (http_request(j->url, NULL, NULL, NULL, jobs_abort(), &body, &size, 30) != 200)
   {
      SDL_free(body);
      body = NULL;
   }
   if (body && SDL_GetAtomicInt(&picker_gen) == j->gen)
   {
      struct anim *a = NULL;
      if (j->animated)
         a = anim_load(body, size, 400, 400,
               SDL_GetAtomicInt(&anim_total) < ANIM_TOTAL ? ANIM_BYTES : ANIM_SMALL);
      if (a)
      {
         SDL_AddAtomicInt(&anim_total, a->w * a->h * 4 * a->count);
         m->ptr = a;
      }
      else
         m->pixels = decode(body, size, 400, 400, &m->w, &m->h);
   }
   SDL_free(body);
   post(m);
   SDL_free(j);
}

/* Art picked whose download isn't in yet: shown as the one in use already.
 * The window's own, but for what its job reads and writes. */
#define CHOICES 16

struct choice
{
   bool used;
   int index;
   enum art_type type;
   unsigned id;
   /* Set when another is picked instead. */
   SDL_AtomicInt stop;
   /* How much of it is in, -1 unknown. */
   SDL_AtomicInt percent;
};

static struct choice choices[CHOICES];

struct choose_job
{
   struct choice *choice;
   int index;
   enum art_type type;
   struct sgdb_art art;
};

static void job_choose(void *data)
{
   struct choose_job *j = data;
   struct msg *m = new_msg(MSG_ART_SAVED);
   char tmp[1100];
   bool ok, changed;

   library_tmp_path(j->index, j->type, tmp, sizeof(tmp));
   ok = SDL_strncmp(j->art.url, "file:", 5) == 0 ? SDL_CopyFile(j->art.url + 5, tmp)
      : http_download(j->art.url, tmp, &j->choice->stop, &j->choice->percent) == 200;
   if (ok)
   {
      library_lock();
      if (SDL_GetAtomicInt(&j->choice->stop))
         SDL_RemovePath(tmp);
      else
      {
         const struct art_choice *a = &library_game(j->index)->art[j->type];
         changed = !a->path[0] || a->sgdb_id != j->art.id;
         /* The user's pick: FuseBox leaves it as it is from now on. */
         if (library_set_art(j->index, j->type, tmp, j->art.ext, j->art.id))
            library_set_picked(j->index, j->type, changed);
      }
      library_unlock();
   }
   if (m)
   {
      m->index = (int)(j->choice - choices);
      post(m);
   }
   SDL_free(j);
}

/* The pick still coming in for the game's art of that type, NULL if none. */
static struct choice *choice_for(int index, enum art_type type)
{
   int i;

   for (i = 0; i < CHOICES; i++)
      if (choices[i].used && choices[i].index == index && choices[i].type == type
            && !SDL_GetAtomicInt(&choices[i].stop))
         return &choices[i];
   return NULL;
}

static bool choosing(void)
{
   int i;

   for (i = 0; i < CHOICES; i++)
      if (choices[i].used && !SDL_GetAtomicInt(&choices[i].stop))
         return true;
   return false;
}

static void choose(int index, enum art_type type, const struct sgdb_art *art)
{
   struct choice *c = choice_for(index, type);
   struct choose_job *j;
   int i;

   if (c && c->id == art->id)
      return;
   /* Another instead: the one before it needn't come in. */
   if (c)
      SDL_SetAtomicInt(&c->stop, 1);
   for (i = 0; i < CHOICES && choices[i].used; i++)
      ;
   if (i == CHOICES || !(j = SDL_calloc(1, sizeof(*j))))
      return;
   c = &choices[i];
   c->used = true;
   c->index = index;
   c->type = type;
   c->id = art->id;
   SDL_SetAtomicInt(&c->stop, 0);
   SDL_SetAtomicInt(&c->percent, -1);
   j->choice = c;
   j->index = index;
   j->type = type;
   j->art = *art;
   jobs_add(job_choose, j, JOB_NOW, true);
}

struct match_job
{
   char name[256];
};

static void job_matches(void *data)
{
   struct match_job *j = data;
   struct msg *m = new_msg(MSG_MATCHES);
   struct sgdb_game *games = SDL_calloc(12, sizeof(*games));

   if (m && games)
   {
      m->count = sgdb_search(j->name, games, 12, jobs_abort(), NULL);
      m->ptr = games;
      post(m);
   }
   else
   {
      SDL_free(m);
      SDL_free(games);
   }
   SDL_free(j);
}

/* A launcher game the next update adds or keeps in Steam. */
static bool wanted_in_steam(const struct game *g)
{
   return g->selected && g->installed && !g->gone && g->exe[0]
      && SDL_strcmp(g->source, "Steam") != 0;
}

struct achievements_job
{
   /* Steam was just started: give it time. Asked by the user: ask
    * RetroAchievements whatever it said minutes ago. */
   bool wait, force;
};

/* RetroAchievements of the ROMs, then Steam given them. */
static void job_achievements(void *data)
{
   struct achievements_job *j = data;
   struct msg *m = new_msg(MSG_ACHIEVEMENTS);
   Uint64 until = SDL_GetTicks() + (j->wait ? 180000 : 0);
   char user[128], note[128];
   enum steamui shown;
   bool ok = ra_refresh(j->force, jobs_abort());
   int games = 0;

   library_save();
   note[0] = '\0';
   if (ra_active())
   {
      while ((shown = steamui_push(&games, jobs_abort())) <= STEAMUI_WAIT
            && SDL_GetTicks() < until && !SDL_GetAtomicInt(jobs_abort()))
         SDL_Delay(1000);
      if (shown == STEAMUI_OK)
         SDL_snprintf(note, sizeof(note), "Steam shows the achievements of %d game%s", games,
               games == 1 ? "" : "s");
      else if (shown == STEAMUI_UNSUPPORTED)
         SDL_strlcpy(note, "This version of Steam isn't supported", sizeof(note));
      else if (shown == STEAMUI_WAIT)
         SDL_strlcpy(note, "Steam isn't ready yet", sizeof(note));
      else
         SDL_strlcpy(note, "Restart Steam to show them", sizeof(note));
   }
   if (!ok && ra_user(user, sizeof(user)))
      SDL_strlcpy(note, "Couldn't get everything from RetroAchievements", sizeof(note));
   if (m)
   {
      SDL_strlcpy(m->text, note, sizeof(m->text));
      post(m);
   }
   SDL_free(j);
}

static void start_achievements(bool wait, bool force)
{
   struct achievements_job *j = SDL_calloc(1, sizeof(*j));

   if (!j)
      return;
   j->wait = wait;
   j->force = force;
   jobs_add(job_achievements, j, JOB_NOW, false);
}

static void job_clear_steam(void *data)
{
   steamui_clear(jobs_abort());
}

struct login_job
{
   char user[128], password[256];
};

static void job_login(void *data)
{
   struct login_job *j = data;
   struct msg *m = new_msg(MSG_LOGIN);
   char error[128];
   bool ok = ra_login(j->user, j->password, jobs_abort(), error, sizeof(error));

   SDL_zerop(j);
   SDL_free(j);
   if (m)
   {
      m->count = ok;
      SDL_strlcpy(m->text, error, sizeof(m->text));
      post(m);
   }
}

struct steam_job
{
   bool close_steam;
};

/* The art first (Steam can run meanwhile), then Steam closed only as long
 * as writing its shortcuts takes. */
static void job_steam(void *data)
{
   struct steam_job *j = data;
   struct msg *m = new_msg(MSG_STEAM_DONE);
   const struct steam_install *st = steam_install();
   bool was_running = false;
   int i, count;

   library_lock();
   count = library_count();
   library_unlock();
   for (i = 0; i < count && sgdb_has_key() && !SDL_GetAtomicInt(jobs_abort()); i++)
   {
      bool want;
      library_lock();
      want = wanted_in_steam(library_game(i));
      library_unlock();
      if (want)
         sgdb_fill(i, false, ART_COVER, jobs_abort());
   }
   if (st && j->close_steam && platform_steam_running(st))
   {
      Uint64 until = SDL_GetTicks() + 30000;
      was_running = true;
      post_status("Restarting Steam...");
      platform_steam_shutdown(st);
      while (platform_steam_running(st) && SDL_GetTicks() < until)
         SDL_Delay(250);
      /* Steam writes its files on the way out: let it finish. */
      SDL_Delay(1500);
   }
   if (m)
   {
      if (st && platform_steam_running(st))
         SDL_strlcpy(m->text, "Steam is still running: close it and try again", sizeof(m->text));
      else
         steam_sync(m->text, sizeof(m->text));
      post(m);
   }
   library_save();
   if (was_running)
   {
      steamui_start(st);
      if (ra_active())
         start_achievements(true, false);
   }
   SDL_free(j);
}

/* ---- Window state ------------------------------------------------------ */

enum page
{
   PAGE_GAME,
   PAGE_SETTINGS
};

enum field
{
   FIELD_NONE,
   FIELD_FILTER,
   FIELD_KEY,
   FIELD_RA_USER,
   FIELD_RA_PASSWORD,
   FIELD_MATCH,
   FIELD_OPTIONS,
   FIELD_NAME
};

enum modal
{
   MODAL_NONE,
   MODAL_MATCH,
   MODAL_STEAM,
   MODAL_GAME,
   /* Picks a ROM folder's system, or a game's emulator. */
   MODAL_SYSTEM,
   MODAL_EMULATOR
};

/* A texture made from one of a game's saved images. */
struct art_tex
{
   GLuint tex;
   int w, h;
   unsigned version;
   int game;
   bool asked;
};

struct pick_item
{
   struct sgdb_art art;
   GLuint tex;
   int w, h;
   bool failed;
   /* Its thumb is asked for: an animated one's once it's in view, as its
    * frames take room. */
   bool asked;
   /* The game's own art of that type when it was listed. */
   bool in_use;
   char url[1100];
   /* Playing: the frame its texture has. */
   struct anim *anim;
   int frame;
};

/* Where the window can't be dragged by (widgets in the top bar). */
#define NODRAG_MAX 8

static struct
{
   SDL_Window *window;
   SDL_GLContext gl;
   float s;
   int width, height;
   bool redraw;
   char data_dir[1024];

   /* The click this frame, in drawable pixels; taken by the widget it hits. */
   bool click;
   float cx, cy, mx, my;
   float wheel, wx, wy;

   /* A tooltip to draw over everything this frame, if any. */
   const char *tip;
   float tip_x, tip_y;

   SDL_FRect nodrag[NODRAG_MAX];
   int nodrag_count;

   /* What the scan found; nothing else is written here. */
   char status[256];
   /* What the last Steam update said (the Steam card shows it). */
   char steam_note[128];
   bool busy;
   bool scanned;
   /* Something changed while a scan ran: scan again after it. */
   bool scan_again;
   enum page page;

   /* The library: indices shown, in name order. */
   int *view;
   int view_count;
   int view_built_for;
   float list_scroll;
   int selected;
   struct art_tex *covers;
   /* FILL_ bits asked for since the start, per game: what isn't found is
    * only asked for once. */
   Uint8 *filled;
   int covers_alloc;

   /* The game page: its art, and the picker. */
   struct art_tex detail[ART_TYPES];
   float game_scroll;
   enum art_type tab;
   bool pick_loading;
   char pick_msg[128];
   struct pick_item *items;
   int item_count;
   /* When the next frame of one in view is due, ms; 0 for none. */
   Uint64 anim_at;
   bool want_list;

   enum field field;
   char filter[128];
   char key[128];
   /* Logging in to RetroAchievements, and what came of it; and how far
    * showing achievements in Steam is. */
   char ra_user[128], ra_password[256];
   bool ra_busy;
   char ra_error[128], ra_note[128];
   char match[256];
   /* And how far it goes, as it was last drawn. */
   float settings_scroll, settings_most;
   /* What was wrong with the last emulator picked, if anything. */
   char emulator_note[128];
   /* The folder or game MODAL_SYSTEM or MODAL_EMULATOR is for. */
   int modal_item;
   float modal_scroll;
   /* A game's name while it's edited on its page: whose, and where. */
   char name[256];
   int name_game;
   SDL_FRect name_rect;
   /* The game settings' launch options while they're edited. */
   char options[512];
   enum modal modal;
   struct sgdb_game *matches;
   int match_count;
   bool matching;
   /* The name changed: searched once typing pauses (match_typed, ms). */
   bool match_pending;
   Uint64 match_typed;
} app;

/* ---- Colours and widgets ----------------------------------------------- */

#define C_BG        0.086f, 0.090f, 0.110f
#define C_SIDE      0.071f, 0.075f, 0.094f
#define C_CARD      0.125f, 0.130f, 0.157f
#define C_TEXT      0.95f, 0.95f, 0.97f
#define C_DIM       0.58f, 0.60f, 0.66f
#define C_ACCENT    0.30f, 0.56f, 1.00f
#define C_RED       0.93f, 0.33f, 0.30f
#define C_GREEN     0.40f, 0.85f, 0.55f

static float S(float v)
{
   return v * app.s;
}

static bool inside(float px, float py, float x, float y, float w, float h)
{
   return px >= x && px < x + w && py >= y && py < y + h;
}

static bool hover(float x, float y, float w, float h)
{
   return app.modal == MODAL_NONE && inside(app.mx, app.my, x, y, w, h);
}

/* True when this frame's click is on the box (and takes it). Under a
 * modal only the modal's widgets (modal true) get clicks. */
static bool clicked_in(float x, float y, float w, float h, bool modal)
{
   if (!app.click || (app.modal != MODAL_NONE) != modal || !inside(app.cx, app.cy, x, y, w, h))
      return false;
   app.click = false;
   app.redraw = true;
   return true;
}

/* Shows text in a small box centred under (cx, y), over everything. */
static void tooltip(float cx, float y, const char *text)
{
   app.tip = text;
   app.tip_x = cx;
   app.tip_y = y;
}

static void draw_tooltip(void)
{
   float w, x;

   if (!app.tip)
      return;
   w = ui_text_width(UI_MEDIUM, S(13), app.tip) + S(20);
   x = SDL_clamp(app.tip_x - w / 2, S(4), app.width - w - S(4));
   ui_rect(x, app.tip_y, w, S(28), S(8), 0.03f, 0.03f, 0.04f, 0.95f);
   ui_ring(x, app.tip_y, w, S(28), S(8), S(1), 1, 1, 1, 0.1f);
   ui_text(UI_MEDIUM, S(13), x + w / 2, app.tip_y + S(14), UI_CENTER, 0, C_TEXT, 1, app.tip);
   app.tip = NULL;
}

static void no_drag(float x, float y, float w, float h)
{
   if (app.nodrag_count < NODRAG_MAX)
      app.nodrag[app.nodrag_count++] = (SDL_FRect){ x, y, w, h };
}

static bool button(float x, float y, float w, float h, const char *label, bool primary,
      bool enabled, bool modal)
{
   bool over = (modal || app.modal == MODAL_NONE) && inside(app.mx, app.my, x, y, w, h);
   float a = enabled ? 1.0f : 0.4f;

   if (primary)
      ui_rect(x, y, w, h, S(12), C_ACCENT, (over && enabled ? 1.0f : 0.88f) * a);
   else
      ui_rect(x, y, w, h, S(12), 1, 1, 1, (over && enabled ? 0.13f : 0.07f) * a);
   ui_text(UI_BOLD, S(15), x + w / 2, y + h / 2, UI_CENTER, w - S(12), C_TEXT, a, label);
   return enabled && clicked_in(x, y, w, h, modal);
}

static float button_w(const char *label)
{
   return ui_text_width(UI_BOLD, S(15), label) + S(36);
}

/* A small rounded label, as on the reference's tags. Returns its width. */
static float chip_w(const char *text)
{
   return ui_text_width(UI_BOLD, S(12), text) + S(20);
}

static float chip(float x, float y, const char *text, float r, float g, float b)
{
   float w = chip_w(text);

   ui_rect(x, y, w, S(24), S(12), 0.05f, 0.05f, 0.07f, 0.72f);
   ui_text(UI_BOLD, S(12), x + w / 2, y + S(12), UI_CENTER, 0, r, g, b, 1, text);
   return w;
}

/* A one-line text field; its text is edited by the key and text events
 * while it's the focused field. */
static void text_field(float x, float y, float w, float h, enum field f, const char *text,
      const char *hint, bool secret, bool modal, float indent)
{
   bool focused = app.field == f;
   char shown[300];
   size_t i, n;

   if (clicked_in(x, y, w, h, modal))
   {
      app.field = f;
      SDL_StartTextInput(app.window);
      focused = true;
   }
   ui_rect(x, y, w, h, S(12), 1, 1, 1, focused ? 0.09f : 0.06f);
   if (focused)
      ui_ring(x, y, w, h, S(12), S(1.5f), C_ACCENT, 0.8f);
   x += indent;
   w -= indent;
   if (!text[0])
   {
      ui_text(UI_MEDIUM, S(15), x + S(14), y + h / 2, UI_LEFT, w - S(28), C_DIM, 1, hint);
      if (!focused)
         return;
   }
   if (secret)
   {
      n = SDL_min(SDL_strlen(text), sizeof(shown) - 1);
      for (i = 0; i < n; i++)
         shown[i] = '*';
      shown[n] = '\0';
   }
   else
      SDL_strlcpy(shown, text, sizeof(shown));
   if (focused)
      SDL_strlcat(shown, "|", sizeof(shown));
   ui_text(UI_MEDIUM, S(15), x + S(14), y + h / 2, UI_LEFT, w - S(28), C_TEXT, 1, shown);
}

static char *field_text(enum field f, size_t *size)
{
   switch (f)
   {
      case FIELD_FILTER: *size = sizeof(app.filter); return app.filter;
      case FIELD_KEY: *size = sizeof(app.key); return app.key;
      case FIELD_RA_USER: *size = sizeof(app.ra_user); return app.ra_user;
      case FIELD_RA_PASSWORD: *size = sizeof(app.ra_password); return app.ra_password;
      case FIELD_MATCH: *size = sizeof(app.match); return app.match;
      case FIELD_NAME: *size = sizeof(app.name); return app.name;
      case FIELD_OPTIONS: *size = sizeof(app.options); return app.options;
      default: *size = 0; return NULL;
   }
}

/* Icons the font has no symbol for, drawn as thin lines (see on_icon). */
enum icon
{
   ICON_GEAR,
   ICON_CLOSE,
   ICON_MIN,
   ICON_MAX,
   ICON_RESTORE,
   ICON_SEARCH,
   ICON_CHECK,
   ICON_BOLT,
   ICONS
};

/* The lightning bolt of FuseBox's icon, in a square from -0.5 to 0.5 (y
 * down). tools/make_icon.py draws the .exe's icon from the same points. */
static const float bolt[][2] = {
   { 0.14f, -0.40f }, { -0.27f, 0.06f }, { -0.01f, 0.06f },
   { -0.14f, 0.40f }, { 0.27f, -0.06f }, { 0.01f, -0.06f },
};

static bool in_bolt(float x, float y)
{
   int i, j, n = (int)SDL_arraysize(bolt);
   bool in = false;

   /* Even-odd: a ray to the right crosses the edges an odd number of
    * times from inside. */
   for (i = 0, j = n - 1; i < n; j = i++)
      if ((bolt[i][1] > y) != (bolt[j][1] > y)
            && x < (bolt[j][0] - bolt[i][0]) * (y - bolt[i][1]) / (bolt[j][1] - bolt[i][1])
               + bolt[i][0])
         in = !in;
   return in;
}

static void draw_icon(enum icon icon, float cx, float cy, float size, float r, float g, float b,
      float a);

/* A checkbox: an outline when off; filled with a check mark when on, blue,
 * or red for "remove". */
static void draw_tick(float x, float cy, bool checked, bool red)
{
   if (!checked)
   {
      ui_ring(x, cy - S(9), S(18), S(18), S(5), S(1.5f), 1, 1, 1, 0.45f);
      return;
   }
   /* "Remove" gets a cross, not a check. */
   if (red)
   {
      ui_rect(x, cy - S(9), S(18), S(18), S(5), C_RED, 1);
      draw_icon(ICON_CLOSE, x + S(9), cy, 19, C_TEXT, 1);
      return;
   }
   ui_rect(x, cy - S(9), S(18), S(18), S(5), C_ACCENT, 1);
   draw_icon(ICON_CHECK, x + S(9), cy, 18, C_TEXT, 1);
}

/* The image scaled to fill the box, its middle kept (cropped, not
 * squashed). */
static void draw_cover(GLuint tex, int tw, int th, float x, float y, float w, float h, float r,
      float a)
{
   float u0 = 0, v0 = 0, u1 = 1, v1 = 1, ta = (float)tw / th, ba = w / h;

   if (ta > ba)
   {
      u0 = (1 - ba / ta) / 2;
      u1 = 1 - u0;
   }
   else
   {
      v0 = (1 - ta / ba) / 2;
      v1 = 1 - v0;
   }
   ui_image_uv(tex, x, y, w, h, r, a, u0, v0, u1, v1);
}

/* The image scaled to fit the box, whole. */
static void draw_fit(GLuint tex, int tw, int th, float x, float y, float w, float h, float r)
{
   float f = SDL_min(w / tw, h / th), dw = tw * f, dh = th * f;
   ui_image(tex, x + (w - dw) / 2, y + (h - dh) / 2, dw, dh, r, 1.0f);
}

/* ---- Steam state ------------------------------------------------------- */

/* Whether the game has a tick: a launcher game (in Steam or not), or
 * anything gone from disk that Steam still has (take it out). Steam's
 * own shortcuts that are still there only get their art. */
static bool tickable(const struct game *g)
{
   /* A ROM without an emulator can't be added, only taken out. */
   return g->gone || (g->installed && g->exe[0] && SDL_strcmp(g->source, "Steam") != 0
         && (!g->no_emulator || g->steam_appid));
}

/* Whether its shortcut in Steam has other settings than the game's (and
 * stays in Steam): the next update sets them. Steam's own shortcuts keep
 * their launch options, which are the user's in Steam. */
static bool settings_differ(const struct game *g)
{
   char options[1100];

   if (!g->steam_appid || g->gone || (tickable(g) && !g->selected))
      return false;
   if (g->art_edited || g->steam_overlay != g->sc_overlay || g->steam_input != g->sc_input
         || (g->rom[0] && SDL_strcmp(g->exe, g->sc_exe) != 0))
      return !g->no_emulator;
   if (SDL_strcmp(g->source, "Steam") == 0)
      return false;
   if (SDL_strcmp(g->name, g->sc_name) != 0)
      return !g->no_emulator;
   library_launch_options(g, options, sizeof(options));
   return SDL_strcmp(options, g->sc_options) != 0 && !g->no_emulator;
}

/* Games to add to or take out of Steam, or whose settings change, on the
 * next update. */
static int steam_changes(void)
{
   int i, n = 0;

   library_lock();
   for (i = 0; i < library_count(); i++)
   {
      const struct game *g = library_game(i);
      if ((g->gone ? g->selected : tickable(g) && g->selected != (g->steam_appid != 0))
            || settings_differ(g))
         n++;
   }
   library_unlock();
   return n;
}

/* Ticked, but not in Steam yet. */
static bool adding(const struct game *g)
{
   return !g->gone && tickable(g) && !g->steam_appid && g->selected;
}

/* What Steam has of the game and what the next update does, with its
 * colour; NULL for nothing to say. */
static const char *steam_state(const struct game *g, float *r, float *gr, float *b)
{
   if (g->gone)
   {
      *r = 0.93f, *gr = 0.33f, *b = 0.30f;
      return g->selected ? "Removing" : "Uninstalled";
   }
   if (tickable(g) && g->steam_appid && !g->selected)
   {
      *r = 0.93f, *gr = 0.33f, *b = 0.30f;
      return "Removing";
   }
   if (adding(g))
   {
      *r = 0.30f, *gr = 0.56f, *b = 1.00f;
      return "Adding";
   }
   if (settings_differ(g))
   {
      *r = 0.30f, *gr = 0.56f, *b = 1.00f;
      return "Edited";
   }
   if (g->steam_appid)
   {
      *r = 0.40f, *gr = 0.85f, *b = 0.55f;
      return "Added";
   }
   return NULL;
}

/* ---- Art textures ------------------------------------------------------ */

/* The saved image, asking for it when it's missing or changed. */
static GLuint art_texture(struct art_tex *t, int index, const struct game *g, enum art_type type,
      int slot, int max_w, int max_h)
{
   const char *path = g->art[type].path;

   if (t->asked && (t->game != index || t->version != g->art_version))
   {
      if (t->tex)
         glDeleteTextures(1, &t->tex);
      t->tex = 0;
      t->asked = false;
   }
   if (path[0] && !t->asked)
   {
      struct cover_job *j = SDL_calloc(1, sizeof(*j));
      t->asked = true;
      t->game = index;
      t->version = g->art_version;
      if (j)
      {
         j->index = index;
         j->version = g->art_version;
         j->slot = slot;
         j->max_w = max_w;
         j->max_h = max_h;
         SDL_strlcpy(j->path, path, sizeof(j->path));
         jobs_add(job_cover, j, JOB_NOW, slot >= 0);
      }
   }
   return t->tex;
}

/* The list's picture of a game: its icon, else its cover (cropped
 * square). covers holds two per game: [2i] the icon, [2i + 1] the cover. */
static struct art_tex *list_art(int index, const struct game *g)
{
   struct art_tex *t;

   if (index >= app.covers_alloc)
      return NULL;
   t = &app.covers[index * 2];
   if (g->art[ART_ICON].path[0] && art_texture(t, index, g, ART_ICON, -2, 128, 128))
      return t;
   t = &app.covers[index * 2 + 1];
   return art_texture(t, index, g, ART_COVER, -1, 240, 360) ? t : NULL;
}

static GLuint detail_for(enum art_type t, const struct game *g)
{
   static const int sizes[ART_TYPES][2] = {
      { 600, 900 }, { 920, 430 }, { 1920, 620 }, { 800, 400 }, { 256, 256 },
   };
   return art_texture(&app.detail[t], app.selected, g, t, (int)t, sizes[t][0], sizes[t][1]);
}

/* ---- The picker -------------------------------------------------------- */

static void picker_clear(void)
{
   int i;

   for (i = 0; i < app.item_count; i++)
   {
      if (app.items[i].tex)
         glDeleteTextures(1, &app.items[i].tex);
      if (app.items[i].anim)
         SDL_AddAtomicInt(&anim_total, -(app.items[i].anim->w * app.items[i].anim->h * 4
                  * app.items[i].anim->count));
      SDL_free(app.items[i].anim);
   }
   SDL_free(app.items);
   app.items = NULL;
   app.item_count = 0;
   app.pick_msg[0] = '\0';
   SDL_AddAtomicInt(&picker_gen, 1);
}

/* Lists the selected game's art of the current tab. */
static void picker_open(void)
{
   struct list_job *j;

   picker_clear();
   app.want_list = false;
   if (app.selected < 0)
      return;
   if (!sgdb_has_key())
   {
      SDL_strlcpy(app.pick_msg, "Add a SteamGridDB API key in Settings to pick art",
            sizeof(app.pick_msg));
      return;
   }
   if (!(j = SDL_calloc(1, sizeof(*j))))
      return;
   j->gen = SDL_GetAtomicInt(&picker_gen);
   j->index = app.selected;
   j->type = app.tab;
   app.pick_loading = true;
   jobs_add(job_list, j, JOB_SELECTED, true);
}

static void ask_thumb(int item)
{
   struct pick_item *it = &app.items[item];
   struct thumb_job *j;

   if (it->asked || !(j = SDL_calloc(1, sizeof(*j))))
      return;
   it->asked = true;
   j->gen = SDL_GetAtomicInt(&picker_gen);
   j->item = item;
   j->animated = it->art.animated;
   SDL_strlcpy(j->url, it->url, sizeof(j->url));
   jobs_add(job_thumb, j, SDL_strncmp(j->url, "file:", 5) == 0 ? JOB_NOW
         : it->in_use ? JOB_SELECTED : JOB_OPTIONS, false);
}

/* The item's texture, moved on to the frame due now if it's playing. */
static GLuint item_texture(struct pick_item *it)
{
   Uint64 now = SDL_GetTicks();
   int frame, next;

   if (!it->anim)
      return it->tex;
   frame = anim_frame(it->anim, now, &next);
   if (frame != it->frame)
   {
      it->frame = frame;
      glBindTexture(GL_TEXTURE_2D, it->tex);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, it->w, it->h, GL_RGBA, GL_UNSIGNED_BYTE,
            it->anim->pixels + (size_t)it->w * it->h * 4 * frame);
      glBindTexture(GL_TEXTURE_2D, 0);
   }
   now += (Uint64)SDL_max(next, 10);
   if (!app.anim_at || now < app.anim_at)
      app.anim_at = now;
   return it->tex;
}

static void on_art_list(struct msg *m)
{
   char saved[1024] = "";
   unsigned chosen = 0;
   int i;

   if (m->gen != SDL_GetAtomicInt(&picker_gen))
   {
      SDL_free(m->ptr);
      return;
   }
   app.pick_loading = false;
   SDL_strlcpy(app.pick_msg, m->text, sizeof(app.pick_msg));
   if (m->count <= 0 || !(app.items = SDL_calloc((size_t)m->count, sizeof(*app.items))))
   {
      SDL_free(m->ptr);
      return;
   }
   app.item_count = m->count;
   library_lock();
   if (library_game(m->index))
   {
      chosen = library_game(m->index)->art[app.tab].sgdb_id;
      SDL_strlcpy(saved, library_game(m->index)->art[app.tab].path, sizeof(saved));
   }
   library_unlock();
   for (i = 0; i < m->count; i++)
   {
      struct pick_item *it = &app.items[i];
      it->art = ((struct sgdb_art *)m->ptr)[i];
      it->in_use = chosen && it->art.id == chosen;
      /* The one in use is on disk: shown at once. (Not an animated one: its
       * small preview is in sooner than its file is decoded.) */
      if (it->in_use && saved[0] && !it->art.animated)
         SDL_snprintf(it->url, sizeof(it->url), "file:%s", saved);
      else
         SDL_strlcpy(it->url, it->art.thumb, sizeof(it->url));
      if (!it->art.animated || it->in_use)
         ask_thumb(i);
   }
   SDL_free(m->ptr);
}

/* Each tile's size by type, in points: like the art's own shape. */
static void tile_size(enum art_type t, float *w, float *h)
{
   static const float sizes[ART_TYPES][2] = {
      { 160, 240 }, { 290, 136 }, { 400, 129 }, { 250, 125 }, { 110, 110 },
   };
   *w = S(sizes[t][0]);
   *h = S(sizes[t][1]);
}

/* A game's missing art is looked up the first time it's shown, what's
 * shown first: a ROM's icon in the list; when it's opened its icon, its art
 * of the kind shown, then the rest. Under the library's lock. */
static void fill_once(int index, Uint8 what, enum job_level level, bool first, bool listed)
{
   if (index < 0 || index >= app.covers_alloc || (app.filled[index] & what))
      return;
   app.filled[index] |= what;
   queue_fill(index, what, level, first, listed);
}

static void fill_row(int index, const struct game *g)
{
   if (g->rom[0] && !g->art[ART_ICON].path[0] && !g->art[ART_COVER].path[0] && sgdb_has_key()
         && SDL_GetAtomicInt(&list_fills) < LIST_FILLS)
      fill_once(index, FILL_ICON, JOB_ICON, false, true);
}

static void fill_open(int index, const struct game *g)
{
   if (!g->installed || !sgdb_has_key())
      return;
   if (!g->art[ART_ICON].path[0])
      fill_once(index, FILL_ICON, JOB_ICON, true, false);
   if (!g->art[app.tab].path[0] && app.tab != ART_ICON)
      fill_once(index, (Uint8)(FILL_TYPE << app.tab), JOB_SELECTED, true, false);
   if (g->rom[0])
      fill_once(index, FILL_ALL, JOB_REST, true, false);
}

/* It was renamed or matched to another game: what wasn't found may be. */
static void fill_again(int index)
{
   if (index >= 0 && index < app.covers_alloc)
      app.filled[index] = 0;
}

static void open_game(int index)
{
   app.page = PAGE_GAME;
   app.game_scroll = 0;
   if (index != app.selected)
   {
      app.selected = index;
      app.want_list = true;
   }
}

/* ---- Sidebar ----------------------------------------------------------- */

static void start_steam_job(bool close_steam)
{
   struct steam_job *j = SDL_calloc(1, sizeof(*j));

   if (!j)
      return;
   j->close_steam = close_steam;
   app.busy = true;
   app.steam_note[0] = '\0';
   jobs_add(job_steam, j, JOB_NOW, true);
}

/* The Open dialog's answer, on whatever thread SDL calls it from: handed
 * to the window as a message. Empty list: cancelled. */
static void SDLCALL on_exe_picked(void *user, const char *const *files, int filter)
{
   struct msg *m;

   if (!files || !files[0] || !(m = new_msg(MSG_ADD_EXE)))
      return;
   m->ptr = SDL_strdup(files[0]);
   post(m);
}

static void pick_exe(void)
{
#ifdef _WIN32
   static const SDL_DialogFileFilter filters[] = { { "Programs", "exe" } };
   SDL_ShowOpenFileDialog(on_exe_picked, NULL, app.window, filters, 1, NULL, false);
#else
   SDL_ShowOpenFileDialog(on_exe_picked, NULL, app.window, NULL, 0, NULL, false);
#endif
}

/* A folder or an emulator picked: handed on like the exe above. */
static void SDLCALL on_rom_folder(void *user, const char *const *files, int filter)
{
   struct msg *m;

   if (!files || !files[0] || !(m = new_msg(MSG_ROM_FOLDER)))
      return;
   m->ptr = SDL_strdup(files[0]);
   post(m);
}

/* user: the game's index. */
static void SDLCALL on_emulator_picked(void *user, const char *const *files, int filter)
{
   struct msg *m;

   if (!files || !files[0] || !(m = new_msg(MSG_EMULATOR)))
      return;
   m->index = (int)(intptr_t)user;
   m->ptr = SDL_strdup(files[0]);
   post(m);
}

static void pick_emulator(int game)
{
   void *user = (void *)(intptr_t)game;
#ifdef _WIN32
   static const SDL_DialogFileFilter filters[] = { { "Programs", "exe" } };
   SDL_ShowOpenFileDialog(on_emulator_picked, user, app.window, filters, 1, NULL, false);
#else
   SDL_ShowOpenFileDialog(on_emulator_picked, user, app.window, NULL, 0, NULL, false);
#endif
}

/* keep_ticks: what's ticked but not in Steam yet (or the other way) stays
 * so. */
static void rescan(bool keep_ticks)
{
   /* One at a time, and not while Steam is updated (which scans after). */
   if (!app.scanned || app.busy)
   {
      app.scan_again = true;
      return;
   }
   /* The status line keeps the last count until the new one is in. */
   app.scanned = false;
   jobs_add(job_scan, keep_ticks ? SDL_strdup("") : NULL, JOB_NOW, true);
}

/* Icons the font has no symbol for, made at the exact pixel size they're
 * drawn at. Line icons are shaded by each pixel's distance to the nearest
 * stroke, so where strokes cross is no brighter than anywhere along them;
 * the filled bolt is sampled 4x4 times a pixel. */

/* Distance from (px, py) to the segment (ax, ay)-(bx, by). */
static float segment_distance(float px, float py, float ax, float ay, float bx, float by)
{
   float dx = bx - ax, dy = by - ay;
   float t = SDL_clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
   float ex = px - (ax + t * dx), ey = py - (ay + t * dy);

   return SDL_sqrtf(ex * ex + ey * ey);
}

/* Distance from (fx, fy), in the icon's square (-0.5 to 0.5), to the
 * middle of its nearest stroke; half is half a stroke, in the same units. */
static float stroke_distance(enum icon icon, float fx, float fy, float half)
{
   if (icon == ICON_GEAR)
   {
      /* A ring, and eight solid square-ended teeth on it, one pointing
       * straight up (y grows down); the middle open. Teeth are filled
       * blocks, not strokes: round-ended strokes this short blur into
       * drops. A block's edge distance less half a stroke shades it as
       * filled. */
      float d = SDL_fabsf(SDL_sqrtf(fx * fx + fy * fy) - 0.28f);
      int k;
      for (k = 0; k < 8; k++)
      {
         float a = -SDL_PI_F / 2 + k * 2 * SDL_PI_F / 8, c = SDL_cosf(a), s = SDL_sinf(a);
         float along = fx * c + fy * s, across = -fx * s + fy * c;
         float qx = SDL_fabsf(along - 0.345f) - 0.065f, qy = SDL_fabsf(across) - 0.05f;
         float ox = SDL_max(qx, 0.0f), oy = SDL_max(qy, 0.0f);
         float box = SDL_sqrtf(ox * ox + oy * oy) + SDL_min(SDL_max(qx, qy), 0.0f);
         d = SDL_min(d, SDL_max(box + half, 0.0f));
      }
      return d;
   }   if (icon == ICON_CHECK)
      return SDL_min(segment_distance(fx, fy, -0.24f, 0.01f, -0.08f, 0.17f),
            segment_distance(fx, fy, -0.08f, 0.17f, 0.25f, -0.17f));
   /* Search: a lens and its handle, down to the left (as Windows' own). */
   if (icon == ICON_SEARCH)
      return SDL_min(SDL_fabsf(SDL_sqrtf((fx - 0.08f) * (fx - 0.08f) + (fy + 0.08f) * (fy + 0.08f))
               - 0.22f),
            segment_distance(fx, fy, -0.08f, 0.08f, -0.32f, 0.32f));
   /* Drawn caption buttons, for when there's no icon font. */
   if (icon == ICON_MIN)
      return segment_distance(fx, fy, -0.35f, 0, 0.35f, 0);
   if (icon == ICON_MAX)
      return SDL_fabsf(SDL_max(SDL_fabsf(fx), SDL_fabsf(fy)) - 0.33f);
   if (icon == ICON_RESTORE)
      return SDL_min(SDL_fabsf(SDL_max(SDL_fabsf(fx + 0.06f), SDL_fabsf(fy - 0.06f)) - 0.27f),
            fx > -0.15f && fy < 0.15f ? SDL_fabsf(SDL_max(SDL_fabsf(fx - 0.06f),
               SDL_fabsf(fy + 0.06f)) - 0.27f) : 1.0f);
   /* Close: two diagonals, each as long as minimize's line (12 at 17). */
   return SDL_min(segment_distance(fx, fy, -0.25f, -0.25f, 0.25f, 0.25f),
         segment_distance(fx, fy, -0.25f, 0.25f, 0.25f, -0.25f));
}

/* Each icon's strokes, in pixels at scale 1: like minimize's line (the
 * gear's a shade finer, having more of them); the check bolder. */
static float stroke_width(enum icon icon, int px)
{
   if (icon == ICON_CHECK)
      return px * 0.15f;
   return S(icon == ICON_GEAR ? 1.3f : 1.5f);
}

/* A few sizes kept per icon (the close icon is drawn at two). */
#define ICON_SIZES 3
static struct
{
   GLuint tex;
   int px;
} icon_tex[ICONS][ICON_SIZES];

/* Windows' own icon font (Segoe MDL2 Assets, or Segoe Fluent Icons on
 * Windows 11): the caption buttons', the settings gear and the check as
 * Windows draws them, made for small sizes. Read from Windows' fonts, not
 * shipped. Without it the drawn ones above. */
static struct
{
   void *data;
   stbtt_fontinfo info;
   bool tried, ok;
} icon_font;

static bool load_icon_font(void)
{
#ifdef _WIN32
   static const char *const files[] = { "SegoeIcons.ttf", "segmdl2.ttf" };
   const char *windir = SDL_getenv_unsafe("WINDIR");
   size_t i;

   if (icon_font.tried)
      return icon_font.ok;
   icon_font.tried = true;
   for (i = 0; windir && i < SDL_arraysize(files) && !icon_font.ok; i++)
   {
      char path[1100];
      SDL_snprintf(path, sizeof(path), "%s\\Fonts\\%s", windir, files[i]);
      SDL_free(icon_font.data);
      icon_font.data = SDL_LoadFile(path, NULL);
      icon_font.ok = icon_font.data && stbtt_InitFont(&icon_font.info, icon_font.data,
            stbtt_GetFontOffsetForIndex(icon_font.data, 0));
   }
#endif
   return icon_font.ok;
}

/* The icon's glyph in that font, and how big its em is against the box. */
static Uint32 icon_glyph(enum icon icon, float *em)
{
   switch (icon)
   {
      case ICON_CLOSE: *em = 0.6f; return 0xE8BB;
      case ICON_MIN: *em = 0.6f; return 0xE921;
      case ICON_MAX: *em = 0.6f; return 0xE922;
      case ICON_RESTORE: *em = 0.6f; return 0xE923;
      default: *em = 0; return 0;
   }
}

/* The glyph into the px-square mask, its ink centred on whole pixels. */
static bool font_icon(enum icon icon, Uint8 *mask, int px)
{
   float em, scale;
   Uint32 cp = icon_glyph(icon, &em);
   int x0, y0, x1, y1, w, h;

   if (!cp || !load_icon_font() || !stbtt_FindGlyphIndex(&icon_font.info, (int)cp))
      return false;
   scale = stbtt_ScaleForMappingEmToPixels(&icon_font.info, SDL_roundf(px * em));
   stbtt_GetCodepointBitmapBox(&icon_font.info, (int)cp, scale, scale, &x0, &y0, &x1, &y1);
   w = SDL_min(x1 - x0, px);
   h = SDL_min(y1 - y0, px);
   SDL_memset(mask, 0, (size_t)px * px);
   stbtt_MakeCodepointBitmap(&icon_font.info, mask + ((px - h) / 2) * px + (px - w) / 2, w, h, px,
         scale, scale, (int)cp);
   return true;
}

static GLuint icon_texture(enum icon icon, int px)
{
   Uint8 *mask;
   int x, y, sx, sy, slot;

   for (slot = 0; slot < ICON_SIZES; slot++)
      if (icon_tex[icon][slot].tex && icon_tex[icon][slot].px == px)
         return icon_tex[icon][slot].tex;
   /* A free slot, else the first (sizes only change with the display's
    * scale). */
   for (slot = 0; slot < ICON_SIZES - 1 && icon_tex[icon][slot].tex; slot++)
      ;
   if (icon_tex[icon][slot].tex)
      glDeleteTextures(1, &icon_tex[icon][slot].tex);
   icon_tex[icon][slot].tex = 0;
   if (px < 1 || !(mask = SDL_malloc((size_t)px * px)))
      return 0;
   for (y = font_icon(icon, mask, px) ? px : 0; y < px; y++)
      for (x = 0; x < px; x++)
      {
         float fx = (x + 0.5f) / px - 0.5f, fy = (y + 0.5f) / px - 0.5f, a;
         if (icon == ICON_BOLT)
         {
            int in = 0;
            for (sy = 0; sy < 4; sy++)
               for (sx = 0; sx < 4; sx++)
                  in += in_bolt((x + (sx + 0.5f) / 4) / px - 0.5f, (y + (sy + 0.5f) / 4) / px - 0.5f);
            a = in / 16.0f;
         }
         else
         {
            /* Coverage of a pixel by a line w wide, from how far its middle
             * is from the line's. */
            float w = stroke_width(icon, px), d = stroke_distance(icon, fx, fy, w / 2 / px) * px;
            a = SDL_clamp(w / 2 - d + 0.5f, 0.0f, 1.0f);
            if (w < 1)
               a *= w;
         }
         mask[y * px + x] = (Uint8)(a * 255 + 0.5f);
      }
   glGenTextures(1, &icon_tex[icon][slot].tex);
   glBindTexture(GL_TEXTURE_2D, icon_tex[icon][slot].tex);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, px, px, 0, GL_RED, GL_UNSIGNED_BYTE, mask);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glBindTexture(GL_TEXTURE_2D, 0);
   SDL_free(mask);
   icon_tex[icon][slot].px = px;
   return icon_tex[icon][slot].tex;
}

/* The icon, size points wide, centred on (cx, cy): on whole pixels, one
 * texel to a pixel, so it stays sharp. */
static void draw_icon(enum icon icon, float cx, float cy, float size, float r, float g, float b,
      float a)
{
   int px = (int)(S(size) + 0.5f);
   float x = SDL_floorf(cx - px / 2.0f + 0.5f), y = SDL_floorf(cy - px / 2.0f + 0.5f);

   ui_mask(icon_texture(icon, px), x, y, (float)px, (float)px, r, g, b, a);
}

static void settings_icon(float cx, float cy, float size, float r, float g, float b)
{
   draw_icon(ICON_GEAR, cx, cy, size, r, g, b, 1);
}

/* The sidebar: the mark, the search, the games, Settings and Steam. */
static void draw_sidebar(float w)
{
   float x = S(16), lw = w - S(32), row = S(44), ch = S(124);
   float card_y = app.height - S(20) - ch;
   float list_top = S(176), list_h = card_y - S(12) - list_top, content, first_y;
   bool all = true, any = false, on;
   char line[64];
   int i, n = steam_changes();

   ui_rect(0, 0, w, (float)app.height, 0, C_SIDE, 1);

   /* The mark: the app's icon, a bolt on a blue tile. */
   ui_rect(S(24), S(24), S(32), S(32), S(9), C_ACCENT, 1);
   draw_icon(ICON_BOLT, S(40), S(40), 32, C_TEXT, 1);
   ui_text(UI_BOLD, S(21), S(68), S(40), UI_LEFT, 0, C_TEXT, 1, "FuseBox");

   /* Search, and beside it + to add a game by its exe. */
   text_field(x, S(80), lw - S(52), S(42), FIELD_FILTER, app.filter, "Search games", false, false,
         S(26));
   {
      float px = x + lw - S(42);
      bool over = hover(px, S(80), S(42), S(42));
      ui_rect(px, S(80), S(42), S(42), S(12), 1, 1, 1, over ? 0.12f : 0.06f);
      ui_rect(px + S(21) - S(7), S(101) - S(0.75f), S(14), S(1.5f), 0, C_TEXT, 0.9f);
      ui_rect(px + S(21) - S(0.75f), S(101) - S(7), S(1.5f), S(14), 0, C_TEXT, 0.9f);
      if (over)
         tooltip(px + S(21), S(80) + S(42) + S(8), "Add game");
      if (clicked_in(px, S(80), S(42), S(42), false))
         pick_exe();
   }
   draw_icon(ICON_SEARCH, x + S(19), S(101), 16, C_DIM, 1);

   /* How many, and select all. */
   library_lock();
   for (i = 0; i < app.view_count; i++)
   {
      const struct game *g = library_game(app.view[i]);
      if (g && tickable(g))
      {
         any = true;
         all &= g->selected;
      }
   }
   /* Select all in line with the rows' ticks; the count across from it. */
   SDL_snprintf(line, sizeof(line), "%d game%s", app.view_count, app.view_count == 1 ? "" : "s");
   ui_text(UI_MEDIUM, S(13), x + lw - S(10), S(150), UI_RIGHT, 0, C_DIM, 1, line);
   if (any)
   {
      float sx = x + S(10);
      draw_tick(sx, S(150), all, false);
      ui_text(UI_MEDIUM, S(13), sx + S(28), S(150), UI_LEFT, 0, C_TEXT, 0.8f, "Select all");
      if (clicked_in(sx - S(6), S(136), S(112), S(28), false))
         for (i = 0; i < app.view_count; i++)
         {
            struct game *g = library_game(app.view[i]);
            if (g && tickable(g))
               g->selected = !all;
         }
   }

   /* The games. */
   content = app.view_count * row;
   if (app.wheel != 0 && inside(app.wx, app.wy, 0, list_top, w, list_h))
   {
      app.list_scroll -= app.wheel * row * 1.5f;
      app.wheel = 0;
   }
   app.list_scroll = SDL_clamp(app.list_scroll, 0, SDL_max(0, content - list_h));
   first_y = list_top - app.list_scroll;
   glEnable(GL_SCISSOR_TEST);
   glScissor(0, app.height - (int)(list_top + list_h), (int)w, (int)list_h);
   for (i = 0; i < app.view_count; i++)
   {
      int index = app.view[i];
      struct game *g = library_game(index);
      float ry = first_y + i * row, tx, sr, sg, sb;
      bool in_list = inside(app.cx, app.cy, 0, list_top, w, list_h);
      const char *state;
      struct art_tex *pic;

      if (!g || ry + row < list_top || ry > list_top + list_h)
         continue;
      /* Every other row a shade lighter, so each reads as its own; the
       * one shown and the one under the mouse lighter still. */
      on = app.page == PAGE_GAME && index == app.selected;
      if (on)
      {
         ui_rect(x, ry + S(1), lw, row - S(2), S(10), C_ACCENT, 0.16f);
         ui_rect(S(4), ry + S(13), S(4), row - S(26), S(2), C_ACCENT, 1);
      }
      else if (hover(x, ry + S(1), lw, row - S(2)) && inside(app.mx, app.my, 0, list_top, w, list_h))
         ui_rect(x, ry + S(1), lw, row - S(2), S(10), 1, 1, 1, 0.07f);
      else if (i % 2 == 0)
         ui_rect(x, ry + S(1), lw, row - S(2), S(10), 1, 1, 1, 0.03f);

      if (tickable(g))
      {
         draw_tick(x + S(10), ry + row / 2, g->selected, g->gone);
         if (in_list && clicked_in(x, ry, S(38), row, false))
            g->selected = !g->selected;
      }

      fill_row(index, g);
      pic = list_art(index, g);
      if (pic)
         draw_cover(pic->tex, pic->w, pic->h, x + S(38), ry + S(8), S(28), S(28), 0, 1);
      else
         ui_rect(x + S(38), ry + S(8), S(28), S(28), S(7), C_CARD, 1);

      /* The name, and under it what Steam has of it (centred alone when
       * there's nothing to say). Where it's from shows on its page. */
      tx = x + S(78);
      state = steam_state(g, &sr, &sg, &sb);
      ui_text(UI_BOLD, S(14), tx, ry + (state ? S(15) : row / 2), UI_LEFT, x + lw - tx - S(10),
            C_TEXT, g->gone ? 0.55f : 1.0f, g->name);
      if (state)
         ui_text(UI_BOLD, S(11.5f), tx, ry + S(31), UI_LEFT, x + lw - tx - S(10), sr, sg, sb, 1,
               state);

      if (in_list && clicked_in(x, ry, lw, row, false))
         open_game(index);
   }
   library_unlock();
   glDisable(GL_SCISSOR_TEST);
   if (!app.view_count)
      ui_text(UI_MEDIUM, S(14), w / 2, list_top + S(30), UI_CENTER, lw, C_DIM, 1,
            !app.scanned ? "Looking for games..." : app.filter[0] ? "No game matches"
            : "No games found");

   /* Steam: what an update would change. */
   ui_ring(x, card_y, lw, ch, S(16), S(1), 1, 1, 1, 0.12f);
   ui_text(UI_BOLD, S(16), x + S(18), card_y + S(26), UI_LEFT, 0, C_TEXT, 1, "Steam");
   /* What's waiting first; else what the last update said. */
   if (!steam_install())
      SDL_strlcpy(line, "Not found", sizeof(line));
   else if (app.busy)
      SDL_strlcpy(line, app.steam_note[0] ? app.steam_note : "Updating...", sizeof(line));
   else if (choosing())
      SDL_strlcpy(line, "Downloading artwork...", sizeof(line));
   else if (n)
      SDL_snprintf(line, sizeof(line), "%d change%s waiting", n, n == 1 ? "" : "s");
   else
      SDL_strlcpy(line, app.steam_note[0] ? app.steam_note : "Up to date", sizeof(line));
   ui_text(UI_MEDIUM, S(13), x + S(18), card_y + S(48), UI_LEFT, lw - S(36), C_DIM, 1, line);
   /* Always there: it also sends art picked since the last update. */
   if (button(x + S(14), card_y + ch - S(54), lw - S(28), S(40),
            app.busy ? "Working..." : "Update Steam Shortcuts", n > 0,
            steam_install() && !app.busy && app.scanned && !choosing(), false))
   {
      if (platform_steam_running(steam_install()))
         app.modal = MODAL_STEAM;
      else
         start_steam_job(false);
   }
}

/* ---- Top bar ----------------------------------------------------------- */

/* Minimize, maximize (or restore) and close, drawn: the window has no
 * frame of its own. */
static void window_buttons(float right, float cy)
{
   bool maximized = (SDL_GetWindowFlags(app.window) & SDL_WINDOW_MAXIMIZED) != 0;
   float bw = S(40), bh = S(30), x = right - 3 * bw, y = cy - bh / 2;
   int i;

   no_drag(x, y, 3 * bw, bh);
   for (i = 0; i < 3; i++, x += bw)
   {
      bool over = hover(x, y, bw, bh);
      float icx = x + bw / 2;

      if (over)
      {
         if (i == 2)
            ui_rect(x, y, bw, bh, S(10), C_RED, 1);
         else
            ui_rect(x, y, bw, bh, S(10), 1, 1, 1, 0.09f);
      }
      draw_icon(i == 0 ? ICON_MIN : i == 2 ? ICON_CLOSE : maximized ? ICON_RESTORE : ICON_MAX,
            icx, cy, 17, C_TEXT, 0.9f);
      if (clicked_in(x, y, bw, bh, false))
      {
         if (i == 0)
            SDL_MinimizeWindow(app.window);
         else if (i == 1 && maximized)
            SDL_RestoreWindow(app.window);
         else if (i == 1)
            SDL_MaximizeWindow(app.window);
         else
         {
            SDL_Event e;
            SDL_zero(e);
            e.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&e);
         }
      }
   }
}

static void draw_top(float x0)
{
   float right = app.width - S(8);

   /* Up in the corner, where windows keep them; Settings just before. */
   window_buttons(right, S(23));
   {
      float bw = S(40), bh = S(30), sx = right - 4 * bw, sy = S(23) - bh / 2;
      bool on = app.page == PAGE_SETTINGS;

      no_drag(sx, sy, bw, bh);
      if (on)
         ui_rect(sx, sy, bw, bh, S(10), C_ACCENT, 0.18f);
      else if (hover(sx, sy, bw, bh))
         ui_rect(sx, sy, bw, bh, S(10), 1, 1, 1, 0.09f);
      if (on)
         settings_icon(sx + bw / 2, S(23), 19, C_ACCENT);
      else
         settings_icon(sx + bw / 2, S(23), 19, C_TEXT);
      /* Again to go back to the game. */
      if (clicked_in(sx, sy, bw, bh, false))
      {
         app.page = on ? PAGE_GAME : PAGE_SETTINGS;
         app.key[0] = '\0';
         SDL_zeroa(app.ra_password);
         app.ra_error[0] = '\0';
      }
      right = sx + bw;
   }
   ui_text(UI_MEDIUM, S(14), x0, S(36), UI_LEFT, right - 3 * S(40) - x0 - S(16), C_DIM, 1,
         app.status);
}

/* ---- The games' list ---------------------------------------------------- */

static int compare_names(const void *a, const void *b)
{
   const struct game *ga = library_game(*(const int *)a), *gb = library_game(*(const int *)b);
   return SDL_strcasecmp(ga->name, gb->name);
}

/* Rebuilt when the library grows or the filter changes. */
static void build_view(void)
{
   char want[128], have[256];
   int i;

   library_lock();
   SDL_free(app.view);
   app.view = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, library_count()));
   app.view_count = 0;
   library_normalize(app.filter, want, sizeof(want));
   for (i = 0; app.view && i < library_count(); i++)
   {
      /* Uninstalled games and lookups stay in library.json (other apps
       * may still want their art) but aren't listed, unless Steam still
       * has a shortcut for them: then they're listed to be taken out. */
      if (!library_game(i)->installed && !library_game(i)->gone)
         continue;
      library_normalize(library_game(i)->name, have, sizeof(have));
      if (!want[0] || SDL_strstr(have, want))
         app.view[app.view_count++] = i;
   }
   if (app.view)
      SDL_qsort(app.view, (size_t)app.view_count, sizeof(int), compare_names);
   app.view_built_for = library_count();
   if (app.covers_alloc < library_count())
   {
      int alloc = library_count() + 64;
      struct art_tex *grown = SDL_realloc(app.covers, sizeof(*grown) * 2 * (size_t)alloc);
      if (grown)
      {
         SDL_memset(grown + 2 * app.covers_alloc, 0,
               sizeof(*grown) * 2 * (size_t)(alloc - app.covers_alloc));
         app.covers = grown;
         Uint8 *filled = SDL_realloc(app.filled, (size_t)alloc);
         if (filled)
         {
            SDL_memset(filled + app.covers_alloc, 0, (size_t)(alloc - app.covers_alloc));
            app.filled = filled;
            app.covers_alloc = alloc;
         }
      }
   }
   library_unlock();
}

/* ---- Game page --------------------------------------------------------- */

static void open_game_settings(void);

static void end_name(void)
{
   app.field = FIELD_NONE;
   SDL_StopTextInput(app.window);
}

/* Takes the name as it was typed. */
static void save_name(void)
{
   library_rename(app.name_game, app.name);
   end_name();
   app.view_built_for = -1;
   /* One not found on SteamGridDB may be by its new name. */
   library_lock();
   if (library_game(app.name_game) && !library_game(app.name_game)->sgdb_id)
   {
      app.want_list = true;
      fill_again(app.name_game);
   }
   library_unlock();
}

static void draw_game(float x, float y, float w, float h)
{
   struct game *g;
   char name[256], own_name[256], source[32], sgdb_name[256], emulator[96], line[300], achievements[48];
   const struct rom_option *chosen_emulator;
   struct choice *coming;
   float top = y, cx, sr = 0, sg = 0, sb = 0;
   float tw, th, tx, content_h, ty, grid_top, grid_h;
   const char *state;
   unsigned chosen;
   bool removing, gone, chosen_own, no_emulator;
   GLuint icon;
   int i, t, cols;

   library_lock();
   g = library_game(app.selected);
   if (!g)
   {
      library_unlock();
      ui_text(UI_MEDIUM, S(18), x + w / 2, y + h / 2 - S(40), UI_CENTER, w, C_DIM, 1,
            app.scanned ? "Pick a game on the left" : "Looking for games...");
      return;
   }
   SDL_strlcpy(name, g->name, sizeof(name));
   SDL_strlcpy(own_name, g->own_name, sizeof(own_name));
   SDL_strlcpy(source, g->source, sizeof(source));
   SDL_strlcpy(sgdb_name, g->sgdb_name, sizeof(sgdb_name));
   chosen = g->art[app.tab].sgdb_id;
   /* Id 0 is the game's own icon, chosen only when it's what's saved. */
   chosen_own = g->art[app.tab].path[0] && chosen == 0;
   /* Just picked: in use as far as anyone can see. */
   if ((coming = choice_for(app.selected, app.tab)))
   {
      chosen = coming->id;
      chosen_own = chosen == 0;
   }
   removing = g->gone && g->selected;
   gone = g->gone;
   no_emulator = g->no_emulator;
   chosen_emulator = no_emulator ? NULL : roms_chosen(g);
   SDL_strlcpy(emulator, chosen_emulator ? chosen_emulator->label : "", sizeof(emulator));
   achievements[0] = '\0';
   if (g->ra_total && g->rom[0])
      SDL_snprintf(achievements, sizeof(achievements), "Achievements %u/%u", g->ra_unlocked,
            g->ra_total);
   fill_open(app.selected, g);
   icon = detail_for(ART_ICON, g);
   /* The list says so already. */
   state = adding(g) ? NULL : steam_state(g, &sr, &sg, &sb);
   library_unlock();

   /* The icon, the name, where it's from and its match, the buttons:
    * fixed, like the tabs under it; only the art scrolls. */
   {
      float sh = S(96), bx, mw = button_w("Wrong game?");

      /* Red while it's set to leave Steam (uninstalled, ticked). */
      if (removing)
      {
         ui_rect(x, y, w, sh, S(20), 0.20f, 0.09f, 0.09f, 1);
         ui_rect(x, y, w, sh, S(20), C_RED, 0.12f);
      }
      else
      {
         ui_rect(x, y, w, sh, S(20), 0.09f, 0.12f, 0.20f, 1);
         ui_rect(x, y, w, sh, S(20), C_ACCENT, 0.12f);
      }
      if (icon)
         draw_cover(icon, app.detail[ART_ICON].w, app.detail[ART_ICON].h, x + S(16), y + S(16),
               sh - S(32), sh - S(32), 0, 1);
      else
         ui_rect(x + S(16), y + S(16), sh - S(32), sh - S(32), S(14), C_CARD, 1);

      /* For when SteamGridDB's art is for another game (a sequel, a
       * remaster): pick the right one. Steam or not is the list's tick. */
      /* Not for an uninstalled game: it's only there to leave Steam. */
      /* Right: the game's settings (its shortcut's overlay, Steam Input,
       * launch options), kept whether it's in Steam yet or not. */
      bx = x + w - S(18) - button_w("Settings");
      if (!gone)
      {
         if (button(bx, y + (sh - S(42)) / 2, button_w("Settings"), S(42), "Settings", false, true,
                  false))
            open_game_settings();
         bx -= S(10);
      }
      else
         bx = x + w + S(18);
      bx -= mw;
      if (!gone && button(bx, y + (sh - S(42)) / 2, mw, S(42), "Wrong game?", false, sgdb_has_key(),
               false))
      {
         SDL_strlcpy(app.match, name, sizeof(app.match));
         app.modal = MODAL_MATCH;
         app.field = FIELD_MATCH;
         app.match_count = 0;
         /* Searched at once for the name it has. */
         app.match_pending = true;
         app.match_typed = 0;
         SDL_StartTextInput(app.window);
      }
      cx = x + sh;
      if (app.field == FIELD_NAME && app.name_game == app.selected)
      {
         /* Being renamed: the field in the name's place, its own name
          * shown where it's blank. */
         float fw = bx - cx - S(16);
         app.name_rect = (SDL_FRect){ cx - S(10), y + S(15), fw + S(10), S(40) };
         ui_rect(cx - S(10), y + S(15), fw + S(10), S(40), S(10), 0, 0, 0, 0.3f);
         ui_ring(cx - S(10), y + S(15), fw + S(10), S(40), S(10), S(1.5f), C_ACCENT, 0.8f);
         SDL_snprintf(line, sizeof(line), "%s|", app.name);
         if (!app.name[0])
            ui_text(UI_BOLD, S(24), cx + S(10), y + S(36), UI_LEFT, fw - S(20), C_DIM, 0.7f,
                  own_name);
         ui_text(UI_BOLD, S(24), cx, y + S(36), UI_LEFT, fw - S(10), C_TEXT, 1, line);
         /* A click in it isn't one outside. */
         clicked_in(cx - S(10), y + S(15), fw + S(10), S(40), false);
      }
      else
      {
         /* Its tags: where it's from, its emulator, its achievements, what
          * Steam has of it.
          * A long name is cut short for them, to half at most. */
         const struct
         {
            const char *text;
            float r, g, b;
         } tags[] = {
            { source, C_TEXT },
            { emulator, C_TEXT },
            { no_emulator ? "No emulator" : "", C_RED },
            { achievements, C_GREEN },
            { state ? state : "", sr, sg, sb },
         };
         float room = bx - cx - S(16), tags_w = 0, max_w, nw, chip_x;

         for (i = 0; i < (int)SDL_arraysize(tags); i++)
            if (tags[i].text[0])
               tags_w += chip_w(tags[i].text) + S(8);
         max_w = SDL_max(room - tags_w - S(6), room / 2);
         nw = SDL_min(ui_text_width(UI_BOLD, S(24), name), max_w);

         /* The name: clicked to rename the game. */
         if (!gone && hover(cx - S(10), y + S(15), nw + S(20), S(40)))
         {
            ui_rect(cx - S(10), y + S(15), nw + S(20), S(40), S(10), 1, 1, 1, 0.08f);
            tooltip(cx + nw / 2, y + S(60), "Rename");
         }
         ui_text(UI_BOLD, S(24), cx, y + S(36), UI_LEFT, max_w, C_TEXT, 1, name);
         if (!gone && clicked_in(cx - S(10), y + S(15), nw + S(20), S(40), false))
         {
            SDL_strlcpy(app.name, name, sizeof(app.name));
            app.name_game = app.selected;
            app.name_rect = (SDL_FRect){ cx - S(10), y + S(15), nw + S(20), S(40) };
            app.field = FIELD_NAME;
            SDL_StartTextInput(app.window);
         }
         chip_x = cx + nw + S(14);
         for (i = 0; i < (int)SDL_arraysize(tags); i++)
            if (tags[i].text[0] && chip_x + chip_w(tags[i].text) < bx)
               chip_x += chip(chip_x, y + S(24), tags[i].text, tags[i].r, tags[i].g, tags[i].b)
                  + S(8);
      }
      SDL_snprintf(line, sizeof(line), "SteamGridDB: %s",
            sgdb_name[0] ? sgdb_name : "not matched yet");
      ui_text(UI_MEDIUM, S(14), cx, y + S(70), UI_LEFT, bx - cx - S(16), C_TEXT, 0.7f, line);
      y += sh + S(34);
   }

   /* The art to pick from. */
   ui_text(UI_BOLD, S(20), x, y + S(14), UI_LEFT, 0, C_TEXT, 1, "Artwork");
   tx = x + ui_text_width(UI_BOLD, S(20), "Artwork") + S(24);
   for (t = 0; t < ART_TYPES; t++)
   {
      float pw = ui_text_width(UI_BOLD, S(14), art_labels[t]) + S(30);
      bool on = (int)app.tab == t;
      if (on)
         ui_rect(tx, y, pw, S(30), S(15), C_ACCENT, 1);
      else
         ui_rect(tx, y, pw, S(30), S(15), 1, 1, 1, hover(tx, y, pw, S(30)) ? 0.12f : 0.06f);
      ui_text(UI_BOLD, S(14), tx + pw / 2, y + S(15), UI_CENTER, 0, C_TEXT, on ? 1.0f : 0.75f,
            art_labels[t]);
      if (clicked_in(tx, y, pw, S(30), false) && !on)
      {
         app.tab = (enum art_type)t;
         app.want_list = true;
      }
      tx += pw + S(8);
   }
   /* Right: animated art instead of still (icons have none). */
   if (app.tab != ART_ICON)
   {
      float lw = ui_text_width(UI_MEDIUM, S(14), "Animated"), kx = x + w - lw - S(28);
      draw_tick(kx, y + S(15), sgdb_animated(), false);
      ui_text(UI_MEDIUM, S(14), kx + S(28), y + S(15), UI_LEFT, 0, C_TEXT, 0.85f, "Animated");
      if (clicked_in(kx - S(6), y, lw + S(34), S(30), false))
      {
         sgdb_set_animated(!sgdb_animated());
         app.want_list = true;
      }
   }
   y += S(44);

   /* The art scrolls in what's left, cut off at its top edge (with room
    * for the chosen one's ring). */
   grid_top = y;
   grid_h = top + h - grid_top;
   tile_size(app.tab, &tw, &th);
   cols = SDL_max(1, (int)((w + S(14)) / (tw + S(14))));
   /* Stretched to fill the row exactly, the art's shape kept: the same
    * margin both sides. */
   {
      float fill = (w - S(14) * (cols - 1)) / cols;
      th *= fill / tw;
      tw = fill;
   }
   content_h = S(6) + ((app.item_count + cols - 1) / cols) * (th + S(14)) + S(26);
   if (app.wheel != 0 && inside(app.wx, app.wy, x, top, w, h))
   {
      app.game_scroll -= app.wheel * S(120);
      app.wheel = 0;
   }
   app.game_scroll = SDL_clamp(app.game_scroll, 0, SDL_max(0, content_h - grid_h));
   glEnable(GL_SCISSOR_TEST);
   glScissor(0, app.height - (int)(grid_top + grid_h), app.width, (int)grid_h);
   y = grid_top + S(6) - app.game_scroll;
   if (app.pick_loading || !app.item_count)
      ui_text(UI_MEDIUM, S(16), x, y + S(20), UI_LEFT, w, C_DIM, 1,
            app.pick_loading ? "Loading..." : app.pick_msg);
   for (i = 0; i < app.item_count; i++)
   {
      struct pick_item *it = &app.items[i];
      float ix = x + (i % cols) * (tw + S(14));
      bool over;

      ty = y + (i / cols) * (th + S(14));
      if (ty + th < grid_top || ty > grid_top + grid_h)
         continue;
      over = hover(ix, ty, tw, th) && inside(app.mx, app.my, x - S(8), grid_top, w + S(16), grid_h);
      ui_rect(ix, ty, tw, th, S(12), 1, 1, 1, over ? 0.09f : 0.04f);
      /* Icons as they are, square corners; other art in rounded tiles. */
      ask_thumb(i);
      if (it->tex)
         draw_fit(item_texture(it), it->w, it->h, ix, ty, tw, th, app.tab == ART_ICON ? 0 : S(12));
      else
         ui_text(UI_MEDIUM, S(13), ix + tw / 2, ty + th / 2, UI_CENTER, tw, C_DIM, 1,
               it->failed ? "No preview" : "...");
      if (it->art.id == 0)
         chip(ix + S(8), ty + th - S(32), "Original", C_TEXT);
      if (it->art.id == chosen && (chosen != 0 || chosen_own))
      {
         ui_ring(ix - S(4), ty - S(4), tw + S(8), th + S(8), S(15), S(3), C_ACCENT, 1);
         if (coming)
         {
            /* How far its download is, kept up as it goes. */
            Uint64 due = SDL_GetTicks() + 100;
            int percent = SDL_GetAtomicInt(&coming->percent);
            SDL_snprintf(line, sizeof(line), percent >= 0 ? "%d%%" : "...", percent);
            chip(ix + S(8), ty + S(8), line, C_TEXT);
            if (!app.anim_at || due < app.anim_at)
               app.anim_at = due;
         }
      }
      else if (over)
         ui_ring(ix, ty, tw, th, S(12), S(1.5f), 1, 1, 1, 0.45f);
      if (inside(app.cx, app.cy, x - S(8), grid_top, w + S(16), grid_h)
            && clicked_in(ix, ty, tw, th, false))
         choose(app.selected, app.tab, &it->art);
   }
   glDisable(GL_SCISSOR_TEST);
}

/* ---- Settings page ----------------------------------------------------- */

static void save_key(void)
{
   if (!app.key[0])
      return;
   sgdb_set_key(app.key);
   SDL_memset(app.key, 0, sizeof(app.key));
   app.field = FIELD_NONE;
   SDL_StopTextInput(app.window);
   app.want_list = true;
   start_fill();
}

static void log_in(void)
{
   struct login_job *j;

   if (!app.ra_user[0] || !app.ra_password[0] || app.ra_busy
         || !(j = SDL_calloc(1, sizeof(*j))))
      return;
   SDL_strlcpy(j->user, app.ra_user, sizeof(j->user));
   SDL_strlcpy(j->password, app.ra_password, sizeof(j->password));
   SDL_zeroa(app.ra_password);
   app.ra_error[0] = '\0';
   app.ra_busy = true;
   app.field = FIELD_NONE;
   SDL_StopTextInput(app.window);
   jobs_add(job_login, j, JOB_NOW, true);
}

static float settings_card(float x, float y, float w, float h, const char *title)
{
   ui_rect(x, y, w, h, S(18), C_CARD, 1);
   ui_text(UI_BOLD, S(18), x + S(24), y + S(34), UI_LEFT, w - S(48), C_TEXT, 1, title);
   return y + S(58);
}

static void open_list_modal(enum modal modal, int what)
{
   app.modal = modal;
   app.modal_item = what;
   app.modal_scroll = 0;
}

/* The ROM folders: each with its system (or told by each ROM), and an x
 * to leave it out. Returns the card's height. */
static float draw_rom_folders(float x, float y, float w)
{
   float row = S(48), cy, h;
   int i, n, unknown, removed = -1;
   char line[256];

   library_lock();
   n = roms_folder_count();
   unknown = roms_unknown();
   h = S(152) + row * n + (unknown ? S(46) : 0);
   cy = settings_card(x, y, w, h, "ROM folders");
   ui_text(UI_MEDIUM, S(14), x + S(24), cy + S(6), UI_LEFT, w - S(48), C_DIM, 1,
         "Folders named ROMs are found by themselves. Add others here.");
   cy += S(28);
   for (i = 0; i < n; i++, cy += row)
   {
      const struct rom_folder *f = roms_folder(i);
      const char *system = f->system[0] ? f->system : "Any system";
      float bw = button_w(system), xx = x + w - S(24) - S(36);

      ui_text(UI_MEDIUM, S(15), x + S(24), cy + S(20), UI_LEFT, w - S(48) - S(56) - bw, C_TEXT, 1,
            f->path);
      if (button(xx - S(8) - bw, cy, bw, S(40), system, false, true, false))
         open_list_modal(MODAL_SYSTEM, i);
      if (hover(xx, cy, S(36), S(40)))
         ui_rect(xx, cy, S(36), S(40), S(12), 1, 1, 1, 0.09f);
      draw_icon(ICON_CLOSE, xx + S(18), cy + S(20), 15, C_TEXT, 0.8f);
      if (clicked_in(xx, cy, S(36), S(40), false))
         removed = i;
   }
   library_unlock();
   if (unknown)
   {
      SDL_snprintf(line, sizeof(line), "%d file%s of no known system: name %s folder for the system "
            "(PSX, SNES...), or pick it above.", unknown, unknown == 1 ? "" : "s",
            unknown == 1 ? "its" : "their");
      ui_text(UI_MEDIUM, S(13), x + S(24), cy + S(18), UI_LEFT, w - S(48), C_DIM, 1, line);
      cy += S(46);
   }
   if (button(x + S(24), cy + S(6), button_w("Add folder"), S(40), "Add folder", false, true, false))
      SDL_ShowOpenFolderDialog(on_rom_folder, NULL, app.window, NULL, false);
   if (removed >= 0)
   {
      library_lock();
      roms_remove_folder(removed);
      library_unlock();
      rescan(true);
   }
   return h;
}

static bool toggle(float x, float y, bool on, bool modal);

/* RetroAchievements: logging in, and whether Steam shows them. Returns the
 * card's height. */
static float draw_achievements_card(float x, float y, float w)
{
   char user[128], line[200];
   bool in = ra_user(user, sizeof(user)), on = ra_in_steam();
   float h = S(in ? 236 : 186) + (in && on ? S(52) : in && app.ra_note[0] ? S(26) : 0);
   float cy, bw, fw, rx = x + w - S(24);

   cy = settings_card(x, y, w, h, "RetroAchievements");
   if (!in)
   {
      if (app.ra_error[0])
         ui_text(UI_MEDIUM, S(14), x + S(24), cy + S(6), UI_LEFT, w - S(48), C_RED, 1,
               app.ra_error);
      else
         ui_text(UI_MEDIUM, S(14), x + S(24), cy + S(6), UI_LEFT, w - S(48), C_DIM, 1,
               app.ra_busy ? "Logging in..."
               : "Your retroachievements.org login. The password isn't kept.");
      bw = S(108);
      fw = (w - S(48) - bw - S(24)) / 2;
      text_field(x + S(24), cy + S(30), fw, S(44), FIELD_RA_USER, app.ra_user, "Username", false,
            false, 0);
      text_field(x + S(24) + fw + S(12), cy + S(30), fw, S(44), FIELD_RA_PASSWORD,
            app.ra_password, "Password", true, false, 0);
      if (button(rx - bw, cy + S(30), bw, S(44), "Log in", true,
               app.ra_user[0] && app.ra_password[0] && !app.ra_busy, false))
         log_in();
      ui_text(UI_MEDIUM, S(13), x + S(24), cy + S(98), UI_LEFT, w - S(48), C_DIM, 1,
            "Also shows achievements in Steam, through a debugging port on this computer.");
      return h;
   }

   bw = button_w("Log out");
   SDL_snprintf(line, sizeof(line), "Logged in as %s", user);
   ui_text(UI_MEDIUM, S(15), x + S(24), cy + S(20), UI_LEFT, w - S(64) - bw, C_TEXT, 1, line);
   if (button(rx - bw, cy, bw, S(40), "Log out", false, true, false))
   {
      ra_logout();
      app.ra_note[0] = '\0';
      jobs_add(job_clear_steam, NULL, JOB_NOW, false);
   }
   cy += S(56);
   ui_rect(x + S(20), cy, w - S(40), S(100), S(12), 1, 1, 1, 0.04f);
   ui_text(UI_BOLD, S(15), x + S(36), cy + S(21), UI_LEFT, w - S(140), C_TEXT, 1,
         "Show achievements in Steam");
   ui_text(UI_MEDIUM, S(13), x + S(36), cy + S(43), UI_LEFT, w - S(140), C_DIM, 1,
         "On ROMs' pages in its library.");
   ui_text(UI_MEDIUM, S(13), x + S(36), cy + S(61), UI_LEFT, w - S(140), C_DIM, 1,
         "Steam then keeps a debugging port open: other programs on this");
   ui_text(UI_MEDIUM, S(13), x + S(36), cy + S(79), UI_LEFT, w - S(140), C_DIM, 1,
         "computer could control Steam through it.");
   if (toggle(rx - S(12) - S(44), cy + S(38), on, false))
   {
      ra_set_in_steam(!on);
      app.ra_note[0] = '\0';
      if (on)
      {
         /* The port itself stays until Steam restarts. */
         SDL_strlcpy(app.ra_note, "Restart Steam to close its port", sizeof(app.ra_note));
         jobs_add(job_clear_steam, NULL, JOB_NOW, false);
      }
      else
         start_achievements(false, false);
   }
   cy += S(112);
   if (on)
   {
      bw = button_w("Refresh");
      ui_text(UI_MEDIUM, S(13), x + S(24), cy + S(20), UI_LEFT, w - S(64) - bw, C_DIM, 1,
            app.ra_note);
      if (button(rx - bw, cy, bw, S(40), "Refresh", false, true, false))
      {
         SDL_strlcpy(app.ra_note, "Refreshing...", sizeof(app.ra_note));
         start_achievements(false, true);
      }
   }
   else if (app.ra_note[0])
      ui_text(UI_MEDIUM, S(13), x + S(24), cy + S(12), UI_LEFT, w - S(48), C_DIM, 1, app.ra_note);
   return h;
}

static void draw_settings(float x, float y, float w, float h)
{
   char line[1200];
   float cw = SDL_min(w, S(720)), cy, top = y, start;
   /* Scrolled out under the top bar, nothing is there to click. */
   bool held = app.click && app.cy < top;

   if (held)
      app.click = false;
   if (app.wheel != 0 && inside(app.wx, app.wy, x - S(32), top, w + S(64), h))
   {
      app.settings_scroll -= app.wheel * S(120);
      app.wheel = 0;
   }
   app.settings_scroll = SDL_clamp(app.settings_scroll, 0, app.settings_most);
   glEnable(GL_SCISSOR_TEST);
   glScissor((int)(x - S(16)), 0, app.width, (int)h);
   start = y -= app.settings_scroll;

   ui_text(UI_BOLD, S(22), x, y + S(30), UI_LEFT, 0, C_TEXT, 1, "Settings");
   y += S(64);

   cy = settings_card(x, y, cw, S(186), "SteamGridDB API key");
   ui_text(UI_MEDIUM, S(14), x + S(24), cy + S(6), UI_LEFT, cw - S(48), C_DIM, 1,
         sgdb_has_key() ? "A key is saved. Paste a new one to replace it."
         : "Free at steamgriddb.com/profile/preferences/api");
   text_field(x + S(24), cy + S(30), cw - S(48) - S(120), S(44), FIELD_KEY, app.key,
         "Paste the key", true, false, 0);
   if (button(x + cw - S(24) - S(108), cy + S(30), S(108), S(44), "Save", true, app.key[0] != '\0',
            false))
      save_key();
   y += S(186) + S(20);

   cy = settings_card(x, y, cw, S(100), "Steam");
   if (steam_install())
      SDL_snprintf(line, sizeof(line), "%s", steam_install()->config);
   else
      SDL_strlcpy(line, "Not found", sizeof(line));
   ui_text(UI_MEDIUM, S(14), x + S(24), cy + S(6), UI_LEFT, cw - S(48), C_DIM, 1, line);
   y += S(100) + S(20);

   y += draw_achievements_card(x, y, cw) + S(20);
   y += draw_rom_folders(x, y, cw) + S(20);
   glDisable(GL_SCISSOR_TEST);

   /* What's left to scroll to, now that it's all been placed. */
   {
      float most = SDL_max(0, y - start - h);
      if (app.settings_scroll > most)
         app.redraw = true;
      app.settings_most = most;
   }
   if (held)
      app.click = true;
}

/* ---- Modals ------------------------------------------------------------ */

static void modal_box(float w, float h, float *x, float *y, const char *title)
{
   ui_rect(0, 0, (float)app.width, (float)app.height, 0, 0, 0, 0, 0.6f);
   *x = (app.width - w) / 2;
   *y = (app.height - h) / 2;
   ui_rect(*x, *y, w, h, S(20), C_CARD, 1);
   ui_text(UI_BOLD, S(22), *x + S(28), *y + S(42), UI_LEFT, w - S(56), C_TEXT, 1, title);
}

static void close_modal(void)
{
   /* Opened from the game's settings: back to them. */
   if (app.modal == MODAL_EMULATOR)
   {
      app.modal = MODAL_GAME;
      return;
   }
   /* The launch options are kept as they were typed. */
   if (app.modal == MODAL_GAME)
   {
      library_lock();
      if (library_game(app.selected))
         SDL_strlcpy(library_game(app.selected)->launch_options, app.options,
               sizeof(library_game(app.selected)->launch_options));
      library_unlock();
      library_changed();
   }
   app.modal = MODAL_NONE;
   app.match_pending = false;
   app.field = FIELD_NONE;
   SDL_StopTextInput(app.window);
}

static void search_matches(void)
{
   struct match_job *j = SDL_calloc(1, sizeof(*j));

   if (!j || !app.match[0])
   {
      SDL_free(j);
      return;
   }
   SDL_strlcpy(j->name, app.match, sizeof(j->name));
   app.matching = true;
   jobs_add(job_matches, j, JOB_NOW, true);
}

/* An on/off switch; true when clicked. */
static bool toggle(float x, float y, bool on, bool modal)
{
   float w = S(44), h = S(24);

   if (on)
      ui_rect(x, y, w, h, h / 2, C_ACCENT, 1);
   else
      ui_rect(x, y, w, h, h / 2, 1, 1, 1, 0.15f);
   ui_rect(on ? x + w - h + S(3) : x + S(3), y + S(3), h - S(6), h - S(6), h, C_TEXT, 1);
   return clicked_in(x - S(4), y - S(4), w + S(8), h + S(8), modal);
}

static void open_game_settings(void)
{
   library_lock();
   if (library_game(app.selected))
      SDL_strlcpy(app.options, library_game(app.selected)->launch_options, sizeof(app.options));
   library_unlock();
   app.emulator_note[0] = '\0';
   app.modal = MODAL_GAME;
}

/* The game's settings. */
static void draw_game_settings(void)
{
   static const char *const labels[2][2] = {
      { "Steam Overlay", "Steam's in-game overlay (Shift+Tab)" },
      { "Steam Input", "Steam's controller support and remapping" },
   };
   float x, y, w = S(560), h, ry;
   bool values[2], own, rom;
   char name[256], emulator[96];
   const struct rom_option *chosen;
   int i, options;

   library_lock();
   if (!library_game(app.selected))
   {
      library_unlock();
      close_modal();
      return;
   }
   SDL_strlcpy(name, library_game(app.selected)->name, sizeof(name));
   values[0] = library_game(app.selected)->steam_overlay;
   values[1] = library_game(app.selected)->steam_input;
   own = SDL_strcmp(library_game(app.selected)->source, "Steam") == 0;
   rom = library_game(app.selected)->rom[0] != '\0';
   options = roms_options(library_game(app.selected), NULL, 0);
   chosen = roms_chosen(library_game(app.selected));
   SDL_strlcpy(emulator, chosen ? chosen->label : "No emulator", sizeof(emulator));
   library_unlock();

   h = S((own ? 256 : 360) + (rom ? 64 : 0));
   modal_box(w, h, &x, &y, name);
   ry = y + S(70);
   for (i = 0; i < 2; i++, ry += S(64))
   {
      ui_rect(x + S(20), ry, w - S(40), S(56), S(12), 1, 1, 1, 0.04f);
      ui_text(UI_BOLD, S(15), x + S(36), ry + S(19), UI_LEFT, w - S(140), C_TEXT, 1, labels[i][0]);
      ui_text(UI_MEDIUM, S(13), x + S(36), ry + S(38), UI_LEFT, w - S(140), C_DIM, 1,
            labels[i][1]);
      if (toggle(x + w - S(36) - S(44), ry + S(16), values[i], true))
      {
         library_lock();
         if (library_game(app.selected))
         {
            if (i == 0)
               library_game(app.selected)->steam_overlay = !values[i];
            else
               library_game(app.selected)->steam_input = !values[i];
         }
         library_unlock();
         library_changed();
      }
   }
   /* A ROM's emulator, to change. */
   if (rom)
   {
      /* None found: straight to picking its program. */
      const char *label = options ? "Change" : "Browse";
      float bw = button_w(label);

      ui_rect(x + S(20), ry, w - S(40), S(56), S(12), 1, 1, 1, 0.04f);
      ui_text(UI_BOLD, S(15), x + S(36), ry + S(19), UI_LEFT, w - S(180), C_TEXT, 1, "Emulator");
      if (options && !app.emulator_note[0])
         ui_text(UI_MEDIUM, S(13), x + S(36), ry + S(38), UI_LEFT, w - S(180), C_DIM, 1, emulator);
      else
         ui_text(UI_MEDIUM, S(13), x + S(36), ry + S(38), UI_LEFT, w - S(180), C_RED, 1,
               app.emulator_note[0] ? app.emulator_note : emulator);
      if (button(x + w - S(36) - bw, ry + S(9), bw, S(38), label, false, true, true))
      {
         app.field = FIELD_NONE;
         SDL_StopTextInput(app.window);
         app.emulator_note[0] = '\0';
         if (!options)
            pick_emulator(app.selected);
         else
         {
            open_list_modal(MODAL_EMULATOR, app.selected);
            return;
         }
      }
      ry += S(64);
   }
   /* Steam's own shortcuts keep their launch options in Steam. */
   if (!own)
   {
      ui_text(UI_BOLD, S(15), x + S(28), ry + S(18), UI_LEFT, 0, C_TEXT, 1, "Launch options");
      text_field(x + S(28), ry + S(36), w - S(56), S(44), FIELD_OPTIONS, app.options,
            "Added after the game's own, e.g. -windowed", false, true, 0);
   }
   if (button(x + w - S(28) - S(110), y + h - S(62), S(110), S(42), "Done", true, true, true))
      close_modal();
}

/* One of a list, each with a smaller line under it (empty for none):
 * which was clicked, -1 none, -2 closed, -3 the other button (extra, NULL
 * for none). */
static int draw_list_modal(const char *title, const char *const *labels, const char *const *subs,
      int count, int current, const char *extra)
{
   float row = S(54), w = S(600), x, y, top, list_h, h, most;
   int i, clicked = -1;

   list_h = SDL_min(row * count, app.height - S(220));
   h = list_h + S(160);
   modal_box(w, h, &x, &y, title);
   top = y + S(76);
   if (app.wheel != 0)
      app.modal_scroll -= app.wheel * row;
   app.wheel = 0;
   most = SDL_max(0, row * count - list_h);
   app.modal_scroll = SDL_clamp(app.modal_scroll, 0, most);
   glEnable(GL_SCISSOR_TEST);
   glScissor((int)x, app.height - (int)(top + list_h), (int)w, (int)list_h);
   for (i = 0; i < count; i++)
   {
      float ry = top + row * i - app.modal_scroll;
      bool in = inside(app.mx, app.my, x, top, w, list_h);

      if (ry + row < top || ry > top + list_h)
         continue;
      if (i == current)
         ui_rect(x + S(20), ry + S(1), w - S(40), row - S(2), S(10), C_ACCENT, 0.16f);
      else if (in && inside(app.mx, app.my, x + S(20), ry, w - S(40), row))
         ui_rect(x + S(20), ry + S(1), w - S(40), row - S(2), S(10), 1, 1, 1, 0.07f);
      ui_text(UI_BOLD, S(15), x + S(34), ry + (subs[i][0] ? S(18) : row / 2), UI_LEFT, w - S(68),
            C_TEXT, 1, labels[i]);
      if (subs[i][0])
         ui_text(UI_MEDIUM, S(12.5f), x + S(34), ry + S(37), UI_LEFT, w - S(68), C_DIM, 1, subs[i]);
      if (inside(app.cx, app.cy, x, top, w, list_h)
            && clicked_in(x + S(20), ry, w - S(40), row, true))
         clicked = i;
   }
   glDisable(GL_SCISSOR_TEST);
   if (button(x + w - S(28) - S(110), y + h - S(62), S(110), S(42), "Cancel", false, true, true))
      return -2;
   if (extra && button(x + S(28), y + h - S(62), button_w(extra), S(42), extra, false, true, true))
      return -3;
   return clicked;
}

static void draw_system_modal(void)
{
   const char *labels[64], *subs[64];
   int i, n = SDL_min(roms_system_count() + 1, 64), current = 0, picked = -2;
   const struct rom_folder *f;

   library_lock();
   if ((f = roms_folder(app.modal_item)))
   {
      for (i = 0; i < n; i++)
      {
         labels[i] = i ? roms_system_name(i - 1) : "Any system";
         subs[i] = i ? "" : "Told by folder names, file types and what's on each disc";
         if (i && SDL_strcmp(f->system, labels[i]) == 0)
            current = i;
      }
      picked = draw_list_modal("What's in this folder?", labels, subs, n, current,
            NULL);
      if (picked >= 0)
         roms_set_folder_system(app.modal_item, picked ? labels[picked] : "");
   }
   library_unlock();
   if (picked != -1)
      close_modal();
   if (picked >= 0)
      rescan(true);
}

static void draw_emulator_modal(void)
{
   const struct rom_option *options[64], *chosen;
   const char *labels[64], *subs[64];
   struct game *g;
   int i, n = 0, current = -1, picked = -2;

   library_lock();
   if ((g = library_game(app.modal_item)))
   {
      n = SDL_min(roms_options(g, options, 64), 64);
      chosen = roms_chosen(g);
      for (i = 0; i < n; i++)
      {
         labels[i] = options[i]->label;
         subs[i] = options[i]->exe;
         if (options[i] == chosen)
            current = i;
      }
   }
   if (n)
      picked = draw_list_modal("Emulator", labels, subs, n, current, "Browse");
   if (picked >= 0)
      roms_choose(g, options[picked]);
   library_unlock();
   /* Any other program, by its file. */
   if (picked == -3)
      pick_emulator(app.modal_item);
   else if (picked != -1)
      close_modal();
}

static void draw_modal(void)
{
   float x, y, w, h;
   int i;

   if (app.modal == MODAL_SYSTEM)
   {
      draw_system_modal();
      return;
   }
   if (app.modal == MODAL_EMULATOR)
   {
      draw_emulator_modal();
      return;
   }
   if (app.modal == MODAL_GAME)
      draw_game_settings();
   else if (app.modal == MODAL_STEAM)
   {
      w = S(520);
      h = S(196);
      modal_box(w, h, &x, &y, "Restart Steam?");
      ui_text(UI_MEDIUM, S(15), x + S(28), y + S(84), UI_LEFT, w - S(56), C_DIM, 1,
            "Steam will restart to take the new shortcuts.");
      if (button(x + w - S(28) - S(170), y + h - S(66), S(170), S(42), "Restart Steam",
               true, true, true))
      {
         close_modal();
         start_steam_job(true);
      }
      if (button(x + w - S(28) - S(290), y + h - S(66), S(110), S(42), "Cancel", false, true,
               true))
         close_modal();
   }
   else if (app.modal == MODAL_MATCH)
   {
      float row = S(42);
      w = S(600);
      h = S(204) + row * SDL_min(app.match_count, 8);
      modal_box(w, h, &x, &y, "Which game is it?");
      /* Searches as the name is typed: no button. */
      text_field(x + S(28), y + S(74), w - S(56), S(44), FIELD_MATCH, app.match,
            "Game name", false, true, 0);
      for (i = 0; i < SDL_min(app.match_count, 8); i++)
      {
         float ry = y + S(132) + i * row;
         if (inside(app.mx, app.my, x + S(20), ry, w - S(40), row))
            ui_rect(x + S(20), ry, w - S(40), row, S(10), 1, 1, 1, 0.07f);
         ui_text(UI_MEDIUM, S(16), x + S(34), ry + row / 2, UI_LEFT, w - S(68), C_TEXT, 1,
               app.matches[i].name);
         if (clicked_in(x + S(20), ry, w - S(40), row, true))
         {
            library_lock();
            if (library_game(app.selected))
            {
               library_game(app.selected)->sgdb_id = app.matches[i].id;
               SDL_strlcpy(library_game(app.selected)->sgdb_name, app.matches[i].name,
                     sizeof(app.matches[i].name));
               fill_again(app.selected);
            }
            library_unlock();
            library_changed();
            library_save();
            close_modal();
            app.want_list = true;
            return;
         }
      }
      if (app.matching)
         ui_text(UI_MEDIUM, S(15), x + S(28), y + h - S(46), UI_LEFT, 0, C_DIM, 1, "Searching...");
      if (button(x + w - S(28) - S(110), y + h - S(62), S(110), S(42), "Cancel", false, true,
               true))
         close_modal();
   }
}

/* ---- Messages and input ------------------------------------------------ */

static void handle_msg(struct msg *m)
{
   switch (m->kind)
   {
      case MSG_SCANNED:
         app.scanned = true;
         SDL_strlcpy(app.status, m->text, sizeof(app.status));
         app.view_built_for = -1;
         start_fill();
         start_achievements(false, false);
         if (app.scan_again && !app.busy)
         {
            app.scan_again = false;
            rescan(true);
         }
         if (!sgdb_has_key())
         {
            app.page = PAGE_SETTINGS;
            app.field = FIELD_KEY;
            SDL_StartTextInput(app.window);
         }
         break;
      case MSG_ART_LIST:
         on_art_list(m);
         break;
      case MSG_THUMB:
         if (m->gen == SDL_GetAtomicInt(&picker_gen) && m->index < app.item_count)
         {
            struct pick_item *it = &app.items[m->index];
            if (m->ptr)
            {
               it->anim = m->ptr;
               m->ptr = NULL;
               it->w = it->anim->w;
               it->h = it->anim->h;
               it->tex = make_texture(it->anim->pixels, it->w, it->h);
               /* Only the full size is kept up as it plays. */
               glBindTexture(GL_TEXTURE_2D, it->tex);
               glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
               glBindTexture(GL_TEXTURE_2D, 0);
            }
            else if (m->pixels)
            {
               it->tex = make_texture(m->pixels, m->w, m->h);
               it->w = m->w;
               it->h = m->h;
            }
            else
               it->failed = true;
         }
         /* For a list that's gone: freed below. */
         if (m->ptr)
         {
            const struct anim *a = m->ptr;
            SDL_AddAtomicInt(&anim_total, -(a->w * a->h * 4 * a->count));
         }
         break;
      case MSG_COVER:
      {
         /* A card's cover (count -1) or the game page's art. */
         struct art_tex *t = m->count < 0
            ? (m->index < app.covers_alloc ? &app.covers[m->index * 2 + (m->count == -1)] : NULL)
            : &app.detail[m->count];
         if (t && t->game == m->index && t->version == m->version)
         {
            if (t->tex)
               glDeleteTextures(1, &t->tex);
            t->tex = m->pixels ? make_texture(m->pixels, m->w, m->h) : 0;
            t->w = m->w;
            t->h = m->h;
         }
         break;
      }
      /* The status line only ever says what the scan found: these only
       * wake the window (new art), or speak in the Steam card. */
      case MSG_ART_SAVED:
         choices[m->index].used = false;
         break;
      case MSG_STATUS:
         if (m->text[0])
            SDL_strlcpy(app.steam_note, m->text, sizeof(app.steam_note));
         break;
      case MSG_MATCHES:
         app.matching = false;
         SDL_free(app.matches);
         app.matches = m->ptr;
         app.match_count = SDL_max(m->count, 0);
         m->ptr = NULL;
         break;
      case MSG_ADD_EXE:
      {
         int index = disk_add_exe(m->ptr);
         if (index >= 0)
         {
            library_save();
            app.view_built_for = -1;
            app.selected = index;
            app.page = PAGE_GAME;
            app.want_list = true;
            start_fill();
         }
         break;
      }
      case MSG_ROM_FOLDER:
         library_lock();
         roms_add_folder(m->ptr);
         library_unlock();
         rescan(true);
         break;
      case MSG_EMULATOR:
      {
         struct game *g;
         bool ok = false;
         library_lock();
         if ((g = library_game(m->index)))
            ok = roms_add_emulator(g, m->ptr);
         library_unlock();
         if (app.modal == MODAL_EMULATOR)
            close_modal();
         app.emulator_note[0] = '\0';
         /* For the system's other games too, where they had none. */
         if (ok)
            rescan(true);
         else
            SDL_strlcpy(app.emulator_note, "RetroArch has no core for this system",
                  sizeof(app.emulator_note));
         break;
      }
      case MSG_LOGIN:
         app.ra_busy = false;
         SDL_strlcpy(app.ra_error, m->text, sizeof(app.ra_error));
         if (m->count)
         {
            app.ra_user[0] = '\0';
            start_achievements(false, true);
         }
         break;
      case MSG_ACHIEVEMENTS:
         SDL_strlcpy(app.ra_note, m->text, sizeof(app.ra_note));
         break;
      case MSG_STEAM_DONE:
         app.busy = false;
         SDL_strlcpy(app.steam_note, m->text, sizeof(app.steam_note));
         /* Scanned again: the counts up top, and what Steam has now. */
         rescan(false);
         break;
   }
   if (m->kind != MSG_MATCHES && m->kind != MSG_ART_LIST)
      SDL_free(m->ptr);
   SDL_free(m->pixels);
   SDL_free(m);
}

/* The game name was edited: search again once typing pauses. */
static void match_typed(void)
{
   if (app.field == FIELD_MATCH)
   {
      app.match_pending = true;
      app.match_typed = SDL_GetTicks();
   }
}

static void on_key(const SDL_KeyboardEvent *k)
{
   size_t size;
   char *text = field_text(app.field, &size);

   if (k->key == SDLK_ESCAPE)
   {
      if (app.modal != MODAL_NONE)
         close_modal();
      else if (app.field != FIELD_NONE)
         end_name();
      else if (app.page == PAGE_SETTINGS)
         app.page = PAGE_GAME;
      return;
   }
   if (!text)
      return;
   if (k->key == SDLK_TAB && (app.field == FIELD_RA_USER || app.field == FIELD_RA_PASSWORD))
      app.field = app.field == FIELD_RA_USER ? FIELD_RA_PASSWORD : FIELD_RA_USER;
   else if (k->key == SDLK_BACKSPACE && text[0])
   {
      size_t len = SDL_strlen(text);
      /* A whole UTF-8 character. */
      while (len > 0 && ((Uint8)text[--len] & 0xC0) == 0x80)
         ;
      text[len] = '\0';
      if (app.field == FIELD_FILTER)
         app.view_built_for = -1;
      match_typed();
   }
   else if (k->key == SDLK_V && (k->mod & SDL_KMOD_CTRL))
   {
      char *clip = SDL_GetClipboardText();
      if (clip)
      {
         char *p;
         /* One line, trimmed: keys are often pasted with a newline. */
         for (p = clip; *p; p++)
            if (*p == '\r' || *p == '\n')
               *p = '\0';
         SDL_strlcat(text, clip, size);
         SDL_free(clip);
      }
      if (app.field == FIELD_FILTER)
         app.view_built_for = -1;
      match_typed();
   }
   else if (k->key == SDLK_RETURN || k->key == SDLK_KP_ENTER)
   {
      if (app.field == FIELD_KEY)
         save_key();
      else if (app.field == FIELD_RA_USER)
         app.field = FIELD_RA_PASSWORD;
      else if (app.field == FIELD_RA_PASSWORD)
         log_in();
      else if (app.field == FIELD_NAME)
         save_name();
      else if (app.field == FIELD_MATCH)
         search_matches();
   }
}

static void on_text(const char *t)
{
   size_t size;
   char *text = field_text(app.field, &size);

   if (!text)
      return;
   SDL_strlcat(text, t, size);
   if (app.field == FIELD_FILTER)
      app.view_built_for = -1;
   match_typed();
}

/* The window has no frame: the top bar drags it (double-click maximizes,
 * and Windows' snapping works), its edges resize it. */
static SDL_HitTestResult SDLCALL hit_test(SDL_Window *window, const SDL_Point *p, void *data)
{
   float d = SDL_GetWindowPixelDensity(window), px = p->x * d, py = p->y * d;
   float edge = 6 * d;
   int w, h, i;

   SDL_GetWindowSizeInPixels(window, &w, &h);
   if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED))
   {
      bool l = px < edge, r = px >= w - edge, t = py < edge, b = py >= h - edge;
      if (t && l) return SDL_HITTEST_RESIZE_TOPLEFT;
      if (t && r) return SDL_HITTEST_RESIZE_TOPRIGHT;
      if (b && l) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
      if (b && r) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
      if (t) return SDL_HITTEST_RESIZE_TOP;
      if (b) return SDL_HITTEST_RESIZE_BOTTOM;
      if (l) return SDL_HITTEST_RESIZE_LEFT;
      if (r) return SDL_HITTEST_RESIZE_RIGHT;
   }
   if (app.modal != MODAL_NONE || py >= S(70))
      return SDL_HITTEST_NORMAL;
   for (i = 0; i < app.nodrag_count; i++)
      if (inside(px, py, app.nodrag[i].x, app.nodrag[i].y, app.nodrag[i].w, app.nodrag[i].h))
         return SDL_HITTEST_NORMAL;
   return SDL_HITTEST_DRAGGABLE;
}

static void draw(void)
{
   float side, x0, top, cw, wheel = app.wheel;

   SDL_GetWindowSizeInPixels(app.window, &app.width, &app.height);
   app.s = SDL_GetWindowDisplayScale(app.window);
   if (app.s <= 0)
      app.s = 1;
   glViewport(0, 0, app.width, app.height);
   glClearColor(C_BG, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   if (!ui_begin(app.width, app.height))
      return;
   app.nodrag_count = 0;
   app.anim_at = 0;
   side = S(330);
   x0 = side + S(32);
   top = S(76);
   cw = app.width - x0 - S(32);
   /* Modals first in input, last on screen: widgets under them must not
    * take their clicks, and they're drawn on top. A click outside every
    * widget just drops a field's focus. */
   /* A click anywhere else takes the name being edited. */
   if (app.click && app.field == FIELD_NAME
         && !inside(app.cx, app.cy, app.name_rect.x, app.name_rect.y, app.name_rect.w,
            app.name_rect.h))
      save_name();
   if (app.modal != MODAL_NONE)
      app.wheel = 0;
   draw_sidebar(side);
   draw_top(x0);
   if (app.page == PAGE_GAME)
      draw_game(x0, top, cw, app.height - top);
   else
      draw_settings(x0, top, cw, app.height - top);
   draw_tooltip();
   app.wheel = wheel;
   draw_modal();
   if (app.click && app.modal == MODAL_NONE && app.field != FIELD_NONE)
   {
      app.field = FIELD_NONE;
      SDL_StopTextInput(app.window);
   }
   app.click = false;
   app.wheel = 0;
   ui_end();
   SDL_GL_SwapWindow(app.window);
}

/* While an edge is dragged, Windows runs its own loop and the main one
 * stops: SDL calls this from inside it with "exposed" events (data1 1 for
 * live resizing), so the window redraws at each step. Without waiting for
 * the display (vsync), which would hold up every step of the drag. */
static bool SDLCALL on_live_resize(void *user, SDL_Event *e)
{
   if (e->type == SDL_EVENT_WINDOW_EXPOSED && e->window.data1 == 1)
   {
      SDL_GL_SetSwapInterval(0);
      draw();
      SDL_GL_SetSwapInterval(1);
   }
   return true;
}

/* The app's icon for the window (taskbar, Alt+Tab; the .exe has its own
 * from tools/make_icon.py): the bolt in white on a blue rounded tile. */
static void set_window_icon(void)
{
   enum { SIZE = 64, SUB = 4 };
   SDL_Surface *s = SDL_CreateSurface(SIZE, SIZE, SDL_PIXELFORMAT_RGBA32);
   const float radius = 0.22f, blue[3] = { C_ACCENT };
   int x, y, sx, sy;

   if (!s)
      return;
   for (y = 0; y < SIZE; y++)
      for (x = 0; x < SIZE; x++)
      {
         float r = 0, g = 0, b = 0, a = 0;
         Uint8 *p = (Uint8 *)s->pixels + y * s->pitch + x * 4;
         for (sy = 0; sy < SUB; sy++)
            for (sx = 0; sx < SUB; sx++)
            {
               float fx = (x + (sx + 0.5f) / SUB) / SIZE - 0.5f;
               float fy = (y + (sy + 0.5f) / SUB) / SIZE - 0.5f;
               float qx = SDL_max(SDL_fabsf(fx) - (0.5f - radius), 0.0f);
               float qy = SDL_max(SDL_fabsf(fy) - (0.5f - radius), 0.0f);
               if (qx * qx + qy * qy > radius * radius)
                  continue;
               a += 1;
               if (in_bolt(fx, fy))
                  r += 1, g += 1, b += 1;
               else
                  r += blue[0], g += blue[1], b += blue[2];
            }
         /* Colour averaged over the covered samples, coverage as alpha. */
         p[0] = a ? (Uint8)(r / a * 255) : 0;
         p[1] = a ? (Uint8)(g / a * 255) : 0;
         p[2] = a ? (Uint8)(b / a * 255) : 0;
         p[3] = (Uint8)(a * 255 / (SUB * SUB));
      }
   SDL_SetWindowIcon(app.window, s);
   SDL_DestroySurface(s);
}

/* ---- Startup ----------------------------------------------------------- */

static SDL_IOStream *log_file;

static void SDLCALL log_to_file(void *user, int category, SDL_LogPriority priority,
      const char *message)
{
   if (log_file)
   {
      SDL_IOprintf(log_file, "%s\n", message);
      SDL_FlushIO(log_file);
   }
}

int main(int argc, char **argv)
{
   char fonts[1024], log_path[1100], config[1100];
   const char *base;
   bool running = true;
   Uint64 saved_at = 0;
   struct msg *m;
   int i;

   if (!SDL_Init(SDL_INIT_VIDEO))
      return 1;
   /* Portable: everything FuseBox keeps is next to fusebox.exe. */
   base = SDL_GetBasePath();
   if (!base || !library_init(base))
   {
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FuseBox", "Can't open FuseBox's folder", NULL);
      return 1;
   }
   SDL_strlcpy(app.data_dir, base, sizeof(app.data_dir));
   SDL_snprintf(log_path, sizeof(log_path), "%sfusebox.log", base);
   if ((log_file = SDL_IOFromFile(log_path, "w")))
      SDL_SetLogOutputFunction(log_to_file, NULL);
   SDL_snprintf(config, sizeof(config), "%sconfig", base);
   sgdb_init(config);
   roms_init(config);
   ra_init(config);
   steam_init();
   msg_lock = SDL_CreateMutex();
   wake_event = SDL_RegisterEvents(1);

   SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
   app.window = SDL_CreateWindow("FuseBox", 1320, 840,
         SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS
         | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
   if (!app.window || !(app.gl = SDL_GL_CreateContext(app.window)) || !gl_load())
   {
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FuseBox", "OpenGL 3.3 isn't available", NULL);
      return 1;
   }
   SDL_SetWindowMinimumSize(app.window, 960, 600);
   SDL_SetWindowHitTest(app.window, hit_test, NULL);
   SDL_AddEventWatch(on_live_resize, NULL);
   platform_window_shadow(app.window);
   set_window_icon();
   SDL_GL_SetSwapInterval(1);
   base = SDL_GetBasePath();
   SDL_snprintf(fonts, sizeof(fonts), "%sfonts", base ? base : "");
   ui_init(fonts);

   app.selected = -1;
   app.view_built_for = -1;
   for (i = 0; i < ART_TYPES; i++)
      app.detail[i].game = -1;
   SDL_strlcpy(app.status, "Looking for games...", sizeof(app.status));
   jobs_init(8);
   jobs_add(job_scan, NULL, JOB_NOW, true);
   SDL_ShowWindow(app.window);
   SDL_StopTextInput(app.window);
   app.redraw = true;

   while (running)
   {
      SDL_Event e;
      /* Idle: wake only for input, a job's message, or a look at
       * library.json (fusebox-cli may add to it) every two seconds. */
      bool got;
      Uint64 now = SDL_GetTicks();
      int wait = app.match_pending ? 50 : 2000;

      /* Or the next frame of animated art in view. */
      if (app.anim_at && !(SDL_GetWindowFlags(app.window) & SDL_WINDOW_MINIMIZED))
         wait = app.anim_at > now ? (int)SDL_min((Uint64)wait, app.anim_at - now) : 0;
      got = app.redraw ? SDL_PollEvent(&e) : SDL_WaitEventTimeout(&e, wait);
      app.redraw |= app.anim_at && SDL_GetTicks() >= app.anim_at
         && !(SDL_GetWindowFlags(app.window) & SDL_WINDOW_MINIMIZED);

      while (got)
      {
         float d = SDL_GetWindowPixelDensity(app.window);

         switch (e.type)
         {
            case SDL_EVENT_QUIT:
               running = false;
               break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
               if (e.button.button == SDL_BUTTON_LEFT)
               {
                  app.click = true;
                  app.cx = e.button.x * d;
                  app.cy = e.button.y * d;
               }
               break;
            case SDL_EVENT_MOUSE_MOTION:
               app.mx = e.motion.x * d;
               app.my = e.motion.y * d;
               break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
               app.mx = app.my = -1;
               break;
            case SDL_EVENT_MOUSE_WHEEL:
               app.wheel += e.wheel.y;
               app.wx = e.wheel.mouse_x * d;
               app.wy = e.wheel.mouse_y * d;
               break;
            case SDL_EVENT_KEY_DOWN:
               on_key(&e.key);
               break;
            case SDL_EVENT_TEXT_INPUT:
               on_text(e.text.text);
               break;
         }
         app.redraw = true;
         got = SDL_PollEvent(&e);
      }
      while ((m = take_msg()))
      {
         handle_msg(m);
         app.redraw = true;
      }
      if (library_refresh())
         app.view_built_for = -1;
      library_lock();
      if (app.view_built_for != library_count())
      {
         library_unlock();
         build_view();
         /* Something to show from the start: the first game. */
         if (app.selected < 0 && app.view_count)
         {
            app.selected = app.view[0];
            app.want_list = true;
         }
         app.redraw = true;
      }
      else
         library_unlock();
      if (app.want_list)
         picker_open();
      /* Typing paused for 300 ms (and no search running): search. */
      if (app.modal == MODAL_MATCH && app.match_pending && !app.matching
            && SDL_GetTicks() - app.match_typed >= 300)
      {
         app.match_pending = false;
         search_matches();
         app.redraw = true;
      }
      if (app.redraw)
      {
         /* A click taken redraws again so the change shows at once. */
         app.redraw = false;
         draw();
      }
      /* Not for every image of a batch: the file can be big. */
      if (SDL_GetTicks() - saved_at >= 2000)
      {
         library_save();
         saved_at = SDL_GetTicks();
      }
   }

   if (app.field == FIELD_NAME)
      save_name();
   for (i = 0; i < CHOICES; i++)
      SDL_SetAtomicInt(&choices[i].stop, 1);
   jobs_quit();
   while ((m = take_msg()))
      handle_msg(m);
   picker_clear();
   SDL_free(icon_font.data);
   for (i = 0; i < ICONS * ICON_SIZES; i++)
      if (icon_tex[i / ICON_SIZES][i % ICON_SIZES].tex)
         glDeleteTextures(1, &icon_tex[i / ICON_SIZES][i % ICON_SIZES].tex);
   ui_quit();
   library_quit();
   http_quit();
   SDL_GL_DestroyContext(app.gl);
   SDL_DestroyWindow(app.window);
   if (log_file)
      SDL_CloseIO(log_file);
   SDL_Quit();
   return 0;
}
