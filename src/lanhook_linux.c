/* libfuseboxlan.so: lanhook.c for Linux, where FuseBox starts a game with it
 * (LD_PRELOAD) and it does nothing until there's a LAN. The same: what a game sends
 * to everybody is sent to each player, it's told its address there, and FuseBox is
 * asked to have a player's port ready before it's used. And what Windows does by
 * itself: a game that has a port on all addresses lets FuseBox have it on the
 * players'. The C library's functions, which Wine's go through too. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "lanhook.h"

/* How long FuseBox may take to answer (ms). */
#define ASK_MS 500
#define HOOK __attribute__((visibility("default")))

static struct lanhook *shared;
static char socket_name[64];

/* This computer's real addresses, and those everybody on its networks gets. */
static uint32_t own[32], all[32];
static int own_count, all_count;

static ssize_t (*real_sendto)(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
static ssize_t (*real_sendmsg)(int, const struct msghdr *, int);
static int (*real_sendmmsg)(int, struct mmsghdr *, unsigned, int);
static int (*real_connect)(int, const struct sockaddr *, socklen_t);
static int (*real_bind)(int, const struct sockaddr *, socklen_t);
static int (*real_ioctl)(int, unsigned long, ...);
static int (*real_getifaddrs)(struct ifaddrs **);
static struct hostent *(*real_hostbyname)(const char *);
static int (*real_hostbyname_r)(const char *, struct hostent *, char *, size_t, struct hostent **,
      int *);
static int (*real_addrinfo)(const char *, const char *, const struct addrinfo *, struct addrinfo **);

/* Others' constructors may come before this one's. */
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void find(void *real, const char *name)
{
   *(void **)real = dlsym(RTLD_NEXT, name);
}

static void read_addresses(void)
{
   struct ifaddrs *list, *a;

   if (real_getifaddrs(&list) != 0)
      return;
   for (a = list; a; a = a->ifa_next)
   {
      uint32_t ip, mask;

      if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || (a->ifa_flags & IFF_LOOPBACK))
         continue;
      ip = ntohl(((struct sockaddr_in *)a->ifa_addr)->sin_addr.s_addr);
      mask = a->ifa_netmask ? ntohl(((struct sockaddr_in *)a->ifa_netmask)->sin_addr.s_addr) : ~0u;
      if (own_count < (int)(sizeof(own) / sizeof(*own)))
         own[own_count++] = ip;
      if (all_count < (int)(sizeof(all) / sizeof(*all)) && mask && mask < 0xFFFFFFFEu)
         all[all_count++] = ip | ~mask;
   }
   freeifaddrs(list);
}

static void set_up(void)
{
   const char *id = getenv(LANHOOK_ENV);
   char name[64];
   struct stat st;
   void *map;
   int fd;

   find(&real_sendto, "sendto");
   find(&real_sendmsg, "sendmsg");
   find(&real_sendmmsg, "sendmmsg");
   find(&real_connect, "connect");
   find(&real_bind, "bind");
   find(&real_ioctl, "ioctl");
   find(&real_getifaddrs, "getifaddrs");
   find(&real_hostbyname, "gethostbyname");
   find(&real_hostbyname_r, "gethostbyname_r");
   find(&real_addrinfo, "getaddrinfo");
   if (!id || !id[0])
      return;
   snprintf(name, sizeof(name), LANHOOK_MEMORY, (unsigned)strtoul(id, NULL, 10));
   snprintf(socket_name, sizeof(socket_name), LANHOOK_SOCKET, (unsigned)strtoul(id, NULL, 10));
   if ((fd = shm_open(name, O_RDONLY | O_CLOEXEC, 0)) < 0)
      return;
   /* Only FuseBox's. */
   if (fstat(fd, &st) == 0 && st.st_uid == getuid() && st.st_size >= (off_t)sizeof(*shared)
         && (map = mmap(NULL, sizeof(*shared), PROT_READ, MAP_SHARED, fd, 0)) != MAP_FAILED)
   {
      if (((struct lanhook *)map)->version == LANHOOK_VERSION)
         shared = map;
      else
         munmap(map, sizeof(*shared));
   }
   close(fd);
   if (shared)
      read_addresses();
}

__attribute__((constructor)) static void start(void)
{
   pthread_once(&once, set_up);
}

