#ifndef _WIN32
#define _DEFAULT_SOURCE
#endif

#include "sockets.h"
#include <SDL3/SDL.h>

#ifdef _WIN32
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#endif

void socket_address(struct sockaddr_in *a, Uint32 address, Uint16 port)
{
   SDL_zerop(a);
   a->sin_family = AF_INET;
   a->sin_addr.s_addr = htonl(address);
   a->sin_port = htons(port);
}

SOCKET tcp_socket(void)
{
   SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

   if (s != INVALID_SOCKET)
      socket_ready(s);
   return s;
}

void socket_set_add(struct socket_set *set, SOCKET s)
{
   if (set->count < SOCKET_SET)
      set->list[set->count++] = s;
}

#ifdef _WIN32

bool sockets_start(void)
{
   WSADATA wsa;

   return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

void sockets_stop(void)
{
   WSACleanup();
}

void socket_ready(SOCKET s)
{
   u_long on = 1;
   BOOL nodelay = TRUE;

   ioctlsocket(s, FIONBIO, &on);
   /* Fails for UDP, which never waits. */
   setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
}

bool socket_waits(void)
{
   return WSAGetLastError() == WSAEWOULDBLOCK;
}

int socket_connected(SOCKET s)
{
   fd_set w, e;
   struct timeval tv = { 0, 0 };

   FD_ZERO(&w);
   FD_ZERO(&e);
   FD_SET(s, &w);
   FD_SET(s, &e);
   if (select(0, NULL, &w, &e, &tv) <= 0)
      return 0;
   return FD_ISSET(s, &w) && !FD_ISSET(s, &e) ? 1 : -1;
}

void socket_set_wait(const struct socket_set *set, int ms)
{
   struct timeval tv = { ms / 1000, ms % 1000 * 1000 };
   fd_set r;
   int i;

   FD_ZERO(&r);
   for (i = 0; i < set->count; i++)
      FD_SET(set->list[i], &r);
   if (set->count)
      select(0, &r, NULL, NULL, &tv);
   else
      SDL_Delay((Uint32)ms);
}

/* Nobody else's port, and nobody's after it. */
static void only_ours(SOCKET s, Uint32 address)
{
   BOOL only = TRUE;

   (void)address;
   setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&only, sizeof(only));
}

/* A port nobody listens on (yet) must not break it. */
static void ignore_unreachable(SOCKET s)
{
   BOOL report = FALSE;
   DWORD bytes;

   WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof(report), NULL, 0, &bytes, NULL, NULL);
}

#else

bool sockets_start(void)
{
   return true;
}

void sockets_stop(void)
{
}

void socket_ready(SOCKET s)
{
   int on = 1;

   fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
   /* Not the games' that are started meanwhile. */
   fcntl(s, F_SETFD, FD_CLOEXEC);
   /* Fails for UDP, which never waits. */
   setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

bool socket_waits(void)
{
   return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR;
}

int socket_connected(SOCKET s)
{
   struct pollfd p = { s, POLLOUT, 0 };
   int error = 0;
   socklen_t len = sizeof(error);

   if (poll(&p, 1, 0) <= 0)
      return 0;
   return getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && !error
      && !(p.revents & (POLLERR | POLLNVAL)) ? 1 : -1;
}

/* select only goes up to 1024, of all the process has open. */
void socket_set_wait(const struct socket_set *set, int ms)
{
   struct pollfd list[SOCKET_SET];
   int i;

   for (i = 0; i < set->count; i++)
   {
      list[i].fd = set->list[i];
      list[i].events = POLLIN;
      list[i].revents = 0;
   }
   poll(list, (nfds_t)set->count, ms);
}

/* Here a port is only ours unless both say otherwise. A game that takes it on all
 * addresses has it on the players' too: those (vlan.h) are shared with it, which
 * it says by lanhook_linux.c; the one for a player's address gets what's sent there. */
static void only_ours(SOCKET s, Uint32 address)
{
   int on = 1;

   if (address != INADDR_LOOPBACK)
      setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
}

/* It doesn't break. */
static void ignore_unreachable(SOCKET s)
{
   (void)s;
}

#endif

SOCKET udp_socket(Uint32 address, Uint16 port)
{
   struct sockaddr_in a;
   SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

   if (s == INVALID_SOCKET)
      return s;
   only_ours(s, address);
   socket_address(&a, address, port);
   if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0)
   {
      closesocket(s);
      return INVALID_SOCKET;
   }
   socket_ready(s);
   ignore_unreachable(s);
   return s;
}

SOCKET tcp_listener(Uint32 address, Uint16 port)
{
   struct sockaddr_in a;
   SOCKET s = tcp_socket();

   if (s == INVALID_SOCKET)
      return s;
   only_ours(s, address);
   socket_address(&a, address, port);
   if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, SOMAXCONN) != 0)
   {
      closesocket(s);
      return INVALID_SOCKET;
   }
   return s;
}
