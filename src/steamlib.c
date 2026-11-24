#ifdef _WIN32

#include <windows.h>

#include "steamlib.h"
#include <SDL3/SDL.h>

/* So many of its functions are looked through. */
#define MAP_SLOTS 200

void *steamlib_open(const char **why)
{
   WCHAR dll[1024];
   DWORD bytes = sizeof(dll);
   HMODULE lib;

   if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"SteamClientDll64",
            RRF_RT_REG_SZ, NULL, dll, &bytes) != ERROR_SUCCESS)
   {
      *why = "Steam wasn't found";
      return NULL;
   }
   if (!(lib = LoadLibraryExW(dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH)))
      *why = "Steam's client can't be loaded";
   return lib;
}

void steamlib_close(void *lib)
{
   FreeLibrary(lib);
}

void *steamlib_symbol(void *lib, const char *name)
{
   return (void *)GetProcAddress(lib, name);
}

void steamlib_be_game(Uint32 app)
{
   char text[32];

   SDL_snprintf(text, sizeof(text), "%u", (unsigned)app);
   SetEnvironmentVariableA("SteamAppId", text);
   SetEnvironmentVariableA("SteamGameId", text);
   SetEnvironmentVariableA("SteamOverlayGameId", text);
}

struct part
{
   const Uint8 *start, *end;
};

static struct part part_of(HMODULE dll, const char *name)
{
   const Uint8 *base = (const Uint8 *)dll;
   const IMAGE_NT_HEADERS *nt = (const void *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
   const IMAGE_SECTION_HEADER *h = IMAGE_FIRST_SECTION(nt);
   struct part part = { NULL, NULL };
   int i;

   for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
      if (SDL_strncmp((const char *)h[i].Name, name, sizeof(h[i].Name)) == 0)
      {
         part.start = base + h[i].VirtualAddress;
         part.end = part.start + h[i].Misc.VirtualSize;
         break;
      }
   return part;
}

void **steamlib_class(void *lib, const char *class)
{
   HMODULE dll = lib;
   char name[64];
   struct part data = part_of(dll, ".data"), consts = part_of(dll, ".rdata");
   const Uint8 *base = (const Uint8 *)dll, *p, *q;
   size_t len;

   SDL_snprintf(name, sizeof(name), ".?AV%s@@", class);
   len = SDL_strlen(name) + 1;
   for (p = data.start; p && p + len <= data.end && SDL_memcmp(p, name, len) != 0; p++)
      ;
   if (!p || p + len > data.end || !consts.start)
      return NULL;
   /* The name's record starts two pointers before it; a class's has where that
    * and itself are, and its functions follow a pointer to it. */
   {
      Uint32 record = (Uint32)(p - base) - 16;

      for (p = consts.start; p + 24 <= consts.end; p += 4)
      {
         const Uint32 *at = (const Uint32 *)p;

         if (at[0] != 1 || at[1] != 0 || at[3] != record || at[5] != (Uint32)(p - base))
            continue;
         for (q = consts.start; q + 16 <= consts.end; q += 8)
            if (*(const Uint8 *const *)q == p)
               return (void **)(q + 8);
      }
   }
   return NULL;
}

int steamlib_named(void *lib, void **functions, const char *name)
{
   HMODULE dll = lib;
   struct part code = part_of(dll, ".text"), consts = part_of(dll, ".rdata");
   size_t len = SDL_strlen(name) + 1;
   int i;

   for (i = 0; code.start && consts.start && i < MAP_SLOTS; i++)
   {
      const Uint8 *f = functions[i], *end;
      const RUNTIME_FUNCTION *entry;
      DWORD64 base;

      if (f < code.start || f >= code.end)
         break;
      /* Where it ends: another's name isn't its own. */
      if (!(entry = RtlLookupFunctionEntry((DWORD64)f, &base, NULL)))
         continue;
      for (end = (const Uint8 *)base + entry->EndAddress; f + 7 <= end; f++)
      {
         Sint32 off;
         const Uint8 *text;

         /* lea of something in the DLL. */
         if ((f[0] & 0xFB) != 0x48 || f[1] != 0x8D || (f[2] & 0xC7) != 0x05)
            continue;
         SDL_memcpy(&off, f + 3, sizeof(off));
         text = f + 7 + off;
         if (text >= consts.start && text + len <= consts.end && SDL_memcmp(text, name, len) == 0)
            return i;
      }
   }
   return -1;
}

bool steamlib_readable(const void *p)
{
   MEMORY_BASIC_INFORMATION m;

   return VirtualQuery(p, &m, sizeof(m)) && m.State == MEM_COMMIT
      && !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD | PAGE_EXECUTE));
}

#endif
