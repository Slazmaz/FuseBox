#include "session.h"

#ifdef _WIN32

#include <windows.h>

#include "inject.h"
#include "lan.h"
#include "library.h"
#include "platform.h"
#include "ra.h"
#include "steam.h"
#include "steamnet.h"
#include "steamui.h"
#include "tunnel.h"
#include "vlan.h"

#include <SDL3/SDL.h>

/* Spacewar: Valve's app for trying Steam's functions out. */
#define APP 480
/* Lobbies of FuseBox's have this; another version's isn't joined. */
#define KEY "fusebox"
#define VERSION "1"
#define MEMBERS 8
/* How often the game, the ports and the members are looked at (ms). */
#define LOOK_MS 1000
/* A game's processes are looked for this often (ms): the DLL is to be in early. */
#define GAME_MS 250
/* A host nobody comes to, or whose guests left, ends after this (ms). */
#define ALONE_MS (10 * 60 * 1000)
#define LEFT_MS (60 * 1000)
/* Steam's answers come within this (ms). */
#define ANSWER_MS 30000
/* An invite follows what's said of it after this (ms). */
#define SAID_MS 1000
/* Spacewar is shown until friends were connected this long, or for so long at
 * most; then again after this (longer each time). Hellos are said this often. */
#define SETTLED_MS 3000
#define COUNTED_MS 30000
#define UNCOUNTED_MS 60000
#define HELLO_MS 2000
#define KEEP_MS 10000
#define ONE_NAME L"FuseBoxSession"

enum state
{
   IDLE,
   CREATING,
   HOSTING,
   /* What lobby it is, then in it. */
   ASKING,
   JOINING,
   JOINED
};

static struct
{
   const char *data_dir, *config_dir;
   enum state state;
   Uint64 since;
   Uint64 lobby, host;
   Uint32 appid;
   /* The FuseBox that runs the game; 0: not seen (yet). */
   Uint32 game;
   bool started;
   /* The game's name, and path (a guest's). */
   char name[256], path[1024];
   /* To be invited once there's a lobby; told (when, 0 for not yet) first. */
   struct
   {
      Uint64 who, told;
   } invites[MEMBERS];
   int invite_count;
   Uint64 members[MEMBERS];
   int member_count;
   bool had_guests;
   Uint64 alone_since;
   struct tunnel_port ports[TUNNEL_PORTS];
   int port_count;
   char ports_text[512];
   struct tunnel *tunnel;
   /* The LAN, for games that look for each other: players by their numbers. */
   struct vlan *lan;
   int me;
   Uint64 players[LANHOOK_PLAYERS];
   char players_text[512];
   Uint64 game_at;
   /* Processes the DLL is in, and those it can't be. */
   int in_game;
   Uint32 refused[32];
   int refused_count;
   /* Spacewar for Steam, while friends are being connected. */
   SDL_Process *counted;
   Uint64 counted_since, settled_since, count_at, hello_at;
   int count_tries;
} ses;

/* Lines from the background FuseBox. */
static struct
{
   SDL_Mutex *lock;
   char lines[16][128];
   int count;
   bool ended;
} in;

static int SDLCALL read_input(void *user)
{
   HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
   char line[128], c;
   size_t n = 0;
   DWORD got;

   while (ReadFile(h, &c, 1, &got, NULL) && got == 1)
   {
      if (c != '\n')
      {
         if (n + 1 < sizeof(line) && c != '\r')
            line[n++] = c;
         continue;
      }
      line[n] = '\0';
      n = 0;
      SDL_LockMutex(in.lock);
      if (in.count < (int)SDL_arraysize(in.lines))
         SDL_strlcpy(in.lines[in.count++], line, sizeof(in.lines[0]));
      SDL_UnlockMutex(in.lock);
   }
   SDL_LockMutex(in.lock);
   in.ended = true;
   SDL_UnlockMutex(in.lock);
   return 0;
}

/* Messages being read: not over until they were. */
static SDL_AtomicInt messages;

static int SDLCALL show(void *text)
{
   SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "FuseBox", text, NULL);
   SDL_free(text);
   SDL_AddAtomicInt(&messages, -1);
   return 0;
}

