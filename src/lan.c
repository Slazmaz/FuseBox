#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#else
#define _DEFAULT_SOURCE
#include <signal.h>
#include <unistd.h>
#endif

#include "lan.h"
#include "platform.h"
#include "steam.h"

#include <SDL3/SDL.h>

/* Ports from here on are whichever was free, for one connection. */
#define EPHEMERAL 49152
/* Steam's interface, when nothing says which (steamui.c). */
#define STEAM_PORT 8080

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
   bool any;
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
   s->any = true;
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
   struct search s = { appid, 0, game, false };

   SDL_snprintf(path, sizeof(path), "%s" PATH_SEP "playing", config_dir);
   if ((appid || game[0]) && platform_exists(path))
      SDL_EnumerateDirectory(path, on_playing, &s);
   return s.pid;
}

bool lan_any(const char *config_dir)
{
   char path[1100];
   struct search s = { 0, 0, "", false };

   SDL_snprintf(path, sizeof(path), "%s" PATH_SEP "playing", config_dir);
   if (platform_exists(path))
      SDL_EnumerateDirectory(path, on_playing, &s);
   return s.any;
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

void lan_read_once(const char *config_dir)
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

Uint32 lan_app(const struct game *g)
{
   Uint32 appid = 0;
   int i;

   /* First: there's one where Steam doesn't have the game's own, whatever the
    * library says, and that's what runs. */
   SDL_LockMutex(once.lock);
   for (i = 0; i < once.count && !appid; i++)
      if (library_same_path(once.list[i].path, library_path(g)))
         appid = once.list[i].appid;
   SDL_UnlockMutex(once.lock);
   return appid ? appid : g->steam_appid;
}

Uint32 lan_once_left(const char *config_dir)
{
   Uint32 appid = 0;
   int i;

   lan_read_once(config_dir);
   SDL_LockMutex(once.lock);
   for (i = 0; i < once.count && !appid; i++)
      if (!lan_running(once.list[i].pid))
         appid = once.list[i].appid;
   SDL_UnlockMutex(once.lock);
   return appid;
}

bool lan_port_allowed(Uint16 port)
{
   return port >= 1024 && port < EPHEMERAL && port != STEAM_PORT && port != steam_port();
}

void lan_write_ports(const struct tunnel_port *ports, int count, char *dst, size_t size)
{
   int i;

   dst[0] = '\0';
   for (i = 0; i < count; i++)
   {
      char text[16];
      SDL_snprintf(text, sizeof(text), "%s%c%u", i ? " " : "", ports[i].udp ? 'u' : 't',
            ports[i].port);
      SDL_strlcat(dst, text, size);
   }
}

int lan_read_ports(const char *text, struct tunnel_port *ports, int max)
{
   int count = 0;

   while (*text && count < max)
   {
      char kind = *text++, *end;
      unsigned long port;

      if (kind != 't' && kind != 'u')
         continue;
      port = SDL_strtoul(text, &end, 10);
      if (end == text)
         continue;
      text = end;
      if (port > 65535 || !lan_port_allowed((Uint16)port))
         continue;
      ports[count].port = (Uint16)port;
      ports[count++].udp = kind == 'u';
   }
   return count;
}

#ifdef _WIN32

#define PIDS 256

struct pids
{
   DWORD list[PIDS];
   int count;
};

static bool has(const struct pids *p, DWORD pid)
{
   int i;

   for (i = 0; i < p->count; i++)
      if (p->list[i] == pid)
         return true;
   return false;
}

static void add_pid(struct pids *p, DWORD pid)
{
   if (p->count < PIDS && !has(p, pid))
      p->list[p->count++] = pid;
}

/* What the process started, and what those did. */
static void descendants(DWORD of, struct pids *p)
{
   HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
   PROCESSENTRY32W e;
   int before;

   if (snap == INVALID_HANDLE_VALUE)
      return;
   do
   {
      before = p->count;
      e.dwSize = sizeof(e);
      if (Process32FirstW(snap, &e))
         do
            if (e.th32ParentProcessID == of || has(p, e.th32ParentProcessID))
               if (e.th32ProcessID != of)
                  add_pid(p, e.th32ProcessID);
         while (Process32NextW(snap, &e));
   } while (p->count != before);
   CloseHandle(snap);
}

/* Not a game's server: the system's services, Steam, and others that games
 * talk to for other things. */
static bool no_server(DWORD pid)
{
   static const WCHAR *const names[] = { L"steam.exe", L"steamwebhelper.exe", L"steamservice.exe",
      L"fusebox.exe", L"discord.exe", L"explorer.exe", L"svchost.exe", L"nvcontainer.exe",
      L"epicgameslauncher.exe", L"battle.net.exe", L"agent.exe", L"galaxyclient.exe" };
   HANDLE p;
   WCHAR exe[1024], *name;
   DWORD size = SDL_arraysize(exe), session = 0;
   size_t i;
   bool ok = false;

   if (pid <= 4 || !ProcessIdToSessionId(pid, &session) || !session
         || !(p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)))
      return true;
   if (QueryFullProcessImageNameW(p, 0, exe, &size))
   {
      ok = true;
      name = wcsrchr(exe, '\\');
      name = name ? name + 1 : exe;
      for (i = 0; i < SDL_arraysize(names); i++)
         ok &= _wcsicmp(name, names[i]) != 0;
   }
   CloseHandle(p);
   return !ok;
}

