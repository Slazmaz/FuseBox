#ifndef FUSEBOX_ANIM_H
#define FUSEBOX_ANIM_H

#include <stddef.h>
#include <SDL3/SDL_stdinc.h>

/* Animated WebP and PNG (APNG): what SteamGridDB's animated art is; and
 * WebM video: its previews of it (the art itself is many megabytes). */

/* An animation's frames, RGBA, in one block: SDL_free it. */
struct anim
{
   int w, h, count;
   /* How long it all takes, ms. */
   int length;
   /* When each frame ends (count of them), and the frames, w * h * 4 each. */
   int *ends;
   Uint8 *pixels;
};

/* Every frame, shrunk to fit max_w x max_h, and further for them all to
 * fit in max_bytes. NULL for one that isn't animated (or on failure). */
struct anim *anim_load(const void *data, size_t size, int max_w, int max_h, size_t max_bytes);
/* The frame to show at a time, and (*next) how long until the next, ms. */
int anim_frame(const struct anim *a, Uint64 ms, int *next);
/* The first frame of any WebP; SDL_free it. NULL if it isn't one. */
Uint8 *anim_first(const void *data, size_t size, int *w, int *h);

#endif