/* Without waiting for it to be read. */
static void say(const char *text)
{
   SDL_Thread *thread;

   SDL_Log("%s", text);
   SDL_AddAtomicInt(&messages, 1);
   if ((thread = SDL_CreateThread(show, "message", SDL_strdup(text))))
      SDL_DetachThread(thread);
   else
      SDL_AddAtomicInt(&messages, -1);
}

static void set_state(enum state state)
{
   ses.state = state;
   ses.since = SDL_GetTicks();
}

/* ---- Connecting ---------------------------------------------------------- */

static void count_spacewar(bool on)
{
   if (on == (ses.counted != NULL))
      return;
   if (on)
   {
      char exe[1100];
      const char *args[] = { exe, "--spacewar", NULL };
      SDL_PropertiesID p = SDL_CreateProperties();

      SDL_snprintf(exe, sizeof(exe), "%sfusebox.exe", SDL_GetBasePath());
      SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args);
      SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);
      SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
      SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
      ses.counted = SDL_CreateProcessWithProperties(p);
      SDL_DestroyProperties(p);
      ses.counted_since = SDL_GetTicks();
      ses.settled_since = 0;
   }
   else
   {
      SDL_KillProcess(ses.counted, true);
      SDL_WaitProcess(ses.counted, true, NULL);
      SDL_DestroyProcess(ses.counted);
      ses.counted = NULL;
   }
   SDL_Log("Connecting friends: %s", on ? ses.counted ? "Spacewar is shown meanwhile"
         : "Spacewar can't be started" : "done, Spacewar isn't shown any more");
}

/* Requests to connect reach only what Steam counts as played. */
static void look_at_links(void)
{
   Uint64 now = SDL_GetTicks();
   int i, missing = 0;
   bool hello = now >= ses.hello_at;

   for (i = 0; i < ses.member_count; i++)
   {
      if (!steamnet_connected(ses.members[i]))
         missing++;
      if (hello)
         steamnet_hello(ses.members[i]);
   }
   if (hello)
      ses.hello_at = now + (missing ? HELLO_MS : KEEP_MS);
   if (steamnet_counted())
      return;
   if (missing)
   {
      ses.settled_since = 0;
      if (!ses.counted && now >= ses.count_at)
         count_spacewar(true);
      else if (ses.counted && now - ses.counted_since > COUNTED_MS)
      {
         count_spacewar(false);
         ses.count_at = now + ((Uint64)UNCOUNTED_MS << SDL_min(ses.count_tries, 4));
         ses.count_tries++;
      }
      return;
   }
   ses.count_tries = 0;
   ses.count_at = 0;
   if (!ses.settled_since)
      ses.settled_since = SDL_max(now, 1);
   if (ses.counted && now - ses.settled_since >= SETTLED_MS)
      count_spacewar(false);
}

int session_count(void)
{
   HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
   char c;
   DWORD got;

   if (!steamnet_count(APP))
      return 1;
   while (ReadFile(h, &c, 1, &got, NULL) && got == 1)
      ;
   steamnet_close();
   return 0;
}

static void end_session(const char *why)
{
   int i;

   if (ses.state == IDLE)
      return;
   SDL_Log("Session over: %s", why);
   count_spacewar(false);
   ses.count_at = ses.hello_at = ses.settled_since = 0;
   ses.count_tries = 0;
   tunnel_destroy(ses.tunnel);
   ses.tunnel = NULL;
   inject_lan(NULL, 0, 0, 0);
   vlan_destroy(ses.lan);
   ses.lan = NULL;
   SDL_zeroa(ses.players);
   ses.players_text[0] = '\0';
   ses.me = ses.in_game = ses.refused_count = 0;
   for (i = 0; i < ses.member_count; i++)
      steamnet_hang_up(ses.members[i]);
   if (ses.lobby && ses.state != ASKING)
      steamnet_leave(ses.lobby);
   ses.lobby = ses.host = 0;
   ses.appid = ses.game = 0;
   ses.started = ses.had_guests = false;
   ses.invite_count = ses.member_count = ses.port_count = 0;
   ses.ports_text[0] = ses.name[0] = ses.path[0] = '\0';
   set_state(IDLE);
}

