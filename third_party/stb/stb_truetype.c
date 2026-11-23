/* stb_truetype v1.26 (public domain, github.com/nothings/stb at
 * 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5): the UI's text (ui.c). */
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_assert.h>

#define STBTT_ifloor(x) ((int)SDL_floor(x))
#define STBTT_iceil(x) ((int)SDL_ceil(x))
#define STBTT_sqrt(x) SDL_sqrt(x)
#define STBTT_pow(x, y) SDL_pow(x, y)
#define STBTT_fmod(x, y) SDL_fmod(x, y)
#define STBTT_cos(x) SDL_cos(x)
#define STBTT_acos(x) SDL_acos(x)
#define STBTT_fabs(x) SDL_fabs(x)
#define STBTT_malloc(x, u) ((void)(u), SDL_malloc(x))
#define STBTT_free(x, u) ((void)(u), SDL_free(x))
#define STBTT_assert(x) SDL_assert(x)
#define STBTT_strlen(x) SDL_strlen(x)
#define STBTT_memcpy SDL_memcpy
#define STBTT_memset SDL_memset
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