static int32_t get(const int32_t *value)
{
   return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static bool on(void)
{
   pthread_once(&once, set_up);
   return shared && get(&shared->active);
}

static uint32_t net(void)
{
   return (uint32_t)get(&shared->net);
}

static uint32_t my_address(void)
{
   return net() + (uint32_t)get(&shared->me);
}

static bool has_player(int n)
{
   return (uint32_t)get(&shared->players) >> n & 1;
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

   return n > 0 && n < LANHOOK_PLAYERS && has_player((int)n) ? (int)n : 0;
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

/* An IPv4 address, as IPv6 sockets have them too. */
static bool ipv4(const struct sockaddr *a, socklen_t len, uint32_t *address, uint16_t *port)
{
   const struct sockaddr_in6 *six = (const void *)a;
   const struct sockaddr_in *four = (const void *)a;

   if (a && len >= sizeof(*four) && a->sa_family == AF_INET)
   {
      *address = ntohl(four->sin_addr.s_addr);
      *port = ntohs(four->sin_port);
      return true;
   }
   if (a && len >= sizeof(*six) && a->sa_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&six->sin6_addr))
   {
      memcpy(address, six->sin6_addr.s6_addr + 12, 4);
      *address = ntohl(*address);
      *port = ntohs(six->sin6_port);
      return true;
   }
   return false;
}

/* The same, with that address for its own. */
static socklen_t with_address(const struct sockaddr *a, uint32_t address, struct sockaddr_storage *to)
{
   uint32_t n = htonl(address);

   if (a->sa_family == AF_INET6)
   {
      struct sockaddr_in6 *six = (void *)to;

      memcpy(six, a, sizeof(*six));
      if (address == INADDR_ANY)
         memset(&six->sin6_addr, 0, sizeof(six->sin6_addr));
      else
         memcpy(six->sin6_addr.s6_addr + 12, &n, 4);
      return sizeof(*six);
   }
   memcpy(to, a, sizeof(struct sockaddr_in));
   ((struct sockaddr_in *)to)->sin_addr.s_addr = n;
   return sizeof(struct sockaddr_in);
}

/* ---- FuseBox ------------------------------------------------------------- */

/* What's been asked for, on the LAN as it is: those stay. */
static pthread_rwlock_t asked_lock = PTHREAD_RWLOCK_INITIALIZER;
static uint32_t asked[1024];
static int asked_count;
static int32_t asked_change;

static bool ask_fusebox(const struct lanhook_ask *q)
{
   struct timeval tv = { 0, ASK_MS * 1000 };
   struct sockaddr_un a;
   struct ucred who;
   socklen_t len = sizeof(who);
   uint8_t answer = 0;
   size_t n = strlen(socket_name);
   int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

   if (s < 0)
      return false;
   memset(&a, 0, sizeof(a));
   a.sun_family = AF_UNIX;
   memcpy(a.sun_path + 1, socket_name, n);
   setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
   /* Anybody could have taken that name. */
   if (real_connect(s, (struct sockaddr *)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n)) != 0
         || getsockopt(s, SOL_SOCKET, SO_PEERCRED, &who, &len) != 0 || who.uid != getuid()
         || send(s, q, sizeof(*q), MSG_NOSIGNAL) != sizeof(*q) || recv(s, &answer, 1, 0) != 1)
      answer = 0;
   close(s);
   return answer != 0;
}

static void ask(bool udp, int player, uint16_t port)
{
   uint32_t key = (udp ? 1u << 31 : 0) | (uint32_t)player << 16 | port;
   struct lanhook_ask q = { udp, (uint8_t)player, port };
   int32_t change = get(&shared->change);
   int i, error = errno;
   bool known = false;

   pthread_rwlock_rdlock(&asked_lock);
   for (i = 0; i < asked_count && asked_change == change && !known; i++)
      known = asked[i] == key;
   pthread_rwlock_unlock(&asked_lock);
   if (!known && ask_fusebox(&q))
   {
      pthread_rwlock_wrlock(&asked_lock);
      if (asked_change != change)
      {
         asked_change = change;
         asked_count = 0;
      }
      if (asked_count < (int)(sizeof(asked) / sizeof(*asked)))
         asked[asked_count++] = key;
      pthread_rwlock_unlock(&asked_lock);
   }
   errno = error;
}

static bool is_udp(int s)
{
   int type = 0, error = errno;
   socklen_t len = sizeof(type);
   bool udp = getsockopt(s, SOL_SOCKET, SO_TYPE, &type, &len) == 0 && type == SOCK_DGRAM;

   errno = error;
   return udp;
}

/* Before something goes to that address. */
static void before(int s, const struct sockaddr *to, socklen_t len)
{
   uint32_t address;
   uint16_t port;
   int player;

   if (ipv4(to, len, &address, &port) && port && (player = player_at(address)))
      ask(is_udp(s), player, port);
}

