#include "tunnel.h"
#include "stream.h"
#include <SDL3/SDL.h>

/* Senders of packets at once; members of a session. */
#define FLOWS 128
#define MEMBERS 16
#define PACKET 65535
/* A sender not heard from this long is gone (ms). */
#define IDLE_MS 60000
/* A packet's message: the port, the sender's number (the guest counts them), then
 * the data; numbers high byte first. */
#define PACKET_HEAD 4

/* A sender of packets: a guest's program (from: where), or what stands for it
 * at the host (s). */
struct flow
{
   bool live;
   Uint64 peer;
   Uint16 num, port;
   SOCKET s;
   struct sockaddr_in from;
   Uint64 heard;
};

struct tunnel
{
   bool host;
   Uint64 peer;
   send_message send;
   void *user;
   struct tunnel_port ports[TUNNEL_PORTS];
   /* A guest's: listening, or taking packets. */
   SOCKET open[TUNNEL_PORTS];
   int port_count;
   Uint64 members[MEMBERS];
   int member_count;
   struct streams streams;
   struct flow flows[FLOWS];
   Uint16 next_flow;
   char buf[PACKET_HEAD + PACKET];
};

static int find_port(const struct tunnel_port *ports, int count, Uint16 port, bool udp)
{
   int i;

   for (i = 0; i < count; i++)
      if (ports[i].port == port && ports[i].udp == udp)
         return i;
   return -1;
}

static bool is_member(const struct tunnel *t, Uint64 id)
{
   int i;

   if (!t->host)
      return id == t->peer;
   for (i = 0; i < t->member_count; i++)
      if (t->members[i] == id)
         return true;
   return false;
}

/* The host's server. */
static bool to_server(void *user, Uint64 peer, Uint16 port, struct sockaddr_in *to,
      struct sockaddr_in *from)
{
   struct tunnel *t = user;

   if (!t->host || find_port(t->ports, t->port_count, port, false) < 0)
      return false;
   socket_address(to, INADDR_LOOPBACK, port);
   return true;
}

static bool member(void *user, Uint64 peer)
{
   return is_member(user, peer);
}

static bool has_port(void *user, Uint16 port)
{
   struct tunnel *t = user;

   return find_port(t->ports, t->port_count, port, false) >= 0;
}

/* ---- Packets ------------------------------------------------------------- */

static void end_flow(struct flow *f)
{
   if (f->s != INVALID_SOCKET)
      closesocket(f->s);
   f->s = INVALID_SOCKET;
   f->live = false;
}

static struct flow *new_flow(struct tunnel *t, Uint64 peer, Uint16 num, Uint16 port)
{
   int i;

   for (i = 0; i < FLOWS; i++)
   {
      struct flow *f = &t->flows[i];

      if (f->live)
         continue;
      SDL_zerop(f);
      f->live = true;
      f->peer = peer;
      f->num = num;
      f->port = port;
      f->s = INVALID_SOCKET;
      f->heard = SDL_GetTicks();
      return f;
   }
   return NULL;
}

static void packet_message(struct tunnel *t, Uint64 from, const char *data, int size)
{
   struct flow *f = NULL;
   Uint16 port, num;
   int i;

   if (size < PACKET_HEAD)
      return;
   port = get16(data);
   num = get16(data + 2);
   if ((i = find_port(t->ports, t->port_count, port, true)) < 0)
      return;
   for (int k = 0; k < FLOWS && !f; k++)
      if (t->flows[k].live && t->flows[k].peer == from && t->flows[k].num == num
            && t->flows[k].port == port)
         f = &t->flows[k];
   if (!t->host)
   {
      if (f && t->open[i] != INVALID_SOCKET)
         sendto(t->open[i], data + PACKET_HEAD, size - PACKET_HEAD, 0, (struct sockaddr *)&f->from,
               sizeof(f->from));
      return;
   }
   if (!f)
   {
      struct sockaddr_in a;

      if (!(f = new_flow(t, from, num, port)))
         return;
      /* The server sees each sender as its own. */
      socket_address(&a, INADDR_LOOPBACK, port);
      if ((f->s = udp_socket(INADDR_LOOPBACK, 0)) == INVALID_SOCKET
            || connect(f->s, (struct sockaddr *)&a, sizeof(a)) != 0)
      {
         end_flow(f);
         return;
      }
   }
   f->heard = SDL_GetTicks();
   send(f->s, data + PACKET_HEAD, size - PACKET_HEAD, SEND_FLAGS);
}

static void send_packet(struct tunnel *t, struct flow *f, int n)
{
   put16(t->buf, f->port);
   put16(t->buf + 2, f->num);
   t->send(t->user, f->peer, TUNNEL_PACKETS, t->buf, PACKET_HEAD + n);
   f->heard = SDL_GetTicks();
}

/* A guest's programs' packets to a port here. */
static void take_packets(struct tunnel *t, int i)
{
   struct sockaddr_in from;
   socklen_t len = sizeof(from);
   int n, k;

   while ((n = recvfrom(t->open[i], t->buf + PACKET_HEAD, PACKET, 0, (struct sockaddr *)&from,
               &len)) > 0)
   {
      struct flow *f = NULL;

      for (k = 0; k < FLOWS && !f; k++)
         if (t->flows[k].live && t->flows[k].port == t->ports[i].port
               && t->flows[k].from.sin_port == from.sin_port)
            f = &t->flows[k];
      if (!f && (f = new_flow(t, t->peer, ++t->next_flow, t->ports[i].port)))
         f->from = from;
      if (f)
         send_packet(t, f, n);
      len = sizeof(from);
   }
}