static void take(Uint64 from, int channel, const void *data, int size)
{
   if (channel == VLAN_STREAMS || channel == VLAN_PACKETS)
   {
      if (ses.lan)
         vlan_receive(ses.lan, from, channel, data, size);
   }
   else if (ses.tunnel)
      tunnel_receive(ses.tunnel, from, channel, data, size);
}

/* ---- The LAN ------------------------------------------------------------- */

static bool is_games(void *user, Uint16 port, bool udp)
{
   return lan_owns(ses.game, port, udp);
}

/* The players as they are now. */
static void update_lan(void)
{
   Uint64 others[LANHOOK_PLAYERS];
   Uint32 bits = 0;
   int i;

   if (!ses.me)
      return;
   if (!ses.lan && !(ses.lan = vlan_create(LANHOOK_NET, ses.me, steamnet_send, NULL, is_games, NULL)))
      return;
   SDL_memcpy(others, ses.players, sizeof(others));
   others[ses.me] = 0;
   for (i = 1; i < LANHOOK_PLAYERS; i++)
      if (others[i])
         bits |= 1u << i;
   vlan_set_players(ses.lan, others);
   inject_lan(ses.lan, LANHOOK_NET, ses.me, bits);
   SDL_Log("On the LAN: this is %d, %s", ses.me, bits ? "with others" : "alone");
}

/* The host numbers the players: itself first, and each keeps theirs. */
static void number_players(void)
{
   char text[512], one[40];
   int i, k;

   ses.me = 1;
   ses.players[1] = steamnet_me();
   for (i = 2; i < LANHOOK_PLAYERS; i++)
   {
      for (k = 0; k < ses.member_count && ses.members[k] != ses.players[i]; k++)
         ;
      if (k == ses.member_count)
         ses.players[i] = 0;
   }
   for (k = 0; k < ses.member_count; k++)
   {
      int spare = 0;

      for (i = 2; i < LANHOOK_PLAYERS && ses.players[i] != ses.members[k]; i++)
         if (!ses.players[i] && !spare)
            spare = i;
      if (i == LANHOOK_PLAYERS && spare)
         ses.players[spare] = ses.members[k];
   }
   text[0] = '\0';
   for (i = 1; i < LANHOOK_PLAYERS; i++)
      if (ses.players[i])
      {
         SDL_snprintf(one, sizeof(one), "%s%d=%llu", text[0] ? " " : "", i,
               (unsigned long long)ses.players[i]);
         SDL_strlcat(text, one, sizeof(text));
      }
   if (SDL_strcmp(text, ses.players_text) == 0)
      return;
   SDL_strlcpy(ses.players_text, text, sizeof(ses.players_text));
   steamnet_set(ses.lobby, "players", text);
   update_lan();
}

/* A guest: as the host has them. */
static void read_players(void)
{
   const char *text = steamnet_get(ses.lobby, "players"), *p;

   if (SDL_strcmp(text, ses.players_text) == 0)
      return;
   SDL_strlcpy(ses.players_text, text, sizeof(ses.players_text));
   SDL_zeroa(ses.players);
   ses.me = 0;
   for (p = ses.players_text; *p;)
   {
      char *end;
      int n = (int)SDL_strtol(p, &end, 10);
      Uint64 id = *end == '=' ? SDL_strtoull(end + 1, &end, 10) : 0;

      if (end == p)
         break;
      if (n > 0 && n < LANHOOK_PLAYERS && id)
      {
         ses.players[n] = id;
         if (id == steamnet_me())
            ses.me = n;
      }
      for (p = end; *p == ' '; p++)
         ;
   }
   update_lan();
}

/* The DLL into what runs of the game. */
static void look_at_game(void)
{
   Uint32 pids[64];
   int i, k, n;

   if (!ses.lan || !ses.game)
      return;
   n = lan_processes(ses.game, pids, SDL_arraysize(pids));
   for (i = ses.in_game = 0; i < n; i++)
   {
      for (k = 0; k < ses.refused_count && ses.refused[k] != pids[i]; k++)
         ;
      if (k < ses.refused_count)
         continue;
      if (inject_into(ses.game, pids[i]))
         ses.in_game++;
      else if (ses.refused_count < (int)SDL_arraysize(ses.refused))
      {
         ses.refused[ses.refused_count++] = pids[i];
         SDL_Log("A process of the game can't be put on the LAN");
      }
   }
}