static void *table(DWORD (WINAPI *get)(void *, DWORD *, BOOL, ULONG, int, ULONG), ULONG family,
      int kind)
{
   DWORD size = 0;
   void *t = NULL;
   int tries;

   for (tries = 0; tries < 4; tries++)
   {
      DWORD r = get(t, &size, FALSE, family, kind, 0);

      if (r == NO_ERROR && t)
         return t;
      SDL_free(t);
      t = NULL;
      if (r != ERROR_INSUFFICIENT_BUFFER && r != NO_ERROR)
         return NULL;
      size += 4096;
      t = SDL_malloc(size);
   }
   SDL_free(t);
   return NULL;
}

static DWORD WINAPI get_tcp(void *t, DWORD *size, BOOL order, ULONG family, int kind, ULONG r)
{
   return GetExtendedTcpTable(t, size, order, family, (TCP_TABLE_CLASS)kind, r);
}

static DWORD WINAPI get_udp(void *t, DWORD *size, BOOL order, ULONG family, int kind, ULONG r)
{
   return GetExtendedUdpTable(t, size, order, family, (UDP_TABLE_CLASS)kind, r);
}

static int add_port(struct tunnel_port *ports, int count, int max, Uint16 port, bool udp)
{
   int i;

   for (i = 0; i < count; i++)
      if (ports[i].port == port && ports[i].udp == udp)
         return count;
   if (count < max && lan_port_allowed(port))
   {
      ports[count].port = port;
      ports[count++].udp = udp;
   }
   return count;
}

int lan_processes(Uint32 pid, Uint32 *pids, int max)
{
   struct pids p = { .count = 0 };
   int i;

   descendants(pid, &p);
   for (i = 0; i < p.count && i < max; i++)
      pids[i] = p.list[i];
   return i;
}

/* What the game has open, as last looked at (ms). */
static struct
{
   Uint32 pid;
   Uint64 at;
   struct tunnel_port ports[512];
   int count;
} owned;

static void own_port(Uint16 port, bool udp)
{
   if (owned.count < (int)SDL_arraysize(owned.ports))
   {
      owned.ports[owned.count].port = port;
      owned.ports[owned.count++].udp = udp;
   }
}

static void look_at_owned(Uint32 pid)
{
   MIB_TCPTABLE_OWNER_PID *tcp = table(get_tcp, AF_INET, TCP_TABLE_OWNER_PID_LISTENER);
   MIB_TCP6TABLE_OWNER_PID *tcp6 = table(get_tcp, AF_INET6, TCP_TABLE_OWNER_PID_LISTENER);
   MIB_UDPTABLE_OWNER_PID *udp = table(get_udp, AF_INET, UDP_TABLE_OWNER_PID);
   MIB_UDP6TABLE_OWNER_PID *udp6 = table(get_udp, AF_INET6, UDP_TABLE_OWNER_PID);
   struct pids game = { .count = 0 };
   DWORD i;

   descendants(pid, &game);
   owned.pid = pid;
   owned.at = SDL_GetTicks();
   owned.count = 0;
   for (i = 0; tcp && i < tcp->dwNumEntries; i++)
      if (has(&game, tcp->table[i].dwOwningPid))
         own_port(ntohs((u_short)tcp->table[i].dwLocalPort), false);
   for (i = 0; tcp6 && i < tcp6->dwNumEntries; i++)
      if (has(&game, tcp6->table[i].dwOwningPid))
         own_port(ntohs((u_short)tcp6->table[i].dwLocalPort), false);
   for (i = 0; udp && i < udp->dwNumEntries; i++)
      if (has(&game, udp->table[i].dwOwningPid))
         own_port(ntohs((u_short)udp->table[i].dwLocalPort), true);
   for (i = 0; udp6 && i < udp6->dwNumEntries; i++)
      if (has(&game, udp6->table[i].dwOwningPid))
         own_port(ntohs((u_short)udp6->table[i].dwLocalPort), true);
   SDL_free(tcp);
   SDL_free(tcp6);
   SDL_free(udp);
   SDL_free(udp6);
}

