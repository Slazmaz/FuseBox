#include "steamnet.h"
#include "vlan.h"

#ifdef _WIN32

#include <windows.h>
#include <SDL3/SDL.h>

/* A function of an interface by its place in it: those of the versions asked
 * for below never move. */
#define SLOT(obj, i) ((*(void ***)(obj))[i])

#define RESULT_OK 1
#define RESULT_LIMIT_EXCEEDED 25
#define LOBBY_PRIVATE 0
#define ENTERED_OK 1
#define IDENTITY_STEAM_ID 16
/* Without waiting for more to send along, and reopened when it broke. */
#define SEND_PACKET (1 | 32)
#define SEND_STREAM (8 | 1 | 32)
/* Says nothing: has a connection made, and kept. */
#define HELLO 4
#define CONNECTED 3

enum
{
   CB_LOBBY_ENTER = 504,
   CB_LOBBY_DATA = 505,
   CB_LOBBY_CHAT = 506,
   CB_LOBBY_CREATED = 513,
   CB_CALL_COMPLETED = 703,
   CB_SESSION_REQUEST = 1251
};

#pragma pack(push, 8)
struct callback
{
   int32_t user, id;
   Uint8 *param;
   int32_t size;
};

struct call_done
{
   Uint64 call;
   int32_t id;
   Uint32 size;
};

struct lobby_created
{
   int32_t result;
   Uint64 lobby;
};

struct lobby_enter
{
   Uint64 lobby;
   Uint32 perms;
   Uint8 locked;
   Uint32 response;
};

struct lobby_data
{
   Uint64 lobby, member;
   Uint8 success;
};

