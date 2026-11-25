#ifndef FUSEBOX_BOLT_H
#define FUSEBOX_BOLT_H

#include <SDL3/SDL.h>

/* FuseBox's icon, for the window and the tray. The .exe's is drawn by
 * tools/make_icon.py: change both together. */

/* Whether a point is in the lightning bolt, in a square from -0.5 to 0.5 (y down). */
bool bolt_in(float x, float y);
/* The icon, the bolt in white on a blue rounded tile: size * size * 4 bytes. */
void bolt_icon(Uint8 *rgba, int size);

#endif
