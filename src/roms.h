#ifndef FUSEBOX_ROMS_H
#define FUSEBOX_ROMS_H

#include <stdbool.h>
#include "library.h"

/* Console games: the ROMs in the folders found for them, each with its
 * system, started with an emulator found on this computer.
 *
 * Lock the library (library_lock) around every function here but
 * scan_roms, and around any pointer they return. */

struct rom_folder
{
   char path[1024];
};

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

/* Finds emulators and ROMs and adds the ROMs to the library (run it after
 * the other scans). Returns how many ROMs it found. Blocking. */
int scan_roms(void);

int roms_folder_count(void);
const struct rom_folder *roms_folder(int i);
/* Files the last scan couldn't tell the system of. */
int roms_unknown(void);

/* The option the game is started with, NULL when no emulator was found. */
const struct rom_option *roms_chosen(const struct game *g);
/* The emulators the last scan found, "Dolphin, PCSX2". */
const char *roms_emulators(void);

#endif