struct lobby_chat
{
   Uint64 lobby, changed, by;
   Uint32 state;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct identity
{
   int32_t type, size;
   union
   {
      Uint64 id;
      Uint8 raw[128];
   } u;
};
#pragma pack(pop)

struct message
{
   void *data;
   int32_t size;
   Uint32 conn;
   struct identity peer;
   Sint64 user_data, received, number;
   void (*free_data)(struct message *);
   void (*release)(struct message *);
};

static struct
{
   HMODULE dll;
   bool (*get_callback)(int32_t, struct callback *);
   void (*free_callback)(int32_t);
   bool (*call_result)(int32_t, Uint64, void *, int, int, bool *);
   void *client, *friends, *matchmaking, *messages, *utils;
   int32_t pipe, user;
   Uint64 me;
   char name[128];
   bool counted;
} net;

static void identify(struct identity *id, Uint64 steam_id)
{
   SDL_zerop(id);
   id->type = IDENTITY_STEAM_ID;
   id->size = sizeof(Uint64);
   id->u.id = steam_id;
}

static void disconnect_steam(void)
{
   if (net.client && net.user)
      ((void (*)(void *, int32_t, int32_t))SLOT(net.client, 4))(net.client, net.pipe, net.user);
   if (net.client && net.pipe)
      ((bool (*)(void *, int32_t))SLOT(net.client, 1))(net.client, net.pipe);
   net.user = net.pipe = 0;
}

static bool fail(const char *why, char *error, size_t size)
{
   SDL_strlcpy(error, why, size);
   steamnet_close();
   return false;
}

/* ---- Not as a game ------------------------------------------------------- */

/* Steam counts a process with an app id in its environment as that game, and
 * shows friends the one that came last: not the game that's played. Its own
 * programs say which app they're for with a function that can leave that out.
 * It isn't of a public interface: its place changes, its names don't. */
#define UTILS_CLASS ".?AVIClientUtilsMap@@"
#define SET_APP "SetAppIDForCurrentPipe"
/* So many of its functions are looked through. */
#define MAP_SLOTS 200

struct part
{
   const Uint8 *start, *end;
};

static struct part part_of(HMODULE dll, const char *name)
{
   const Uint8 *base = (const Uint8 *)dll;
   const IMAGE_NT_HEADERS *nt = (const void *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
   const IMAGE_SECTION_HEADER *h = IMAGE_FIRST_SECTION(nt);
   struct part part = { NULL, NULL };
   int i;

   for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
      if (SDL_strncmp((const char *)h[i].Name, name, sizeof(h[i].Name)) == 0)
      {
         part.start = base + h[i].VirtualAddress;
         part.end = part.start + h[i].Misc.VirtualSize;
         break;
      }
   return part;
}

/* The functions of the class of that name, by what C++ keeps of it. */
static void **class_functions(HMODULE dll, const char *name)
{
   struct part data = part_of(dll, ".data"), consts = part_of(dll, ".rdata");
   const Uint8 *base = (const Uint8 *)dll, *p, *q;
   size_t len = SDL_strlen(name) + 1;

   for (p = data.start; p && p + len <= data.end && SDL_memcmp(p, name, len) != 0; p++)
      ;
   if (!p || p + len > data.end || !consts.start)
      return NULL;
   /* The name's record starts two pointers before it; a class's has where that
    * and itself are, and its functions follow a pointer to it. */
   {
      Uint32 record = (Uint32)(p - base) - 16;

      for (p = consts.start; p + 24 <= consts.end; p += 4)
      {
         const Uint32 *at = (const Uint32 *)p;

         if (at[0] != 1 || at[1] != 0 || at[3] != record || at[5] != (Uint32)(p - base))
            continue;
         for (q = consts.start; q + 16 <= consts.end; q += 8)
            if (*(const Uint8 *const *)q == p)
               return (void **)(q + 8);
      }
   }
   return NULL;
}

/* The place of the function that has its own name in it; -1 for none. */
static int named_function(HMODULE dll, void **functions, const char *name)
{
   struct part code = part_of(dll, ".text"), consts = part_of(dll, ".rdata");
   size_t len = SDL_strlen(name) + 1;
   int i;

   for (i = 0; code.start && consts.start && i < MAP_SLOTS; i++)
   {
      const Uint8 *f = functions[i], *end;
      const RUNTIME_FUNCTION *entry;
      DWORD64 base;

      if (f < code.start || f >= code.end)
         break;
      /* Where it ends: another's name isn't its own. */
      if (!(entry = RtlLookupFunctionEntry((DWORD64)f, &base, NULL)))
         continue;
      for (end = (const Uint8 *)base + entry->EndAddress; f + 7 <= end; f++)
      {
         Sint32 off;
         const Uint8 *text;

         /* lea of something in the DLL. */
         if ((f[0] & 0xFB) != 0x48 || f[1] != 0x8D || (f[2] & 0xC7) != 0x05)
            continue;
         SDL_memcpy(&off, f + 3, sizeof(off));
         text = f + 7 + off;
         if (text >= consts.start && text + len <= consts.end && SDL_memcmp(text, name, len) == 0)
            return i;
      }
   }
   return -1;
}

static bool readable(const void *p)
{
   MEMORY_BASIC_INFORMATION m;

   return VirtualQuery(p, &m, sizeof(m)) && m.State == MEM_COMMIT
      && !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD | PAGE_EXECUTE));
}

/* The app for what's opened next, without Steam taking this for a game. */
static bool set_quiet_app(Uint32 app)
{
   void **functions = class_functions(net.dll, UTILS_CLASS), **fields;
   int i, slot = functions ? named_function(net.dll, functions, SET_APP) : -1;

   if (slot < 0)
      return false;
   fields = ((void *(*)(void *, int32_t, const char *))SLOT(net.client, 9))(net.client, net.pipe,
         "SteamUtils010");
   /* The public interface has the one it's made of. */
   for (i = 0; fields && i < 8 && readable(fields + i); i++)
      if (fields[i] && ((uintptr_t)fields[i] & 7) == 0 && readable(fields[i])
            && *(void ***)fields[i] == functions)
      {
         ((Uint32 (*)(void *, Uint32, bool))functions[slot])(fields[i], app, false);
         return ((Uint32 (*)(void *))SLOT(fields, 9))(fields) == app;
      }
   return false;
}