/* Who else is in the lobby now. */
static void look_at_members(void)
{
   Uint64 ids[MEMBERS + 1];
   int i, k, n = steamnet_members(ses.lobby, ids, SDL_arraysize(ids)), count = 0;

   for (i = 0; i < n; i++)
      if (ids[i] != steamnet_me() && count < MEMBERS)
         ids[count++] = ids[i];
   for (i = 0; i < ses.member_count; i++)
   {
      for (k = 0; k < count && ids[k] != ses.members[i]; k++)
         ;
      if (k == count)
         steamnet_hang_up(ses.members[i]);
   }
   /* Somebody's new: connected now, not after a wait for another. */
   for (i = 0; i < count; i++)
   {
      for (k = 0; k < ses.member_count && ses.members[k] != ids[i]; k++)
         ;
      if (k == ses.member_count)
         ses.count_at = ses.hello_at = 0;
   }
   SDL_memcpy(ses.members, ids, sizeof(*ids) * (size_t)count);
   ses.member_count = count;
   if (ses.state == HOSTING && ses.tunnel)
      tunnel_set_members(ses.tunnel, ses.members, count);
   if (count)
      ses.had_guests = true;
   if (count || !ses.alone_since)
      ses.alone_since = SDL_GetTicks();
}

static bool is_member(Uint64 id)
{
   int i;

   look_at_members();
   for (i = 0; i < ses.member_count; i++)
      if (ses.members[i] == id)
         return true;
   return false;
}

/* ---- Which game ---------------------------------------------------------- */

/* Plain letters and digits: names differ in signs (TM). */
static void simple(const char *name, char *dst, size_t size)
{
   size_t n = 0;

   for (; *name && n + 1 < size; name++)
      if (SDL_isalnum((unsigned char)*name) && (Uint8)*name < 0x80)
         dst[n++] = (char)SDL_tolower((unsigned char)*name);
   dst[n] = '\0';
}

/* What the lobby says of the host's game. */
static void describe_game(void)
{
   const struct game *g = NULL;
   int i;

   if (!library_init(ses.data_dir))
      return;
   lan_read_once(ses.config_dir);
   library_lock();
   for (i = 0; i < library_count() && !g; i++)
      if (lan_app(library_game(i)) == ses.appid)
         g = library_game(i);
   if (g)
   {
      steamnet_set(ses.lobby, "id", g->id);
      /* Its own: what the user renamed it to, friends do not have. */
      SDL_strlcpy(ses.name, g->own_name, sizeof(ses.name));
      steamnet_set(ses.lobby, "name", g->own_name);
      steamnet_set(ses.lobby, "source", g->rom[0] ? g->source : "");
      steamnet_set(ses.lobby, "hash", g->ra_hash);
      SDL_Log("The game: %s", g->name);
   }
   library_unlock();
   library_discard();
   library_quit();
}

/* A guest's own copy of it: how well that fits, 0 for not at all. */
static int fits(const struct game *g, const char *id, const char *name, const char *source,
      const char *hash)
{
   char a[256], b[256];

   if (SDL_strcmp(g->source, "Steam") == 0)
      return 0;
   if (hash[0] && SDL_strcmp(g->ra_hash, hash) == 0)
      return 4;
   if (id[0] && SDL_strcmp(g->id, id) == 0)
      return 3;
   /* The same system's, or no console's either. */
   if (SDL_strcmp(g->rom[0] ? g->source : "", source) != 0)
      return 0;
   simple(name, a, sizeof(a));
   simple(g->own_name, b, sizeof(b));
   if (a[0] && SDL_strcmp(a, b) == 0)
      return 2;
   simple(g->name, b, sizeof(b));
   return a[0] && SDL_strcmp(a, b) == 0 ? 1 : 0;
}

