#ifndef FUSEBOX_STEAMNET_H
#define FUSEBOX_STEAMNET_H

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL_stdinc.h>
#include "tunnel.h"

/* Steam's lobbies and network, through the steamclient64.dll of the Steam that
 * runs. Steam shows the process as playing the app it opens with, so this is
 * for one that's only there for that (session.h). Both need an app id Steam
 * knows; those of non-Steam games are refused. One thread. Windows. */

enum steamnet_kind
{
   /* ok: whether it was; lobby. */
   STEAMNET_CREATED,
   STEAMNET_ENTERED,
   /* Its data came or changed; ok: whether the lobby is there. */
   STEAMNET_DATA,
   /* who came or left. */
   STEAMNET_MEMBERS,
   /* who wants to send messages: steamnet_accept. */
   STEAMNET_REQUEST
};

struct steamnet_event
{
   enum steamnet_kind kind;
   bool ok;
   Uint64 lobby, who;
};

/* As that app. False with why in error. */
bool steamnet_open(Uint32 app, char *error, size_t size);
void steamnet_close(void);
Uint64 steamnet_me(void);
/* A friend's name; valid until the next call. */
const char *steamnet_name(Uint64 id);
/* What happened, one at a time; false when there's no more. */
bool steamnet_next(struct steamnet_event *e);

/* Lobbies: created ones are private, for that many. The answers are events. */
void steamnet_create(int members);
void steamnet_join(Uint64 lobby);
void steamnet_leave(Uint64 lobby);
bool steamnet_invite(Uint64 lobby, Uint64 who);
/* Asks for the data of a lobby one isn't in. */
bool steamnet_ask(Uint64 lobby);
int steamnet_members(Uint64 lobby, Uint64 *ids, int max);
Uint64 steamnet_owner(Uint64 lobby);
const char *steamnet_get(Uint64 lobby, const char *key);
void steamnet_set(Uint64 lobby, const char *key, const char *value);

void steamnet_accept(Uint64 who);
void steamnet_hang_up(Uint64 who);
/* A send_message. */
enum sent steamnet_send(void *user, Uint64 to, int channel, const void *data, int size);
/* Hands over the messages that came. */
void steamnet_receive(void (*take)(Uint64 from, int channel, const void *data, int size));

#endif
