#ifndef FUSEBOX_TUNNEL_H
#define FUSEBOX_TUNNEL_H

#include <stdbool.h>
#include <SDL3/SDL_stdinc.h>

/* Carries connections between players over messages (Steam's: steamnet.h), so
 * a game finds a friend's server as if it ran here. The host has the server:
 * its ports on 127.0.0.1 are shared. A guest has the same ports open on
 * 127.0.0.1, and what connects or sends there goes to the host's. Only those
 * ports, and only members' messages. One thread. Windows. */

#define TUNNEL_PORTS 32
/* The channels messages are sent on. */
#define TUNNEL_STREAMS 0
#define TUNNEL_PACKETS 1

struct tunnel_port
{
   Uint16 port;
   bool udp;
};

#ifdef _WIN32
#include "stream.h"

struct tunnel;

/* peer: a guest's host, 0 to be the host. */
struct tunnel *tunnel_create(Uint64 peer, send_message send, void *user);
void tunnel_destroy(struct tunnel *t);
/* The ports shared, or opened here; connections on others end. A guest's that
 * can't be opened (in use) are left out: how many. */
int tunnel_set_ports(struct tunnel *t, const struct tunnel_port *ports, int count);
/* The host's guests; others' connections end. */
void tunnel_set_members(struct tunnel *t, const Uint64 *ids, int count);
/* A message that arrived; one from somebody else is left alone. */
void tunnel_receive(struct tunnel *t, Uint64 from, int channel, const void *data, int size);
/* Moves what there is to move; waits that long at most for something here. */
void tunnel_run(struct tunnel *t, int wait_ms);
/* Connections and senders going through it now. */
int tunnel_active(const struct tunnel *t);
#endif

#endif