bool lan_owns(Uint32 pid, Uint16 port, bool udp)
{
   int i, again;

   if (!pid)
      return false;
   for (again = 0; again < 2; again++)
   {
      Uint64 age = SDL_GetTicks() - owned.at;

      /* One that isn't there may be new. */
      if (owned.pid != pid || !owned.at || age > (again ? 100 : 1000))
         look_at_owned(pid);
      for (i = 0; i < owned.count; i++)
         if (owned.ports[i].port == port && owned.ports[i].udp == udp)
            return true;
   }
   return false;
}

int lan_ports(Uint32 pid, bool own, struct tunnel_port *ports, int count, int max)
{
   MIB_TCPTABLE_OWNER_PID *tcp = table(get_tcp, AF_INET, TCP_TABLE_OWNER_PID_ALL);
   MIB_TCP6TABLE_OWNER_PID *tcp6 = table(get_tcp, AF_INET6, TCP_TABLE_OWNER_PID_LISTENER);
   MIB_UDPTABLE_OWNER_PID *udp = table(get_udp, AF_INET, UDP_TABLE_OWNER_PID);
   MIB_UDP6TABLE_OWNER_PID *udp6 = table(get_udp, AF_INET6, UDP_TABLE_OWNER_PID);
   struct pids game = { .count = 0 }, servers;
   DWORD i, k;

   descendants(pid, &game);
   servers = game;
   if (!own)
      servers.count = 0;
   /* Connected to a server here: on 127.0.0.1, or by this computer's own
    * address. */
   for (i = 0; tcp && i < tcp->dwNumEntries; i++)
   {
      const MIB_TCPROW_OWNER_PID *c = &tcp->table[i];
      DWORD owner = 0;

      if (c->dwState != MIB_TCP_STATE_ESTAB || !has(&game, c->dwOwningPid)
            || ((ntohl(c->dwRemoteAddr) >> 24) != 127 && c->dwRemoteAddr != c->dwLocalAddr))
         continue;
      for (k = 0; k < tcp->dwNumEntries && !owner; k++)
         if (tcp->table[k].dwState == MIB_TCP_STATE_LISTEN
               && (Uint16)tcp->table[k].dwLocalPort == (Uint16)c->dwRemotePort)
            owner = tcp->table[k].dwOwningPid;
      for (k = 0; tcp6 && k < tcp6->dwNumEntries && !owner; k++)
         if ((Uint16)tcp6->table[k].dwLocalPort == (Uint16)c->dwRemotePort)
            owner = tcp6->table[k].dwOwningPid;
      if (!owner || (has(&game, owner) ? !own : no_server(owner)))
         continue;
      count = add_port(ports, count, max, ntohs((u_short)c->dwRemotePort), false);
      add_pid(&servers, owner);
   }
   for (i = 0; tcp && i < tcp->dwNumEntries; i++)
      if (tcp->table[i].dwState == MIB_TCP_STATE_LISTEN && has(&servers, tcp->table[i].dwOwningPid))
         count = add_port(ports, count, max, ntohs((u_short)tcp->table[i].dwLocalPort), false);
   for (i = 0; tcp6 && i < tcp6->dwNumEntries; i++)
      if (has(&servers, tcp6->table[i].dwOwningPid))
         count = add_port(ports, count, max, ntohs((u_short)tcp6->table[i].dwLocalPort), false);
   for (i = 0; udp && i < udp->dwNumEntries; i++)
      if (has(&servers, udp->table[i].dwOwningPid))
         count = add_port(ports, count, max, ntohs((u_short)udp->table[i].dwLocalPort), true);
   for (i = 0; udp6 && i < udp6->dwNumEntries; i++)
      if (has(&servers, udp6->table[i].dwOwningPid))
         count = add_port(ports, count, max, ntohs((u_short)udp6->table[i].dwLocalPort), true);
   SDL_free(tcp);
   SDL_free(tcp6);
   SDL_free(udp);
   SDL_free(udp6);
   return count;
}

#else

int lan_processes(Uint32 pid, Uint32 *pids, int max)
{
   return 0;
}

bool lan_owns(Uint32 pid, Uint16 port, bool udp)
{
   return false;
}

int lan_ports(Uint32 pid, bool own, struct tunnel_port *ports, int count, int max)
{
   return count;
}

#endif
