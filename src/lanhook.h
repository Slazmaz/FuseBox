#ifndef FUSEBOX_LANHOOK_H
#define FUSEBOX_LANHOOK_H

#include <stdint.h>

/* Between FuseBox (inject.c) and what it has in a game: fuseboxlan.dll (lanhook.c),
 * or libfuseboxlan.so (lanhook_linux.c). */

/* Players on the LAN are net + 1 to 15: 127.77.0.1... */
#define LANHOOK_NET 0x7F4D0000u
#define LANHOOK_PLAYERS 16
#define LANHOOK_VERSION 1

#ifdef _WIN32

#include <windows.h>

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

#else

/* Shared memory, by the id of the FuseBox that runs the game (lan.h): it makes it
 * and has it in the game's environment, so all that the game starts has it too. */
#define LANHOOK_ENV "FUSEBOX_LAN"
#define LANHOOK_LIBRARY "libfuseboxlan.so"
#define LANHOOK_MEMORY "/fuseboxlan-%u"
struct lanhook
{
   int32_t version;
   /* Whether there's a LAN now; its addresses, this player's number, and a bit
    * for each other's. Another number each time they change. */
   int32_t active, net, me, players, change;
};

/* A socket (abstract, SOCK_SEQPACKET): asks for a player's port to be there before
 * something is sent to it; answered by a byte, when it is. */
#define LANHOOK_SOCKET "fuseboxlan-%u"
struct lanhook_ask
{
   uint8_t udp, player;
   uint16_t port;
};

#endif

#endif