/* The guest's copy: fusebox "<its path>", which has Steam run it. */
static void start_game(void)
{
   char id[256], name[256], source[64], hash[64], text[700];
   int i, fit, best = 0;
   Uint32 appid = 0;

   SDL_strlcpy(id, steamnet_get(ses.lobby, "id"), sizeof(id));
   SDL_strlcpy(name, steamnet_get(ses.lobby, "name"), sizeof(name));
   SDL_strlcpy(source, steamnet_get(ses.lobby, "source"), sizeof(source));
   SDL_strlcpy(hash, steamnet_get(ses.lobby, "hash"), sizeof(hash));
   ses.path[0] = '\0';
   if (library_init(ses.data_dir))
   {
      library_lock();
      for (i = 0; i < library_count(); i++)
      {
         const struct game *g = library_game(i);

         /* One that's in Steam first; uninstalled ones stay in the library. */
         if (!(fit = fits(g, id, name, source, hash)) || fit * 2 + (g->steam_appid != 0) <= best
               || !platform_exists(library_path(g)))
            continue;
         best = fit * 2 + (g->steam_appid != 0);
         appid = g->steam_appid;
         SDL_strlcpy(ses.path, library_path(g), sizeof(ses.path));
      }
      library_unlock();
      library_discard();
      library_quit();
   }
   ses.appid = appid;
   SDL_strlcpy(ses.name, name, sizeof(ses.name));
   if (ses.path[0] && (ses.game = lan_game(ses.config_dir, appid, ses.path)))
      SDL_Log("%s runs already", name);
   else if (ses.path[0])
      SDL_Log("Starting %s: %s", name, platform_start_game(ses.path) ? "yes" : "no");
   else
   {
      SDL_snprintf(text, sizeof(text), "You don't have this game in your library:\n\n%s\n\n"
            "You're connected to %s. If you do have it, start it yourself.", name,
            steamnet_name(ses.host));
      say(text);
   }
   ses.started = true;
}

/* ---- What happens -------------------------------------------------------- */

static void invite(Uint32 appid, Uint64 who)
{
   /* Another game's session is over then; a guest's friends come to its host. */
   if ((ses.state == CREATING || ses.state == HOSTING) && ses.appid != appid)
      end_session("another game");
   if (ses.state != CREATING && ses.state != HOSTING && ses.state != JOINED)
   {
      end_session("an invite");
      ses.appid = appid;
      set_state(CREATING);
      steamnet_create(MEMBERS);
   }
   if (ses.invite_count < MEMBERS)
   {
      ses.invites[ses.invite_count].who = who;
      ses.invites[ses.invite_count++].told = 0;
   }
   ses.alone_since = SDL_GetTicks();
}

/* Sends those that wait. */
static void send_invites(void)
{
   Uint64 now = SDL_GetTicks();
   int i;

   if (ses.state != HOSTING && ses.state != JOINED)
      return;
   for (i = 0; i < ses.invite_count; i++)
      if (!ses.invites[i].told)
      {
         if (ses.name[0])
            steamui_invited(ses.invites[i].who, ses.name, NULL);
         ses.invites[i].told = SDL_max(now, 1);
      }
      else if (now - ses.invites[i].told >= SAID_MS)
      {
         SDL_Log("Inviting %s: %s", steamnet_name(ses.invites[i].who),
               steamnet_invite(ses.lobby, ses.invites[i].who) ? "sent" : "refused");
         SDL_memmove(&ses.invites[i], &ses.invites[i + 1],
               sizeof(ses.invites[0]) * (size_t)(--ses.invite_count - i));
         i--;
      }
}

static void join(Uint64 lobby, Uint64 host)
{
   if (lobby == ses.lobby)
      return;
   end_session("another invite");
   ses.lobby = lobby;
   ses.host = host;
   set_state(ASKING);
   if (!steamnet_ask(lobby))
   {
      set_state(JOINING);
      steamnet_join(lobby);
   }
}

static void command(const char *line)
{
   unsigned long long a, b;

   if (SDL_sscanf(line, "invite %llu %llu", &a, &b) == 2)
      invite((Uint32)a, b);
   else if (SDL_sscanf(line, "join %llu %llu", &a, &b) == 2)
      join(a, b);
}

static void on_created(const struct steamnet_event *e)
{
   /* It comes twice. */
   if (ses.state != CREATING)
   {
      if (e->ok && e->lobby != ses.lobby)
         steamnet_leave(e->lobby);
      return;
   }
   if (!e->ok || !(ses.tunnel = tunnel_create(0, steamnet_send, NULL)))
   {
      say("Steam couldn't make a session: the invite wasn't sent.");
      end_session("no lobby");
      return;
   }
   ses.lobby = e->lobby;
   ses.host = steamnet_me();
   ses.game = lan_game(ses.config_dir, ses.appid, "");
   ses.alone_since = SDL_GetTicks();
   set_state(HOSTING);
   describe_game();
   steamnet_set(ses.lobby, KEY, VERSION);
}

