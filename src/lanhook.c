/* fuseboxlan.dll: in a game while it's played with friends (session.h). The game
 * is on a LAN then whose players are addresses of this computer's own (127.77.0.x),
 * where FuseBox stands in for them (vlan.h): what's sent to one, and what comes
 * from one, needs nothing done. Here: what a game sends to everybody (by broadcast
 * or multicast) is sent to each of them, the game is told its address there, and
 * FuseBox is asked to have a player's port ready before it's used. Windows' own
 * functions, so it's the same for every game. Does nothing while there's no LAN. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "MinHook.h"
#include "lanhook.h"

/* How long FuseBox may take to answer (ms). */
#define ASK_MS 500

static struct lanhook *shared;
static WCHAR pipe_name[64];

/* This computer's real addresses, and those everybody on its networks gets. */
static uint32_t own[32], all[32];
static int own_count, all_count;

static int (WSAAPI *real_sendto)(SOCKET, const char *, int, int, const struct sockaddr *, int);
static int (WSAAPI *real_wsasendto)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const struct sockaddr *,
      int, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
static int (WSAAPI *real_connect)(SOCKET, const struct sockaddr *, int);
static int (WSAAPI *real_wsaconnect)(SOCKET, const struct sockaddr *, int, LPWSABUF, LPWSABUF, LPQOS,
      LPQOS);
static BOOL (PASCAL *real_connectex)(SOCKET, const struct sockaddr *, int, PVOID, DWORD, LPDWORD,
      LPOVERLAPPED);
static int (WSAAPI *real_bind)(SOCKET, const struct sockaddr *, int);
static ULONG (WINAPI *real_adapters)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);
static DWORD (WINAPI *real_adapters_info)(PIP_ADAPTER_INFO, PULONG);
static DWORD (WINAPI *real_table)(PMIB_IPADDRTABLE, PULONG, BOOL);
static struct hostent *(WSAAPI *real_hostbyname)(const char *);
static INT (WSAAPI *real_addrinfo)(PCSTR, PCSTR, const ADDRINFOA *, PADDRINFOA *);
static INT (WSAAPI *real_addrinfow)(PCWSTR, PCWSTR, const ADDRINFOW *, PADDRINFOW *);

static bool on(void)
{
   return shared->active != 0;
}

static uint32_t net(void)
{
   return (uint32_t)shared->net;
}

static uint32_t my_address(void)
{
   return net() + (uint32_t)shared->me;
}

static bool is_own(uint32_t address)
{
   int i;

   for (i = 0; i < own_count; i++)
      if (own[i] == address)
         return true;
   return false;
}

/* The player with that address, 0 for none. */
static int player_at(uint32_t address)
{
   uint32_t n = address - net();

   return n > 0 && n < LANHOOK_PLAYERS && (shared->players >> n & 1) ? (int)n : 0;
}

/* What everybody gets. Not what finds routers and printers. */
static bool is_everybody(uint32_t address)
{
   int i;

   if (address == INADDR_BROADCAST || address == net() + 255)
      return true;
   for (i = 0; i < all_count; i++)
      if (all[i] == address)
         return true;
   return address >> 28 == 0xE && address != 0xEFFFFFFAu && address != 0xE00000FBu
      && address != 0xE00000FCu;
}

static const struct sockaddr_in *ipv4(const struct sockaddr *a, int len)
{
   return a && len >= (int)sizeof(struct sockaddr_in) && a->sa_family == AF_INET
      ? (const struct sockaddr_in *)a : NULL;
}

/* ---- FuseBox ------------------------------------------------------------- */

/* What's been asked for: those stay. */
static SRWLOCK asked_lock = SRWLOCK_INIT;
static uint32_t asked[1024];
static int asked_count;

