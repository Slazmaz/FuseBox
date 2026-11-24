#ifndef FUSEBOX_SOCKETS_H
#define FUSEBOX_SOCKETS_H

#include <stdbool.h>
#include <SDL3/SDL_stdinc.h>

/* Windows' sockets and everybody else's. */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
/* A connection that's gone must not end the process. */
#define SEND_FLAGS MSG_NOSIGNAL
#endif

/* Windows' have to be started, as often as they're stopped. */
bool sockets_start(void);
void sockets_stop(void);

/* On that address; they don't wait. */
void socket_address(struct sockaddr_in *a, Uint32 address, Uint16 port);
SOCKET udp_socket(Uint32 address, Uint16 port);
SOCKET tcp_listener(Uint32 address, Uint16 port);
SOCKET tcp_socket(void);
void socket_ready(SOCKET s);
/* Whether the last call on one failed only for now. */
bool socket_waits(void);
/* One that's connecting: 1 once it is, -1 if it can't, 0 for not yet. */
int socket_connected(SOCKET s);

#define SOCKET_SET 1024
struct socket_set
{
   SOCKET list[SOCKET_SET];
   int count;
};

void socket_set_add(struct socket_set *set, SOCKET s);
/* Until something comes from one of them, that long at most. */
void socket_set_wait(const struct socket_set *set, int ms);

#endif
