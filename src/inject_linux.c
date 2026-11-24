#ifndef _WIN32

#define _GNU_SOURCE
#include "inject.h"
#include "platform.h"

#include <SDL3/SDL.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/un.h>

#define GAMES 8
/* How often it's looked whether it's over (ms). */
#define LOOK_MS 200

static struct lanhook *map_memory(Uint32 pid, bool make)
{
   char name[64];
   struct stat st;
   void *map = MAP_FAILED;
   int fd;

   SDL_snprintf(name, sizeof(name), LANHOOK_MEMORY, (unsigned)pid);
   /* One with that number that didn't get to end. */
   if (make)
      shm_unlink(name);
   if ((fd = shm_open(name, O_RDWR | O_CLOEXEC | (make ? O_CREAT | O_EXCL : 0), 0600)) < 0)
      return NULL;
   if ((!make || ftruncate(fd, sizeof(struct lanhook)) == 0) && fstat(fd, &st) == 0
         && st.st_uid == getuid() && st.st_size >= (off_t)sizeof(struct lanhook))
      map = mmap(NULL, sizeof(struct lanhook), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   close(fd);
   return map == MAP_FAILED ? NULL : map;
}

/* ---- Where the game is started -------------------------------------------- */

/* The library's path, as LD_PRELOAD can have it: without spaces and colons. */
static bool library(char *dst, size_t size)
{
   const char *run = SDL_getenv("XDG_RUNTIME_DIR");
   char real[1100];

   SDL_snprintf(real, sizeof(real), "%s" LANHOOK_LIBRARY, SDL_GetBasePath());
   if (!platform_exists(real))
      return false;
   SDL_strlcpy(dst, real, size);
   if (!SDL_strpbrk(real, " :"))
      return true;
   /* By a link, where only the user can write. */
   if (!run || !run[0] || SDL_strpbrk(run, " :"))
      return false;
   SDL_snprintf(dst, size, "%s/" LANHOOK_LIBRARY, run);
   unlink(dst);
   return symlink(real, dst) == 0;
}

bool inject_prepare(void)
{
   char path[1200], preload[4096], pid[16];
   const char *had = getenv("LD_PRELOAD");
   struct lanhook *shared;

   if (!library(path, sizeof(path)) || !(shared = map_memory((Uint32)getpid(), true)))
      return false;
   shared->version = LANHOOK_VERSION;
   munmap(shared, sizeof(*shared));
   SDL_snprintf(pid, sizeof(pid), "%u", (unsigned)getpid());
   /* After Steam's overlay. */
   SDL_snprintf(preload, sizeof(preload), "%s%s%s", had ? had : "", had && had[0] ? ":" : "", path);
   setenv(LANHOOK_ENV, pid, 1);
   setenv("LD_PRELOAD", preload, 1);
   return true;
}

void inject_over(void)
{
   char name[64];

   SDL_snprintf(name, sizeof(name), LANHOOK_MEMORY, (unsigned)getpid());
   shm_unlink(name);
}

/* ---- The session ---------------------------------------------------------- */

/* A game: by the FuseBox that runs it. */
struct game
{
   Uint32 pid;
   struct lanhook *shared;
   int s;
   SDL_Thread *thread;
   SDL_AtomicInt over;
};

static struct
{
   SDL_Mutex *lock;
   struct vlan *v;
   Uint32 net, players;
   int me;
   struct game *games[GAMES];
   int count;
} in;

static void init(void)
{
   if (!in.lock)
      in.lock = SDL_CreateMutex();
}

static void set(int32_t *value, int32_t v)
{
   __atomic_store_n(value, v, __ATOMIC_RELEASE);
}

static void answer_one(int s)
{
   struct timeval tv = { 0, 500000 };
   struct lanhook_ask q;
   struct ucred who;
   socklen_t len = sizeof(who);
   Uint8 ok = 0;

   setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   if (getsockopt(s, SOL_SOCKET, SO_PEERCRED, &who, &len) == 0 && who.uid == getuid()
         && recv(s, &q, sizeof(q), 0) == sizeof(q))
   {
      SDL_LockMutex(in.lock);
      ok = in.v && vlan_open(in.v, q.player, q.port, q.udp != 0);
      SDL_UnlockMutex(in.lock);
      send(s, &ok, 1, MSG_NOSIGNAL);
   }
   close(s);
}

/* Answers what's in the game. */
static int SDLCALL answer(void *user)
{
   struct game *g = user;

   while (!SDL_GetAtomicInt(&g->over))
   {
      struct pollfd p = { g->s, POLLIN, 0 };
      int s;

      if (poll(&p, 1, LOOK_MS) > 0 && (s = accept4(g->s, NULL, NULL, SOCK_CLOEXEC)) >= 0)
         answer_one(s);
   }
   return 0;
}

static void inform(struct game *g)
{
   set(&g->shared->net, (int32_t)in.net);
   set(&g->shared->me, in.me);
   set(&g->shared->players, (int32_t)in.players);
   set(&g->shared->change, g->shared->change + 1);
   set(&g->shared->active, in.v != NULL);
}

void inject_lan(struct vlan *v, Uint32 net, int me, Uint32 players)
{
   int i;

   init();
   SDL_LockMutex(in.lock);
   in.v = v;
   in.net = net;
   in.me = me;
   in.players = players;
   for (i = 0; i < in.count; i++)
      inform(in.games[i]);
   SDL_UnlockMutex(in.lock);
}

static void drop(struct game *g)
{
   SDL_SetAtomicInt(&g->over, 1);
   if (g->thread)
      SDL_WaitThread(g->thread, NULL);
   if (g->s >= 0)
      close(g->s);
   if (g->shared)
   {
      set(&g->shared->active, 0);
      munmap(g->shared, sizeof(*g->shared));
   }
   SDL_free(g);
}

static int listener(Uint32 pid)
{
   struct sockaddr_un a;
   int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
   int n;

   SDL_zero(a);
   a.sun_family = AF_UNIX;
   n = SDL_snprintf(a.sun_path + 1, sizeof(a.sun_path) - 1, LANHOOK_SOCKET, (unsigned)pid);
   if (s >= 0 && (bind(s, (struct sockaddr *)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)n)) != 0
            || listen(s, 64) != 0))
   {
      close(s);
      s = -1;
   }
   return s;
}