static void ask(bool udp, int player, uint16_t port)
{
   uint32_t key = (udp ? 1u << 31 : 0) | (uint32_t)player << 16 | port;
   struct lanhook_ask q = { (BYTE)udp, (BYTE)player, port };
   BYTE answer = 0;
   DWORD got = 0;
   int i, error = WSAGetLastError();
   bool known = false;

   AcquireSRWLockShared(&asked_lock);
   for (i = 0; i < asked_count && !known; i++)
      known = asked[i] == key;
   ReleaseSRWLockShared(&asked_lock);
   if (!known && CallNamedPipeW(pipe_name, &q, sizeof(q), &answer, 1, &got, ASK_MS) && answer)
   {
      AcquireSRWLockExclusive(&asked_lock);
      if (asked_count < (int)ARRAYSIZE(asked))
         asked[asked_count++] = key;
      ReleaseSRWLockExclusive(&asked_lock);
   }
   WSASetLastError(error);
}

static bool is_udp(SOCKET s)
{
   int type = 0, len = sizeof(type);

   return getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&type, &len) == 0 && type == SOCK_DGRAM;
}

/* Before something goes to that address. */
static void before(SOCKET s, const struct sockaddr_in *to)
{
   int player = to ? player_at(ntohl(to->sin_addr.s_addr)) : 0;

   if (player && to->sin_port)
      ask(is_udp(s), player, ntohs(to->sin_port));
}

static struct sockaddr_in at_player(int player, uint16_t port)
{
   struct sockaddr_in a;

   ZeroMemory(&a, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = htonl(net() + (uint32_t)player);
   a.sin_port = htons(port);
   return a;
}

/* ---- Sending ------------------------------------------------------------- */

static int WSAAPI my_sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to,
      int tolen)
{
   const struct sockaddr_in *a = on() ? ipv4(to, tolen) : NULL;
   int i, sent = 0;

   if (a && is_everybody(ntohl(a->sin_addr.s_addr)))
   {
      for (i = 1; i < LANHOOK_PLAYERS; i++)
         if (shared->players >> i & 1)
         {
            struct sockaddr_in p = at_player(i, ntohs(a->sin_port));

            ask(true, i, ntohs(a->sin_port));
            if (real_sendto(s, buf, len, flags, (struct sockaddr *)&p, sizeof(p)) != SOCKET_ERROR)
               sent++;
         }
      if (sent)
         return len;
   }
   before(s, a);
   return real_sendto(s, buf, len, flags, to, tolen);
}

static int WSAAPI my_wsasendto(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD bytes, DWORD flags,
      const struct sockaddr *to, int tolen, LPWSAOVERLAPPED over,
      LPWSAOVERLAPPED_COMPLETION_ROUTINE done)
{
   const struct sockaddr_in *a = on() ? ipv4(to, tolen) : NULL;

   if (a && is_everybody(ntohl(a->sin_addr.s_addr)))
   {
      struct sockaddr_in p;
      int i, last = 0;

      for (i = 1; i < LANHOOK_PLAYERS; i++)
      {
         DWORD n;

         if (!(shared->players >> i & 1))
            continue;
         ask(true, i, ntohs(a->sin_port));
         if (last)
         {
            p = at_player(last, ntohs(a->sin_port));
            real_wsasendto(s, bufs, count, &n, flags, (struct sockaddr *)&p, sizeof(p), NULL, NULL);
         }
         last = i;
      }
      /* The one whose end the game waits for. */
      if (last)
      {
         p = at_player(last, ntohs(a->sin_port));
         return real_wsasendto(s, bufs, count, bytes, flags, (struct sockaddr *)&p, sizeof(p), over,
               done);
      }
   }
   before(s, a);
   return real_wsasendto(s, bufs, count, bytes, flags, to, tolen, over, done);
}

static int WSAAPI my_connect(SOCKET s, const struct sockaddr *to, int tolen)
{
   if (on())
      before(s, ipv4(to, tolen));
   return real_connect(s, to, tolen);
}

static int WSAAPI my_wsaconnect(SOCKET s, const struct sockaddr *to, int tolen, LPWSABUF a, LPWSABUF b,
      LPQOS c, LPQOS d)
{
   if (on())
      before(s, ipv4(to, tolen));
   return real_wsaconnect(s, to, tolen, a, b, c, d);
}

static BOOL PASCAL my_connectex(SOCKET s, const struct sockaddr *to, int tolen, PVOID buf, DWORD len,
      LPDWORD sent, LPOVERLAPPED over)
{
   if (on())
      before(s, ipv4(to, tolen));
   return real_connectex(s, to, tolen, buf, len, sent, over);
}

