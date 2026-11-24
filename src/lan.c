#ifdef _WIN32
#include <windows.h>
#else
#define _DEFAULT_SOURCE
#include <signal.h>
#include <unistd.h>
#endif

#include "lan.h"
#include "platform.h"

#include <SDL3/SDL.h>

static Uint32 own_pid(void)
{
#ifdef _WIN32
   return GetCurrentProcessId();
#else
   return (Uint32)getpid();
#endif
}

void lan_playing(const char *config_dir, Uint32 appid, const char *game, bool on)
{
   char path[1100], text[1100];

   SDL_snprintf(path, sizeof(path), "%s" PATH_SEP "playing" PATH_SEP "%u", config_dir,
         (unsigned)own_pid());
   if (!on)
   {
      SDL_RemovePath(path);
      return;
   }
   SDL_snprintf(path, sizeof(path), "%s" PATH_SEP "playing", config_dir);
   SDL_CreateDirectory(path);
   SDL_snprintf(path, sizeof(path), "%s" PATH_SEP "playing" PATH_SEP "%u", config_dir,
         (unsigned)own_pid());
   SDL_snprintf(text, sizeof(text), "%u\n%s", (unsigned)appid, game);
   SDL_SaveFile(path, text, SDL_strlen(text));
}

bool lan_running(Uint32 pid)
{
#ifdef _WIN32
   HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
   WCHAR exe[1024];
   DWORD size = SDL_arraysize(exe);
   size_t n;
   bool running;

   if (!p)
      return false;
   /* The number may be another's by now. */
   running = WaitForSingleObject(p, 0) == WAIT_TIMEOUT && QueryFullProcessImageNameW(p, 0, exe, &size)
      && (n = wcslen(exe)) >= 11 && _wcsicmp(exe + n - 11, L"fusebox.exe") == 0;
   CloseHandle(p);
   return running;
#else
   return kill((pid_t)pid, 0) == 0;
#endif
}

struct search
{
   Uint32 appid, pid;
   const char *path;
};

static SDL_EnumerationResult SDLCALL on_playing(void *user, const char *dir, const char *name)
{
   struct search *s = user;
   char path[1200], *text, *game;
   Uint32 appid;
   Uint32 pid = (Uint32)SDL_strtoul(name, NULL, 10);

   SDL_snprintf(path, sizeof(path), "%s%s", dir, name);
   if (!lan_running(pid))
   {
      /* One that didn't get to end. */
      SDL_RemovePath(path);
      return SDL_ENUM_CONTINUE;
   }
   if (!(text = SDL_LoadFile(path, NULL)))
      return SDL_ENUM_CONTINUE;
   appid = (Uint32)SDL_strtoul(text, NULL, 10);
   game = SDL_strchr(text, '\n');
   if ((s->appid && appid == s->appid)
         || (s->path[0] && game && library_same_path(game + 1, s->path)))
      s->pid = pid;
   SDL_free(text);
   return SDL_ENUM_CONTINUE;
}

Uint32 lan_game(const char *config_dir, Uint32 appid, const char *game)
{
   char path[1100];
   struct search s = { appid, 0, game };

   SDL_snprintf(path, sizeof(path), "%s" PATH_SEP "playing", config_dir);
   if ((appid || game[0]) && platform_exists(path))
      SDL_EnumerateDirectory(path, on_playing, &s);
   return s.pid;
}

/* ---- Shortcuts that are there only meanwhile ------------------------------ */

#define ONCE_MAX 16

static struct
{
   SDL_Mutex *lock;
   struct
   {
      Uint32 appid, pid;
      char path[1024];
   } list[ONCE_MAX];
   int count;
} once;

static void once_path(const char *config_dir, Uint32 appid, char *dst, size_t size)
{
   if (appid)
      SDL_snprintf(dst, size, "%s" PATH_SEP "once" PATH_SEP "%u", config_dir, (unsigned)appid);
   else
      SDL_snprintf(dst, size, "%s" PATH_SEP "once", config_dir);
}

void lan_once(const char *config_dir, Uint32 appid, const char *path, bool on)
{
   char file[1100], text[1100];

   once_path(config_dir, 0, file, sizeof(file));
   SDL_CreateDirectory(file);
   once_path(config_dir, appid, file, sizeof(file));
   if (!on)
   {
      SDL_RemovePath(file);
      return;
   }
   SDL_snprintf(text, sizeof(text), "%u\n%s", (unsigned)own_pid(), path);
   SDL_SaveFile(file, text, SDL_strlen(text));
}

static SDL_EnumerationResult SDLCALL on_once(void *user, const char *dir, const char *name)
{
   char file[1200], *text, *path;

   SDL_snprintf(file, sizeof(file), "%s%s", dir, name);
   if (once.count < ONCE_MAX && (text = SDL_LoadFile(file, NULL)))
   {
      once.list[once.count].appid = (Uint32)SDL_strtoul(name, NULL, 10);
      once.list[once.count].pid = (Uint32)SDL_strtoul(text, NULL, 10);
      path = SDL_strchr(text, '\n');
      SDL_strlcpy(once.list[once.count].path, path ? path + 1 : "", sizeof(once.list[0].path));
      once.count++;
      SDL_free(text);
   }
   return SDL_ENUM_CONTINUE;
}

static void read_once(const char *config_dir)
{
   char dir[1100];

   if (!once.lock)
      once.lock = SDL_CreateMutex();
   once_path(config_dir, 0, dir, sizeof(dir));
   SDL_LockMutex(once.lock);
   once.count = 0;
   if (platform_exists(dir))
      SDL_EnumerateDirectory(dir, on_once, NULL);
   SDL_UnlockMutex(once.lock);
}

Uint32 lan_once_left(const char *config_dir)
{
   Uint32 appid = 0;
   int i;

   read_once(config_dir);
   SDL_LockMutex(once.lock);
   for (i = 0; i < once.count && !appid; i++)
      if (!lan_running(once.list[i].pid))
         appid = once.list[i].appid;
   SDL_UnlockMutex(once.lock);
   return appid;
}
