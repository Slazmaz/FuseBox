#ifndef FUSEBOX_SCAN_H
#define FUSEBOX_SCAN_H

#include <stdbool.h>
#include "library.h"

/* Finds installed games in other launchers and adds them to the library
 * (installed set): Epic Games Launcher, GOG Galaxy and Battle.net on Windows, Heroic
 * (Epic and GOG) on both. Returns how many it found. Blocking: a few
 * small files and registry keys. */
int scan_launchers(void);

/* The program a game comes with: its launcher, or a ROM's emulator. Its exe,
 * and its name as it's shown. False for a game with none (or none that's
 * there). Under the library's lock, if the game is the library's. */
bool scan_program(const struct game *g, char *exe, size_t size, char *name, size_t name_size);

#endif
