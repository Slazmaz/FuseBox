#include "stream.h"
#include <SDL3/SDL.h>

/* A server that's just starting is tried this long, this often (ms). */
#define CONNECT_MS 20000
#define RETRY_MS 250
/* What may wait for a program that isn't reading: then it's cut off. */
#define WAITING_MAX (8 * 1024 * 1024)

/* A message: what, the connection's number (the side that opens counts them),
 * then the port to open or the data; numbers high byte first. In small letters
 * from the side that didn't open it: both count from 1. */
#define OPEN 'O'
#define DATA 'D'
#define CLOSE 'C'
#define OTHERS ('a' - 'A')

Uint16 get16(const void *p)
{
   const Uint8 *b = p;

   return (Uint16)(b[0] << 8 | b[1]);
}

void put16(void *p, Uint16 v)
{
   Uint8 *b = p;

   b[0] = (Uint8)(v >> 8);
   b[1] = (Uint8)v;
}

static enum sent send_stream(struct streams *t, const struct stream *st, char what, char *msg, int len)
{
   msg[0] = (char)(st->mine ? what : what + OTHERS);
   put16(msg + 1, st->num);
   return t->send(t->send_user, st->peer, t->channel, msg, STREAM_HEAD + len);
}

/* tell: the other end hears of it. */
static void end_stream(struct streams *t, struct stream *st, bool tell)
{
   char msg[STREAM_HEAD];

   if (st->s != INVALID_SOCKET)
      closesocket(st->s);
   st->s = INVALID_SOCKET;
   SDL_free(st->out);
   st->out = NULL;
   st->out_len = st->out_alloc = 0;
   st->held_len = 0;
   st->connecting = st->closing = false;
   st->telling = tell && st->live && send_stream(t, st, CLOSE, msg, 0) == SENT_FULL;
   st->live = st->telling;
}

static struct stream *find_stream(struct streams *t, Uint64 peer, Uint16 num, bool mine)
{
   int i;

   for (i = 0; i < STREAMS; i++)
      if (t->list[i].live && t->list[i].peer == peer && t->list[i].num == num
            && t->list[i].mine == mine)
         return &t->list[i];
   return NULL;
}

static struct stream *new_stream(struct streams *t, Uint64 peer, Uint16 num, Uint16 port, bool mine)
{
   int i;

   for (i = 0; i < STREAMS; i++)
   {
      struct stream *st = &t->list[i];

      if (st->live)
         continue;
      SDL_zerop(st);
      st->live = true;
      st->peer = peer;
      st->num = num;
      st->port = port;
      st->mine = mine;
      st->s = INVALID_SOCKET;
      st->opened = SDL_GetTicks();
      return st;
   }
   return NULL;
}

static void connect_stream(struct streams *t, struct stream *st)
{
   struct sockaddr_in to, from;

   SDL_zero(from);
   if (!t->target(t->user, st->peer, st->port, &to, &from) || (st->s = tcp_socket()) == INVALID_SOCKET)
      return;
   st->connecting = true;
   if ((from.sin_family == AF_INET && bind(st->s, (struct sockaddr *)&from, sizeof(from)) != 0)
         || (connect(st->s, (struct sockaddr *)&to, sizeof(to)) != 0
            && !socket_waits()))
   {
      closesocket(st->s);
      st->s = INVALID_SOCKET;
   }
}

static void check_connect(struct streams *t, struct stream *st, Uint64 now)
{
   if (st->s != INVALID_SOCKET)
   {
      int connected = socket_connected(st->s);

      if (!connected)
         return;
      if (connected > 0)
      {
         st->connecting = false;
         return;
      }
      closesocket(st->s);
      st->s = INVALID_SOCKET;
      st->retry_at = now + RETRY_MS;
   }
   if (now - st->opened > CONNECT_MS)
      end_stream(t, st, true);
   else if (now >= st->retry_at)
   {
      st->retry_at = now + RETRY_MS;
      connect_stream(t, st);
   }
}

static bool take(struct stream *st, const char *data, size_t len)
{
   if (st->out_len + len > WAITING_MAX)
      return false;
   if (st->out_len + len > st->out_alloc)
   {
      size_t alloc = st->out_alloc ? st->out_alloc : 65536;
      char *grown;

      while (alloc < st->out_len + len)
         alloc *= 2;
      if (!(grown = SDL_realloc(st->out, alloc)))
         return false;
      st->out = grown;
      st->out_alloc = alloc;
   }
   SDL_memcpy(st->out + st->out_len, data, len);
   st->out_len += len;
   return true;
}

void streams_init(struct streams *t, int channel, send_message send, void *send_user,
      stream_target target, stream_member member, void *user)
{
   int i;

   SDL_zerop(t);
   t->channel = channel;
   t->send = send;
   t->send_user = send_user;
   t->target = target;
   t->member = member;
   t->user = user;
   for (i = 0; i < STREAMS; i++)
      t->list[i].s = INVALID_SOCKET;
}

void streams_end(struct streams *t)
{
   int i;

   for (i = 0; i < STREAMS; i++)
      end_stream(t, &t->list[i], true);
}

