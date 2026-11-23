#include "disk.h"
#include "library.h"
#include "platform.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define EXE_GLOB "*.exe"
#else
#define EXE_GLOB "*.x86_64"
#endif

/* What gives a folder away as a game: files (or folders, ending in '/')
 * that games' engines and libraries leave next to their exe. */
static const char *const fingerprints[] = {
   "UnityPlayer.dll", "UnityPlayer.so", "GameAssembly.dll",
   "Engine/",                          /* Unreal */
   "*.pck",                            /* Godot */
   "data.win",                         /* GameMaker */
   "MonoGame.Framework.dll", "FNA.dll", "Content/*.xnb", "Content/*/*.xnb", "Microsoft.Xna.Framework*.dll",
   "SDL2.dll", "SDL3.dll", "libSDL2*.so*",
   "fmod*.dll", "bink2w64.dll", "binkw32.dll", "binkw64.dll", "OpenAL32.dll",
   "steam_api.dll", "steam_api64.dll", "EOSSDK-Win*.dll", "Galaxy*.dll",
   "renpy/", "www/js/rpg_core.js", "nw.pak",
};

/* Exes that are never the game itself. */
static const char *const not_games[] = {
   "unins*", "*setup*", "*install*", "*redist*", "*crash*", "*report*", "*patcher*",
   "*update*", "*helper*", "dxsetup*", "UE4PrereqSetup*", "UEPrereqSetup*", "*benchmark*",
   "java.exe", "javaw.exe", "Runner.exe", "*server*", "*config*", "*editor*", "*7z*",
};

/* Top-level folders that hold no games of their own (or only Steam's,
 * which Steam has). */
static const char *const skip_dirs[] = {
   "Windows", "Program Files", "Program Files (x86)", "ProgramData", "Users", "PerfLogs",
   "Recovery", "System Volume Information", "$*", "msys64", "Steam", "SteamLibrary",
   "XboxGames", "WpSystem", "Battle.net", "Epic Games", "GOG Galaxy", "Common Files",
   "WindowsApps", "Microsoft*", "Windows*", "Intel", "AMD", "NVIDIA*",
   /* Big personal folders: slow to look through, and not where games go. */
   "Documents", "Downloads", "Desktop", "Videos", "Pictures", "Music", "OneDrive*",
};

struct found
{
   char exe[1024];
   char dir[1024];
};

static struct
{
   struct found *list;
   int count, alloc;
   int added;
   int verbose;
} st;

static bool match(const char *name, const char *pattern)
{
   /* Case-insensitive glob: '*' only. */
   while (*pattern)
   {
      if (*pattern == '*')
      {
         pattern++;
         if (!*pattern)
            return true;
         for (; *name; name++)
            if (match(name, pattern))
               return true;
         return false;
      }
      if (!*name || SDL_tolower((unsigned char)*name) != SDL_tolower((unsigned char)*pattern))
         return false;
      name++;
      pattern++;
   }
   return !*name;
}

static bool match_any(const char *name, const char *const *patterns, size_t n)
{
   size_t i;

   for (i = 0; i < n; i++)
      if (match(name, patterns[i]))
         return true;
   return false;
}

static const char *base_name(const char *path)
{
   const char *a = SDL_strrchr(path, '/'), *b = SDL_strrchr(path, '\\');
   const char *p = a > b ? a : b;
   return p ? p + 1 : path;
}

static void parent_dir(const char *path, char *dst, size_t size)
{
   const char *name = base_name(path);
   SDL_strlcpy(dst, path, SDL_min(size, (size_t)(name - path) + (name > path ? 0 : 1)));
   if (name > path)
      dst[name - path - 1] = '\0';
}

/* Whether path is dir or inside it (case-insensitive, either separator). */
static bool within(const char *path, const char *dir)
{
   size_t n = SDL_strlen(dir);

   while (n && (dir[n - 1] == '\\' || dir[n - 1] == '/'))
      n--;
   return n && SDL_strncasecmp(path, dir, n) == 0
      && (path[n] == '\0' || path[n] == '\\' || path[n] == '/');
}

