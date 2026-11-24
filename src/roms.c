#include "roms.h"
#include "disk.h"
#include "json.h"
#include "library.h"
#include "platform.h"

#include <SDL3/SDL.h>

/* Lists in the tables below are separated by '|'. */

#ifdef _WIN32
#define OS(windows, linux) windows
#define CORE_EXT ".dll"
#else
#define OS(windows, linux) linux
#define CORE_EXT ".so"
#endif

struct system
{
   /* Also the game's source, as the window shows it. */
   const char *name;
   /* Folders named for it (library_normalize'd). */
   const char *folders;
   /* File types only it has, and those it shares with others (which only
    * count in a folder of its own, or by what's in the file). Where a game
    * has several files, the earliest type is the one to start. */
   const char *exts, *shared;
   /* RetroArch's cores for it, best first. */
   const char *cores;
};

static const struct system systems[] = {
   { "NES", "nes|famicom|fc|fds|nintendoentertainmentsystem|nintendonintendoentertainmentsystem"
      "|nintendofamilycomputerdisksystem|nintendofamicomdisksystem",
      "nes|fds|unf|unif", "zip|7z", "mesen|nestopia|fceumm|quicknes" },
   { "SNES", "snes|sfc|superfamicom|supernintendo|supernes|supernintendoentertainmentsystem"
      "|nintendosupernintendoentertainmentsystem",
      "sfc|smc|fig|swc", "zip|7z", "snes9x|bsnes|bsnes_hd_beta|mesen-s" },
   { "N64", "n64|nintendo64|nintendonintendo64", "z64|n64|v64", "zip|7z",
      "mupen64plus_next|parallel_n64" },
   { "GameCube", "gc|gcn|ngc|gamecube|nintendogamecube", "gcm", "m3u|rvz|iso|gcz|wia|ciso",
      "dolphin" },
   { "Wii", "wii|nintendowii", "wbfs", "m3u|rvz|iso|gcz|wia|ciso", "dolphin" },
   { "Wii U", "wiiu|nintendowiiu", "wua|wux|wud|rpx", "", "" },
   { "Switch", "switch|nsw|nintendoswitch", "xci|nsp", "", "" },
   { "Game Boy", "gb|gameboy|nintendogameboy", "gb", "zip|7z", "gambatte|sameboy|mgba" },
   { "Game Boy Color", "gbc|gameboycolor|nintendogameboycolor", "gbc", "zip|7z",
      "gambatte|sameboy|mgba" },
   { "Game Boy Advance", "gba|gameboyadvance|nintendogameboyadvance", "gba", "zip|7z",
      "mgba|vba_next|vbam|gpsp" },
   { "Nintendo DS", "nds|ds|nintendods|nintendonintendods", "nds|dsi", "zip|7z",
      "melondsds|melonds|desmume" },
   { "Nintendo 3DS", "3ds|n3ds|nintendo3ds|nintendonintendo3ds", "3ds|cci|cxi|zcci|zcxi|3dsx", "",
      "azahar|citra" },
   { "Virtual Boy", "vb|virtualboy|nintendovirtualboy", "", "vb|zip|7z", "mednafen_vb" },
   { "Genesis", "genesis|megadrive|md|segagenesis|segamegadrive|segamegadrivegenesis",
      "gen|smd", "md|bin|zip|7z", "genesis_plus_gx|blastem|picodrive" },
   { "Sega 32X", "32x|sega32x", "32x", "zip|7z", "picodrive" },
   { "Sega CD", "segacd|megacd|segamegacd|segamegacdsegacd", "", "m3u|cue|chd|iso",
      "genesis_plus_gx|picodrive" },
   { "Master System", "sms|mastersystem|segamastersystem|segamastersystemmarkiii", "sms", "zip|7z",
      "genesis_plus_gx|gearsystem|picodrive" },
   { "Game Gear", "gg|gamegear|segagamegear", "", "gg|zip|7z", "genesis_plus_gx|gearsystem" },
   { "Saturn", "saturn|segasaturn", "", "m3u|cue|chd|ccd|iso",
      "mednafen_saturn|kronos|yabasanshiro|yabause" },
   { "Dreamcast", "dc|dreamcast|segadreamcast", "gdi|cdi", "m3u|cue|chd", "flycast" },
   { "PlayStation", "ps|psx|ps1|psone|playstation|sonyplaystation", "",
      "m3u|cue|chd|pbp|ccd|mds|ecm|iso",
      "swanstation|mednafen_psx_hw|mednafen_psx|pcsx_rearmed" },
   { "PlayStation 2", "ps2|playstation2|sonyplaystation2", "zso", "m3u|chd|cso|iso|bin|gz",
      "pcsx2" },
   { "PlayStation 3", "ps3|playstation3|sonyplaystation3", "", "iso", "" },
   { "PSP", "psp|playstationportable|sonyplaystationportable", "", "chd|cso|pbp|iso", "ppsspp" },
   { "Xbox", "xbox|microsoftxbox", "", "iso|xiso", "" },
   { "Xbox 360", "xbox360|x360|microsoftxbox360", "xex", "iso|zar", "" },
   { "PC Engine", "pce|pcengine|tg16|turbografx16|turbografx|pcenginecd|tgcd"
      "|necpcengineturbografx16|necpcengine|necpcenginecdturbografxcd", "",
      "m3u|pce|cue|chd|zip|7z", "mednafen_pce|mednafen_pce_fast|mednafen_supergrafx" },
   { "Neo Geo Pocket", "ngp|ngpc|neogeopocket|neogeopocketcolor|snkneogeopocket"
      "|snkneogeopocketcolor", "ngp", "ngc|zip|7z", "mednafen_ngp|race" },
   { "WonderSwan", "ws|wsc|wonderswan|wonderswancolor|bandaiwonderswan|bandaiwonderswancolor",
      "wsc", "ws|zip|7z", "mednafen_wswan" },
   { "Atari 2600", "atari2600|2600", "a26", "bin|zip|7z", "stella|stella2014" },
   { "Atari 7800", "atari7800|7800", "a78", "bin|zip|7z", "prosystem" },
   { "Atari Lynx", "lynx|atarilynx", "lnx", "zip|7z", "handy|mednafen_lynx" },
   { "Arcade", "arcade|mame|fbneo|fba|finalburnneo|neogeo|cps1|cps2|cps3", "", "zip|7z",
      "fbneo|mame|mame2003_plus" },
};

#define SYSTEMS ((int)SDL_arraysize(systems))

struct emulator
{
   const char *name;
   /* Its program's names ('*' globs, .exe left out), best first. */
   const char *exes;
   /* {rom}: the ROM; {dir} and {stem}: its folder and its name without the
    * type; {core}: RetroArch's core. All quoted. */
   const char *args;
   /* NULL: those whose cores it has (RetroArch). */
   const char *systems;
};

/* Where several run a system, the first is used until one is picked: each
 * system's own before those for many. */
