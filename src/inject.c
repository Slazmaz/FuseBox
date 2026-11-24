#ifdef _WIN32

#include "inject.h"
#include <SDL3/SDL.h>
#include <wchar.h>

#define GAMES 64

struct game
{
   DWORD pid;
   HANDLE memory, thread, process;
   struct lanhook *shared;
   volatile LONG over;
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

/* Answers the DLL. */
static DWORD WINAPI answer(void *user)
{
   struct game *g = user;
   WCHAR name[64];
   HANDLE pipe;

   wsprintfW(name, LANHOOK_PIPE, g->pid);
   pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
         PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_REJECT_REMOTE_CLIENTS, 1, 64, 64, 0, NULL);
   while (pipe != INVALID_HANDLE_VALUE && !g->over)
   {
      struct lanhook_ask q;
      DWORD n = 0;
      BYTE ok = 0;

      if (!ConnectNamedPipe(pipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED)
         break;
      if (!g->over && ReadFile(pipe, &q, sizeof(q), &n, NULL) && n == sizeof(q))
      {
         SDL_LockMutex(in.lock);
         ok = in.v && vlan_open(in.v, q.player, q.port, q.udp != 0);
         SDL_UnlockMutex(in.lock);
         WriteFile(pipe, &ok, 1, &n, NULL);
         FlushFileBuffers(pipe);
      }
      DisconnectNamedPipe(pipe);
   }
   if (pipe != INVALID_HANDLE_VALUE)
      CloseHandle(pipe);
   return 0;
}

static void inform(struct game *g)
{
   g->shared->net = (LONG)in.net;
   g->shared->me = in.me;
   g->shared->players = (LONG)in.players;
   InterlockedExchange(&g->shared->active, in.v != NULL);
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
   WCHAR name[64];
   BYTE none, got;
   DWORD n;

   InterlockedExchange(&g->over, 1);
   if (g->shared)
      InterlockedExchange(&g->shared->active, 0);
   if (g->thread)
   {
      /* It waits for a question. */
      wsprintfW(name, LANHOOK_PIPE, g->pid);
      none = 0;
      CallNamedPipeW(name, &none, 1, &got, 1, &n, 100);
      WaitForSingleObject(g->thread, 2000);
      CloseHandle(g->thread);
   }
   if (g->shared)
      UnmapViewOfFile(g->shared);
   if (g->memory)
      CloseHandle(g->memory);
   if (g->process)
      CloseHandle(g->process);
   SDL_free(g);
}

static bool load_into(HANDLE process)
{
   WCHAR dll[1024];
   DWORD n = GetModuleFileNameW(NULL, dll, SDL_arraysize(dll) - 16), code = 0;
   WCHAR *slash = n ? wcsrchr(dll, '\\') : NULL;
   SIZE_T size;
   void *there;
   HANDLE thread;
   bool ok = false;

   if (!slash)
      return false;
   wcscpy(slash + 1, L"fuseboxlan.dll");
   size = (wcslen(dll) + 1) * sizeof(WCHAR);
   if (!(there = VirtualAllocEx(process, NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)))
      return false;
   /* Windows' own libraries are at the same place in every process. */
   if (WriteProcessMemory(process, there, dll, size, NULL)
         && (thread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)(void *)
               GetProcAddress(GetModuleHandleW(L"kernel32"), "LoadLibraryW"), there, 0, NULL)))
   {
      ok = WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0 && GetExitCodeThread(thread, &code)
         && code;
      CloseHandle(thread);
   }
   VirtualFreeEx(process, there, 0, MEM_RELEASE);
   return ok;
}

bool inject_into(Uint32 pid)
{
   WCHAR name[64];
   BOOL small = FALSE;
   struct game *g;
   int i;

   init();
   SDL_LockMutex(in.lock);
   for (i = 0; i < in.count; i++)
      if (in.games[i]->pid == pid)
      {
         SDL_UnlockMutex(in.lock);
         return true;
      }
   SDL_UnlockMutex(in.lock);
   if (in.count >= GAMES || !(g = SDL_calloc(1, sizeof(*g))))
      return false;
   g->pid = pid;
   wsprintfW(name, LANHOOK_MEMORY, pid);
   if (!(g->process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION
               | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid))
         || !IsWow64Process(g->process, &small) || small
         || !(g->memory = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
               sizeof(*g->shared), name))
         || !(g->shared = MapViewOfFile(g->memory, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*g->shared))))
   {
      drop(g);
      return false;
   }
   g->shared->version = LANHOOK_VERSION;
   SDL_LockMutex(in.lock);
   inform(g);
   SDL_UnlockMutex(in.lock);
   g->thread = CreateThread(NULL, 0, answer, g, 0, NULL);
   if (!g->thread || !load_into(g->process))
   {
      drop(g);
      return false;
   }
   SDL_LockMutex(in.lock);
   in.games[in.count++] = g;
   SDL_UnlockMutex(in.lock);
   return true;
}

bool inject_ready(Uint32 pid)
{
   bool ready = false;
   int i;

   init();
   SDL_LockMutex(in.lock);
   for (i = 0; i < in.count; i++)
      ready |= in.games[i]->pid == pid && in.games[i]->shared->ready;
   SDL_UnlockMutex(in.lock);
   return ready;
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