/* Spacewar's own, or another program's: Steam's to join. */
static void not_ours(void)
{
   Uint64 lobby = ses.lobby, host = ses.host;

   if (steamnet_get(lobby, KEY)[0])
   {
      say("Your friend has another version of FuseBox: both need the newest.");
      end_session("another version");
      return;
   }
   end_session("not FuseBox's");
   steamui_pass(lobby, host, NULL);
}

static bool is_ours(void)
{
   return SDL_strcmp(steamnet_get(ses.lobby, KEY), VERSION) == 0;
}

static void on_entered(const struct steamnet_event *e)
{
   char text[300];

   if (ses.state != JOINING || (e->lobby && e->lobby != ses.lobby))
      return;
   if (!e->ok)
   {
      say("That session is over.");
      end_session("can't join");
      return;
   }
   if (!is_ours())
   {
      not_ours();
      return;
   }
   ses.host = steamnet_owner(ses.lobby);
   if (!(ses.tunnel = tunnel_create(ses.host, steamnet_send, NULL)))
   {
      end_session("no tunnel");
      return;
   }
   set_state(JOINED);
   SDL_snprintf(text, sizeof(text), "Joined %s", steamnet_name(ses.host));
   SDL_Log("%s", text);
}

/* The host's ports, opened here. */
static void read_host_ports(void)
{
   char text[sizeof(ses.ports_text)];
   int failed;

   SDL_strlcpy(text, steamnet_get(ses.lobby, "ports"), sizeof(text));
   if (SDL_strcmp(text, ses.ports_text) == 0)
      return;
   SDL_strlcpy(ses.ports_text, text, sizeof(ses.ports_text));
   ses.port_count = lan_read_ports(text, ses.ports, TUNNEL_PORTS);
   failed = tunnel_set_ports(ses.tunnel, ses.ports, ses.port_count);
   SDL_Log("The host's ports: %s (%d of them in use here)", text, failed);
}

static void handle(const struct steamnet_event *e)
{
   switch (e->kind)
   {
      case STEAMNET_CREATED:
         on_created(e);
         break;
      case STEAMNET_ENTERED:
         on_entered(e);
         break;
      case STEAMNET_DATA:
         if (e->lobby != ses.lobby || ses.state != ASKING)
            break;
         /* One that doesn't say is asked inside. */
         if (e->ok && !is_ours())
            not_ours();
         else
         {
            set_state(JOINING);
            steamnet_join(ses.lobby);
         }
         break;
      case STEAMNET_MEMBERS:
         if (e->lobby != ses.lobby || (ses.state != HOSTING && ses.state != JOINED))
            break;
         SDL_Log("%s %s", steamnet_name(e->who), e->ok ? "joined" : "left");
         look_at_members();
         if (ses.state == JOINED && !e->ok && e->who == ses.host)
            end_session("the host left");
         break;
      case STEAMNET_REQUEST:
         /* Only members'. */
         if ((ses.state == HOSTING || ses.state == JOINED) && is_member(e->who))
            steamnet_accept(e->who);
         break;
   }
}