static const struct emulator emulators[] = {
   { "Dolphin", OS("Dolphin", "dolphin-emu|org.DolphinEmu.dolphin-emu|dolphin*.AppImage"),
      "-b -e {rom}", "GameCube|Wii" },
   { "PCSX2", OS("pcsx2-qt*", "pcsx2-qt|net.pcsx2.PCSX2|pcsx2*.AppImage"),
      "-batch -fullscreen -- {rom}", "PlayStation 2" },
   { "DuckStation", OS("duckstation-qt*",
         "duckstation-qt|org.duckstation.DuckStation|duckstation*.AppImage"),
      "-batch -fullscreen -- {rom}", "PlayStation" },
   { "RPCS3", OS("rpcs3", "rpcs3|net.rpcs3.RPCS3|rpcs3*.AppImage"), "--no-gui {rom}",
      "PlayStation 3" },
   { "PPSSPP", OS("PPSSPPWindows64|PPSSPPWindows|PPSSPPQt|PPSSPPSDL",
         "PPSSPPSDL|PPSSPPQt|ppsspp|org.ppsspp.PPSSPP|PPSSPP*.AppImage"),
      "--fullscreen {rom}", "PSP" },
   { "Cemu", OS("Cemu", "cemu|info.cemu.Cemu|Cemu*.AppImage"), "-f -g {rom}", "Wii U" },
   { "Ryujinx", OS("Ryujinx", "Ryujinx|io.github.ryubing.Ryujinx|org.ryujinx.Ryujinx"
         "|Ryujinx*.AppImage"), "--fullscreen {rom}", "Switch" },
   { "Eden", OS("eden", "eden|dev.eden_emu.eden|Eden*.AppImage"), "-f -g {rom}", "Switch" },
   { "Citron", OS("citron", "citron|Citron*.AppImage"), "-f -g {rom}", "Switch" },
   { "Sudachi", OS("sudachi", "sudachi|Sudachi*.AppImage"), "-f -g {rom}", "Switch" },
   { "suyu", OS("suyu", "suyu|suyu*.AppImage"), "-f -g {rom}", "Switch" },
   { "yuzu", OS("yuzu", "yuzu|org.yuzu_emu.yuzu|yuzu*.AppImage"), "-f -g {rom}", "Switch" },
   { "Azahar", OS("azahar", "azahar|org.azahar_emu.Azahar|azahar*.AppImage"), "{rom}",
      "Nintendo 3DS" },
   { "Lime3DS", OS("lime3ds|lime3ds-gui|lime-qt", "lime3ds|lime3ds-gui|lime3ds*.AppImage"),
      "{rom}", "Nintendo 3DS" },
   { "Citra", OS("citra-qt", "citra-qt|org.citra_emu.citra|citra-qt*.AppImage"), "{rom}",
      "Nintendo 3DS" },
   { "melonDS", OS("melonDS", "melonDS|net.kuribo64.melonDS|melonDS*.AppImage"), "-f {rom}",
      "Nintendo DS" },
   { "DeSmuME", OS("DeSmuME*", "desmume|org.desmume.DeSmuME"), "{rom}", "Nintendo DS" },
   { "mGBA", OS("mGBA", "mgba-qt|io.mgba.mGBA|mGBA*.AppImage"), "-f {rom}",
      "Game Boy Advance|Game Boy Color|Game Boy" },
   { "SameBoy", OS("sameboy", "sameboy|io.github.sameboy.SameBoy"), "{rom}",
      "Game Boy|Game Boy Color" },
   { "Snes9x", OS("snes9x-x64|snes9x*", "snes9x-gtk|com.snes9x.Snes9x|Snes9x*.AppImage"),
      OS("-fullscreen {rom}", "{rom}"), "SNES" },
   { "bsnes", OS("bsnes*", "bsnes|dev.bsnes.bsnes"), "--fullscreen {rom}", "SNES" },
   { "FCEUX", OS("fceux64|fceux", "fceux|com.fceux.fceux"), "{rom}", "NES" },
   { "Nestopia", OS("nestopia", "nestopia|ca._0ldsk00l.Nestopia"), "{rom}", "NES" },
   { "RMG", OS("RMG", "RMG|com.github.Rosalie241.RMG|RMG*.AppImage"), "-f -q {rom}", "N64" },
   { "simple64", OS("simple64-gui", "simple64-gui|io.github.simple64.simple64"), "{rom}", "N64" },
   { "Project64", OS("Project64", ""), "{rom}", "N64" },
   { "Mupen64Plus", OS("mupen64plus-ui-console|mupen64plus", "mupen64plus"), "--fullscreen {rom}",
      "N64" },
   { "BlastEm", OS("blastem", "blastem"), "-f {rom}", "Genesis" },
   { "Flycast", OS("flycast", "flycast|org.flycast.Flycast|flycast*.AppImage"), "{rom}",
      "Dreamcast" },
   { "redream", OS("redream", "redream"), "{rom}", "Dreamcast" },
   { "xemu", OS("xemu", "xemu|app.xemu.xemu|xemu*.AppImage"), "-full-screen -dvd_path {rom}",
      "Xbox" },
   { "Xenia Canary", OS("xenia_canary", ""), "--fullscreen=true {rom}", "Xbox 360" },
   { "Xenia", OS("xenia", ""), "--fullscreen=true {rom}", "Xbox 360" },
   { "Stella", OS("Stella", "stella|io.github.stella_emu.Stella"), "-fullscreen 1 {rom}",
      "Atari 2600" },
   { "Mesen", OS("Mesen", "Mesen|mesen|Mesen*.AppImage"), "--fullscreen {rom}",
      "NES|SNES|Game Boy|Game Boy Color|Game Boy Advance|PC Engine|Master System|Game Gear"
      "|WonderSwan" },
   { "ares", OS("ares", "ares|dev.ares.ares"), "--fullscreen {rom}",
      "N64|NES|SNES|Genesis|Sega 32X|Sega CD|Master System|Game Gear|PC Engine|Neo Geo Pocket"
      "|WonderSwan" },
   { "Mednafen", OS("mednafen", "mednafen"), "-video.fs 1 {rom}",
      "Saturn|PlayStation|PC Engine|Atari Lynx|Neo Geo Pocket|WonderSwan|Virtual Boy" },
   { "MAME", OS("mame|mame64", "mame|org.mamedev.MAME"), "-rompath {dir} {stem}", "Arcade" },
   { "RetroArch", OS("retroarch", "retroarch|org.libretro.RetroArch|RetroArch*.AppImage"),
      "-f -L {core} {rom}", NULL },
};

#define EMULATORS ((int)SDL_arraysize(emulators))

/* Arcade boards' own chips: they come with the games but don't start. */
static const char arcade_bios[] = "neogeo|qsound|qsound_hle|pgm|decocass|skns|nmk004|ym2608|isgsm"
   "|bubsys|cchip|coleco|naomi|naomi2|naomigd|hikaru|awbios|stvbios|konamigx|megaplay|megatech"
   "|playch10|midssio|taitofx1|tps|cpzn1|cpzn2|neocdz";

/* An emulator the user added: one FuseBox knows, or (with the system it's
 * for) any other. */
struct added
{
   char exe[1024];
   char system[32];
};

/* Under the library's lock, but for file. */
static struct
{
   char file[1100];
   struct rom_folder *folders;
   int folder_count;
   /* Found folders the user removed: not listed again. */
   char (*ignored)[1024];
   int ignored_count;
   struct added *added;
   int added_count;
   /* What the last scan found. */
   struct rom_option *options;
   int option_count;
   int unknown;
   char found[512];
} cfg;

/* ---- Small helpers ------------------------------------------------------ */

/* Room for one more at the end of a list (list: where its pointer is),
 * zeroed; NULL if there's none. The room doubles at each power of two. */
static void *grow(void *list, int *count, size_t item)
{
   char *items;

   /* Copied: the pointer is of the list's own type, not char's. */
   SDL_memcpy(&items, list, sizeof(items));
   if ((*count & (*count - 1)) == 0)
   {
      if (!(items = SDL_realloc(items, item * (size_t)(*count ? *count * 2 : 1))))
         return NULL;
      SDL_memcpy(list, &items, sizeof(items));
   }
   items += item * (size_t)(*count)++;
   SDL_memset(items, 0, item);
   return items;
}

static const char *base_name(const char *path)
{
   const char *p, *name = path;

   for (p = path; *p; p++)
#ifdef _WIN32
      if (*p == '/' || *p == '\\')
#else
      if (*p == '/')
#endif
         name = p + 1;
   return name;
}

static void parent_dir(const char *path, char *dst, size_t size)
{
   size_t n = (size_t)(base_name(path) - path);

   SDL_strlcpy(dst, path, SDL_min(size, n + 1));
   /* "C:\" and "/" keep theirs. */
   if (n > 1 && n < size && dst[n - 2] != ':')
      dst[n - 1] = '\0';
}

static void join(char *dst, size_t size, const char *dir, const char *name)
{
   size_t n = SDL_strlen(dir);
   SDL_snprintf(dst, size, "%s%s%s", dir,
         n && (dir[n - 1] == '/' || dir[n - 1] == '\\') ? "" : PATH_SEP, name);
}

/* As the file system compares paths. */
static bool same_path(const char *a, const char *b)
{
#ifdef _WIN32
   for (; *a && *b; a++, b++)
      if ((*a == '/' ? '\\' : SDL_tolower((unsigned char)*a))
            != (*b == '/' ? '\\' : SDL_tolower((unsigned char)*b)))
         return false;
   return !*a && !*b;
#else
   return SDL_strcmp(a, b) == 0;
#endif
}

/* Item i of a list in the tables above; false past its end. */
static bool item(const char *list, int i, char *dst, size_t size)
{
   const char *end;

   while (i-- > 0)
   {
      if (!(list = SDL_strchr(list, '|')))
         return false;
      list++;
   }
   end = SDL_strchr(list, '|');
   SDL_strlcpy(dst, list, end ? SDL_min(size, (size_t)(end - list + 1)) : size);
   return dst[0] != '\0';
}

