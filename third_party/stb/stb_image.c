/* stb_image v2.30 (public domain, github.com/nothings/stb at
 * 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5): PNG and JPEG only, for
 * SteamGridDB covers. */
#include <SDL3/SDL_stdinc.h>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MALLOC(size) SDL_malloc(size)
#define STBI_REALLOC(p, size) SDL_realloc(p, size)
#define STBI_FREE(p) SDL_free(p)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
