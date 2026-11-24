#ifndef FUSEBOX_STREAM_H
#define FUSEBOX_STREAM_H

#include "sockets.h"

#include <stdbool.h>
#include <SDL3/SDL_stdinc.h>

/* TCP connections carried over messages that arrive in order: for tunnel.c and
 * vlan.c. Either side opens them. One thread. */

/* At once; a piece of one's data. */
#define STREAMS 128
#define STREAM_PIECE 16384
#define STREAM_HEAD 3

enum sent
{
   SENT_FAILED,
   SENT_OK,
   /* No room for it now: again later. */
   SENT_FULL
};

typedef enum sent (*send_message)(void *user, Uint64 to, int channel, const void *data, int size);
/* Where an opened connection goes, and from which address (port 0); false:
 * nowhere. */
typedef bool (*stream_target)(void *user, Uint64 peer, Uint16 port, struct sockaddr_in *to,
      struct sockaddr_in *from);
typedef bool (*stream_member)(void *user, Uint64 peer);

struct stream
{
   bool live;
   Uint64 peer;
   Uint16 num, port;
   /* This side opened it. */
   bool mine;
   /* INVALID_SOCKET: none yet. */
   SOCKET s;
   bool connecting;
   /* The other end closed it: once what came before that is written. */
   bool closing;
   /* Ended here: the other end is still to hear of it. */
   bool telling;
   Uint64 opened, retry_at;
   /* From the other end, not written here yet. */
   char *out;
   size_t out_len, out_alloc;
   /* For the other end, which had no room for it: one message. */
   char held[STREAM_HEAD + STREAM_PIECE];
   int held_len;
};

struct streams
{
   struct stream list[STREAMS];
   Uint16 next;
   int channel;
   send_message send;
   stream_target target;
   stream_member member;
   void *send_user, *user;
   char buf[STREAM_HEAD + STREAM_PIECE];
};

void streams_init(struct streams *t, int channel, send_message send, void *send_user,
      stream_target target, stream_member member, void *user);
void streams_end(struct streams *t);
/* A connection made here, to that port of the peer's; s is its own from now on. */
void streams_open(struct streams *t, Uint64 peer, Uint16 port, SOCKET s);
void streams_receive(struct streams *t, Uint64 from, const char *data, int size);
/* Those on a port for which this is false end. */
void streams_keep(struct streams *t, bool (*keep)(void *user, Uint16 port), void *user);
/* Those worth waiting for. */
void streams_wait(const struct streams *t, struct socket_set *set);
void streams_run(struct streams *t);
int streams_active(const struct streams *t);

Uint16 get16(const void *p);
void put16(void *p, Uint16 v);

#endif
