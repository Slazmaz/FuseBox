#ifndef FUSEBOX_LAN_H
#define FUSEBOX_LAN_H

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL_stdinc.h>
#include "library.h"
#include "tunnel.h"

/* The games that run through FuseBox (background_game), and what they're
 * connected to on this computer. */

/* Notes that this process runs the game of that shortcut (0: none) and path
 * (empty: not known), or that it's over. <config>/playing/<process>. */
void lan_playing(const char *config_dir, Uint32 appid, const char *path, bool on);
/* The process that runs the game of that shortcut or that path (0, empty: not
 * asked for), 0 if none does. */
Uint32 lan_game(const char *config_dir, Uint32 appid, const char *path);
bool lan_running(Uint32 pid);
/* Whether any game runs. */
bool lan_any(const char *config_dir);

/* Notes the app id of a shortcut that's there only while this process waits for
 * its game (steamui_once), or that it's gone. <config>/once/<app id>. */
void lan_once(const char *config_dir, Uint32 appid, const char *path, bool on);
/* Reads those that are there now and the games that run, for lan_app. */
void lan_read_once(const char *config_dir);
/* The game's shortcut's app id: that of the one it runs by, of one that's there
 * only meanwhile, or its own; 0 for none. */
Uint32 lan_app(const struct game *g);
/* One whose process is gone, 0 for none (lan_once then). */
Uint32 lan_once_left(const char *config_dir);

/* The ports a game's friends need, going by what runs now: those on this
 * computer that the game (what that process started) is connected to, the
 * others of the servers there, and with own its own (where it's not on a LAN:
 * vlan.h). Added to the count ports there are; returns how many there are then. */
int lan_ports(Uint32 pid, bool own, struct tunnel_port *ports, int count, int max);
/* The game's processes: what that one started. How many. */
int lan_processes(Uint32 pid, Uint32 *pids, int max);
/* Whether one of them has that port open. */
bool lan_owns(Uint32 pid, Uint16 port, bool udp);
/* Whether a friend's port may be opened here: not one of the system's, or
 * where FuseBox looks for Steam. */
bool lan_port_allowed(Uint16 port);

/* Ports as text: "t1027 u7777". */
void lan_write_ports(const struct tunnel_port *ports, int count, char *dst, size_t size);
int lan_read_ports(const char *text, struct tunnel_port *ports, int max);

#endif
