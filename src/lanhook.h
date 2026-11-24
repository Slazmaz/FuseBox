#ifndef FUSEBOX_LANHOOK_H
#define FUSEBOX_LANHOOK_H

#include <windows.h>

/* Between FuseBox (inject.c) and fuseboxlan.dll in a game (lanhook.c). */

/* Players on the LAN are net + 1 to 15: 127.77.0.1... */
#define LANHOOK_NET 0x7F4D0000u
#define LANHOOK_PLAYERS 16
#define LANHOOK_VERSION 1

/* Shared memory, by the game's process id. */
#define LANHOOK_MEMORY L"Local\\FuseBoxLan-%lu"
struct lanhook
{
   LONG version;
   /* The DLL is at work. */
   volatile LONG ready;
   /* Whether there's a LAN now; its addresses, this player's number, and a bit
    * for each other's. */
   volatile LONG active, net, me, players;
};

/* A pipe: asks for a player's port to be there before something is sent to it;
 * answered by a byte, when it is. */
#define LANHOOK_PIPE L"\\\\.\\pipe\\FuseBoxLan-%lu"
struct lanhook_ask
{
   BYTE udp, player;
   WORD port;
};

#endif
