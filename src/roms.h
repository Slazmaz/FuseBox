#ifndef FUSEBOX_ROMS_H
#define FUSEBOX_ROMS_H

#include <stdbool.h>
#include "library.h"

/* Emulators on this computer, and the consoles they run.
 *
 * Lock the library (library_lock) around every function here but
 * scan_roms, and around any pointer they return. */

/* One way to start a system's games: an emulator, for RetroArch with one
 * of its cores. */
struct rom_option
{
   int system;
   int emulator;
   char label[96];
   char exe[1024];
   /* RetroArch's core, its name and its file; else empty. */
   char core[64];
   char core_path[1024];
};

/* Finds emulators; returns how many. Blocking. */
int scan_roms(void);

/* The emulators the last scan found, "Dolphin, PCSX2". */
const char *roms_emulators(void);

#endif