/* Where name is in a list, -1 if it isn't. */
static int in_list(const char *list, const char *name)
{
   char text[128];
   int i;

   for (i = 0; item(list, i, text, sizeof(text)); i++)
      if (SDL_strcasecmp(text, name) == 0)
         return i;
   return -1;
}

static int system_by_name(const char *name)
{
   int i;

   for (i = 0; i < SYSTEMS; i++)
      if (SDL_strcmp(systems[i].name, name) == 0)
         return i;
   return -1;
}

static int system_by_folder(const char *name)
{
   char norm[256];
   int i;

   library_normalize(name, norm, sizeof(norm));
   for (i = 0; norm[0] && i < SYSTEMS; i++)
      if (in_list(systems[i].folders, norm) >= 0)
         return i;
   return -1;
}

/* ---- roms.json ---------------------------------------------------------- */

static void save(void)
{
   char tmp[1200];
   SDL_IOStream *io;
   int i, n;

   SDL_snprintf(tmp, sizeof(tmp), "%s.tmp", cfg.file);
   if (!cfg.file[0] || !(io = SDL_IOFromFile(tmp, "wb")))
   {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Can't write %s: %s", tmp, SDL_GetError());
      return;
   }
   /* Found folders are found again. */
   SDL_IOprintf(io, "{\n  \"folders\": [");
   for (i = n = 0; i < cfg.folder_count; i++)
      if (!cfg.folders[i].found)
      {
         SDL_IOprintf(io, "%s\n    { \"path\": ", n++ ? "," : "");
         json_write_string(io, cfg.folders[i].path);
         SDL_IOprintf(io, " }");
      }
   SDL_IOprintf(io, "\n  ],\n  \"ignored\": [");
   for (i = 0; i < cfg.ignored_count; i++)
   {
      SDL_IOprintf(io, "%s\n    ", i ? "," : "");
      json_write_string(io, cfg.ignored[i]);
   }
   SDL_IOprintf(io, "\n  ],\n  \"emulators\": [");
   for (i = 0; i < cfg.added_count; i++)
   {
      SDL_IOprintf(io, "%s\n    { \"exe\": ", i ? "," : "");
      json_write_string(io, cfg.added[i].exe);
      SDL_IOprintf(io, ", \"system\": ");
      json_write_string(io, cfg.added[i].system);
      SDL_IOprintf(io, " }");
   }
   SDL_IOprintf(io, "\n  ]\n}\n");
   if (!SDL_CloseIO(io) || !SDL_RenamePath(tmp, cfg.file))
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Can't write %s: %s", cfg.file, SDL_GetError());
}

static struct json *list_of(const struct json *root, const char *key)
{
   struct json *list = json_get(root, key);
   return list ? list->child : NULL;
}

void roms_init(const char *config_dir)
{
   struct json *root, *o;
   char *text;

   SDL_CreateDirectory(config_dir);
   SDL_snprintf(cfg.file, sizeof(cfg.file), "%s" PATH_SEP "roms.json", config_dir);
   if (!(text = SDL_LoadFile(cfg.file, NULL)))
      return;
   root = json_parse(text);
   SDL_free(text);
   for (o = list_of(root, "folders"); o; o = o->next)
   {
      struct rom_folder *f;
      if (json_str(o, "path", "")[0] && (f = grow(&cfg.folders, &cfg.folder_count, sizeof(*f))))
         SDL_strlcpy(f->path, json_str(o, "path", ""), sizeof(f->path));
   }
   for (o = list_of(root, "ignored"); o; o = o->next)
   {
      char *path;
      if (o->type == JSON_STRING && (path = grow(&cfg.ignored, &cfg.ignored_count, 1024)))
         SDL_strlcpy(path, o->str, 1024);
   }
   for (o = list_of(root, "emulators"); o; o = o->next)
   {
      struct added *a;
      if (json_str(o, "exe", "")[0] && (a = grow(&cfg.added, &cfg.added_count, sizeof(*a))))
      {
         SDL_strlcpy(a->exe, json_str(o, "exe", ""), sizeof(a->exe));
         SDL_strlcpy(a->system, json_str(o, "system", ""), sizeof(a->system));
      }
   }
   json_free(root);
}

/* ---- What the window asks ------------------------------------------------ */

int roms_folder_count(void)
{
   return cfg.folder_count;
}

const struct rom_folder *roms_folder(int i)
{
   return i >= 0 && i < cfg.folder_count ? &cfg.folders[i] : NULL;
}

static bool ignored(const char *path)
{
   int i;

   for (i = 0; i < cfg.ignored_count; i++)
      if (same_path(cfg.ignored[i], path))
         return true;
   return false;
}

void roms_add_folder(const char *path)
{
   struct rom_folder *f;
   int i;

   for (i = 0; i < cfg.ignored_count; i++)
      if (same_path(cfg.ignored[i], path))
      {
         SDL_memmove(cfg.ignored + i, cfg.ignored + i + 1,
               sizeof(*cfg.ignored) * (size_t)(cfg.ignored_count - i - 1));
         cfg.ignored_count--;
         break;
      }
   for (i = 0; i < cfg.folder_count; i++)
      if (same_path(cfg.folders[i].path, path))
      {
         cfg.folders[i].found = false;
         save();
         return;
      }
   if ((f = grow(&cfg.folders, &cfg.folder_count, sizeof(*f))))
      SDL_strlcpy(f->path, path, sizeof(f->path));
   save();
}

void roms_remove_folder(int i)
{
   char *path;

   if (!roms_folder(i))
      return;
   /* Whoever added it, the scan might find it. */
   if (!ignored(cfg.folders[i].path)
         && (path = grow(&cfg.ignored, &cfg.ignored_count, sizeof(*cfg.ignored))))
      SDL_strlcpy(path, cfg.folders[i].path, sizeof(*cfg.ignored));
   SDL_memmove(cfg.folders + i, cfg.folders + i + 1,
         sizeof(*cfg.folders) * (size_t)(cfg.folder_count - i - 1));
   cfg.folder_count--;
   save();
}

int roms_unknown(void)
{
   return cfg.unknown;
}

int roms_options(const struct game *g, const struct rom_option **out, int max)
{
   int i, n = 0, system = g->rom[0] ? system_by_name(g->source) : -1;

   for (i = 0; i < cfg.option_count; i++)
      if (cfg.options[i].system == system)
      {
         if (n < max)
            out[n] = &cfg.options[i];
         n++;
      }
   return n;
}

/* g NULL: a game not in the library yet, with no choice. */
static const struct rom_option *chosen_of(const struct rom_option *options, int count, int system,
      const struct game *g)
{
   const struct rom_option *first = NULL;
   int i;

   for (i = 0; i < count; i++)
   {
      if (options[i].system != system)
         continue;
      if (!first)
         first = &options[i];
      if (g && g->emulator[0] && same_path(g->emulator, options[i].exe)
            && SDL_strcmp(g->core, options[i].core) == 0)
         return &options[i];
   }
   return first;
}

const struct rom_option *roms_chosen(const struct game *g)
{
   return chosen_of(cfg.options, cfg.option_count, g->rom[0] ? system_by_name(g->source) : -1, g);
}

/* A quoted argument: on Linux what a shell would take for its own is
 * escaped (Steam starts shortcuts through one). */
static bool append_quoted(char *dst, size_t size, const char *text)
{
   size_t n = SDL_strlen(dst);

   if (n + 1 < size)
      dst[n++] = '"';
   for (; *text && n + 3 < size; text++)
   {
#ifndef _WIN32
      if (*text == '"' || *text == '\\' || *text == '$' || *text == '`')
         dst[n++] = '\\';
#endif
      dst[n++] = *text;
   }
   if (*text || n + 1 >= size)
   {
      dst[n] = '\0';
      return false;
   }
   dst[n++] = '"';
   dst[n] = '\0';
   return true;
}