/* ---- Sending ------------------------------------------------------------- */

static bool to_everybody(const struct sockaddr *to, socklen_t len, uint16_t *port)
{
   uint32_t address;

   return ipv4(to, len, &address, port) && is_everybody(address);
}

HOOK ssize_t sendto(int s, const void *buf, size_t len, int flags, const struct sockaddr *to,
      socklen_t tolen)
{
   uint16_t port;
   int i;
   bool sent = false;

   if (!on())
      return real_sendto(s, buf, len, flags, to, tolen);
   if (to_everybody(to, tolen, &port))
   {
      for (i = 1; i < LANHOOK_PLAYERS; i++)
         if (has_player(i))
         {
            struct sockaddr_storage p;
            socklen_t n = with_address(to, net() + (uint32_t)i, &p);

            ask(true, i, port);
            sent |= real_sendto(s, buf, len, flags, (struct sockaddr *)&p, n) >= 0;
         }
      if (sent)
         return (ssize_t)len;
   }
   before(s, to, tolen);
   return real_sendto(s, buf, len, flags, to, tolen);
}

HOOK ssize_t sendmsg(int s, const struct msghdr *msg, int flags)
{
   uint16_t port;
   ssize_t r, sent = -1;
   int i;

   if (!on() || !msg || !msg->msg_name)
      return real_sendmsg(s, msg, flags);
   if (to_everybody(msg->msg_name, msg->msg_namelen, &port))
   {
      for (i = 1; i < LANHOOK_PLAYERS; i++)
         if (has_player(i))
         {
            struct sockaddr_storage p;
            struct msghdr m = *msg;

            m.msg_name = &p;
            m.msg_namelen = with_address(msg->msg_name, net() + (uint32_t)i, &p);
            /* By which network, that is: none of them. */
            m.msg_control = NULL;
            m.msg_controllen = 0;
            ask(true, i, port);
            if ((r = real_sendmsg(s, &m, flags)) >= 0)
               sent = r;
         }
      if (sent >= 0)
         return sent;
   }
   before(s, msg->msg_name, msg->msg_namelen);
   return real_sendmsg(s, msg, flags);
}

HOOK int sendmmsg(int s, struct mmsghdr *list, unsigned count, int flags)
{
   unsigned i;

   if (!on())
      return real_sendmmsg(s, list, count, flags);
   for (i = 0; i < count; i++)
   {
      ssize_t n = sendmsg(s, &list[i].msg_hdr, flags);

      if (n < 0)
         return i ? (int)i : -1;
      list[i].msg_len = (unsigned)n;
   }
   return (int)i;
}

HOOK int connect(int s, const struct sockaddr *to, socklen_t len)
{
   if (on())
      before(s, to, len);
   return real_connect(s, to, len);
}

/* ---- The game's ports ---------------------------------------------------- */

static bool any_address(const struct sockaddr *a, socklen_t len)
{
   uint32_t address;
   uint16_t port;

   if (a && len >= sizeof(struct sockaddr_in6) && a->sa_family == AF_INET6)
      return IN6_IS_ADDR_UNSPECIFIED(&((const struct sockaddr_in6 *)a)->sin6_addr);
   return ipv4(a, len, &address, &port) && address == INADDR_ANY;
}

/* Lets FuseBox have the port on the players' addresses, and nobody else: another
 * would have to say so too. */
static void share(int s)
{
   int yes = 1, error = errno;

   setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
   errno = error;
}

/* Whether it's only FuseBox that has the port, for other players: then it's free at
 * this one's address, where somebody on all addresses would be in the way. */
static bool only_players(int s, const struct sockaddr *at)
{
   struct sockaddr_in mine;
   int error = errno, test = socket(AF_INET, (is_udp(s) ? SOCK_DGRAM : SOCK_STREAM) | SOCK_CLOEXEC, 0);
   bool is;

   memset(&mine, 0, sizeof(mine));
   mine.sin_family = AF_INET;
   mine.sin_addr.s_addr = htonl(my_address());
   mine.sin_port = at->sa_family == AF_INET6 ? ((const struct sockaddr_in6 *)at)->sin6_port
      : ((const struct sockaddr_in *)at)->sin_port;
   is = test >= 0 && real_bind(test, (struct sockaddr *)&mine, sizeof(mine)) == 0;
   if (test >= 0)
      close(test);
   errno = error;
   return is;
}