/* As a game's process, for a Steam where that can't be done. */
static void be_game(Uint32 app)
{
   char text[32];

   SDL_snprintf(text, sizeof(text), "%u", (unsigned)app);
   SetEnvironmentVariableA("SteamAppId", text);
   SetEnvironmentVariableA("SteamGameId", text);
   SetEnvironmentVariableA("SteamOverlayGameId", text);
}

/* Steam's client, and connected to it. */
static const char *load_steam(const WCHAR *dll)
{
   void *(*create)(const char *, int *);

   if (!(net.dll = LoadLibraryExW(dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH)))
      return "Steam's client can't be loaded";
   create = (void *)GetProcAddress(net.dll, "CreateInterface");
   net.get_callback = (void *)GetProcAddress(net.dll, "Steam_BGetCallback");
   net.free_callback = (void *)GetProcAddress(net.dll, "Steam_FreeLastCallback");
   net.call_result = (void *)GetProcAddress(net.dll, "Steam_GetAPICallResult");
   if (!create || !net.get_callback || !net.free_callback || !net.call_result
         || !(net.client = create("SteamClient020", NULL)))
      return "This Steam isn't supported";
   net.pipe = ((int32_t (*)(void *))SLOT(net.client, 0))(net.client);
   if (net.pipe)
      net.user = ((int32_t (*)(void *, int32_t))SLOT(net.client, 2))(net.client, net.pipe);
   return net.user ? NULL : "Steam isn't running or logged in";
}

static bool find_steam(WCHAR *dll, DWORD bytes)
{
   return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"SteamClientDll64",
         RRF_RT_REG_SZ, NULL, dll, &bytes) == ERROR_SUCCESS;
}

bool steamnet_count(Uint32 app)
{
   WCHAR dll[1024];

   SDL_zero(net);
   be_game(app);
   return find_steam(dll, sizeof(dll)) && !load_steam(dll);
}

bool steamnet_counted(void)
{
   return net.counted;
}

bool steamnet_open(Uint32 app, char *error, size_t size)
{
   void *(*get)(void *, int32_t, int32_t, const char *);
   const char *why;
   void *user;
   WCHAR dll[1024];

   SDL_zero(net);
   if (!find_steam(dll, sizeof(dll)))
      return fail("Steam wasn't found", error, size);
   if ((why = load_steam(dll)))
      return fail(why, error, size);
   if (!set_quiet_app(app))
   {
      /* It's read when the library is loaded. */
      HMODULE was = net.dll;

      disconnect_steam();
      FreeLibrary(was);
      be_game(app);
      if ((why = load_steam(dll)))
         return fail(why, error, size);
      net.counted = true;
   }
   SDL_Log("Steam shows %s", net.counted ? "Spacewar (this Steam can't do without)" : "the game");
   get = (void *(*)(void *, int32_t, int32_t, const char *))SLOT(net.client, 12);
   user = get(net.client, net.user, net.pipe, "SteamUser021");
   net.friends = get(net.client, net.user, net.pipe, "SteamFriends017");
   net.matchmaking = get(net.client, net.user, net.pipe, "SteamMatchMaking009");
   net.messages = get(net.client, net.user, net.pipe, "SteamNetworkingMessages002");
   net.utils = get(net.client, net.user, net.pipe, "SteamNetworkingUtils004");
   if (!user || !net.friends || !net.matchmaking || !net.messages)
      return fail("This Steam isn't supported", error, size);
   ((Uint64 *(*)(void *, Uint64 *))SLOT(user, 2))(user, &net.me);
   /* Its relays are looked for now, not once they're needed. */
   if (net.utils)
      ((bool (*)(void *, float))SLOT(net.utils, 7))(net.utils, 1e10f);
   return true;
}

void steamnet_close(void)
{
   disconnect_steam();
   /* The library itself stays: its threads may still be there. */
   SDL_zero(net);
}

