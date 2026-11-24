#ifndef FUSEBOX_STEAMLIB_H
#define FUSEBOX_STEAMLIB_H

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL_stdinc.h>

/* Steam's client library, as Windows and Linux have it: for steamnet.c. */

/* Of the Steam that runs; NULL with why. */
void *steamlib_open(const char **why);
void steamlib_close(void *lib);
void *steamlib_symbol(void *lib, const char *name);
/* Has Steam count this process as playing it, once the library's loaded. */
void steamlib_be_game(Uint32 app);
/* Steam's client can be loaded again for that; else this program has to be. */
#ifndef _WIN32
bool steamlib_is_game(Uint32 app);
void steamlib_again(Uint32 app);
#endif

/* The functions of a class, by the name C++ keeps of it; NULL if it isn't there. */
void **steamlib_class(void *lib, const char *name);
/* The place among them of the one that has its own name in it; -1 for none. */
int steamlib_named(void *lib, void **functions, const char *name);
bool steamlib_readable(const void *p);

#endif
