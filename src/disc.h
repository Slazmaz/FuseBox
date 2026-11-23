#ifndef FUSEBOX_DISC_H
#define FUSEBOX_DISC_H

#include <stdbool.h>
#include <SDL3/SDL_stdinc.h>

/* GameCube and Wii disc images: plain (.iso/.gcm), GCZ, WIA and RVZ.
 * Reads give the bytes of the original disc, as Dolphin sees it; Wii
 * partitions in WIA/RVZ are re-hashed and re-encrypted. Not thread-safe:
 * one user per open disc. */

enum disc_kind
{
   DISC_UNKNOWN,
   DISC_GAMECUBE,
   DISC_WII
};

struct disc;

/* NULL if path isn't a GameCube or Wii disc image. */
struct disc *disc_open(const char *path);
void disc_close(struct disc *d);

enum disc_kind disc_kind(const struct disc *d);
/* Which a disc image is, from its first bytes only (no tables read):
 * quick enough to ask of every game. */
enum disc_kind disc_peek(const char *path);
Uint64 disc_size(const struct disc *d);
bool disc_read(struct disc *d, Uint64 offset, void *dst, size_t size);

#endif
