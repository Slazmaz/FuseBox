#ifndef FUSEBOX_ROMS_H
#define FUSEBOX_ROMS_H

#include <stdbool.h>
#include "library.h"

/* Console games: the ROMs in the ROM folders (found ones and the user's),
 * each with its system, started with an emulator found on this computer.
 * The folders are in <data>/config/roms.json.
 *
 * Lock the library (library_lock) around every function here but
 * roms_init and scan_roms, and around any pointer they return. */

struct rom_folder
{
   char path[1024];
   /* The system of what's in it, empty to tell by each ROM. */
   char system[32];
   /* Found by the scan (not added by the user). */
   bool found;
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

void roms_init(const char *config_dir);
/* Finds emulators and ROMs and adds the ROMs to the library (run it after
 * the other scans). Returns how many ROMs it found. Blocking. */
int scan_roms(void);

int roms_folder_count(void);
const struct rom_folder *roms_folder(int i);
/* These take effect with the next scan. */
void roms_add_folder(const char *path);
void roms_remove_folder(int i);
void roms_set_folder_system(int i, const char *system);
/* Files the last scan couldn't tell the system of. */
int roms_unknown(void);

/* The option the game is started with, NULL when no emulator was found. */
int roms_system_count(void);
const char *roms_system_name(int system);

const struct rom_option *roms_chosen(const struct game *g);
/* The emulators the last scan found, "Dolphin, PCSX2". */
const char *roms_emulators(void);

#endif