Uint64 steamnet_me(void)
{
   return net.me;
}

const char *steamnet_name(Uint64 id)
{
   const char *name = ((const char *(*)(void *, Uint64))SLOT(net.friends, 7))(net.friends, id);

   SDL_strlcpy(net.name, name ? name : "", sizeof(net.name));
   return net.name;
}

/* False for what's of no interest. */
static bool read_event(int id, const void *param, int size, struct steamnet_event *e)
{
   SDL_zerop(e);
   if (id == CB_LOBBY_CREATED && size >= (int)sizeof(struct lobby_created))
   {
      const struct lobby_created *c = param;
      e->kind = STEAMNET_CREATED;
      e->ok = c->result == RESULT_OK;
      e->lobby = c->lobby;
      return true;
   }
   if (id == CB_LOBBY_ENTER && size >= (int)sizeof(struct lobby_enter))
   {
      const struct lobby_enter *c = param;
      e->kind = STEAMNET_ENTERED;
      e->ok = c->response == ENTERED_OK;
      e->lobby = c->lobby;
      return true;
   }
   if (id == CB_LOBBY_DATA && size >= (int)sizeof(struct lobby_data))
   {
      const struct lobby_data *c = param;
      e->kind = STEAMNET_DATA;
      e->ok = c->success != 0;
      e->lobby = c->lobby;
      return c->lobby == c->member || !c->success;
   }
   if (id == CB_LOBBY_CHAT && size >= (int)sizeof(struct lobby_chat))
   {
      const struct lobby_chat *c = param;
      e->kind = STEAMNET_MEMBERS;
      e->ok = (c->state & 1) != 0;
      e->lobby = c->lobby;
      e->who = c->changed;
      return true;
   }
   if (id == CB_SESSION_REQUEST && size >= (int)sizeof(struct identity))
   {
      const struct identity *c = param;
      e->kind = STEAMNET_REQUEST;
      e->who = c->type == IDENTITY_STEAM_ID ? c->u.id : 0;
      return e->who != 0;
   }
   return false;
}

bool steamnet_next(struct steamnet_event *e)
{
   struct callback cb;

   while (net.get_callback(net.pipe, &cb))
   {
      bool got = false;

      if (cb.id == CB_CALL_COMPLETED)
      {
         struct call_done done;
         bool failed = true;
         Uint8 *result;

         SDL_memcpy(&done, cb.param, sizeof(done));
         result = SDL_calloc(1, done.size + 1);
         if (result && net.call_result(net.pipe, done.call, result, (int)done.size, done.id, &failed)
               && !failed)
            got = read_event(done.id, result, (int)done.size, e);
         else if (done.id == CB_LOBBY_CREATED || done.id == CB_LOBBY_ENTER)
         {
            SDL_zerop(e);
            e->kind = done.id == CB_LOBBY_CREATED ? STEAMNET_CREATED : STEAMNET_ENTERED;
            got = true;
         }
         SDL_free(result);
      }
      else
         got = read_event(cb.id, cb.param, cb.size, e);
      net.free_callback(net.pipe);
      if (got)
         return true;
   }
   return false;
}

void steamnet_create(int members)
{
   ((Uint64 (*)(void *, int, int))SLOT(net.matchmaking, 13))(net.matchmaking, LOBBY_PRIVATE, members);
}

void steamnet_join(Uint64 lobby)
{
   ((Uint64 (*)(void *, Uint64))SLOT(net.matchmaking, 14))(net.matchmaking, lobby);
}

void steamnet_leave(Uint64 lobby)
{
   ((void (*)(void *, Uint64))SLOT(net.matchmaking, 15))(net.matchmaking, lobby);
}

bool steamnet_invite(Uint64 lobby, Uint64 who)
{
   return ((bool (*)(void *, Uint64, Uint64))SLOT(net.matchmaking, 16))(net.matchmaking, lobby, who);
}