static bool make_args(const struct rom_option *o, const char *rom, char *dst, size_t size)
{
   char dir[1024], stem[256], *dot;
   const char *t = o->emulator >= 0 ? emulators[o->emulator].args : "{rom}";
   const struct
   {
      const char *token, *value;
   } values[] = { { "{rom}", rom }, { "{dir}", dir }, { "{stem}", stem },
      { "{core}", o->core_path } };
   size_t i, n;

   parent_dir(rom, dir, sizeof(dir));
   SDL_strlcpy(stem, base_name(rom), sizeof(stem));
   if ((dot = SDL_strrchr(stem, '.')))
      *dot = '\0';
   dst[0] = '\0';
   while (*t)
   {
      for (i = 0; i < SDL_arraysize(values); i++)
         if (SDL_strncmp(t, values[i].token, SDL_strlen(values[i].token)) == 0)
            break;
      if (i < SDL_arraysize(values))
      {
         if (!append_quoted(dst, size, values[i].value))
            return false;
         t += SDL_strlen(values[i].token);
         continue;
      }
      n = SDL_strlen(dst);
      if (n + 1 >= size)
         return false;
      dst[n] = *t++;
      dst[n + 1] = '\0';
   }
   return true;
}

/* How the game is started with it; false where that gets too long. */
static bool set_emulator(struct game *g, const struct rom_option *o)
{
   char args[1024];

   if (!make_args(o, g->rom, args, sizeof(args)))
      return false;
   SDL_strlcpy(g->args, args, sizeof(g->args));
   SDL_strlcpy(g->exe, o->exe, sizeof(g->exe));
   parent_dir(o->exe, g->start_dir, sizeof(g->start_dir));
   return true;
}

void roms_choose(struct game *g, const struct rom_option *option)
{
   if (!set_emulator(g, option))
      return;
   g->no_emulator = false;
   SDL_strlcpy(g->emulator, option->exe, sizeof(g->emulator));
   SDL_strlcpy(g->core, option->core, sizeof(g->core));
   library_changed();
}

const char *roms_emulators(void)
{
   return cfg.found;
}

/* ---- Emulators ---------------------------------------------------------- */

struct found_emulator
{
   int emulator;
   /* Which of its names the exe has (the lowest in a folder is used). */
   int rank;
   char exe[1024];
};

struct rom
{
   /* Its game's id: the system and the name, so it stays the same game
    * when its folder moves or its file type changes. */
   char id[256];
   char path[1024];
   /* The file's name without type and disc, for the game's name. */
   char stem[256];
   int system;
   /* The lowest is the one to start, where a game has several files. */
   int rank;
};

struct scan
{
   struct found_emulator *emulators;
   int emulator_count;
   char (*folders)[1024];
   int folder_count;
   struct rom_option *options;
   int option_count;
   struct rom *roms;
   int rom_count;
   int unknown;
};

/* The emulator a program's name is of, -1 if none. */
static int emulator_by_exe(const char *name, int *rank)
{
   char stem[256], pattern[128];
   int e;

   SDL_strlcpy(stem, name, sizeof(stem));
#ifdef _WIN32
   {
      size_t n = SDL_strlen(stem);
      if (n < 5 || SDL_strcasecmp(stem + n - 4, ".exe") != 0)
         return -1;
      stem[n - 4] = '\0';
   }
#endif
   for (e = 0; e < EMULATORS; e++)
      for (*rank = 0; item(emulators[e].exes, *rank, pattern, sizeof(pattern)); (*rank)++)
         if (disk_match(stem, pattern))
            return e;
   return -1;
}

static void add_emulator(struct scan *sc, const char *dir, const char *name)
{
   struct found_emulator *f;
   char path[1100], other[1024];
   int rank, i, e = emulator_by_exe(name, &rank);

   if (e < 0)
      return;
   join(path, sizeof(path), dir, name);
   if (SDL_strlen(path) >= sizeof(f->exe))
      return;
   for (i = 0; i < sc->emulator_count; i++)
   {
      f = &sc->emulators[i];
      if (f->emulator != e)
         continue;
      parent_dir(f->exe, other, sizeof(other));
      /* Several builds side by side (64-bit, 32-bit): one is enough. */
      if (same_path(other, dir))
      {
         if (rank < f->rank)
         {
            f->rank = rank;
            SDL_strlcpy(f->exe, path, sizeof(f->exe));
         }
         return;
      }
   }
   if (!(f = grow(&sc->emulators, &sc->emulator_count, sizeof(*f))))
      return;
   f->emulator = e;
   f->rank = rank;
   SDL_strlcpy(f->exe, path, sizeof(f->exe));
}

static void found_folder(struct scan *sc, const char *path)
{
   char *f;
   int i;

   if (SDL_strlen(path) >= 1024)
      return;
   for (i = 0; i < sc->folder_count; i++)
      if (same_path(sc->folders[i], path))
         return;
   if ((f = grow(&sc->folders, &sc->folder_count, sizeof(*sc->folders))))
      SDL_strlcpy(f, path, sizeof(*sc->folders));
}

/* Whether the folder's name says there are emulators further in. */
static bool emulator_folder(const char *name)
{
   static const char *const names[] = { "*emu*", "*retro*", "*dolphin*", "*pcsx2*",
      "*duckstation*", "*rpcs3*", "*ppsspp*", "*ryujinx*", "*yuzu*", "*citra*", "*azahar*",
      "*melonds*", "*mgba*", "*snes9x*", "*mesen*", "*project64*", "*flycast*", "*xenia*", "*mame*",
      "stella*", "ares*", "bsnes*" };
   size_t i;

   for (i = 0; i < SDL_arraysize(names); i++)
      if (disk_match(name, names[i]))
         return true;
   return false;
}

/* Emulators in dir and the folders in it (further in those named for
 * it), and folders named ROMs on the way. */
static void walk(struct scan *sc, const char *dir, int depth, bool named)
{
   struct listing l;
   int i;

   disk_list(dir, &l);
   for (i = 0; i < l.count; i++)
   {
      char sub[1100];
      bool in;

      if (!l.dirs[i])
      {
         add_emulator(sc, dir, l.names[i]);
         continue;
      }
      /* RetroArch's cheats are in folders named for emulators. */
      if (l.names[i][0] == '.' || l.names[i][0] == '$'
            || SDL_strcasecmp(l.names[i], "cheats") == 0)
         continue;
      join(sub, sizeof(sub), dir, l.names[i]);
      if (SDL_strcasecmp(l.names[i], "roms") == 0)
      {
         found_folder(sc, sub);
         continue;
      }
      in = named || emulator_folder(l.names[i]);
      /* One look into any folder; further only into those named for it,
       * and Games as another place to start from. */
      if (!named && depth == 0 && SDL_strlen(sub) < 200 && disk_match(l.names[i], "*games*"))
         walk(sc, sub, 0, false);
      else if (depth == 0 || (in && depth < 4))
         walk(sc, sub, depth + 1, in);
   }
   disk_list_free(&l);
}

#ifdef _WIN32

static void walk_env(struct scan *sc, const char *var, const char *sub)
{
   const char *base = SDL_getenv_unsafe(var);
   char path[1100];

   if (!base)
      return;
   SDL_snprintf(path, sizeof(path), "%s%s", base, sub);
   walk(sc, path, 0, false);
}

static void find_emulators(struct scan *sc)
{
   char path[64];
   int i;

   for (i = 0; i < 26; i++)
   {
      SDL_snprintf(path, sizeof(path), "%c:\\", 'A' + i);
      if (!disk_fixed_drive(path))
         continue;
      walk(sc, path, 0, false);
      /* Steam has emulators too, RetroArch among them. */
      SDL_snprintf(path, sizeof(path), "%c:\\SteamLibrary\\steamapps\\common", 'A' + i);
      walk(sc, path, 0, false);
   }
   walk_env(sc, "ProgramFiles", "");
   walk_env(sc, "ProgramFiles(x86)", "");
   walk_env(sc, "ProgramFiles(x86)", "\\Steam\\steamapps\\common");
   walk_env(sc, "LOCALAPPDATA", "");
   walk_env(sc, "LOCALAPPDATA", "\\Programs");
   walk_env(sc, "APPDATA", "");
   walk_env(sc, "APPDATA", "\\EmuDeck\\Emulators");
   walk_env(sc, "USERPROFILE", "");
   walk_env(sc, "USERPROFILE", "\\Desktop");
   walk_env(sc, "USERPROFILE", "\\Documents");
   walk_env(sc, "USERPROFILE", "\\Downloads");
}

#else