static bool join(Uint32 pid)
{
   struct game *g;
   int i;

   init();
   for (i = 0; i < in.count; i++)
      if (in.games[i]->pid == pid)
         return true;
   if (in.count >= GAMES || !(g = SDL_calloc(1, sizeof(*g))))
      return false;
   g->pid = pid;
   g->s = -1;
   if (!(g->shared = map_memory(pid, false)) || g->shared->version != LANHOOK_VERSION
         || (g->s = listener(pid)) < 0 || !(g->thread = SDL_CreateThread(answer, "lan", g)))
   {
      drop(g);
      return false;
   }
   SDL_LockMutex(in.lock);
   inform(g);
   in.games[in.count++] = g;
   SDL_UnlockMutex(in.lock);
   return true;
}

/* Whether the process has the library: not one that's linked statically, 32-bit, or
 * in a sandbox. */
static bool has_library(Uint32 pid)
{
   char path[64], line[2048];
   bool found = false;
   FILE *f;

   SDL_snprintf(path, sizeof(path), "/proc/%u/maps", (unsigned)pid);
   if (!(f = fopen(path, "re")))
      return false;
   while (!found && fgets(line, sizeof(line), f))
      found = SDL_strstr(line, "/" LANHOOK_LIBRARY) != NULL;
   fclose(f);
   return found;
}

bool inject_into(Uint32 game, Uint32 pid)
{
   return join(game) && has_library(pid);
}

bool inject_ready(Uint32 pid)
{
   return has_library(pid);
}

void inject_end(void)
{
   if (!in.lock)
      return;
   inject_lan(NULL, 0, 0, 0);
   while (in.count)
      drop(in.games[--in.count]);
}

#endif
