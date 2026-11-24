#ifndef FUSEBOX_SESSION_H
#define FUSEBOX_SESSION_H

/* Playing a game's LAN mode, or on a server on this computer, with friends:
 * fusebox --session, a process of its own for as long as that lasts (it's
 * Spacewar's for Steam: lobbies and network need an app id, steamnet.h). The
 * background FuseBox starts it when a friend is invited, or an invite is
 * accepted, in Steam (steamui.h), and tells it through its input:
 *
 *   invite <shortcut's app id> <friend's Steam id>
 *   join <lobby> <host's Steam id>
 *
 * The host's lobby says which game it is and which ports (lan.h) of servers are
 * shared; a guest has them on 127.0.0.1 (tunnel.h), and its own copy of the game
 * is started. The games are on a LAN (vlan.h, lanhook.c). Over with the game, the lobby, or the background FuseBox. */

/* fusebox --spacewar: while its input is open, Steam counts Spacewar as played.
 * The session has one for as long as friends are being connected. */
int session_count(void);
/* Until it's over. Windows; nothing elsewhere. */
int session_run(const char *data_dir, const char *config_dir);

#endif
