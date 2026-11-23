#ifndef FUSEBOX_DISK_H
#define FUSEBOX_DISK_H

#include <stdbool.h>
#include <stddef.h>

/* Finds games without asking any launcher, and adds them to the library
 * (installed set), skipping Steam's own games and games a launcher scan
 * already found (run it after scan_launchers):
 * - Windows' record of what it has seen running as a game (the Game Bar's
 *   GameConfigStore), for those whose exe is still there;
 * - folders with a game engine's or game library's fingerprints (Unity,
 *   Unreal, Godot, GameMaker, MonoGame, SDL, FMOD...) at the top of each
 *   drive, in Program Files, and in folders named Games.
 * Returns how many it added. verbose: log each one and why (fusebox-cli
 * scan). */
int scan_disks(int verbose);

/* Adds the exe the user picked as a game (source "Added", kept while the
 * exe is there; see disk_keep_added), or finds the one it already is.
 * Returns its index, -1 if it isn't there. */
int disk_add_exe(const char *exe);
/* The game's exe in its folder (named like it, or the biggest), for
 * launchers that only say where a game is. False if there's none. */
bool disk_main_exe(const char *dir, char *dst, size_t size);
/* Marks the games added by hand installed while their exe is there: for
 * each scan, which doesn't find them otherwise. */
void disk_keep_added(void);

#endif