bool steamnet_ask(Uint64 lobby)
{
   return ((bool (*)(void *, Uint64))SLOT(net.matchmaking, 28))(net.matchmaking, lobby);
}

int steamnet_members(Uint64 lobby, Uint64 *ids, int max)
{
   int i, n = ((int (*)(void *, Uint64))SLOT(net.matchmaking, 17))(net.matchmaking, lobby), got = 0;

   for (i = 0; i < n && got < max; i++)
   {
      Uint64 id = 0;
      ((Uint64 *(*)(void *, Uint64 *, Uint64, int))SLOT(net.matchmaking, 18))(net.matchmaking, &id,
            lobby, i);
      if (id)
         ids[got++] = id;
   }
   return got;
}

Uint64 steamnet_owner(Uint64 lobby)
{
   Uint64 id = 0;

   ((Uint64 *(*)(void *, Uint64 *, Uint64))SLOT(net.matchmaking, 35))(net.matchmaking, &id, lobby);
   return id;
}

const char *steamnet_get(Uint64 lobby, const char *key)
{
   const char *v = ((const char *(*)(void *, Uint64, const char *))SLOT(net.matchmaking, 19))(
         net.matchmaking, lobby, key);

   return v ? v : "";
}

void steamnet_set(Uint64 lobby, const char *key, const char *value)
{
   ((bool (*)(void *, Uint64, const char *, const char *))SLOT(net.matchmaking, 20))(net.matchmaking,
         lobby, key, value);
}

void steamnet_accept(Uint64 who)
{
   struct identity id;

   identify(&id, who);
   ((bool (*)(void *, const struct identity *))SLOT(net.messages, 2))(net.messages, &id);
}

void steamnet_hang_up(Uint64 who)
{
   struct identity id;

   identify(&id, who);
   ((bool (*)(void *, const struct identity *))SLOT(net.messages, 3))(net.messages, &id);
}

void steamnet_hello(Uint64 who)
{
   struct identity id;

   identify(&id, who);
   ((int (*)(void *, const struct identity *, const void *, Uint32, int, int))SLOT(net.messages, 0))(
         net.messages, &id, "", 1, SEND_STREAM, HELLO);
}

bool steamnet_connected(Uint64 who)
{
   struct identity id;

   identify(&id, who);
   return ((int (*)(void *, const struct identity *, void *, void *))SLOT(net.messages, 5))(
         net.messages, &id, NULL, NULL) == CONNECTED;
}

enum sent steamnet_send(void *user, Uint64 to, int channel, const void *data, int size)
{
   struct identity id;
   int result;

   identify(&id, to);
   result = ((int (*)(void *, const struct identity *, const void *, Uint32, int, int))
         SLOT(net.messages, 0))(net.messages, &id, data, (Uint32)size,
            channel == TUNNEL_STREAMS || channel == VLAN_STREAMS ? SEND_STREAM : SEND_PACKET, channel);
   return result == RESULT_OK ? SENT_OK
      : result == RESULT_LIMIT_EXCEEDED ? SENT_FULL : SENT_FAILED;
}

void steamnet_receive(void (*take)(Uint64 from, int channel, const void *data, int size))
{
   static const int channels[] = { TUNNEL_STREAMS, TUNNEL_PACKETS, VLAN_STREAMS,
      VLAN_PACKETS, HELLO };
   struct message *list[32];
   size_t c;
   int i, n;

   for (c = 0; c < SDL_arraysize(channels); c++)
      do
      {
         n = ((int (*)(void *, int, struct message **, int))SLOT(net.messages, 1))(net.messages,
               channels[c], list, SDL_arraysize(list));
         for (i = 0; i < n; i++)
         {
            if (list[i]->peer.type == IDENTITY_STEAM_ID && channels[c] != HELLO)
               take(list[i]->peer.u.id, channels[c], list[i]->data, list[i]->size);
            list[i]->release(list[i]);
         }
      } while (n == (int)SDL_arraysize(list));
}

#endif