static bool is_steam(const char *path)
{
   return SDL_strcasestr(path, "steamapps") != NULL;
}

/* Whether a launcher scan or this one has the folder or anything in it
 * (or something it's inside) already. */
static bool known(const char *dir)
{
   int i;
   bool seen = false;

   for (i = 0; i < st.count && !seen; i++)
      seen = within(st.list[i].dir, dir) || within(dir, st.list[i].dir);
   library_lock();
   for (i = 0; i < library_count() && !seen; i++)
   {
      const struct game *g = library_game(i);
      if (g->installed && g->start_dir[0] && SDL_strncmp(g->id, "disk:", 5) != 0)
         seen = within(g->start_dir, dir) || within(dir, g->start_dir);
   }
   library_unlock();
   return seen;
}

/* A name for the game from its exe: the exe's own name when it reads like
 * a title, else the nearest folder that does. */
static void game_name(const char *exe, char *dst, size_t size)
{
   static const char *const generic[] = {
      "bin", "binaries", "win32", "win64", "x64", "x86", "app", "game", "client", "data",
      "base*", "shipping*", "versions", "instances", "*.*.*",
   };
   char path[1024], stem[256], *dot, *p;
   bool lower = true;

   SDL_strlcpy(stem, base_name(exe), sizeof(stem));
   if ((dot = SDL_strrchr(stem, '.')))
      *dot = '\0';
   for (p = stem; *p; p++)
      if (*p >= 'A' && *p <= 'Z')
         lower = false;
   /* "game.exe", "client.exe", "retroarch.exe": a folder says it better. */
   if (lower || match_any(stem, generic, SDL_arraysize(generic)) || SDL_strstr(stem, "-Win64"))
   {
      SDL_strlcpy(path, exe, sizeof(path));
      while (true)
      {
         char up[1024];
         parent_dir(path, up, sizeof(up));
         if (!up[0] || SDL_strcmp(up, path) == 0 || SDL_strlen(base_name(up)) == 0
               || SDL_strchr(base_name(up), ':'))
            break;
         SDL_strlcpy(path, up, sizeof(path));
         if (!match_any(base_name(path), generic, SDL_arraysize(generic)))
         {
            SDL_strlcpy(stem, base_name(path), sizeof(stem));
            break;
         }
      }
   }
   /* "Zelda_3", "RetroArch-Bomba-GUI", "Minecraft.Windows": spaces read
    * better, and a platform tacked on the end isn't part of the name. */
   for (p = stem; *p; p++)
      if (*p == '_' || *p == '.' || (*p == '-' && p[1] != ' ' && p > stem && p[-1] != ' '))
         *p = ' ';
   {
      static const char *const tails[] = { " Windows", " Win64", " Win32", " x64", " Shipping" };
      size_t i, n = SDL_strlen(stem);
      for (i = 0; i < SDL_arraysize(tails); i++)
      {
         size_t tn = SDL_strlen(tails[i]);
         if (n > tn && SDL_strcasecmp(stem + n - tn, tails[i]) == 0)
            stem[n -= tn] = '\0';
      }
   }
   /* "StardewModdingAPI": a long name run together gets its spaces back. */
   if (!SDL_strchr(stem, ' ') && SDL_strlen(stem) > 12)
   {
      char spaced[300];
      size_t n = 0;
      for (p = stem; *p && n + 2 < sizeof(spaced); p++)
      {
         if (p > stem && *p >= 'A' && *p <= 'Z' && p[-1] >= 'a' && p[-1] <= 'z')
            spaced[n++] = ' ';
         spaced[n++] = *p;
      }
      spaced[n] = '\0';
      SDL_strlcpy(stem, spaced, sizeof(stem));
   }
   SDL_strlcpy(dst, stem, size);
}