/* ---- The tunnel ---------------------------------------------------------- */

struct tunnel *tunnel_create(Uint64 peer, send_message send, void *user)
{
   struct tunnel *t = SDL_calloc(1, sizeof(*t));
   int i;

   if (!t)
      return NULL;
   if (!sockets_start())
   {
      SDL_free(t);
      return NULL;
   }
   t->host = !peer;
   t->peer = peer;
   t->send = send;
   t->user = user;
   streams_init(&t->streams, TUNNEL_STREAMS, send, user, to_server, member, t);
   for (i = 0; i < FLOWS; i++)
      t->flows[i].s = INVALID_SOCKET;
   for (i = 0; i < TUNNEL_PORTS; i++)
      t->open[i] = INVALID_SOCKET;
   return t;
}

void tunnel_destroy(struct tunnel *t)
{
   int i;

   if (!t)
      return;
   streams_end(&t->streams);
   for (i = 0; i < FLOWS; i++)
      end_flow(&t->flows[i]);
   for (i = 0; i < TUNNEL_PORTS; i++)
      if (t->open[i] != INVALID_SOCKET)
         closesocket(t->open[i]);
   SDL_free(t);
   sockets_stop();
}

int tunnel_set_ports(struct tunnel *t, const struct tunnel_port *ports, int count)
{
   struct tunnel_port had[TUNNEL_PORTS];
   SOCKET open[TUNNEL_PORTS];
   int i, k, had_count = t->port_count, failed = 0;

   SDL_memcpy(had, t->ports, sizeof(had));
   SDL_memcpy(open, t->open, sizeof(open));
   for (i = 0; i < TUNNEL_PORTS; i++)
      t->open[i] = INVALID_SOCKET;
   t->port_count = 0;
   for (i = 0; i < count && t->port_count < TUNNEL_PORTS; i++)
   {
      SOCKET s = INVALID_SOCKET;

      if (find_port(t->ports, t->port_count, ports[i].port, ports[i].udp) >= 0)
         continue;
      if (!t->host)
      {
         if ((k = find_port(had, had_count, ports[i].port, ports[i].udp)) >= 0)
         {
            s = open[k];
            open[k] = INVALID_SOCKET;
         }
         else
            s = ports[i].udp ? udp_socket(INADDR_LOOPBACK, ports[i].port)
               : tcp_listener(INADDR_LOOPBACK, ports[i].port);
         if (s == INVALID_SOCKET)
         {
            failed++;
            continue;
         }
      }
      t->ports[t->port_count] = ports[i];
      t->open[t->port_count++] = s;
   }
   for (i = 0; i < had_count; i++)
      if (open[i] != INVALID_SOCKET)
         closesocket(open[i]);
   streams_keep(&t->streams, has_port, t);
   for (i = 0; i < FLOWS; i++)
      if (t->flows[i].live && find_port(t->ports, t->port_count, t->flows[i].port, true) < 0)
         end_flow(&t->flows[i]);
   return failed;
}

void tunnel_set_members(struct tunnel *t, const Uint64 *ids, int count)
{
   t->member_count = SDL_min(count, MEMBERS);
   SDL_memcpy(t->members, ids, sizeof(*ids) * (size_t)t->member_count);
}

void tunnel_receive(struct tunnel *t, Uint64 from, int channel, const void *data, int size)
{
   if (!is_member(t, from))
      return;
   if (channel == TUNNEL_STREAMS)
      streams_receive(&t->streams, from, data, size);
   else if (channel == TUNNEL_PACKETS)
      packet_message(t, from, data, size);
}

void tunnel_run(struct tunnel *t, int wait_ms)
{
   struct socket_set set;
   Uint64 now;
   SOCKET s;
   int i, n;

   set.count = 0;
   for (i = 0; i < t->port_count; i++)
      if (t->open[i] != INVALID_SOCKET)
         socket_set_add(&set, t->open[i]);
   streams_wait(&t->streams, &set);
   for (i = 0; i < FLOWS; i++)
      if (t->flows[i].live && t->flows[i].s != INVALID_SOCKET)
         socket_set_add(&set, t->flows[i].s);
   if (wait_ms)
      socket_set_wait(&set, wait_ms);

   for (i = 0; i < t->port_count; i++)
      if (t->open[i] == INVALID_SOCKET)
         ;
      else if (t->ports[i].udp)
         take_packets(t, i);
      else
         while ((s = accept(t->open[i], NULL, NULL)) != INVALID_SOCKET)
            streams_open(&t->streams, t->peer, t->ports[i].port, s);
   now = SDL_GetTicks();
   streams_run(&t->streams);
   for (i = 0; i < FLOWS; i++)
   {
      struct flow *f = &t->flows[i];

      if (!f->live)
         continue;
      if (now - f->heard > IDLE_MS || !is_member(t, f->peer))
         end_flow(f);
      else if (f->s != INVALID_SOCKET)
         while ((n = recv(f->s, t->buf + PACKET_HEAD, PACKET, 0)) > 0)
            send_packet(t, f, n);
   }
}

int tunnel_active(const struct tunnel *t)
{
   int i, n = streams_active(&t->streams);

   for (i = 0; i < FLOWS; i++)
      n += t->flows[i].live;
   return n;
}
