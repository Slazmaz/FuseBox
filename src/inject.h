#ifndef FUSEBOX_INJECT_H
#define FUSEBOX_INJECT_H

#include "vlan.h"

/* fuseboxlan.dll (lanhook.c) into games, and its questions answered. On Linux
 * games are started with libfuseboxlan.so (lanhook_linux.c), where it can be. */

#ifndef _WIN32
/* In this process' environment, for the game it starts; and once that's over. */
bool inject_prepare(void);
void inject_over(void);
#endif

/* The LAN there is now; NULL for none. players: a bit for each other's number. */
void inject_lan(struct vlan *v, Uint32 net, int me, Uint32 players);
/* Into a process of the game which that FuseBox runs, once; false if it can't be
 * (a 32-bit one, a protected one). */
bool inject_into(Uint32 game, Uint32 pid);
/* Whether it's at work in it. */
bool inject_ready(Uint32 pid);
/* The games go on without; the DLL stays in them, doing nothing. */
void inject_end(void);

#endif
