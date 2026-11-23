#include "credentials.h"

#include <SDL3/SDL.h>
#include <string.h>

/* Finds key = "value" in the config text. */
static bool cfg_value(const char *text, const char *key, char *dst, size_t size)
{
   size_t len = SDL_strlen(key);
   const char *line = text;

   while (line && *line)
   {
      const char *p = line;
      while (*p == ' ' || *p == '\t')
         p++;
      if (SDL_strncmp(p, key, len) == 0 && (p[len] == ' ' || p[len] == '=' || p[len] == '\t'))
      {
         p += len;
         while (*p == ' ' || *p == '\t')
            p++;
         if (*p == '=')
         {
            const char *end;
            p++;
            while (*p == ' ' || *p == '\t')
               p++;
            if (*p == '"')
               end = SDL_strchr(++p, '"');
            else
               end = p + strcspn(p, "\r\n");
            if (!end)
               end = p + SDL_strlen(p);
            SDL_strlcpy(dst, p, SDL_min(size, (size_t)(end - p) + 1));
            return dst[0] != '\0';
         }
      }
      line = SDL_strchr(line, '\n');
      if (line)
         line++;
   }
   return false;
}

/* Whether the line sets key. */
static bool sets_key(const char *line, const char *key)
{
   size_t len = SDL_strlen(key);

   while (*line == ' ' || *line == '\t')
      line++;
   return SDL_strncmp(line, key, len) == 0
      && (line[len] == ' ' || line[len] == '=' || line[len] == '\t');
}

bool credentials_set(const char *dir, const char *key, const char *value)
{
   char path[1024];
   size_t length = 0;
   char *text, *line, *next;
   SDL_IOStream *f;
   bool ok;

   SDL_snprintf(path, sizeof(path), "%s/credentials.cfg", dir);
   text = SDL_LoadFile(path, &length);
   SDL_CreateDirectory(dir);
   if (!(f = SDL_IOFromFile(path, "wb")))
   {
      if (text)
         SDL_memset(text, 0, length);
      SDL_free(text);
      return false;
   }
   /* The other keys as they were; this one last, unless cleared. */
   for (line = text; line && *line; line = next)
   {
      next = SDL_strchr(line, '\n');
      if (next)
         *next++ = '\0';
      if (*line && !sets_key(line, key))
         SDL_IOprintf(f, "%s\n", line);
   }
   if (value && value[0])
      SDL_IOprintf(f, "%s = \"%s\"\n", key, value);
   ok = SDL_CloseIO(f);
   if (text)
      SDL_memset(text, 0, length);
   SDL_free(text);
   return ok;
}

bool credentials_get(const char *config_dir, const char *key, char *dst, size_t size)
{
   char path[1024];
   size_t length = 0;
   char *text;
   bool ok;

   dst[0] = '\0';
   SDL_snprintf(path, sizeof(path), "%s/credentials.cfg", config_dir);
   if (!(text = SDL_LoadFile(path, &length)))
      return false;
   ok = cfg_value(text, key, dst, size);
   SDL_memset(text, 0, length);
   SDL_free(text);
   if (!ok)
      dst[0] = '\0';
   return ok;
}
