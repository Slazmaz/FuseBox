#ifndef FUSEBOX_LIBRARY_H
#define FUSEBOX_LIBRARY_H

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL_stdinc.h>

/* The games FuseBox knows and the art picked for each, kept in
 * <data>/library.json with the images in <data>/art/<game>/<type>.<ext>.
 * library.json is what other apps read (see README.md); fusebox-cli adds
 * to it too, so saving merges in what another process wrote meanwhile.
 * Games are only ever added: an index stays the same game. Lock around
 * every use of a game from more than one thread. */

enum art_type
{
   ART_COVER,
   ART_WIDE,
   ART_HERO,
   ART_LOGO,
   ART_ICON,
   ART_TYPES
};

/* As library.json and fusebox-cli name them: "cover", "wide", ... */
extern const char *const art_names[ART_TYPES];
/* As the window shows them. */
extern const char *const art_labels[ART_TYPES];
/* ART_TYPES for a name that isn't one. */
enum art_type art_from_name(const char *name);

struct art_choice
{
   /* The image, empty if none is picked. */
   char path[1024];
   /* Its SteamGridDB id, 0 if unknown (or the game's own exe icon). */
   unsigned sgdb_id;
   /* Picked by the user (not filled in by FuseBox): kept as it is. */
   bool picked;
};

struct game
{
   /* "<source>:<the launcher's id>", e.g. "epic:Fortnite". */
   char id[256];
   /* As it's shown: the user's name for it, else its own. */
   char name[256];
   char own_name[256];
   char custom_name[256];
   char source[32];
   /* How to start it (empty for one only looked up for its art). */
   char exe[1024];
   /* The game's own exe, where exe starts a launcher (for its icon). */
   char game_exe[1024];
   char start_dir[1024];
   char args[1024];
   /* A console game's ROM, which exe (an emulator) is started with; else
    * empty. */
   char rom[1024];
   /* The emulator (and RetroArch core) the user picked for it, empty for
    * the first that can run it. */
   char emulator[1024];
   char core[64];
   /* The Steam shortcut's app id once it's in Steam, else 0. */
   Uint32 steam_appid;
   /* The id its shortcut last had: how it's told again after a rename. */
   Uint32 shortcut_id;
   /* The SteamGridDB game its art comes from, 0 until matched. */
   unsigned sgdb_id;
   char sgdb_name[256];
   struct art_choice art[ART_TYPES];
   /* The user picked art since it was last sent to Steam. */
   bool art_edited;
   /* Its shortcut's settings in Steam, kept whether it has one yet or not:
    * the overlay, Steam Input, and options added after its own. */
   bool steam_overlay;
   bool steam_input;
   char launch_options[512];
   /* A ROM's RetroAchievements (ra.h): its hash (empty when it can't be
    * hashed) and the file it's of, the game it's known as there (0: none),
    * how many achievements that has and how many the user unlocked. */
   char ra_hash[33];
   Sint64 ra_size, ra_time;
   unsigned ra_id, ra_total, ra_unlocked;

   /* Not saved: what its shortcut in Steam has now (read by each scan),
    * to tell when the settings above differ. */
   bool sc_overlay, sc_input;
   char sc_exe[1024];
   char sc_options[2048];
   char sc_name[256];

   /* Not saved, found again by each scan:
    * installed: a launcher has it, or its Steam shortcut's folder is there.
    * gone: Steam has a shortcut for it whose folder isn't there any more.
    * selected: the tick (on after a scan only for what Steam has). An
    * installed game's means "in Steam", a gone one's "take it out of Steam".
    * art_version: bumped whenever art changes. */
   bool installed;
   bool gone;
   bool selected;
   /* Those, and steam_appid, as the scan under way has found them: they
    * become the above all at once when it's done (library_scan_end), so
    * nobody sees, or saves, half a scan. */
   struct
   {
      bool installed, gone, selected;
      Uint32 steam_appid;
   } scan;
   unsigned art_version;
   /* A ROM nothing here can start (exe is empty, or what it last was). */
   bool no_emulator;
   /* Art being looked up now, or waiting to be (the window's fill bits). */
   Uint8 filling;
};

bool library_init(const char *data_dir);
void library_quit(void);
const char *library_dir(void);

void library_lock(void);
void library_unlock(void);
/* Under the lock. The pointer stays valid only until the lock is let go
 * (adding a game can move them). */
int library_count(void);
struct game *library_game(int index);
int library_find(const char *id);
/* By name, ignoring case, spaces and punctuation; or by id. -1 if none. */
int library_match(const char *name_or_id);
/* Adds the game, or updates the one with its id (keeping its art and
 * match); returns its index. */
int library_add(const struct game *game);

/* Around a scan: what's found goes in each game's scan, and from there into
 * the game. keep_ticks: ticks the user changed (during it too) stay. */
void library_scan_begin(void);
void library_scan_end(bool keep_ticks);

/* The user's name for the game; empty for its own again. */
void library_rename(int index, const char *name);

/* Moves the downloaded image at tmp_path into the game's folder as its
 * art of that type (replacing the old one) and marks the library changed. */
bool library_set_art(int index, enum art_type type, const char *tmp_path,
      const char *ext, unsigned sgdb_id);
/* Marks the game's art of that type as the user's pick; changed: it's not
 * the one it had. */
void library_set_picked(int index, enum art_type type, bool changed);
/* Where a download for the game's art can go before library_set_art. */
void library_tmp_path(int index, enum art_type type, char *dst, size_t size);
void library_changed(void);

/* Forgets that anything changed: nothing is written (a dry run). */
void library_discard(void);
/* Writes library.json if it changed (merged with what's there first). */
bool library_save(void);
/* Picks up what another process (fusebox-cli) added since; true if it
 * found anything. Cheap when nothing changed. */
bool library_refresh(void);

/* The path that's the game's, whatever starts it (a ROM, its own exe, else
 * its folder), as its Steam shortcut and others give it to fusebox. */
const char *library_path(const struct game *g);
/* Whether they're the same, but for how they're written. */
bool library_same_path(const char *a, const char *b);
/* The game with that path, -1 if none; where several have it, the one whose
 * shortcut's app id that is (0: not known), else one in Steam. Under the lock. */
int library_by_path(const char *path, Uint32 appid);
/* What it's started with: the launcher's own (args), then the user's
 * (launch_options), but for ones with %command%, which are Steam's. */
void library_launch_options(const struct game *g, char *dst, size_t size);

/* Lower case letters and digits only: "The Witcher 3: Wild Hunt" ->
 * "thewitcher3wildhunt". */
void library_normalize(const char *name, char *dst, size_t size);
Uint32 fb_crc32(const void *data, size_t size);

#endif