static void find_emulators(struct scan *sc)
{
   static const char *const bins[] = { "/usr/bin", "/usr/local/bin", "/usr/games",
      "/var/lib/flatpak/exports/bin", "/snap/bin" };
   static const char *const homes[] = { ".local/bin", ".local/share/flatpak/exports/bin" };
   char home[1024], path[1100];
   struct listing l;
   size_t i;
   int n;

   platform_home(home, sizeof(home));
   for (i = 0; i < SDL_arraysize(bins) + SDL_arraysize(homes); i++)
   {
      if (i < SDL_arraysize(bins))
         SDL_strlcpy(path, bins[i], sizeof(path));
      else
         join(path, sizeof(path), home, homes[i - SDL_arraysize(bins)]);
      disk_list(path, &l);
      for (n = 0; n < l.count; n++)
         if (!l.dirs[n])
            add_emulator(sc, path, l.names[n]);
      disk_list_free(&l);
   }
   walk(sc, home, 0, false);
   walk(sc, "/opt", 0, false);
   join(path, sizeof(path), home, ".local/share/Steam/steamapps/common");
   walk(sc, path, 0, false);
   /* SteamOS's memory cards. */
   walk(sc, "/run/media", 1, true);
}

#endif

/* RetroArch's folder of cores: what its settings say, else the usual one. */
static void cores_dir(const char *exe, char *dst, size_t size)
{
   char dir[1024], settings[1100], *text, *line, *end;

   parent_dir(exe, dir, sizeof(dir));
#ifdef _WIN32
   join(dst, size, dir, "cores");
   join(settings, sizeof(settings), dir, "retroarch.cfg");
#else
   {
      char home[1024];
      platform_home(home, sizeof(home));
      if (SDL_strstr(exe, "flatpak"))
         SDL_snprintf(settings, sizeof(settings),
               "%s/.var/app/org.libretro.RetroArch/config/retroarch/retroarch.cfg", home);
      else if (SDL_strstr(exe, "steamapps"))
         join(settings, sizeof(settings), dir, "retroarch.cfg");
      else
         SDL_snprintf(settings, sizeof(settings), "%s/.config/retroarch/retroarch.cfg", home);
      parent_dir(settings, dst, size);
      SDL_strlcat(dst, "/cores", size);
   }
#endif
   if (!(text = SDL_LoadFile(settings, NULL)))
      return;
   if ((line = SDL_strstr(text, "libretro_directory = \"")) && (line == text || line[-1] == '\n'))
   {
      line += SDL_strlen("libretro_directory = \"");
      if ((end = SDL_strchr(line, '"')))
         *end = '\0';
      /* ":\cores" is relative to RetroArch, "~" to the home folder. */
      if (line[0] == ':')
         SDL_snprintf(dst, size, "%s%s", dir, line + 1);
      else if (line[0] == '~')
      {
         char home[1024];
         platform_home(home, sizeof(home));
         SDL_snprintf(dst, size, "%s%s", home, line + 1);
      }
      else if (line[0] && SDL_strcmp(line, "default") != 0)
         SDL_strlcpy(dst, line, size);
   }
   SDL_free(text);
}

static struct rom_option *add_option(struct scan *sc, int system, const struct found_emulator *f)
{
   struct rom_option *o = grow(&sc->options, &sc->option_count, sizeof(*o));

   if (!o)
      return NULL;
   o->system = system;
   o->emulator = f->emulator;
   SDL_strlcpy(o->exe, f->exe, sizeof(o->exe));
   if (f->emulator >= 0)
      SDL_strlcpy(o->label, emulators[f->emulator].name, sizeof(o->label));
   else
   {
      char *dot;
      SDL_strlcpy(o->label, base_name(f->exe), sizeof(o->label));
      if ((dot = SDL_strrchr(o->label, '.')) && dot > o->label)
         *dot = '\0';
   }
   return o;
}

/* What a known emulator can start. */
static void emulator_options(struct scan *sc, const struct found_emulator *f)
{
   const struct emulator *e = &emulators[f->emulator];
   char cores[1100], name[64], path[1200];
   int s, c;

   if (e->systems)
   {
      for (s = 0; s < SYSTEMS; s++)
         if (in_list(e->systems, systems[s].name) >= 0)
            add_option(sc, s, f);
      return;
   }
   cores_dir(f->exe, cores, sizeof(cores));
   for (s = 0; s < SYSTEMS; s++)
      for (c = 0; item(systems[s].cores, c, name, sizeof(name)); c++)
      {
         struct rom_option *o;
         SDL_snprintf(path, sizeof(path), "%s" PATH_SEP "%s_libretro" CORE_EXT, cores, name);
         if (!platform_exists(path) || !(o = add_option(sc, s, f)))
            continue;
         SDL_snprintf(o->label, sizeof(o->label), "%s (%s)", e->name, name);
         SDL_strlcpy(o->core, name, sizeof(o->core));
         SDL_strlcpy(o->core_path, path, sizeof(o->core_path));
      }
}

/* The first option with that program for the system, NULL if none. */
static const struct rom_option *option_of(const struct rom_option *options, int count, int system,
      const char *exe)
{
   int i;

   for (i = 0; i < count; i++)
      if (options[i].system == system && same_path(options[i].exe, exe))
         return &options[i];
   return NULL;
}

/* Those of the scan's options that aren't offered yet, after the others: the
 * first of a system's stays its first. */
static int offer_new(const struct scan *sc)
{
   int i, k, added = 0;

   for (i = 0; i < sc->option_count; i++)
   {
      struct rom_option *n;

      for (k = 0; k < cfg.option_count; k++)
         if (cfg.options[k].system == sc->options[i].system
               && same_path(cfg.options[k].exe, sc->options[i].exe)
               && SDL_strcmp(cfg.options[k].core, sc->options[i].core) == 0)
            break;
      if (k == cfg.option_count && (n = grow(&cfg.options, &cfg.option_count, sizeof(*n))))
      {
         *n = sc->options[i];
         added++;
      }
   }
   return added;
}

bool roms_add_emulator(struct game *g, const char *exe)
{
   const struct rom_option *o;
   struct found_emulator f;
   struct scan sc;
   struct added *a;
   int system = system_by_name(g->source);

   if (!g->rom[0] || system < 0 || SDL_strlen(exe) >= sizeof(f.exe))
      return false;
   /* Already known for the system: only picked. */
   if (!(o = option_of(cfg.options, cfg.option_count, system, exe)))
   {
      SDL_zero(sc);
      SDL_zero(f);
      SDL_strlcpy(f.exe, exe, sizeof(f.exe));
      if ((f.emulator = emulator_by_exe(base_name(exe), &f.rank)) >= 0)
         emulator_options(&sc, &f);
      if (!option_of(sc.options, sc.option_count, system, exe))
      {
         /* RetroArch does nothing without a core. */
         if (f.emulator >= 0 && !emulators[f.emulator].systems)
         {
            SDL_free(sc.options);
            return false;
         }
         f.emulator = -1;
         add_option(&sc, system, &f);
      }
      offer_new(&sc);
      SDL_free(sc.options);
      if ((a = grow(&cfg.added, &cfg.added_count, sizeof(*a))))
      {
         SDL_strlcpy(a->exe, exe, sizeof(a->exe));
         if (f.emulator < 0)
            SDL_strlcpy(a->system, systems[system].name, sizeof(a->system));
      }
      save();
      if (!(o = option_of(cfg.options, cfg.option_count, system, exe)))
         return false;
   }
   roms_choose(g, o);
   return true;
}

/* Every way to start each system, in the order of the emulators' table. */
static void make_options(struct scan *sc)
{
   int e, i;

   for (e = 0; e < EMULATORS; e++)
      for (i = 0; i < sc->emulator_count; i++)
      {
         const struct found_emulator *f = &sc->emulators[i];

         if (f->emulator != e)
            continue;
         SDL_Log("Emulator: %s (%s)", emulators[e].name, f->exe);
         emulator_options(sc, f);
      }
   /* Then the user's own, for what they were added for. */
   library_lock();
   for (i = 0; i < cfg.added_count; i++)
   {
      struct found_emulator f;
      int s = system_by_name(cfg.added[i].system);

      if (s < 0 || !platform_exists(cfg.added[i].exe)
            || option_of(sc->options, sc->option_count, s, cfg.added[i].exe))
         continue;
      SDL_zero(f);
      f.emulator = -1;
      SDL_strlcpy(f.exe, cfg.added[i].exe, sizeof(f.exe));
      add_option(sc, s, &f);
   }
   library_unlock();
}

/* ---- ROMs --------------------------------------------------------------- */

static bool read_at(SDL_IOStream *io, Sint64 at, void *dst, size_t size)
{
   return SDL_SeekIO(io, at, SDL_IO_SEEK_SET) == at && SDL_ReadIO(io, dst, size) == size;
}

