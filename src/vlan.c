#ifdef _WIN32

#include "vlan.h"
#include <SDL3/SDL.h>

/* Players' ports at once. */
#define PORTS 512
/* One nobody asked for goes when it's been quiet this long (ms). */
#define IDLE_MS 120000
/* A packet's message: the sender's port, the receiver's, then the data. */
#define PACKET_HEAD 4
#define PACKET 65535

/* A player's port, here. */
struct place
{
   bool live, udp;
   /* The game asked for it. */
   bool stays;
   int player;
   Uint16 port;
   SOCKET s;
   Uint64 used;
};

struct vlan
{
   SDL_Mutex *lock;
   Uint32 net;
   int me;
   send_message send;
   void *send_user, *user;
   bool (*owns)(void *user, Uint16 port, bool udp);
   Uint64 players[LANHOOK_PLAYERS];
   struct place places[PORTS];
   struct streams streams;
   char buf[PACKET_HEAD + PACKET];
};

static int player_of(const struct vlan *v, Uint64 id)
{
   int i;

   for (i = 1; i < LANHOOK_PLAYERS && id; i++)
      if (v->players[i] == id)
         return i;
   return 0;
}

/* The game's port, from that player's address. */
static bool to_game(void *user, Uint64 peer, Uint16 port, struct sockaddr_in *to,
      struct sockaddr_in *from)
{
   struct vlan *v = user;
   int player = player_of(v, peer);

   if (!player || !v->owns(v->user, port, false))
      return false;
   socket_address(to, v->net + (Uint32)v->me, port);
   socket_address(from, v->net + (Uint32)player, 0);
   return true;
}

static bool member(void *user, Uint64 peer)
{
   return player_of(user, peer) != 0;
}

struct vlan *vlan_create(Uint32 net, int me, send_message send, void *send_user,
      bool (*owns)(void *user, Uint16 port, bool udp), void *user)
{
   struct vlan *v = SDL_calloc(1, sizeof(*v));
   WSADATA wsa;

   if (!v)
      return NULL;
   if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0 || !(v->lock = SDL_CreateMutex()))
   {
      SDL_free(v);
      return NULL;
   }
   v->net = net;
   v->me = me;
   v->send = send;
   v->send_user = send_user;
   v->owns = owns;
   v->user = user;
   streams_init(&v->streams, VLAN_STREAMS, send, send_user, to_game, member, v);
   return v;
}

static void end_place(struct place *p)
{
   if (p->live)
      closesocket(p->s);
   p->live = false;
}

void vlan_destroy(struct vlan *v)
{
   int i;

   if (!v)
      return;
   streams_end(&v->streams);
   for (i = 0; i < PORTS; i++)
      end_place(&v->places[i]);
   SDL_DestroyMutex(v->lock);
   SDL_free(v);
   WSACleanup();
}

void vlan_set_players(struct vlan *v, const Uint64 ids[LANHOOK_PLAYERS])
{
   int i;

   SDL_LockMutex(v->lock);
   for (i = 0; i < PORTS; i++)
      if (v->places[i].live && v->players[v->places[i].player] != ids[v->places[i].player])
         end_place(&v->places[i]);
   SDL_memcpy(v->players, ids, sizeof(v->players));
   SDL_UnlockMutex(v->lock);
}

/* Under the lock. */
static struct place *find_place(struct vlan *v, int player, Uint16 port, bool udp)
{
   struct place *p, *spare = NULL;
   int i;

   for (i = 0; i < PORTS; i++)
   {
      p = &v->places[i];
      if (p->live && p->player == player && p->port == port && p->udp == udp)
         return p;
      /* A free one, else the one that's been quiet longest. */
      if (!spare || (spare->live && (!p->live || (!p->stays && (spare->stays || p->used < spare->used)))))
         spare = p;
   }
   if (player <= 0 || player >= LANHOOK_PLAYERS || !v->players[player] || !port
         || (spare->live && spare->stays))
      return NULL;
   end_place(spare);
   spare->s = udp ? udp_socket(v->net + (Uint32)player, port)
      : tcp_listener(v->net + (Uint32)player, port);
   if (spare->s == INVALID_SOCKET)
      return NULL;
   spare->live = true;
   spare->udp = udp;
   spare->stays = false;
   spare->player = player;
   spare->port = port;
   spare->used = SDL_GetTicks();
   return spare;
}

bool vlan_open(struct vlan *v, int player, Uint16 port, bool udp)
{
   struct place *p;

   SDL_LockMutex(v->lock);
   if ((p = find_place(v, player, port, udp)))
      p->stays = true;
   SDL_UnlockMutex(v->lock);
   return p != NULL;
}

void vlan_receive(struct vlan *v, Uint64 from, int channel, const void *data, int size)
{
   const char *d = data;
   struct sockaddr_in to;
   struct place *p;
   int player;

   SDL_LockMutex(v->lock);
   if (!(player = player_of(v, from)))
      ;
   else if (channel == VLAN_STREAMS)
      streams_receive(&v->streams, from, d, size);
   else if (channel == VLAN_PACKETS && size >= PACKET_HEAD && v->owns(v->user, get16(d + 2), true)
         && (p = find_place(v, player, get16(d), true)))
   {
      p->used = SDL_GetTicks();
      socket_address(&to, v->net + (Uint32)v->me, get16(d + 2));
      sendto(p->s, d + PACKET_HEAD, size - PACKET_HEAD, 0, (struct sockaddr *)&to, sizeof(to));
   }
   SDL_UnlockMutex(v->lock);
}

void vlan_run(struct vlan *v, int wait_ms)
{
   struct timeval tv = { 0, wait_ms * 1000 };
   fd_set set;
   Uint64 now;
   int i, n;

   if (wait_ms)
   {
      SDL_LockMutex(v->lock);
      FD_ZERO(&set);
      for (i = 0; i < PORTS; i++)
         if (v->places[i].live)
            FD_SET(v->places[i].s, &set);
      streams_wait(&v->streams, &set);
      SDL_UnlockMutex(v->lock);
      if (set.fd_count)
         select(0, &set, NULL, NULL, &tv);
      else
         SDL_Delay((Uint32)wait_ms);
   }
   SDL_LockMutex(v->lock);
   now = SDL_GetTicks();
   for (i = 0; i < PORTS; i++)
   {
      struct place *p = &v->places[i];
      struct sockaddr_in from;
      int len = sizeof(from);
      SOCKET s;

      if (!p->live)
         continue;
      if (!p->udp)
         while ((s = accept(p->s, NULL, NULL)) != INVALID_SOCKET)
            streams_open(&v->streams, v->players[p->player], p->port, s);
      else
         while ((n = recvfrom(p->s, v->buf + PACKET_HEAD, PACKET, 0, (struct sockaddr *)&from, &len)) >= 0)
         {
            put16(v->buf, ntohs(from.sin_port));
            put16(v->buf + 2, p->port);
            v->send(v->send_user, v->players[p->player], VLAN_PACKETS, v->buf, PACKET_HEAD + n);
            p->used = now;
            len = sizeof(from);
         }
      if (!p->stays && now - p->used > IDLE_MS)
         end_place(p);
   }
   streams_run(&v->streams);
   SDL_UnlockMutex(v->lock);
}

#endif
