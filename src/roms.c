#include "roms.h"
#include "disk.h"
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
    * count in a folder of its own). Where a game has several files, the
    * earliest type is the one to start. */
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

/* Under the library's lock. */
static struct
{
   /* What the last scan found. */
   struct rom_option *options;
   int option_count;
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

/* ---- What the window asks ------------------------------------------------ */

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

struct scan
{
   struct found_emulator *emulators;
   int emulator_count;
   struct rom_option *options;
   int option_count;
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
 * it). */
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
   SDL_strlcpy(o->label, emulators[f->emulator].name, sizeof(o->label));
   return o;
}

/* What an emulator can start. */
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
}

/* ---- The scan ------------------------------------------------------------ */

int scan_roms(void)
{
   struct scan sc;
   int i, n;

   SDL_zero(sc);
   find_emulators(&sc);
   make_options(&sc);

   library_lock();
   SDL_free(cfg.options);
   cfg.options = sc.options;
   cfg.option_count = sc.option_count;
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
   n = sc.emulator_count;
   SDL_free(sc.emulators);
   return n;
}
