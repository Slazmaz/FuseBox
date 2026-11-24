#ifndef _WIN32

#define _GNU_SOURCE
#include "steamlib.h"
#include "platform.h"

#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <link.h>
#include <stdlib.h>
#include <sys/uio.h>
#include <unistd.h>

/* So many of its functions are looked through. */
#define MAP_SLOTS 300
#define PARTS 8

static void *open_one(const char *path)
{
   return platform_exists(path) ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
}

void *steamlib_open(const char **why)
{
   struct steam_install steam;
   char path[1200], home[1024];
   void *lib;

   if (!platform_find_steam(&steam))
   {
      *why = "Steam wasn't found";
      return NULL;
   }
   platform_home(home, sizeof(home));
   SDL_snprintf(path, sizeof(path), "%s/.steam/sdk64/steamclient.so", home);
   if (steam.flatpak || !(lib = open_one(path)))
   {
      SDL_snprintf(path, sizeof(path), "%s/linux64/steamclient.so", steam.root);
      lib = open_one(path);
   }
   if (!lib)
   {
      const char *error = dlerror();

      SDL_Log("%s: %s", path, error ? error : "isn't there");
      *why = "Steam's client can't be loaded";
   }
   return lib;
}

void steamlib_close(void *lib)
{
   dlclose(lib);
}

void *steamlib_symbol(void *lib, const char *name)
{
   return dlsym(lib, name);
}

void steamlib_be_game(Uint32 app)
{
   char text[32];

   SDL_snprintf(text, sizeof(text), "%u", (unsigned)app);
   setenv("SteamAppId", text, 1);
   setenv("SteamGameId", text, 1);
   setenv("SteamOverlayGameId", text, 1);
}

bool steamlib_is_game(Uint32 app)
{
   const char *text = getenv("SteamAppId");

   return text && SDL_strtoul(text, NULL, 10) == app;
}

/* The library stays loaded when it's closed, and only reads it once. */
void steamlib_again(Uint32 app)
{
   static char line[4096];
   char *args[32], exe[1024];
   size_t size = 0, i;
   ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
   SDL_IOStream *io = SDL_IOFromFile("/proc/self/cmdline", "rb");
   int count = 0;

   if (io)
   {
      size = SDL_ReadIO(io, line, sizeof(line) - 1);
      SDL_CloseIO(io);
   }
   for (i = 0; i < size && count + 1 < (int)SDL_arraysize(args); i += SDL_strlen(line + i) + 1)
      args[count++] = line + i;
   args[count] = NULL;
   if (n <= 0 || !count)
      return;
   exe[n] = '\0';
   steamlib_be_game(app);
   execv(exe, args);
   SDL_Log("Can't start again");
}

/* ---- What's in it --------------------------------------------------------- */

struct part
{
   const Uint8 *start, *end;
   bool code;
};

struct parts
{
   const Uint8 *base;
   struct part list[PARTS];
   int count;
   /* Where its functions start: so many, in order. */
   const Uint8 *table;
   Uint32 functions;
};

static int on_library(struct dl_phdr_info *info, size_t size, void *user)
{
   struct parts *p = user;
   int i;

   if ((const Uint8 *)info->dlpi_addr != p->base)
      return 0;
   for (i = 0; i < info->dlpi_phnum; i++)
   {
      const ElfW(Phdr) *h = &info->dlpi_phdr[i];
      const Uint8 *at = p->base + h->p_vaddr;

      if (h->p_type == PT_LOAD && p->count < PARTS)
      {
         p->list[p->count].start = at;
         p->list[p->count].end = at + h->p_memsz;
         p->list[p->count++].code = (h->p_flags & PF_X) != 0;
      }
      /* As linkers make it: 4 bytes each, from the header on. */
      else if (h->p_type == PT_GNU_EH_FRAME && at[0] == 1 && at[2] == 0x03 && at[3] == 0x3B)
      {
         SDL_memcpy(&p->functions, at + 8, 4);
         p->table = at + 12;
         if (p->functions > (h->p_memsz - 12) / 8)
            p->functions = 0;
      }
   }
   return 1;
}

static bool read_parts(void *lib, struct parts *p)
{
   struct link_map *map = NULL;

   SDL_zerop(p);
   if (dlinfo(lib, RTLD_DI_LINKMAP, &map) != 0 || !map)
      return false;
   p->base = (const Uint8 *)map->l_addr;
   dl_iterate_phdr(on_library, p);
   return p->count > 0 && p->functions > 0;
}