static void add_game(const char *exe, const char *dir, const char *why)
{
   struct game g;
   char lower[1024], norm[200];
   size_t i;

   if (st.count == st.alloc)
   {
      int alloc = st.alloc ? st.alloc * 2 : 32;
      struct found *grown = SDL_realloc(st.list, sizeof(*grown) * (size_t)alloc);
      if (!grown)
         return;
      st.list = grown;
      st.alloc = alloc;
   }
   SDL_strlcpy(st.list[st.count].exe, exe, sizeof(st.list[0].exe));
   SDL_strlcpy(st.list[st.count].dir, dir, sizeof(st.list[0].dir));
   st.count++;

   SDL_zero(g);
   game_name(exe, g.name, sizeof(g.name));
   for (i = 0; exe[i] && i + 1 < sizeof(lower); i++)
      lower[i] = (char)SDL_tolower((unsigned char)exe[i]);
   lower[i] = '\0';
   library_normalize(g.name, norm, sizeof(norm));
   /* Stable while the exe stays where it is. */
   SDL_snprintf(g.id, sizeof(g.id), "disk:%s-%08x", norm, (unsigned)fb_crc32(lower, i));
   SDL_strlcpy(g.source, "PC", sizeof(g.source));
   SDL_strlcpy(g.exe, exe, sizeof(g.exe));
   SDL_strlcpy(g.game_exe, exe, sizeof(g.game_exe));
   parent_dir(exe, g.start_dir, sizeof(g.start_dir));
   g.installed = true;
   /* Steam's own defaults for a new shortcut. */
   g.steam_overlay = g.steam_input = true;
   library_add(&g);
   st.added++;
   if (st.verbose)
      SDL_Log("found %s (%s): %s", g.name, why, exe);
}

/* One folder's entries, files and folders, not what's inside them. */
struct listing
{
   char **names;
   bool *dirs;
   int count, alloc;
};

#ifndef _WIN32
static SDL_EnumerationResult SDLCALL on_entry(void *user, const char *dir, const char *name)
{
   struct listing *l = user;
   char path[1100];
   SDL_PathInfo info;

   SDL_snprintf(path, sizeof(path), "%s%s", dir, name);
   if (!SDL_GetPathInfo(path, &info))
      return SDL_ENUM_CONTINUE;
   if (l->count == l->alloc)
   {
      int alloc = l->alloc ? l->alloc * 2 : 32;
      char **names = SDL_realloc(l->names, sizeof(char *) * (size_t)alloc);
      bool *dirs;
      if (!names)
         return SDL_ENUM_FAILURE;
      l->names = names;
      if (!(dirs = SDL_realloc(l->dirs, sizeof(bool) * (size_t)alloc)))
         return SDL_ENUM_FAILURE;
      l->dirs = dirs;
      l->alloc = alloc;
   }
   l->dirs[l->count] = info.type == SDL_PATHTYPE_DIRECTORY;
   l->names[l->count++] = SDL_strdup(name);
   return SDL_ENUM_CONTINUE;
}
#endif

#ifdef _WIN32
/* FindFirstFile gives each entry's type with its name: no look at each
 * one after, which is most of a scan's time otherwise. */
