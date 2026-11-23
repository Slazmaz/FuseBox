#ifndef FUSEBOX_SGDB_H
#define FUSEBOX_SGDB_H

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL_atomic.h>
#include "library.h"

/* SteamGridDB's API (v2): finding a game by name and listing its art.
 * Blocking: for worker threads. Needs steamgriddb_api_key in
 * <data>/credentials.cfg (sgdb_set_key). */

struct sgdb_game
{
   unsigned id;
   char name[256];
};

struct sgdb_art
{
   unsigned id;
   char url[512];
   char thumb[512];
   int width, height;
   /* "png" or "jpg". */
   char ext[8];
};

void sgdb_init(const char *config_dir);
bool sgdb_has_key(void);
void sgdb_set_key(const char *key);

/* Up to max games matching the name, best first; -1 on a failure (no
 * key, network, refused key). *status gets the HTTP status if not NULL. */
int sgdb_search(const char *name, struct sgdb_game *games, int max, SDL_AtomicInt *abort,
      int *status);
/* The game's art of that type, most popular first; *out SDL_free'd by the
 * caller. -1 on a failure. */
int sgdb_list(unsigned game_id, enum art_type type, struct sgdb_art **out,
      SDL_AtomicInt *abort);
/* GET url into path; true on 200. */
bool sgdb_download(const char *url, const char *path, SDL_AtomicInt *abort);

/* Matches the game (if not yet) and saves its most popular art of each
 * type it has none of. Returns how many types it saved. For the bulk
 * "fill in what's missing" and fusebox-cli. */
int sgdb_fill(int index, bool only_type_set, enum art_type only_type, SDL_AtomicInt *abort);

#endif
