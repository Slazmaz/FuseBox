#ifndef FUSEBOX_ROMS_H
#define FUSEBOX_ROMS_H

#include <stdbool.h>
#include "library.h"

/* Console games: the ROMs in the ROM folders (found ones and the user's),
 * each with its system, started with an emulator found on this computer.
 * Where several can run a game, the user can pick one for it (kept with
 * the game); the folders are in <data>/config/roms.json.
 *
 * Lock the library (library_lock) around every function here but
 * roms_init and the two scans, and around any pointer they return. */

struct rom_folder
{
   char path[1024];
   /* Found by the scan (not added by the user). */
   bool found;
};

/* One way to start a system's games: an emulator, for RetroArch with one
 * of its cores. */
struct rom_option
{
   int system;
   /* -1: a program FuseBox doesn't know. */
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
/* Only finds emulators that weren't there before, to be picked: no game
 * changes. Returns how many ways to start a system they add. Blocking; locks
 * the library itself. */
int roms_find_emulators(void);

int roms_folder_count(void);
const struct rom_folder *roms_folder(int i);
/* These take effect with the next scan. */
void roms_add_folder(const char *path);
void roms_remove_folder(int i);
/* Files the last scan couldn't tell the system of. */
int roms_unknown(void);

/* The game's options, at most max of them; returns how many it has. */
int roms_options(const struct game *g, const struct rom_option **out, int max);
/* The one used: the user's choice while it's there, else the first; NULL
 * when no emulator was found. */
const struct rom_option *roms_chosen(const struct game *g);
/* Picks the option for the game, which starts with it from now on. */
void roms_choose(struct game *g, const struct rom_option *option);
/* Picks a program for the game, and offers it for its system's others from
 * now on: an emulator FuseBox knows, or any other (given just the ROM).
 * False if it's RetroArch without a core for the system. */
bool roms_add_emulator(struct game *g, const char *exe);
/* What a program is given to start a ROM with it: an emulator FuseBox knows, or
 * any other (just the ROM). g: the ROM's game, NULL if it isn't in the library.
 * False if it's RetroArch without a core for it. */
bool roms_emulator_args(const struct game *g, const char *rom, const char *exe, char *args,
      size_t size);
/* An emulator's name by its exe: the one FuseBox knows it by, else the
 * file's. */
void roms_emulator_name(const char *exe, char *dst, size_t size);

#endif
