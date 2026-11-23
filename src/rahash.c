#include "rahash.h"

#include <SDL3/SDL.h>
#include <rc_consoles.h>
#include <rc_hash.h>
#include <stdio.h>

static const struct
{
   /* As roms.c names its systems. */
   const char *system;
   unsigned console;
} consoles[] = {
   { "NES", RC_CONSOLE_NINTENDO },
   { "SNES", RC_CONSOLE_SUPER_NINTENDO },
   { "N64", RC_CONSOLE_NINTENDO_64 },
   { "GameCube", RC_CONSOLE_GAMECUBE },
   { "Wii", RC_CONSOLE_WII },
   { "Game Boy", RC_CONSOLE_GAMEBOY },
   { "Game Boy Color", RC_CONSOLE_GAMEBOY_COLOR },
   { "Game Boy Advance", RC_CONSOLE_GAMEBOY_ADVANCE },
   { "Nintendo DS", RC_CONSOLE_NINTENDO_DS },
   { "Virtual Boy", RC_CONSOLE_VIRTUAL_BOY },
   { "Genesis", RC_CONSOLE_MEGA_DRIVE },
   { "Sega 32X", RC_CONSOLE_SEGA_32X },
   { "Sega CD", RC_CONSOLE_SEGA_CD },
   { "Master System", RC_CONSOLE_MASTER_SYSTEM },
   { "Game Gear", RC_CONSOLE_GAME_GEAR },
   { "Saturn", RC_CONSOLE_SATURN },
   { "Dreamcast", RC_CONSOLE_DREAMCAST },
   { "PlayStation", RC_CONSOLE_PLAYSTATION },
   { "PlayStation 2", RC_CONSOLE_PLAYSTATION_2 },
   { "PSP", RC_CONSOLE_PSP },
   { "PC Engine", RC_CONSOLE_PC_ENGINE },
   { "Neo Geo Pocket", RC_CONSOLE_NEOGEO_POCKET },
   { "WonderSwan", RC_CONSOLE_WONDERSWAN },
   { "Atari 2600", RC_CONSOLE_ATARI_2600 },
   { "Atari 7800", RC_CONSOLE_ATARI_7800 },
   { "Atari Lynx", RC_CONSOLE_ATARI_LYNX },
   { "Arcade", RC_CONSOLE_ARCADE },
};

static const char *extension(const char *path)
{
   const char *dot = SDL_strrchr(path, '.');

   if (!dot || SDL_strchr(dot, '/') || SDL_strchr(dot, '\\'))
      return "";
   return dot + 1;
}

static bool any_of(const char *ext, const char *const *list)
{
   for (; *list; list++)
      if (SDL_strcasecmp(ext, *list) == 0)
         return true;
   return false;
}

unsigned ra_console(const char *system, const char *rom)
{
   static const char *const discs[] = { "cue", "chd", "m3u", NULL };
   const char *ext = extension(rom);
   size_t i;

   for (i = 0; i < SDL_arraysize(consoles); i++)
      if (SDL_strcmp(consoles[i].system, system) == 0)
         break;
   if (i == SDL_arraysize(consoles))
      return 0;
   /* One system here, two there. */
   if (consoles[i].console == RC_CONSOLE_NINTENDO && SDL_strcasecmp(ext, "fds") == 0)
      return RC_CONSOLE_FAMICOM_DISK_SYSTEM;
   if (consoles[i].console == RC_CONSOLE_NINTENDO_DS && SDL_strcasecmp(ext, "dsi") == 0)
      return RC_CONSOLE_NINTENDO_DSI;
   if (consoles[i].console == RC_CONSOLE_PC_ENGINE && any_of(ext, discs))
      return RC_CONSOLE_PC_ENGINE_CD;
   return consoles[i].console;
}

/* --- Files --- */

/* Its reader is passed no data of ours; one ROM is hashed at a time. */
static SDL_AtomicInt *hash_abort;

static void *hash_open(const char *path)
{
   return SDL_IOFromFile(path, "rb");
}

static void hash_seek(void *handle, int64_t offset, int origin)
{
   SDL_SeekIO(handle, offset, origin == SEEK_CUR ? SDL_IO_SEEK_CUR
         : origin == SEEK_END ? SDL_IO_SEEK_END : SDL_IO_SEEK_SET);
}

static int64_t hash_tell(void *handle)
{
   return SDL_TellIO(handle);
}

static size_t hash_read(void *handle, void *buffer, size_t bytes)
{
   if (hash_abort && SDL_GetAtomicInt(hash_abort))
      return 0;
   return SDL_ReadIO(handle, buffer, bytes);
}

static void hash_close(void *handle)
{
   SDL_CloseIO(handle);
}

static void hash_message(const char *message, const rc_hash_iterator_t *iterator)
{
   SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "rc_hash: %s", message);
}

bool ra_hashable(const char *rom)
{
   /* Compressed: rcheevos would hash the wrong bytes, or none. */
   static const char *const cannot[] = { "chd", "rvz", "wia", "gcz", "wbfs", "ciso", "cso", "zso",
      "pbp", "7z", "ecm", "gz", "mds", "ccd", NULL };

   return !any_of(extension(rom), cannot);
}

bool ra_hash(unsigned console, const char *rom, char hash[RA_HASH_SIZE], SDL_AtomicInt *abort)
{
   rc_hash_iterator_t it;
   bool ok;

   hash[0] = '\0';
   if (!console || !ra_hashable(rom))
      return false;
   /* Arcade games go by their .zip's name. */
   if (console != RC_CONSOLE_ARCADE && SDL_strcasecmp(extension(rom), "zip") == 0)
      return false;
   hash_abort = abort;
   rc_hash_initialize_iterator(&it, rom, NULL, 0);
   it.callbacks.filereader.open = hash_open;
   it.callbacks.filereader.seek = hash_seek;
   it.callbacks.filereader.tell = hash_tell;
   it.callbacks.filereader.read = hash_read;
   it.callbacks.filereader.close = hash_close;
   /* CD images (cue, gdi) go through the file reader above. */
   rc_hash_get_default_cdreader(&it.callbacks.cdreader);
   it.callbacks.error_message = hash_message;
   ok = rc_hash_generate(hash, console, &it) != 0;
   rc_hash_destroy_iterator(&it);
   hash_abort = NULL;
   if (!ok || (abort && SDL_GetAtomicInt(abort)))
      hash[0] = '\0';
   return hash[0] != '\0';
}
