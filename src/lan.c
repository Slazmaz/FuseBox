#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#else
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
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
   char path[64], exe[1024], *name;
   ssize_t n;

   SDL_snprintf(path, sizeof(path), "/proc/%u/exe", (unsigned)pid);
   if (!pid || (n = readlink(path, exe, sizeof(exe) - 1)) <= 0)
      return false;
   exe[n] = '\0';
   name = SDL_strrchr(exe, '/');
   /* The number may be another's by now; the second: updated since. */
   return name && (SDL_strcmp(name, "/fusebox") == 0 || SDL_strcmp(name, "/fusebox (deleted)") == 0);
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
   } list[ONCE_MAX], playing[ONCE_MAX];
   int count, playing_count;
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

/* Games that run, by any shortcut. */
static SDL_EnumerationResult SDLCALL on_running(void *user, const char *dir, const char *name)
{
   char file[1200], *text, *path;
   Uint32 pid = (Uint32)SDL_strtoul(name, NULL, 10), appid;

   SDL_snprintf(file, sizeof(file), "%s%s", dir, name);
   if (once.playing_count < ONCE_MAX && lan_running(pid) && (text = SDL_LoadFile(file, NULL)))
   {
      appid = (Uint32)SDL_strtoul(text, NULL, 10);
      path = SDL_strchr(text, '\n');
      if (appid && path && path[1])
      {
         once.playing[once.playing_count].appid = appid;
         once.playing[once.playing_count].pid = pid;
         SDL_strlcpy(once.playing[once.playing_count++].path, path + 1,
               sizeof(once.playing[0].path));
      }
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
   once.count = once.playing_count = 0;
   if (platform_exists(dir))
      SDL_EnumerateDirectory(dir, on_once, NULL);
   SDL_snprintf(dir, sizeof(dir), "%s" PATH_SEP "playing", config_dir);
   if (platform_exists(dir))
      SDL_EnumerateDirectory(dir, on_running, NULL);
   SDL_UnlockMutex(once.lock);
}

Uint32 lan_app(const struct game *g)
{
   Uint32 appid = 0;
   int i;

   /* First: there's one where Steam doesn't have the game's own, whatever the
    * library says, and that's what runs. */
   SDL_LockMutex(once.lock);
   for (i = 0; i < once.playing_count && !appid; i++)
      if (library_same_path(once.playing[i].path, library_path(g)))
         appid = once.playing[i].appid;
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

#define PIDS 256

struct pids
{
   Uint32 list[PIDS];
   int count;
};

static bool has(const struct pids *p, Uint32 pid)
{
   int i;

   for (i = 0; i < p->count; i++)
      if (p->list[i] == pid)
         return true;
   return false;
}

static void add_pid(struct pids *p, Uint32 pid)
{
   if (p->count < PIDS && !has(p, pid))
      p->list[p->count++] = pid;
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

static void look_at_owned(Uint32 pid);

#ifdef _WIN32

/* What the process started, and what those did. */
static void descendants(Uint32 of, struct pids *p)
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

/* A process' number by its folder's name in /proc, 0 for what isn't one. */
static Uint32 pid_of(const char *name)
{
   const char *p;

   for (p = name; *p >= '0' && *p <= '9'; p++)
      ;
   return *p ? 0 : (Uint32)SDL_strtoul(name, NULL, 10);
}

static Uint32 parent_of(Uint32 pid)
{
   char path[64], text[512], *p;
   unsigned parent = 0;
   size_t n;
   FILE *f;

   SDL_snprintf(path, sizeof(path), "/proc/%u/stat", (unsigned)pid);
   if (!(f = fopen(path, "re")))
      return 0;
   n = fread(text, 1, sizeof(text) - 1, f);
   fclose(f);
   text[n] = '\0';
   /* Its name, in brackets, may have anything in it. */
   if (!(p = SDL_strrchr(text, ')')) || SDL_sscanf(p + 1, " %*c %u", &parent) != 1)
      return 0;
   return parent;
}

/* What the process started, and what those did. */
static void descendants(Uint32 of, struct pids *p)
{
   static struct
   {
      Uint32 pid, parent;
   } list[8192];
   struct dirent *e;
   DIR *dir = opendir("/proc");
   int i, n = 0, before;

   if (!dir)
      return;
   while ((e = readdir(dir)) && n < (int)SDL_arraysize(list))
      if ((list[n].pid = pid_of(e->d_name)) && (list[n].parent = parent_of(list[n].pid)))
         n++;
   closedir(dir);
   do
   {
      before = p->count;
      for (i = 0; i < n; i++)
         if (list[i].pid != of && (list[i].parent == of || has(p, list[i].parent)))
            add_pid(p, list[i].pid);
   } while (p->count != before);
}

/* Not a game's server: the desktop's programs, Steam, and others that games talk
 * to for other things. Other users' aren't known to begin with. */
static bool no_server(Uint32 pid)
{
   /* As /proc has them: 15 letters at most. */
   static const char *const names[] = { "steam", "steamwebhelper", "fusebox", "Discord", "systemd",
      "pipewire", "pipewire-pulse", "wireplumber", "pulseaudio", "dbus-daemon", "dbus-broker",
      "Xorg", "Xwayland", "gamescope", "gamescope-wl", "heroic", "legendary", "lutris", "ssh",
      "sshd" };
   char path[64], name[64] = "";
   size_t i;
   FILE *f;

   SDL_snprintf(path, sizeof(path), "/proc/%u/comm", (unsigned)pid);
   if (!(f = fopen(path, "re")))
      return true;
   if (!fgets(name, sizeof(name), f))
      name[0] = '\0';
   fclose(f);
   for (i = 0; name[i] && name[i] != 10; i++)
      ;
   name[i] = 0;
   for (i = 0; i < SDL_arraysize(names); i++)
      if (SDL_strcmp(name, names[i]) == 0)
         return true;
   return !name[0];
}

struct sock
{
   Uint32 local, remote;
   Uint16 port, remote_port;
   bool udp, listens, connected;
   unsigned long inode;
   Uint32 pid;
};

struct socks
{
   struct sock *list;
   int count, alloc;
};

/* The kernel's list of one kind. IPv6's addresses: only IPv4 ones among them. */
static void read_socks(struct socks *s, const char *path, bool udp, bool six)
{
   char line[512];
   FILE *f = fopen(path, "re");

   if (!f)
      return;
   while (fgets(line, sizeof(line), f))
   {
      char local[40], remote[40];
      unsigned port, remote_port, state;
      unsigned long inode;
      struct sock *k;

      if (SDL_sscanf(line, " %*d: %32[0-9A-Fa-f]:%x %32[0-9A-Fa-f]:%x %x %*s %*s %*s %*u %*d %lu",
               local, &port, remote, &remote_port, &state, &inode) != 6)
         continue;
      if (s->count == s->alloc)
      {
         int alloc = s->alloc ? s->alloc * 2 : 256;
         struct sock *grown = SDL_realloc(s->list, sizeof(*grown) * (size_t)alloc);

         if (!grown)
            break;
         s->list = grown;
         s->alloc = alloc;
      }
      k = &s->list[s->count++];
      SDL_zerop(k);
      k->udp = udp;
      /* As the kernel has them in memory. */
      k->local = SDL_Swap32BE((Uint32)SDL_strtoul(local + (six ? 24 : 0), NULL, 16));
      k->remote = SDL_Swap32BE((Uint32)SDL_strtoul(remote + (six ? 24 : 0), NULL, 16));
      k->port = (Uint16)port;
      k->remote_port = (Uint16)remote_port;
      k->listens = !udp && state == 10;
      k->connected = state == 1;
      k->inode = inode;
   }
   fclose(f);
}

static void all_socks(struct socks *s)
{
   SDL_zerop(s);
   read_socks(s, "/proc/net/tcp", false, false);
   read_socks(s, "/proc/net/tcp6", false, true);
   read_socks(s, "/proc/net/udp", true, false);
   read_socks(s, "/proc/net/udp6", true, true);
}

static int by_inode(const void *a, const void *b)
{
   unsigned long x = ((const struct sock *)a)->inode, y = ((const struct sock *)b)->inode;

   return x < y ? -1 : x > y;
}

/* Which of them are that process's; sorted by inode. */
static void find_owner(struct socks *s, Uint32 pid)
{
   char path[64], link[400], target[64];
   struct dirent *e;
   DIR *dir;

   SDL_snprintf(path, sizeof(path), "/proc/%u/fd", (unsigned)pid);
   if (!(dir = opendir(path)))
      return;
   while ((e = readdir(dir)))
   {
      struct sock key, *k;
      ssize_t n;

      SDL_snprintf(link, sizeof(link), "%s/%s", path, e->d_name);
      if ((n = readlink(link, target, sizeof(target) - 1)) <= 0)
         continue;
      target[n] = '\0';
      if (SDL_strncmp(target, "socket:[", 8) != 0)
         continue;
      key.inode = SDL_strtoul(target + 8, NULL, 10);
      /* The first that has it: what a process starts may have it too. */
      if ((k = SDL_bsearch(&key, s->list, (size_t)s->count, sizeof(key), by_inode)) && !k->pid)
         k->pid = pid;
   }
   closedir(dir);
}

static void find_owners(struct socks *s, const struct pids *only)
{
   int i;

   SDL_qsort(s->list, (size_t)s->count, sizeof(*s->list), by_inode);
   /* The game's first. */
   for (i = 0; i < only->count; i++)
      find_owner(s, only->list[i]);
}

static void find_others(struct socks *s)
{
   struct dirent *e;
   Uint32 pid;
   DIR *dir = opendir("/proc");

   while (dir && (e = readdir(dir)))
      if ((pid = pid_of(e->d_name)))
         find_owner(s, pid);
   if (dir)
      closedir(dir);
}

static void look_at_owned(Uint32 pid)
{
   struct pids game = { .count = 0 };
   struct socks s;
   int i;

   descendants(pid, &game);
   all_socks(&s);
   find_owners(&s, &game);
   owned.pid = pid;
   owned.at = SDL_GetTicks();
   owned.count = 0;
   for (i = 0; i < s.count; i++)
      if (s.list[i].pid && (s.list[i].udp || s.list[i].listens))
         own_port(s.list[i].port, s.list[i].udp);
   SDL_free(s.list);
}

int lan_ports(Uint32 pid, bool own, struct tunnel_port *ports, int count, int max)
{
   struct pids game = { .count = 0 }, servers;
   struct socks s;
   int i, k;

   descendants(pid, &game);
   all_socks(&s);
   find_owners(&s, &game);
   find_others(&s);
   servers = game;
   if (!own)
      servers.count = 0;
   /* Connected to a server here: on 127.0.0.1, or by this computer's own
    * address. */
   for (i = 0; i < s.count; i++)
   {
      const struct sock *c = &s.list[i];
      Uint32 owner = 0;

      if (c->udp || !c->connected || !c->pid || !has(&game, c->pid)
            || (c->remote >> 24 != 127 && c->remote != c->local))
         continue;
      for (k = 0; k < s.count && !owner; k++)
         if (s.list[k].listens && s.list[k].port == c->remote_port)
            owner = s.list[k].pid;
      if (!owner || (has(&game, owner) ? !own : no_server(owner)))
         continue;
      count = add_port(ports, count, max, c->remote_port, false);
      add_pid(&servers, owner);
   }
   /* Not packets' that are for one other end only: those ask servers elsewhere. */
   for (i = 0; i < s.count; i++)
      if (s.list[i].pid && has(&servers, s.list[i].pid)
            && (s.list[i].listens || (s.list[i].udp && !s.list[i].connected)))
         count = add_port(ports, count, max, s.list[i].port, s.list[i].udp);
   SDL_free(s.list);
   return count;
}

#endif

int lan_processes(Uint32 pid, Uint32 *pids, int max)
{
   struct pids p = { .count = 0 };
   int i;

   descendants(pid, &p);
   for (i = 0; i < p.count && i < max; i++)
      pids[i] = p.list[i];
   return i;
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