/* A disc image's system by what's on it: raw ones (a .cue's) have 2352
 * bytes a sector, the data 16 or (PlayStation's) 24 in. */
static int sniff_disc(SDL_IOStream *io, bool raw)
{
   static const Sint64 x360[] = { 0xFDA0000, 0x2090000 };
   static const Sint64 xbox[] = { 0x10000, 0x18310000 };
   Uint8 b[48];
   int i;

   if (!raw)
   {
      if (read_at(io, 0, b, 32))
      {
         if (SDL_memcmp(b + 0x18, "\x5D\x1C\x9E\xA3", 4) == 0)
            return system_by_name("Wii");
         if (SDL_memcmp(b + 0x1C, "\xC2\x33\x9F\x3D", 4) == 0)
            return system_by_name("GameCube");
      }
      if (read_at(io, 0x800, b, 12) && SDL_memcmp(b, "PlayStation3", 12) == 0)
         return system_by_name("PlayStation 3");
      for (i = 0; i < (int)SDL_arraysize(x360); i++)
         if (read_at(io, x360[i], b, 20) && SDL_memcmp(b, "MICROSOFT*XBOX*MEDIA", 20) == 0)
            return system_by_name("Xbox 360");
      for (i = 0; i < (int)SDL_arraysize(xbox); i++)
         if (read_at(io, xbox[i], b, 20) && SDL_memcmp(b, "MICROSOFT*XBOX*MEDIA", 20) == 0)
            return system_by_name("Xbox");
   }
   if (read_at(io, raw ? 16 : 0, b, 16))
   {
      if (SDL_memcmp(b, "SEGA SEGASATURN", 15) == 0)
         return system_by_name("Saturn");
      if (SDL_memcmp(b, "SEGADISCSYSTEM", 14) == 0)
         return system_by_name("Sega CD");
      if (SDL_memcmp(b, "SEGA SEGAKATANA", 15) == 0)
         return system_by_name("Dreamcast");
   }
   /* ISO 9660's volume, in sector 16, says whose disc it is. */
   for (i = raw ? 16 : 0; i <= (raw ? 24 : 0); i += 8)
   {
      if (!read_at(io, (raw ? 2352 : 2048) * 16 + i, b, sizeof(b))
            || SDL_memcmp(b + 1, "CD001", 5) != 0)
         continue;
      if (SDL_memcmp(b + 8, "PSP GAME", 8) == 0)
         return system_by_name("PSP");
      /* PlayStation 2 games' images are .iso, PlayStation's .bin. */
      if (SDL_memcmp(b + 8, "PLAYSTATION", 11) == 0)
         return system_by_name(raw ? "PlayStation" : "PlayStation 2");
   }
   return -1;
}

/* A .cue's first track. */
static int sniff_cue(const char *path)
{
   char dir[1024], bin[1300], name[256];
   char *text = SDL_LoadFile(path, NULL), *p, *end;
   SDL_IOStream *io;
   int system = -1;

   if (!text)
      return -1;
   if ((p = SDL_strstr(text, "FILE \"")) && (end = SDL_strchr(p + 6, '"')))
   {
      *end = '\0';
      SDL_strlcpy(name, p + 6, sizeof(name));
      parent_dir(path, dir, sizeof(dir));
      join(bin, sizeof(bin), dir, name);
      if ((io = SDL_IOFromFile(bin, "rb")))
      {
         system = sniff_disc(io, true);
         SDL_CloseIO(io);
      }
   }
   SDL_free(text);
   return system;
}

/* A file's system by what's in it, -1 if that doesn't tell. */
static int sniff(const char *path, const char *ext)
{
   SDL_IOStream *io;
   Uint8 b[4];
   int system = -1;

   if (SDL_strcasecmp(ext, "cue") == 0)
      return sniff_cue(path);
   if (in_list("gcz|ciso", ext) >= 0)
      return system_by_name("GameCube");
   if (in_list("iso|rvz|wia", ext) < 0 || !(io = SDL_IOFromFile(path, "rb")))
      return -1;
   if (SDL_strcasecmp(ext, "iso") == 0)
      system = sniff_disc(io, false);
   /* Dolphin's: the second header starts with the disc's type. */
   else if (read_at(io, 0x48, b, sizeof(b)))
      system = system_by_name(b[3] == 2 ? "Wii" : "GameCube");
   SDL_CloseIO(io);
   return system;
}

/* The file's system: its folder's where that has files of its type, else
 * by the type alone, else by what's in it. *ext_rank gets how soon among
 * the system's types it comes. */
static int rom_system(const char *path, const char *ext, int folder_system, int *ext_rank)
{
   int i, n;

   if (folder_system >= 0)
   {
      const struct system *s = &systems[folder_system];
      if ((n = in_list(s->exts, ext)) >= 0 || (n = in_list(s->shared, ext)) >= 0)
      {
         *ext_rank = n;
         return folder_system;
      }
   }
   for (i = 0; i < SYSTEMS; i++)
      if ((*ext_rank = in_list(systems[i].exts, ext)) >= 0)
         return i;
   if ((i = sniff(path, ext)) >= 0)
      *ext_rank = SDL_max(0, in_list(systems[i].shared, ext));
   return i;
}

/* Takes "(Disc 2)" and the like out of a name; the disc's number, 0 if
 * it has none. */
static int take_disc(char *stem)
{
   static const char *const marks[] = { "(Disc ", "(Disk ", "(CD ", "(CD" };
   size_t i;

   for (i = 0; i < SDL_arraysize(marks); i++)
   {
      char *p = SDL_strcasestr(stem, marks[i]), *end;
      int c, disc;

      if (!p)
         continue;
      c = SDL_tolower((unsigned char)p[SDL_strlen(marks[i])]);
      disc = c >= '0' && c <= '9' ? SDL_atoi(p + SDL_strlen(marks[i]))
         : c >= 'a' && c <= 'd' ? c - 'a' + 1 : 0;
      if (!disc || !(end = SDL_strchr(p, ')')))
         continue;
      end++;
      while (*end == ' ')
         end++;
      while (p > stem && p[-1] == ' ')
         p--;
      /* What follows keeps its space: "Game (Disc 1) (Rev 1)". */
      if (*end && p > stem)
         *p++ = ' ';
      SDL_memmove(p, end, SDL_strlen(end) + 1);
      return disc;
   }
   return 0;
}

static struct rom *add_rom(struct scan *sc, int system, const char *path, const char *stem,
      int rank)
{
   struct rom *r;

   /* A path too long to keep can't be started. */
   if (SDL_strlen(path) >= sizeof(r->path)
         || !(r = grow(&sc->roms, &sc->rom_count, sizeof(*r))))
      return NULL;
   r->system = system;
   r->rank = rank;
   SDL_strlcpy(r->path, path, sizeof(r->path));
   SDL_strlcpy(r->stem, stem, sizeof(r->stem));
   /* A name too long for an id keeps its start, and a crc of it all. */
   if (SDL_snprintf(r->id, sizeof(r->id), "rom:%s/%s", systems[system].name, stem)
         >= (int)sizeof(r->id))
   {
      char tail[16];
      size_t at = sizeof(r->id) - 1;
      SDL_snprintf(tail, sizeof(tail), "-%08x", (unsigned)fb_crc32(stem, SDL_strlen(stem)));
      at -= SDL_strlen(tail);
      while (at && ((Uint8)r->id[at] & 0xC0) == 0x80)
         at--;
      SDL_strlcpy(r->id + at, tail, sizeof(r->id) - at);
   }
   return r;
}

/* A Switch file that's an update or DLC: its title id doesn't end in 000. */
static bool switch_extra(const char *stem)
{
   const char *p;
   int n;

   if (SDL_strcasestr(stem, "[UPD]") || SDL_strcasestr(stem, "[DLC"))
      return true;
   for (p = stem; (p = SDL_strchr(p, '[')); p++)
   {
      for (n = 0; SDL_isxdigit((unsigned char)p[1 + n]); n++)
         ;
      if (n == 16 && p[17] == ']')
         return SDL_strncmp(p + 14, "000", 3) != 0;
   }
   return false;
}

