#include "rahash.h"

#include <SDL3/SDL.h>
#include <rc_consoles.h>
#include <rc_hash.h>
#include <stdio.h>
#include <zlib.h>

/* The biggest ROM unpacked from a .zip: more than any cartridge. */
#define MAX_UNPACKED (512u << 20)

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

/* --- Zip: the ROM in it, which is its biggest file --- */

static Uint32 le16(const Uint8 *p)
{
   return (Uint32)p[0] | (Uint32)p[1] << 8;
}

static Uint32 le32(const Uint8 *p)
{
   return le16(p) | le16(p + 2) << 16;
}

static bool read_at(SDL_IOStream *io, Sint64 at, void *dst, size_t size)
{
   return SDL_SeekIO(io, at, SDL_IO_SEEK_SET) == at && SDL_ReadIO(io, dst, size) == size;
}

static Uint8 *inflate_all(Uint8 *src, Uint32 size, Uint32 out_size)
{
   Uint8 *out = SDL_malloc(out_size);
   z_stream z;
   bool ok = false;

   SDL_zero(z);
   /* Raw deflate: a zip has headers of its own. */
   if (out && inflateInit2(&z, -MAX_WBITS) == Z_OK)
   {
      z.next_in = src;
      z.avail_in = size;
      z.next_out = out;
      z.avail_out = out_size;
      ok = inflate(&z, Z_FINISH) == Z_STREAM_END && z.total_out == out_size;
      inflateEnd(&z);
   }
   if (!ok)
   {
      SDL_free(out);
      return NULL;
   }
   return out;
}

static Uint8 *unzip_rom(const char *path, size_t *size, char *name, size_t name_size)
{
   SDL_IOStream *io = SDL_IOFromFile(path, "rb");
   Uint8 *tail = NULL, *dir = NULL, *packed = NULL, *rom = NULL, head[30];
   Sint64 total = io ? SDL_GetIOSize(io) : 0;
   Uint32 n, i, count, dir_size, dir_at, best = 0, best_at = 0, best_packed = 0, method = 0;

   if (total < 22)
      goto done;
   /* The end record, behind a comment of up to 64 KB. */
   n = (Uint32)SDL_min(total, 65557);
   if (!(tail = SDL_malloc(n)) || !read_at(io, total - n, tail, n))
      goto done;
   for (i = n - 22; i > 0 && le32(tail + i) != 0x06054b50; i--)
      ;
   if (le32(tail + i) != 0x06054b50)
      goto done;
   count = le16(tail + i + 10);
   dir_size = le32(tail + i + 12);
   dir_at = le32(tail + i + 16);
   if ((Sint64)dir_at + dir_size > total || !(dir = SDL_malloc((size_t)dir_size + 1))
         || !read_at(io, dir_at, dir, dir_size))
      goto done;
   for (i = 0; count && (Uint64)i + 46 <= dir_size && le32(dir + i) == 0x02014b50; count--)
   {
      Uint32 unpacked = le32(dir + i + 24), name_len = le16(dir + i + 28);

      if ((Uint64)i + 46 + name_len > dir_size)
         break;
      if (unpacked > best && unpacked <= MAX_UNPACKED)
      {
         method = le16(dir + i + 10);
         best_packed = le32(dir + i + 20);
         best = unpacked;
         best_at = le32(dir + i + 42);
         SDL_strlcpy(name, (const char *)dir + i + 46, SDL_min(name_size, (size_t)name_len + 1));
      }
      i += 46 + name_len + le16(dir + i + 30) + le16(dir + i + 32);
   }
   if (!best || !read_at(io, best_at, head, sizeof(head)) || le32(head) != 0x04034b50)
      goto done;
   if (method == 0)
      best_packed = best;
   if ((Sint64)best_packed > total || !(packed = SDL_malloc((size_t)best_packed + 1))
         || !read_at(io, (Sint64)best_at + 30 + le16(head + 26) + le16(head + 28), packed,
            best_packed))
      goto done;
   if (method == 0)
   {
      rom = packed;
      packed = NULL;
   }
   else if (method == 8)
      rom = inflate_all(packed, best_packed, best);
   *size = best;
done:
   if (io)
      SDL_CloseIO(io);
   SDL_free(tail);
   SDL_free(dir);
   SDL_free(packed);
   return rom;
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
   const char *ext = extension(rom);
   rc_hash_iterator_t it;
   Uint8 *unpacked = NULL;
   char name[1024];
   size_t size = 0;
   bool ok;

   hash[0] = '\0';
   if (!console || !ra_hashable(rom))
      return false;
   SDL_strlcpy(name, rom, sizeof(name));
   /* Arcade games go by their .zip's name. */
   if (console != RC_CONSOLE_ARCADE && SDL_strcasecmp(ext, "zip") == 0
         && !(unpacked = unzip_rom(rom, &size, name, sizeof(name))))
      return false;
   hash_abort = abort;
   rc_hash_initialize_iterator(&it, name, unpacked, size);
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
   SDL_free(unpacked);
   if (!ok || (abort && SDL_GetAtomicInt(abort)))
      hash[0] = '\0';
   return hash[0] != '\0';
}
