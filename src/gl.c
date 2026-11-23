#include "gl.h"

#include <SDL3/SDL.h>

struct gl_funcs gl;

bool gl_load(void)
{
#define X(type, name) \
   gl.name = (type)SDL_GL_GetProcAddress("gl" #name); \
   if (!gl.name) \
   { \
      SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Missing GL function gl%s", #name); \
      return false; \
   }
   FUSEBOX_GL_FUNCS(X)
#undef X
   return true;
}