static void list_dir(const char *dir, struct listing *l)
{
   char pattern[1100];
   WCHAR *wide;
   WIN32_FIND_DATAW fd;
   HANDLE h;
   size_t len = SDL_strlen(dir);

   SDL_zerop(l);
   SDL_snprintf(pattern, sizeof(pattern), "%s%s*", dir,
         len && (dir[len - 1] == '\\' || dir[len - 1] == '/') ? "" : "\\");
   if (!(wide = (WCHAR *)SDL_iconv_string("UTF-16LE", "UTF-8", pattern, SDL_strlen(pattern) + 1)))
      return;
   h = FindFirstFileExW(wide, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
         FIND_FIRST_EX_LARGE_FETCH);
   SDL_free(wide);
   if (h == INVALID_HANDLE_VALUE)
      return;
   do
   {
      char *name;
      if (fd.cFileName[0] == '.' && (!fd.cFileName[1] || (fd.cFileName[1] == '.' && !fd.cFileName[2])))
         continue;
      if (l->count == l->alloc)
      {
         int alloc = l->alloc ? l->alloc * 2 : 32;
         char **names = SDL_realloc(l->names, sizeof(char *) * (size_t)alloc);
         bool *dirs;
         if (!names)
            break;
         l->names = names;
         if (!(dirs = SDL_realloc(l->dirs, sizeof(bool) * (size_t)alloc)))
            break;
         l->dirs = dirs;
         l->alloc = alloc;
      }
      name = SDL_iconv_string("UTF-8", "UTF-16LE", (const char *)fd.cFileName,
            (SDL_wcslen(fd.cFileName) + 1) * sizeof(WCHAR));
      if (!name)
         continue;
      l->dirs[l->count] = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      l->names[l->count++] = name;
   } while (FindNextFileW(h, &fd));
   FindClose(h);
}
#else
static void list_dir(const char *dir, struct listing *l)
{
   SDL_zerop(l);
   SDL_EnumerateDirectory(dir, on_entry, l);
}
#endif

static void free_listing(struct listing *l)
{
   int i;

   for (i = 0; i < l->count; i++)
      SDL_free(l->names[i]);
   SDL_free(l->names);
   SDL_free(l->dirs);
}

/* Whether dir has what pattern names: "a/b/c" a path of globs, a folder
 * where it ends in '/'. Only the folders on the way are listed. */
static bool has(const char *dir, const char *pattern)
{
   const char *slash = SDL_strchr(pattern, '/');
   char part[128], sub[1100];
   struct listing l;
   bool found = false;
   int i;

   SDL_strlcpy(part, pattern, SDL_min(sizeof(part), (size_t)(slash ? slash - pattern + 1
         : (ptrdiff_t)SDL_strlen(pattern) + 1)));
   list_dir(dir, &l);
   for (i = 0; i < l.count && !found; i++)
   {
      if (!match(l.names[i], part))
         continue;
      if (!slash)
         found = !l.dirs[i];
      else if (!slash[1])
         found = l.dirs[i];
      else if (l.dirs[i])
      {
         SDL_snprintf(sub, sizeof(sub), "%s" PATH_SEP "%s", dir, l.names[i]);
         found = has(sub, slash + 1);
      }
   }
   free_listing(&l);
   return found;
}

/* The first fingerprint found in dir, NULL if none. */
static const char *fingerprint(const char *dir)
{
   const char *why = NULL;
   struct listing l;
   size_t i;
   int j;

   /* One listing for the plain names; only paths (Content, then .xnb) list
    * further. */
   list_dir(dir, &l);
   for (i = 0; i < SDL_arraysize(fingerprints) && !why; i++)
   {
      const char *f = fingerprints[i], *slash = SDL_strchr(f, '/');
      char part[128];

      if (slash && slash[1])
      {
         if (has(dir, f))
            why = f;
         continue;
      }
      SDL_strlcpy(part, f, slash ? (size_t)(slash - f + 1) : sizeof(part));
      for (j = 0; j < l.count && !why; j++)
         if (l.dirs[j] == (slash != NULL) && match(l.names[j], part))
            why = f;
   }
   free_listing(&l);
   return why;
}

struct exe_pick
{
   const char *folder;
   char best[1024];
   Sint64 best_size;
   int best_rank;
};

/* The game's exe in dir: one named like the folder, or a Unity game's
 * (next to its <name>_Data), else the biggest. */