/* Now and then. False when there's nothing left to be here for. */
static bool look(void)
{
   Uint64 now = SDL_GetTicks();

   switch (ses.state)
   {
      case IDLE:
         return SDL_GetAtomicInt(&messages) > 0;
      case CREATING:
      case ASKING:
      case JOINING:
         if (now - ses.since > ANSWER_MS)
         {
            say("Steam didn't answer.");
            end_session("no answer");
         }
         return true;
      case HOSTING:
      {
         char text[sizeof(ses.ports_text)];

         if (ses.game && !lan_running(ses.game))
         {
            end_session("the game ended");
            return true;
         }
         if (!ses.game)
            ses.game = lan_game(ses.config_dir, ses.appid, "");
         look_at_members();
         look_at_links();
         number_players();
         /* Not now: that's before what look_at_members just set it to. */
         if (SDL_GetTicks() - ses.alone_since > (ses.had_guests ? LEFT_MS : ALONE_MS))
         {
            end_session("nobody's here");
            return true;
         }
         /* First: its own ports aren't shared then. */
         look_at_game();
         if (ses.game)
            ses.port_count = lan_ports(ses.game, !ses.in_game, ses.ports, ses.port_count,
                  TUNNEL_PORTS);
         lan_write_ports(ses.ports, ses.port_count, text, sizeof(text));
         if (SDL_strcmp(text, ses.ports_text) != 0)
         {
            SDL_strlcpy(ses.ports_text, text, sizeof(ses.ports_text));
            tunnel_set_ports(ses.tunnel, ses.ports, ses.port_count);
            steamnet_set(ses.lobby, "ports", text);
            SDL_Log("Sharing ports: %s", text);
         }
         return true;
      }
      case JOINED:
         if (steamnet_owner(ses.lobby) != ses.host)
         {
            end_session("the host left");
            return true;
         }
         look_at_members();
         look_at_links();
         read_players();
         read_host_ports();
         if (!ses.started)
            start_game();
         if (ses.game && !lan_running(ses.game))
         {
            end_session("the game ended");
            return true;
         }
         if (!ses.game)
            ses.game = lan_game(ses.config_dir, ses.appid, ses.path);
         return true;
   }
   return true;
}

static SDL_IOStream *log_file;

static void SDLCALL log_line(void *user, int category, SDL_LogPriority priority, const char *message)
{
   if (log_file)
   {
      SDL_IOprintf(log_file, "%s\n", message);
      SDL_FlushIO(log_file);
   }
}

int session_run(const char *data_dir, const char *config_dir)
{
   char path[1100], error[128];
   struct steamnet_event e;
   Uint64 next = 0;
   bool going = true;

   if (!CreateMutexW(NULL, FALSE, ONE_NAME) || GetLastError() == ERROR_ALREADY_EXISTS)
      return 1;
   SDL_snprintf(path, sizeof(path), "%sfusebox-session.log", data_dir);
   log_file = SDL_IOFromFile(path, "w");
   SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
   SDL_SetLogOutputFunction(log_line, NULL);
   ses.data_dir = data_dir;
   ses.config_dir = config_dir;
   ra_init(config_dir);
   steam_read_settings(config_dir);
   if (!(in.lock = SDL_CreateMutex()))
      return 1;
   if (!steamnet_open(APP, error, sizeof(error)))
   {
      char text[300];
      SDL_snprintf(text, sizeof(text), "No playing with friends: %s.", error);
      SDL_Log("%s", text);
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FuseBox", text, NULL);
      return 1;
   }
   SDL_DetachThread(SDL_CreateThread(read_input, "input", NULL));
   /* Until something is asked for, then until that's over. */
   next = SDL_GetTicks() + ANSWER_MS;
   while (going)
   {
      char lines[16][128];
      int i, count;

      SDL_LockMutex(in.lock);
      count = in.count;
      SDL_memcpy(lines, in.lines, sizeof(lines));
      in.count = 0;
      going = !in.ended;
      SDL_UnlockMutex(in.lock);
      for (i = 0; i < count; i++)
      {
         command(lines[i]);
         next = 0;
      }
      while (steamnet_next(&e))
         handle(&e);
      steamnet_receive(take);
      send_invites();
      if (SDL_GetTicks() >= next)
      {
         going &= look();
         next = SDL_GetTicks() + LOOK_MS;
      }
      if (SDL_GetTicks() >= ses.game_at)
      {
         if ((ses.state == HOSTING || ses.state == JOINED) && !ses.game)
            ses.game = lan_game(ses.config_dir, ses.appid, ses.path);
         look_at_game();
         ses.game_at = SDL_GetTicks() + GAME_MS;
      }
      if (ses.lan)
         vlan_run(ses.lan, 0);
      if (ses.tunnel)
         tunnel_run(ses.tunnel, 1);
      else
         SDL_Delay(20);
   }
   end_session("the end");
   inject_end();
   steamnet_close();
   SDL_CloseIO(log_file);
   log_file = NULL;
   return 0;
}

#else

int session_count(void)
{
   return 0;
}

int session_run(const char *data_dir, const char *config_dir)
{
   return 0;
}

#endif