/* One at a real address reaches no other player: at any, then. */
static int WSAAPI my_bind(SOCKET s, const struct sockaddr *at, int len)
{
   const struct sockaddr_in *a = on() ? ipv4(at, len) : NULL;

   if (a && is_own(ntohl(a->sin_addr.s_addr)))
   {
      struct sockaddr_in any = *a;

      any.sin_addr.s_addr = htonl(INADDR_ANY);
      return real_bind(s, (struct sockaddr *)&any, sizeof(any));
   }
   return real_bind(s, at, len);
}

/* ---- The game's address -------------------------------------------------- */

static ULONG WINAPI my_adapters(ULONG family, ULONG flags, PVOID reserved, PIP_ADAPTER_ADDRESSES list,
      PULONG size)
{
   ULONG r = real_adapters(family, flags, reserved, list, size);
   PIP_ADAPTER_ADDRESSES a;
   PIP_ADAPTER_UNICAST_ADDRESS u;

   if (r != NO_ERROR || !on())
      return r;
   for (a = list; a; a = a->Next)
   {
      if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
         continue;
      for (u = a->FirstUnicastAddress; u; u = u->Next)
         if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET)
         {
            ((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr = htonl(my_address());
            u->OnLinkPrefixLength = 24;
         }
   }
   return r;
}

static void write_address(char *dst, uint32_t a)
{
   wsprintfA(dst, "%u.%u.%u.%u", a >> 24, a >> 16 & 255, a >> 8 & 255, a & 255);
}

static DWORD WINAPI my_adapters_info(PIP_ADAPTER_INFO list, PULONG size)
{
   DWORD r = real_adapters_info(list, size);

   if (r == NO_ERROR && on())
      for (; list; list = list->Next)
      {
         if (lstrcmpA(list->IpAddressList.IpAddress.String, "0.0.0.0") == 0)
            continue;
         write_address(list->IpAddressList.IpAddress.String, my_address());
         write_address(list->IpAddressList.IpMask.String, 0xFFFFFF00u);
      }
   return r;
}

static DWORD WINAPI my_table(PMIB_IPADDRTABLE t, PULONG size, BOOL order)
{
   DWORD i, r = real_table(t, size, order);

   if (r == NO_ERROR && t && on())
      for (i = 0; i < t->dwNumEntries; i++)
         if (is_own(ntohl(t->table[i].dwAddr)))
         {
            t->table[i].dwAddr = htonl(my_address());
            t->table[i].dwMask = htonl(0xFFFFFF00u);
         }
   return r;
}

/* This computer, by its name. */
static struct hostent *WSAAPI my_hostbyname(const char *name)
{
   struct hostent *h = real_hostbyname(name);
   int i;

   if (h && h->h_addrtype == AF_INET && on())
      for (i = 0; h->h_addr_list[i]; i++)
         if (is_own(ntohl(*(u_long *)h->h_addr_list[i])))
            *(u_long *)h->h_addr_list[i] = htonl(my_address());
   return h;
}

static void my_result(struct sockaddr *a)
{
   struct sockaddr_in *in = (struct sockaddr_in *)a;

   if (a && a->sa_family == AF_INET && is_own(ntohl(in->sin_addr.s_addr)))
      in->sin_addr.s_addr = htonl(my_address());
}

static INT WSAAPI my_addrinfo(PCSTR node, PCSTR service, const ADDRINFOA *hints, PADDRINFOA *result)
{
   INT r = real_addrinfo(node, service, hints, result);
   ADDRINFOA *i;

   if (r == 0 && on())
      for (i = *result; i; i = i->ai_next)
         my_result(i->ai_addr);
   return r;
}

static INT WSAAPI my_addrinfow(PCWSTR node, PCWSTR service, const ADDRINFOW *hints, PADDRINFOW *result)
{
   INT r = real_addrinfow(node, service, hints, result);
   ADDRINFOW *i;

   if (r == 0 && on())
      for (i = *result; i; i = i->ai_next)
         my_result(i->ai_addr);
   return r;
}

/* ---- Setting up ---------------------------------------------------------- */

static void read_addresses(void)
{
   ULONG size = 64 * 1024;
   IP_ADAPTER_ADDRESSES *list = HeapAlloc(GetProcessHeap(), 0, size), *a;
   IP_ADAPTER_UNICAST_ADDRESS *u;

   if (list && GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST
            | GAA_FLAG_SKIP_DNS_SERVER, NULL, list, &size) == NO_ERROR)
      for (a = list; a; a = a->Next)
         for (u = a->FirstUnicastAddress; u && a->IfType != IF_TYPE_SOFTWARE_LOOPBACK; u = u->Next)
         {
            uint32_t ip = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);

            if (own_count < (int)ARRAYSIZE(own))
               own[own_count++] = ip;
            if (all_count < (int)ARRAYSIZE(all) && u->OnLinkPrefixLength > 0
                  && u->OnLinkPrefixLength < 31)
               all[all_count++] = ip | (0xFFFFFFFFu >> u->OnLinkPrefixLength);
         }
   HeapFree(GetProcessHeap(), 0, list);
}