static void consider_exes(struct exe_pick *pick, const char *dir)
{
   char folder_norm[256], stem_norm[256], path[1100], data[1100];
   struct listing l;
   int i;

   library_normalize(base_name(pick->folder), folder_norm, sizeof(folder_norm));
   list_dir(dir, &l);
   for (i = 0; i < l.count; i++)
   {
      SDL_PathInfo info;
      char stem[256], *dot;
      int rank = 0;

      if (l.dirs[i] || !match(l.names[i], EXE_GLOB)
            || match_any(l.names[i], not_games, SDL_arraysize(not_games)))
         continue;
      SDL_snprintf(path, sizeof(path), "%s" PATH_SEP "%s", dir, l.names[i]);
      if (!SDL_GetPathInfo(path, &info))
         continue;
      SDL_strlcpy(stem, l.names[i], sizeof(stem));
      if ((dot = SDL_strrchr(stem, '.')))
         *dot = '\0';
      library_normalize(stem, stem_norm, sizeof(stem_norm));
      SDL_snprintf(data, sizeof(data), "%s" PATH_SEP "%s_Data", dir, stem);
      if (platform_exists(data))
         rank = 2;
      else if (stem_norm[0] && (SDL_strstr(folder_norm, stem_norm) || SDL_strstr(stem_norm, folder_norm)))
         rank = 1;
      if (rank > pick->best_rank || (rank == pick->best_rank && (Sint64)info.size > pick->best_size))
      {
         pick->best_rank = rank;
         pick->best_size = info.size;
         SDL_strlcpy(pick->best, path, sizeof(pick->best));
      }
   }
   free_listing(&l);
}

/* A folder that might be a game: fingerprints in it or the folders just
 * under it, and an exe to start it. */
static void consider_folder(const char *dir)
{
   struct exe_pick pick;
   struct listing l;
   const char *why;
   char sub[1100];
   int i;

   if (is_steam(dir) || known(dir))
      return;
   why = fingerprint(dir);
   list_dir(dir, &l);
   for (i = 0; i < l.count && !why; i++)
      if (l.dirs[i])
      {
         SDL_snprintf(sub, sizeof(sub), "%s" PATH_SEP "%s", dir, l.names[i]);
         why = fingerprint(sub);
      }
   if (why)
   {
      SDL_zero(pick);
      pick.folder = dir;
      consider_exes(&pick, dir);
      /* Unreal and others keep the real exe a level down. */
      for (i = 0; i < l.count && !pick.best[0]; i++)
         if (l.dirs[i])
         {
            SDL_snprintf(sub, sizeof(sub), "%s" PATH_SEP "%s", dir, l.names[i]);
            consider_exes(&pick, sub);
         }
      if (pick.best[0])
         add_game(pick.best, dir, why);
      else if (st.verbose)
         SDL_Log("skipped %s (%s): no exe", dir, why);
   }
   free_listing(&l);
}
/* Each folder in dir is a candidate; one named like "Games" holds more. */
static void scan_children(const char *dir, int depth)
{
   struct listing subs;
   char path[1100];
   int i;

   list_dir(dir, &subs);
   for (i = 0; i < subs.count; i++)
   {
      if (!subs.dirs[i] || match_any(subs.names[i], skip_dirs, SDL_arraysize(skip_dirs)))
         continue;
      SDL_snprintf(path, sizeof(path), "%s%s%s", dir,
            dir[SDL_strlen(dir) - 1] == '\\' || dir[SDL_strlen(dir) - 1] == '/' ? "" : PATH_SEP,
            subs.names[i]);
      if (depth < 2 && match(subs.names[i], "*games*"))
         scan_children(path, depth + 1);
      else
         consider_folder(path);
   }
   free_listing(&subs);
}

#ifdef _WIN32

/* Windows' Game Bar keeps a record of each game it has seen running. */
static void on_game_record(const char *subkey, void *user)
{
   char key[512], exe[1024], dir[1024];

   SDL_snprintf(key, sizeof(key), "System\\GameConfigStore\\Children\\%s", subkey);
   if (!platform_reg_string("HKCU", key, "MatchedExeFullPath", exe, sizeof(exe))
         || !platform_exists(exe) || is_steam(exe)
         || SDL_strcasestr(exe, "\\AppData\\Local\\Packages\\")
         || SDL_strncasecmp(exe, "C:\\Windows", 10) == 0
         || match_any(base_name(exe), not_games, SDL_arraysize(not_games)))
      return;
   parent_dir(exe, dir, sizeof(dir));
   if (!known(dir))
      add_game(exe, dir, "Windows saw it run as a game");
}