static const struct part *part_at(const struct parts *p, const void *at, size_t size)
{
   const Uint8 *b = at;
   int i;

   for (i = 0; i < p->count; i++)
      if (b >= p->list[i].start && b < p->list[i].end && size <= (size_t)(p->list[i].end - b))
         return &p->list[i];
   return NULL;
}

/* The next place that has that address, from there on; NULL for none. */
static void **find_pointer(const struct parts *p, const void *to, void **from)
{
   int i;

   for (i = 0; i < p->count; i++)
   {
      void **at = (void **)p->list[i].start, **end = (void **)p->list[i].end;

      if (p->list[i].code || from >= end)
         continue;
      for (at = from > at ? from : at; at + 1 <= end; at++)
         if (*at == to)
            return at;
   }
   return NULL;
}

/* Where that text is, a whole one. */
static const char *find_text(const struct parts *p, const char *text)
{
   size_t len = SDL_strlen(text) + 1;
   int i;

   for (i = 0; i < p->count; i++)
   {
      const Uint8 *at = p->list[i].start, *end = p->list[i].end;

      if (p->list[i].code)
         continue;
      while (at + len <= end && (at = memchr(at, text[0], (size_t)(end - at) - len + 1)))
      {
         if (SDL_memcmp(at, text, len) == 0)
            return (const char *)at;
         at++;
      }
   }
   return NULL;
}

void **steamlib_class(void *lib, const char *name)
{
   struct parts p;
   const char *text;
   void **type, **table;
   char full[64];

   /* As C++ has it here: how long it is, first. */
   SDL_snprintf(full, sizeof(full), "%d%s", (int)SDL_strlen(name), name);
   if (!read_parts(lib, &p) || !(text = find_text(&p, full)))
      return NULL;
   /* A class' record has the name second; its functions follow where they're from
    * (0: the class itself) and a pointer to the record. */
   for (type = NULL; (type = find_pointer(&p, text, type)); type++)
      for (table = NULL; (table = find_pointer(&p, type - 1, table)); table++)
         if (part_at(&p, table - 1, 24) && !table[-1]
               && part_at(&p, table[1], 1) && part_at(&p, table[1], 1)->code)
            return table + 1;
   return NULL;
}

/* Where the function ends: where the next one starts. */
static const Uint8 *function_end(const struct parts *p, const Uint8 *f)
{
   const Uint8 *header = p->table - 12;
   Uint32 low = 0, high = p->functions;
   Sint32 off = (Sint32)(f - header), start;

   while (high - low > 1)
   {
      Uint32 mid = low + (high - low) / 2;

      SDL_memcpy(&start, p->table + mid * 8, 4);
      if (start <= off)
         low = mid;
      else
         high = mid;
   }
   SDL_memcpy(&start, p->table + low * 8, 4);
   if (start != off || low + 1 >= p->functions)
      return f;
   SDL_memcpy(&start, p->table + (low + 1) * 8, 4);
   return header + start;
}

int steamlib_named(void *lib, void **functions, const char *name)
{
   size_t len = SDL_strlen(name) + 1;
   struct parts p;
   int i;

   if (!read_parts(lib, &p))
      return -1;
   for (i = 0; i < MAP_SLOTS && part_at(&p, functions + i, sizeof(void *)); i++)
   {
      const Uint8 *f = functions[i], *end;
      const struct part *part = part_at(&p, f, 1);

      if (!part || !part->code)
         break;
      end = function_end(&p, f);
      for (end = SDL_min(end, part->end); f + 7 <= end; f++)
      {
         Sint32 off;
         const Uint8 *text;

         /* lea of something in the library. */
         if ((f[0] & 0xFB) != 0x48 || f[1] != 0x8D || (f[2] & 0xC7) != 0x05)
            continue;
         SDL_memcpy(&off, f + 3, sizeof(off));
         text = f + 7 + off;
         if (part_at(&p, text, len) && !part_at(&p, text, len)->code
               && SDL_memcmp(text, name, len) == 0)
            return i;
      }
   }
   return -1;
}

bool steamlib_readable(const void *p)
{
   Uint64 value;
   struct iovec here = { &value, sizeof(value) }, there = { (void *)p, sizeof(value) };

   return process_vm_readv(getpid(), &here, 1, &there, 1, 0) == (ssize_t)sizeof(value);
}

#endif