/* ConnectEx isn't exported: Windows tells where it is. */
static void *find_connectex(void)
{
   GUID id = WSAID_CONNECTEX;
   void *f = NULL;
   DWORD n;
   SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

   if (s == INVALID_SOCKET)
      return NULL;
   WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof(id), &f, sizeof(f), &n, NULL, NULL);
   closesocket(s);
   return f;
}

static DWORD WINAPI set_up(void *user)
{
   static const struct
   {
      const WCHAR *dll;
      const char *name;
      void *mine;
      void **real;
   } hooks[] = {
      { L"ws2_32", "sendto", my_sendto, (void **)&real_sendto },
      { L"ws2_32", "WSASendTo", my_wsasendto, (void **)&real_wsasendto },
      { L"ws2_32", "connect", my_connect, (void **)&real_connect },
      { L"ws2_32", "WSAConnect", my_wsaconnect, (void **)&real_wsaconnect },
      { L"ws2_32", "bind", my_bind, (void **)&real_bind },
      { L"ws2_32", "gethostbyname", my_hostbyname, (void **)&real_hostbyname },
      { L"ws2_32", "getaddrinfo", my_addrinfo, (void **)&real_addrinfo },
      { L"ws2_32", "GetAddrInfoW", my_addrinfow, (void **)&real_addrinfow },
      { L"iphlpapi", "GetAdaptersAddresses", my_adapters, (void **)&real_adapters },
      { L"iphlpapi", "GetAdaptersInfo", my_adapters_info, (void **)&real_adapters_info },
      { L"iphlpapi", "GetIpAddrTable", my_table, (void **)&real_table },
   };
   WSADATA wsa;
   void *connectex;
   size_t i;

   (void)user;
   WSAStartup(MAKEWORD(2, 2), &wsa);
   read_addresses();
   connectex = find_connectex();
   if (MH_Initialize() != MH_OK)
      return 1;
   for (i = 0; i < ARRAYSIZE(hooks); i++)
      MH_CreateHookApi(hooks[i].dll, hooks[i].name, hooks[i].mine, hooks[i].real);
   if (connectex)
      MH_CreateHook(connectex, (void *)my_connectex, (void **)&real_connectex);
   if (MH_EnableHook(MH_ALL_HOOKS) == MH_OK)
      InterlockedExchange(&shared->ready, 1);
   return 0;
}

BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, LPVOID reserved)
{
   WCHAR name[64];
   HANDLE memory, thread;

   (void)reserved;
   if (reason != DLL_PROCESS_ATTACH)
      return TRUE;
   DisableThreadLibraryCalls(dll);
   wsprintfW(name, LANHOOK_MEMORY, GetCurrentProcessId());
   wsprintfW(pipe_name, LANHOOK_PIPE, GetCurrentProcessId());
   /* Only where FuseBox wants it. */
   if (!(memory = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name))
         || !(shared = MapViewOfFile(memory, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*shared)))
         || shared->version != LANHOOK_VERSION)
      return FALSE;
   /* Not here: other threads are stopped for it. */
   if (!(thread = CreateThread(NULL, 0, set_up, NULL, 0, NULL)))
      return FALSE;
   CloseHandle(thread);
   return TRUE;
}
