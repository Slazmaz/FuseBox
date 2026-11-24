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