static void rom_file(struct scan *sc, const char *dir, const char *name, int folder_system)
{
   char path[1500], stem[256], parent[1300];
   const char *dot = SDL_strrchr(name, '.'), *ext;
   int system, ext_rank = 0, disc;
   size_t n;

   if (!dot || dot == name || name[0] == '.')
      return;
   ext = dot + 1;
   SDL_strlcpy(stem, name, SDL_min(sizeof(stem), (size_t)(dot - name + 1)));
   /* "Game.nkit.iso" */
   n = SDL_strlen(stem);
   if (n > 5 && SDL_strcasecmp(stem + n - 5, ".nkit") == 0)
      stem[n - 5] = '\0';
   if (SDL_strncasecmp(stem, "readme", 6) == 0 || SDL_strcasestr(stem, "[BIOS]")
         || SDL_strcasestr(stem, "(Track "))
      return;
   join(path, sizeof(path), dir, name);
   if ((system = rom_system(path, ext, folder_system, &ext_rank)) < 0)
   {
      /* Only what could be a game is worth a word about. */
      if (in_list("iso|cue|chd|cso|pbp|m3u|zip|7z", ext) >= 0)
         sc->unknown++;
      return;
   }
   if (SDL_strcmp(systems[system].name, "Switch") == 0 && switch_extra(stem))
      return;
   if (SDL_strcmp(systems[system].name, "Arcade") == 0 && in_list(arcade_bios, stem) >= 0)
      return;
   /* Installed games' programs, named by their folder: an Xbox 360 game's
    * default.xex, a Wii U game's code\<program>.rpx. */
   if (SDL_strcasecmp(ext, "xex") == 0)
   {
      if (SDL_strcasecmp(stem, "default") != 0)
         return;
      SDL_strlcpy(stem, base_name(dir), sizeof(stem));
   }
   else if (SDL_strcasecmp(ext, "rpx") == 0)
   {
      if (SDL_strcasecmp(base_name(dir), "code") != 0)
         return;
      parent_dir(dir, parent, sizeof(parent));
      SDL_strlcpy(stem, base_name(parent), sizeof(stem));
   }
   disc = take_disc(stem);
   add_rom(sc, system, path, stem, disc * 100 + ext_rank);
}

static Uint32 le(const Uint8 *d, int bytes)
{
   Uint32 v = 0;

   while (bytes--)
      v = v << 8 | d[bytes];
   return v;
}

/* A text in a PARAM.SFO (PlayStation 3's description of a game). */
static bool sfo_text(const Uint8 *d, size_t size, const char *key, char *dst, size_t dst_size)
{
   Uint32 count, i;
   char *p;

   dst[0] = '\0';
   if (size < 20 || SDL_memcmp(d, "\0PSF", 4) != 0)
      return false;
   count = le(d + 16, 4);
   for (i = 0; i < count && 20 + ((size_t)i + 1) * 16 <= size; i++)
   {
      const Uint8 *e = d + 20 + (size_t)i * 16;
      size_t k = (size_t)le(d + 8, 4) + le(e, 2), v = (size_t)le(d + 12, 4) + le(e + 12, 4);
      size_t len = le(e + 4, 4);

      /* SDL_LoadFile's data ends with a 0 past size. */
      if (k >= size || SDL_strcmp((const char *)d + k, key) != 0)
         continue;
      if (v >= size || len > size - v)
         return false;
      SDL_strlcpy(dst, (const char *)d + v, SDL_min(dst_size, len + 1));
      for (p = dst; *p; p++)
         if ((Uint8)*p < ' ')
            *p = ' ';
      return dst[0] != '\0';
   }
   return false;
}

/* A PlayStation 3 game's folder (a disc's, or an installed one's): adds
 * it and returns true, so it isn't looked into for anything else. */
static bool ps3_folder(struct scan *sc, const char *dir, const struct listing *l)
{
   char base[1300], sfo[1400], boot[1400], id[64], title[256], category[16];
   bool disc = false, installed = false;
   struct rom *r;
   Uint8 *data;
   size_t size;
   int i;

   for (i = 0; i < l->count; i++)
   {
      disc |= l->dirs[i] && SDL_strcasecmp(l->names[i], "PS3_GAME") == 0;
      installed |= l->dirs[i] && SDL_strcasecmp(l->names[i], "USRDIR") == 0;
   }
   if (!disc && !installed)
      return false;
   if (disc)
      join(base, sizeof(base), dir, "PS3_GAME");
   else
      SDL_strlcpy(base, dir, sizeof(base));
   join(sfo, sizeof(sfo), base, "PARAM.SFO");
   SDL_snprintf(boot, sizeof(boot), "%s" PATH_SEP "USRDIR" PATH_SEP "EBOOT.BIN", base);
   if (!(data = SDL_LoadFile(sfo, &size)))
      return disc;
   sfo_text(data, size, "CATEGORY", category, sizeof(category));
   /* Discs and installed games; not updates, DLC or saves. */
   if ((SDL_strcmp(category, "DG") == 0 || SDL_strcmp(category, "HG") == 0)
         && platform_exists(boot) && sfo_text(data, size, "TITLE", title, sizeof(title))
         && sfo_text(data, size, "TITLE_ID", id, sizeof(id))
         && (r = add_rom(sc, system_by_name("PlayStation 3"), boot, title, 0)))
      /* Its own id is surer than its title. */
      SDL_snprintf(r->id, sizeof(r->id), "rom:PlayStation 3/%s", id);
   SDL_free(data);
   return true;
}

/* More than anyone has, and an end to folders linked in circles. */
#define ROMS_MAX 20000

static void rom_dir(struct scan *sc, const char *dir, int folder_system, int depth)
{
   struct listing l;
   int i;

   disk_list(dir, &l);
   if (!ps3_folder(sc, dir, &l))
      for (i = 0; i < l.count && sc->rom_count < ROMS_MAX; i++)
      {
         char sub[1200];
         int s;

         if (!l.dirs[i])
         {
            rom_file(sc, dir, l.names[i], folder_system);
            continue;
         }
         if (l.names[i][0] == '.' || depth >= 6 || SDL_strcasecmp(l.names[i], "bios") == 0)
            continue;
         join(sub, sizeof(sub), dir, l.names[i]);
         s = system_by_folder(l.names[i]);
         rom_dir(sc, sub, s >= 0 ? s : folder_system, depth + 1);
      }
   disk_list_free(&l);
}

static int SDLCALL compare_roms(const void *a, const void *b)
{
   const struct rom *ra = a, *rb = b;
   int c = SDL_strcasecmp(ra->id, rb->id);

   if (!c)
      c = ra->rank - rb->rank;
   return c ? c : SDL_strcmp(ra->path, rb->path);
}

/* "Legend of Zelda, The - A Link to the Past (USA) [!]" -> "The Legend of
 * Zelda: A Link to the Past". */
static void clean_name(const char *stem, char *dst, size_t size)
{
   char text[300], moved[300], *p, *sep;
   size_t n = 0, depth = 0;
   const char *s;

   for (s = stem; *s && n + 1 < sizeof(text); s++)
   {
      if (*s == '(' || *s == '[')
         depth++;
      else if ((*s == ')' || *s == ']') && depth)
         depth--;
      else if (!depth)
      {
         char c = *s == '_' ? ' ' : *s;
         if (c != ' ' || (n && text[n - 1] != ' '))
            text[n++] = c;
      }
   }
   while (n && text[n - 1] == ' ')
      n--;
   text[n] = '\0';
   if (!n)
   {
      SDL_strlcpy(dst, stem, size);
      return;
   }
   sep = SDL_strstr(text, " - ");
   if ((p = SDL_strstr(text, ", The")) && p > text && (!p[5] || p + 5 == sep))
   {
      *p = '\0';
      SDL_strlcpy(moved, "The ", sizeof(moved));
      SDL_strlcat(moved, text, sizeof(moved));
      SDL_strlcat(moved, p + 5, sizeof(moved));
      SDL_strlcpy(text, moved, sizeof(text));
      sep = SDL_strstr(text, " - ");
   }
   /* File names can't have a colon. */
   if (sep)
   {
      sep[0] = ':';
      SDL_memmove(sep + 1, sep + 2, SDL_strlen(sep + 2) + 1);
   }
   SDL_strlcpy(dst, text, size);
}

/* ---- The scan ------------------------------------------------------------ */

static bool name_taken(char (*names)[256], int count, const char *name)
{
   int i;

   for (i = 0; i < count; i++)
      if (SDL_strcasecmp(names[i], name) == 0)
         return true;
   return false;
}