void streams_open(struct streams *t, Uint64 peer, Uint16 port, SOCKET s)
{
   char msg[STREAM_HEAD + 2];
   struct stream *st;

   while (find_stream(t, peer, ++t->next, true))
      ;
   if (!(st = new_stream(t, peer, t->next, port, true)))
   {
      closesocket(s);
      return;
   }
   socket_ready(s);
   st->s = s;
   put16(msg + STREAM_HEAD, port);
   switch (send_stream(t, st, OPEN, msg, 2))
   {
      case SENT_OK:
         break;
      case SENT_FULL:
         SDL_memcpy(st->held, msg, sizeof(msg));
         st->held_len = sizeof(msg);
         break;
      default:
         end_stream(t, st, false);
         break;
   }
}

void streams_receive(struct streams *t, Uint64 from, const char *data, int size)
{
   struct sockaddr_in to, at;
   struct stream *st;
   bool mine;
   Uint16 num;
   char what;

   if (size < STREAM_HEAD)
      return;
   mine = data[0] >= 'a';
   what = (char)(mine ? data[0] - OTHERS : data[0]);
   num = get16(data + 1);
   st = find_stream(t, from, num, mine);
   if (what == OPEN && !mine)
   {
      if (size < STREAM_HEAD + 2)
         return;
      if (st)
         end_stream(t, st, false);
      if (!(st = new_stream(t, from, num, get16(data + STREAM_HEAD), false)))
      {
         char msg[STREAM_HEAD];
         msg[0] = CLOSE + OTHERS;
         put16(msg + 1, num);
         t->send(t->send_user, from, t->channel, msg, STREAM_HEAD);
      }
      else if (!t->target(t->user, from, st->port, &to, &at))
         end_stream(t, st, true);
      else
         connect_stream(t, st);
   }
   else if (!st || st->closing || st->telling)
      ; /* One that ended. */
   else if (what == DATA)
   {
      if (!take(st, data + STREAM_HEAD, (size_t)(size - STREAM_HEAD)))
         end_stream(t, st, true);
   }
   else if (what == CLOSE)
      st->closing = true;
}

static void move_stream(struct streams *t, struct stream *st)
{
   int n = 0;

   while (st->out_len)
   {
      n = send(st->s, st->out, (int)SDL_min(st->out_len, 1 << 20), SEND_FLAGS);
      if (n <= 0)
         break;
      SDL_memmove(st->out, st->out + n, st->out_len - (size_t)n);
      st->out_len -= (size_t)n;
   }
   if (st->out_len && !socket_waits())
   {
      end_stream(t, st, true);
      return;
   }
   if (st->closing)
   {
      if (!st->out_len)
         end_stream(t, st, false);
      return;
   }
   /* In order: nothing new until the held one went. */
   if (st->held_len)
   {
      if (t->send(t->send_user, st->peer, t->channel, st->held, st->held_len) == SENT_FULL)
         return;
      st->held_len = 0;
   }
   while ((n = recv(st->s, t->buf + STREAM_HEAD, STREAM_PIECE, 0)) > 0)
   {
      enum sent sent = send_stream(t, st, DATA, t->buf, n);

      if (sent == SENT_FULL)
      {
         st->held_len = STREAM_HEAD + n;
         SDL_memcpy(st->held, t->buf, (size_t)st->held_len);
         return;
      }
      if (sent == SENT_FAILED)
      {
         end_stream(t, st, false);
         return;
      }
   }
   if (n == 0 || !socket_waits())
      end_stream(t, st, true);
}

void streams_keep(struct streams *t, bool (*keep)(void *user, Uint16 port), void *user)
{
   int i;

   for (i = 0; i < STREAMS; i++)
      if (t->list[i].live && !keep(user, t->list[i].port))
         end_stream(t, &t->list[i], true);
}

void streams_wait(const struct streams *t, struct socket_set *set)
{
   int i;

   for (i = 0; i < STREAMS; i++)
      /* One that can't send isn't worth waking up for. */
      if (t->list[i].live && t->list[i].s != INVALID_SOCKET && !t->list[i].connecting
            && !t->list[i].held_len && !t->list[i].closing)
         socket_set_add(set, t->list[i].s);
}

void streams_run(struct streams *t)
{
   Uint64 now = SDL_GetTicks();
   int i;

   for (i = 0; i < STREAMS; i++)
   {
      struct stream *st = &t->list[i];

      if (!st->live)
         continue;
      if (!t->member(t->user, st->peer))
      {
         end_stream(t, st, true);
         st->live = st->telling = false;
      }
      else if (st->telling)
         end_stream(t, st, true);
      else if (st->connecting || st->s == INVALID_SOCKET)
         check_connect(t, st, now);
      if (st->live && !st->connecting && st->s != INVALID_SOCKET)
         move_stream(t, st);
   }
}

int streams_active(const struct streams *t)
{
   int i, n = 0;

   for (i = 0; i < STREAMS; i++)
      n += t->list[i].live;
   return n;
}