static void scan_roots(void)
{
   DWORD drives = GetLogicalDrives();
   const char *env[] = { "ProgramFiles", "ProgramFiles(x86)" };
   char root[1100];
   int i;

   for (i = 0; i < 26; i++)
   {
      char letter[4] = { (char)('A' + i), ':', '\\', '\0' };
      if ((drives & (1u << i)) && GetDriveTypeA(letter) == DRIVE_FIXED)
         scan_children(letter, 0);
   }
   for (i = 0; i < (int)SDL_arraysize(env); i++)
      if (SDL_getenv_unsafe(env[i]))
         scan_children(SDL_getenv_unsafe(env[i]), 1);
   if (SDL_getenv_unsafe("LOCALAPPDATA"))
   {
      SDL_snprintf(root, sizeof(root), "%s\\Programs", SDL_getenv_unsafe("LOCALAPPDATA"));
      scan_children(root, 1);
   }
}

#else

static void scan_roots(void)
{
   char home[1024], path[1100];

   platform_home(home, sizeof(home));
   scan_children(home, 0);
   SDL_snprintf(path, sizeof(path), "%s/Games", home);
   scan_children(path, 1);
   scan_children("/opt", 1);
}

#endif

int disk_add_exe(const char *exe)
{
   struct game g;
   char lower[1024], norm[200];
   size_t i;
   int index;

   if (!platform_exists(exe))
      return -1;
   /* Already known (a scan found it, or it was added before). */
   library_lock();
   for (index = 0; index < library_count(); index++)
      if (SDL_strcasecmp(library_game(index)->exe, exe) == 0)
      {
         library_game(index)->installed = true;
         library_game(index)->selected = true;
         library_unlock();
         library_changed();
         return index;
      }
   library_unlock();

   SDL_zero(g);
   game_name(exe, g.name, sizeof(g.name));
   for (i = 0; exe[i] && i + 1 < sizeof(lower); i++)
      lower[i] = (char)SDL_tolower((unsigned char)exe[i]);
   lower[i] = '\0';
   library_normalize(g.name, norm, sizeof(norm));
   SDL_snprintf(g.id, sizeof(g.id), "added:%s-%08x", norm, (unsigned)fb_crc32(lower, i));
   SDL_strlcpy(g.source, "Added", sizeof(g.source));
   SDL_strlcpy(g.exe, exe, sizeof(g.exe));
   SDL_strlcpy(g.game_exe, exe, sizeof(g.game_exe));
   parent_dir(exe, g.start_dir, sizeof(g.start_dir));
   g.installed = true;
   /* Picked on purpose: meant for Steam. */
   g.selected = true;
   g.steam_overlay = g.steam_input = true;
   return library_add(&g);
}

bool disk_main_exe(const char *dir, char *dst, size_t size)
{
   struct exe_pick pick;

   SDL_zero(pick);
   pick.folder = dir;
   consider_exes(&pick, dir);
   SDL_strlcpy(dst, pick.best, size);
   return pick.best[0] != '\0';
}

void disk_keep_added(void)
{
   int i;

   library_lock();
   for (i = 0; i < library_count(); i++)
   {
      struct game *g = library_game(i);
      if (SDL_strncmp(g->id, "added:", 6) == 0 && platform_exists(g->exe))
         g->installed = true;
   }
   library_unlock();
}

int scan_disks(int verbose)
{
   SDL_zero(st);
   st.verbose = verbose;
#ifdef _WIN32
   platform_reg_subkeys("HKCU", "System\\GameConfigStore\\Children", on_game_record, NULL);
#endif
   scan_roots();
   SDL_free(st.list);
   return st.added;
}