HOOK int bind(int s, const struct sockaddr *at, socklen_t len)
{
   struct sockaddr_storage any;
   uint32_t address;
   uint16_t port;
   int r;

   if (!at || (at->sa_family != AF_INET && at->sa_family != AF_INET6))
   {
      pthread_once(&once, set_up);
      return real_bind(s, at, len);
   }
   /* One at a real address reaches no other player, nor does what's sent to each
    * of them reach one at everybody's: at any, then. */
   if (on() && ipv4(at, len, &address, &port) && (is_own(address) || is_everybody(address)))
   {
      len = with_address(at, INADDR_ANY, &any);
      at = (struct sockaddr *)&any;
   }
   r = real_bind(s, at, len);
   if (!shared || !any_address(at, len))
      return r;
   if (r == 0)
      share(s);
   else if (errno == EADDRINUSE && on() && only_players(s, at))
   {
      share(s);
      r = real_bind(s, at, len);
   }
   return r;
}

/* ---- The game's address -------------------------------------------------- */

static void set_address(struct sockaddr *a, uint32_t address)
{
   if (a && a->sa_family == AF_INET)
      ((struct sockaddr_in *)a)->sin_addr.s_addr = htonl(address);
}

HOOK int getifaddrs(struct ifaddrs **list)
{
   struct ifaddrs *a;
   bool lan = on();
   int r = real_getifaddrs(list);

   if (r == 0 && lan)
      for (a = *list; a; a = a->ifa_next)
         if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && !(a->ifa_flags & IFF_LOOPBACK))
         {
            set_address(a->ifa_addr, my_address());
            set_address(a->ifa_netmask, 0xFFFFFF00u);
            if (a->ifa_flags & IFF_BROADCAST)
               set_address(a->ifa_broadaddr, net() + 255);
         }
   return r;
}

HOOK int ioctl(int fd, unsigned long what, ...)
{
   struct ifreq *one;
   struct ifconf *conf;
   va_list args;
   void *arg;
   int i, r;

   va_start(args, what);
   arg = va_arg(args, void *);
   va_end(args);
   pthread_once(&once, set_up);
   r = real_ioctl(fd, what, arg);
   if (r != 0 || (what != SIOCGIFADDR && what != SIOCGIFNETMASK && what != SIOCGIFBRDADDR
            && what != SIOCGIFCONF) || !arg || !on())
      return r;
   one = arg;
   if (what == SIOCGIFCONF)
   {
      conf = arg;
      for (i = 0; conf->ifc_req && i < conf->ifc_len / (int)sizeof(struct ifreq); i++)
         if (is_own(ntohl(((struct sockaddr_in *)&conf->ifc_req[i].ifr_addr)->sin_addr.s_addr)))
            set_address(&conf->ifc_req[i].ifr_addr, my_address());
   }
   else if (strncmp(one->ifr_name, "lo", IFNAMSIZ) == 0)
      ;
   else if (what == SIOCGIFADDR)
      set_address(&one->ifr_addr, my_address());
   else if (what == SIOCGIFNETMASK)
      set_address(&one->ifr_netmask, 0xFFFFFF00u);
   else
      set_address(&one->ifr_broadaddr, net() + 255);
   return r;
}

/* This computer, by its name: 127.0.1.1 is what Debian's say. */
static bool is_here(uint32_t address)
{
   return is_own(address) || address == 0x7F000101u;
}

static void my_names(struct hostent *h)
{
   int i;

   for (i = 0; h && h->h_addrtype == AF_INET && h->h_addr_list[i]; i++)
   {
      uint32_t a;

      memcpy(&a, h->h_addr_list[i], 4);
      if (is_here(ntohl(a)))
      {
         a = htonl(my_address());
         memcpy(h->h_addr_list[i], &a, 4);
      }
   }
}

HOOK struct hostent *gethostbyname(const char *name)
{
   bool lan = on();
   struct hostent *h = real_hostbyname(name);

   if (lan)
      my_names(h);
   return h;
}

HOOK int gethostbyname_r(const char *name, struct hostent *ret, char *buf, size_t size,
      struct hostent **result, int *error)
{
   bool lan = on();
   int r = real_hostbyname_r(name, ret, buf, size, result, error);

   if (r == 0 && lan)
      my_names(*result);
   return r;
}

HOOK int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
      struct addrinfo **result)
{
   bool lan = on();
   int r = real_addrinfo(node, service, hints, result);
   struct addrinfo *i;

   if (r == 0 && lan)
      for (i = *result; i; i = i->ai_next)
         if (i->ai_addr && i->ai_addr->sa_family == AF_INET
               && is_here(ntohl(((struct sockaddr_in *)i->ai_addr)->sin_addr.s_addr)))
            set_address(i->ai_addr, my_address());
   return r;
}
