#ifndef FUSEBOX_VLAN_H
#define FUSEBOX_VLAN_H

#include "stream.h"
#include "lanhook.h"

/* A LAN of a session's players, for games that look for each other (lanhook.c in
 * them). On each computer a player is an address of its own, net + number, where
 * this stands in for the others: what the game sends to one of their ports goes to
 * that player's computer, and comes to the game there from the sender's address
 * and port, so it's answered the same way. Only to the game's own ports. */

#define VLAN_STREAMS 2
#define VLAN_PACKETS 3

struct vlan;

/* owns: whether a port is the game's here. */
struct vlan *vlan_create(Uint32 net, int me, send_message send, void *send_user,
      bool (*owns)(void *user, Uint16 port, bool udp), void *user);
void vlan_destroy(struct vlan *v);
/* By their numbers; 0 for none. */
void vlan_set_players(struct vlan *v, const Uint64 ids[LANHOOK_PLAYERS]);
/* Has a player's port there for the game to send or connect to; any thread. */
bool vlan_open(struct vlan *v, int player, Uint16 port, bool udp);
void vlan_receive(struct vlan *v, Uint64 from, int channel, const void *data, int size);
void vlan_run(struct vlan *v, int wait_ms);

#endif