/* A name for each ROM, no two the same: Steam tells shortcuts apart by
 * the program (the same emulator) and the name. A game keeps the name it
 * has; a new one that's taken gets its system, or its region and all. */
static void name_roms(const struct scan *sc, char (*names)[256], char (*taken)[256])
{
   int i, n, index, count = 0;

   library_lock();
   for (i = 0; i < sc->rom_count; i++)
   {
      names[i][0] = '\0';
      if ((index = library_find(sc->roms[i].id)) < 0)
         continue;
      SDL_strlcpy(names[i], library_game(index)->own_name, 256);
      if (names[i][0])
         SDL_strlcpy(taken[count++], names[i], 256);
      /* What the user renamed it to is taken too. */
      if (library_game(index)->custom_name[0])
         SDL_strlcpy(taken[count++], library_game(index)->custom_name, 256);
   }
   library_unlock();
   for (i = 0; i < sc->rom_count; i++)
   {
      const struct rom *r = &sc->roms[i];
      char base[256], name[256];

      if (names[i][0])
         continue;
      clean_name(r->stem, base, sizeof(base));
      SDL_strlcpy(name, base, sizeof(name));
      /* Another system's game, or another region or version of one. */
      for (n = 0; n < sc->rom_count; n++)
         if (n != i && sc->roms[n].system == r->system && SDL_strcasecmp(names[n], base) == 0)
            break;
      if (n == sc->rom_count && name_taken(taken, count, name))
         SDL_snprintf(name, sizeof(name), "%.200s (%s)", base, systems[r->system].name);
      if (name_taken(taken, count, name))
         SDL_strlcpy(name, r->stem, sizeof(name));
      for (n = 2; name_taken(taken, count, name); n++)
         SDL_snprintf(name, sizeof(name), "%.200s (%s) %d", base, systems[r->system].name, n);
      SDL_strlcpy(names[i], name, 256);
      SDL_strlcpy(taken[count++], name, 256);
   }
}

static void add_games(struct scan *sc)
{
   size_t size = 256 * (size_t)SDL_max(1, sc->rom_count);
   char (*names)[256] = SDL_malloc(size), (*taken)[256] = SDL_malloc(size * 2);
   struct game *g = SDL_malloc(sizeof(*g));
   int i, index;

   if (names && taken && g)
   {
      name_roms(sc, names, taken);
      for (i = 0; i < sc->rom_count; i++)
      {
         const struct rom *r = &sc->roms[i];
         const struct rom_option *o;

         SDL_zerop(g);
         SDL_strlcpy(g->id, r->id, sizeof(g->id));
         SDL_strlcpy(g->name, names[i], sizeof(g->name));
         SDL_strlcpy(g->source, systems[r->system].name, sizeof(g->source));
         SDL_strlcpy(g->rom, r->path, sizeof(g->rom));
         g->scan.installed = true;
         /* Steam's own defaults for a new shortcut. */
         g->steam_overlay = g->steam_input = true;
         library_lock();
         index = library_find(r->id);
         o = chosen_of(sc->options, sc->option_count, r->system, library_game(index));
         if (!o || !set_emulator(g, o))
         {
            g->no_emulator = true;
            /* What it last had, so a shortcut in Steam stays its own. */
            if (index >= 0)
            {
               SDL_strlcpy(g->exe, library_game(index)->exe, sizeof(g->exe));
               SDL_strlcpy(g->start_dir, library_game(index)->start_dir, sizeof(g->start_dir));
               SDL_strlcpy(g->args, library_game(index)->args, sizeof(g->args));
            }
         }
         library_add(g);
         library_unlock();
      }
   }
   SDL_free(g);
   SDL_free(taken);
   SDL_free(names);
}

/* The found folders into the list: new ones added, those gone dropped. */
static void update_folders(const struct scan *sc)
{
   int i, n;

   for (n = 0; n < cfg.folder_count; n++)
   {
      bool still = !cfg.folders[n].found;
      for (i = 0; i < sc->folder_count && !still; i++)
         still = same_path(cfg.folders[n].path, sc->folders[i]);
      if (still)
         continue;
      SDL_memmove(cfg.folders + n, cfg.folders + n + 1,
            sizeof(*cfg.folders) * (size_t)(cfg.folder_count - n - 1));
      cfg.folder_count--;
      n--;
   }
   for (i = 0; i < sc->folder_count; i++)
   {
      struct rom_folder *f;
      bool known = ignored(sc->folders[i]);
      for (n = 0; n < cfg.folder_count && !known; n++)
         known = same_path(cfg.folders[n].path, sc->folders[i]);
      if (!known && (f = grow(&cfg.folders, &cfg.folder_count, sizeof(*f))))
      {
         SDL_strlcpy(f->path, sc->folders[i], sizeof(f->path));
         f->found = true;
      }
   }
}

/* The emulators on this computer and what they can start. */
static void scan_options(struct scan *sc)
{
   char dir[1024];
   int i;

   find_emulators(sc);
   library_lock();
   for (i = 0; i < cfg.added_count; i++)
      if (!cfg.added[i].system[0] && platform_exists(cfg.added[i].exe))
      {
         parent_dir(cfg.added[i].exe, dir, sizeof(dir));
         add_emulator(sc, dir, base_name(cfg.added[i].exe));
      }
   library_unlock();
   make_options(sc);
}

int roms_find_emulators(void)
{
   struct scan sc;
   int n;

   SDL_zero(sc);
   scan_options(&sc);
   library_lock();
   n = offer_new(&sc);
   library_unlock();
   SDL_free(sc.options);
   SDL_free(sc.emulators);
   SDL_free(sc.folders);
   return n;
}

int scan_roms(void)
{
   struct scan sc;
   struct rom_folder *folders = NULL;
   int i, n, count = 0;
   char dir[1024], path[1100];

   SDL_zero(sc);
   scan_options(&sc);

   /* RPCS3 keeps installed games itself; others often a folder of ROMs. */
   for (i = 0; i < sc.emulator_count; i++)
   {
      parent_dir(sc.emulators[i].exe, dir, sizeof(dir));
      if (SDL_strcmp(emulators[sc.emulators[i].emulator].name, "RPCS3") == 0)
      {
#ifdef _WIN32
         SDL_snprintf(path, sizeof(path), "%s\\dev_hdd0\\game", dir);
#else
         char home[1024];
         platform_home(home, sizeof(home));
         SDL_snprintf(path, sizeof(path), "%s%s/rpcs3/dev_hdd0/game", home,
               SDL_strstr(sc.emulators[i].exe, "flatpak") ? "/.var/app/net.rpcs3.RPCS3/config"
               : "/.config");
#endif
         if (platform_exists(path))
            found_folder(&sc, path);
      }
      join(path, sizeof(path), dir, "Roms");
      if (platform_exists(path))
         found_folder(&sc, path);
   }

   library_lock();
   update_folders(&sc);
   for (i = 0; i < cfg.folder_count; i++)
   {
      struct rom_folder *f = grow(&folders, &count, sizeof(*f));
      if (f)
         *f = cfg.folders[i];
   }
   library_unlock();
   for (i = 0; i < count; i++)
      rom_dir(&sc, folders[i].path, system_by_folder(base_name(folders[i].path)), 0);
   SDL_free(folders);

   /* One file per game, the one to start it with. */
   if (sc.rom_count)
      SDL_qsort(sc.roms, (size_t)sc.rom_count, sizeof(*sc.roms), compare_roms);
   for (i = n = 0; i < sc.rom_count; i++)
      if (!n || SDL_strcasecmp(sc.roms[i].id, sc.roms[n - 1].id) != 0)
      {
         if (i != n)
            sc.roms[n] = sc.roms[i];
         n++;
      }
   sc.rom_count = n;
   add_games(&sc);

   library_lock();
   SDL_free(cfg.options);
   cfg.options = sc.options;
   cfg.option_count = sc.option_count;
   cfg.unknown = sc.unknown;
   cfg.found[0] = '\0';
   for (n = 0; n < EMULATORS; n++)
      for (i = 0; i < sc.emulator_count; i++)
         if (sc.emulators[i].emulator == n)
         {
            SDL_strlcat(cfg.found, cfg.found[0] ? ", " : "", sizeof(cfg.found));
            SDL_strlcat(cfg.found, emulators[n].name, sizeof(cfg.found));
            break;
         }
   library_unlock();
   n = sc.rom_count;
   SDL_free(sc.roms);
   SDL_free(sc.emulators);
   SDL_free(sc.folders);
   return n;
}
