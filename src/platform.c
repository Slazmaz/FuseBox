#ifndef _WIN32
/* realpath and kill under -std=c11. */
#define _DEFAULT_SOURCE
#endif

#include "platform.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
/* Windows 10's DPI functions. */
#undef _WIN32_WINNT
#undef WINVER
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#include <windows.h>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <userenv.h>
#include <wchar.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

void platform_home(char *dst, size_t size)
{
   const char *home = SDL_GetUserFolder(SDL_FOLDER_HOME);
   size_t len;

   SDL_strlcpy(dst, home ? home : ".", size);
   len = SDL_strlen(dst);
   while (len > 1 && (dst[len - 1] == '/' || dst[len - 1] == '\\'))
      dst[--len] = '\0';
}

bool platform_exists(const char *path)
{
   return SDL_GetPathInfo(path, NULL);
}

/* The account that logged in last, 0 if that isn't known. Going by its files'
 * times alone it'd still be the one before for a while. */
static Uint32 last_login(const struct steam_install *steam)
{
   char path[1100], *text, *p, *end;
   Uint64 user = 0, number, newest = 0;
   Uint32 account = 0;
   bool time = false;

   SDL_snprintf(path, sizeof(path), "%s" PATH_SEP "config" PATH_SEP "loginusers.vdf", steam->root);
   if (!(text = SDL_LoadFile(path, NULL)))
      return 0;
   /* Quoted strings: each account's Steam id, then its names and values. */
   for (p = text; (p = SDL_strchr(p, '"')); p = end + 1)
   {
      for (end = ++p; *end && *end != '"'; end++)
         if (*end == '\\' && end[1])
            end++;
      if (!*end)
         break;
      *end = '\0';
      number = SDL_strtoull(p, NULL, 10);
      if (time && number > newest)
      {
         newest = number;
         account = (Uint32)user;
      }
      if (end - p == 17 && number >> 32 == 0x01100001)
         user = number;
      time = SDL_strcasecmp(p, "timestamp") == 0;
   }
   SDL_free(text);
   return account;
}

/* The userdata/<id> whose localconfig.vdf changed last: the account used
 * most recently. */
struct newest
{
   const char *userdata;
   SDL_Time time;
   char best[1024];
};

static SDL_EnumerationResult SDLCALL on_user(void *user, const char *dir, const char *name)
{
   struct newest *n = user;
   char path[1024];
   SDL_PathInfo info;
   const char *p;

   /* Numbers only: "0" and "anonymous" aren't accounts. */
   for (p = name; *p >= '0' && *p <= '9'; p++)
      ;
   if (!name[0] || *p || SDL_strcmp(name, "0") == 0)
      return SDL_ENUM_CONTINUE;
   SDL_snprintf(path, sizeof(path), "%s%s" PATH_SEP "config" PATH_SEP "localconfig.vdf", dir, name);
   if (!SDL_GetPathInfo(path, &info))
      info.modify_time = 0;
   if (!n->best[0] || info.modify_time > n->time)
   {
      n->time = info.modify_time;
      SDL_snprintf(n->best, sizeof(n->best), "%s%s" PATH_SEP "config", dir, name);
   }
   return SDL_ENUM_CONTINUE;
}

/* account: the one that's logged in, 0 if none is or it isn't known. */
static bool pick_user(struct steam_install *steam, Uint32 account)
{
   struct newest n;
   char userdata[1100];

   SDL_zero(n);
   SDL_snprintf(userdata, sizeof(userdata), "%s" PATH_SEP "userdata", steam->root);
   if (account || (account = last_login(steam)))
   {
      SDL_snprintf(n.best, sizeof(n.best), "%s" PATH_SEP "%u" PATH_SEP "config", userdata,
            (unsigned)account);
      if (platform_exists(n.best))
      {
         SDL_strlcpy(steam->config, n.best, sizeof(steam->config));
         return true;
      }
      n.best[0] = '\0';
   }
   if (!SDL_EnumerateDirectory(userdata, on_user, &n) || !n.best[0])
      return false;
   SDL_strlcpy(steam->config, n.best, sizeof(steam->config));
   return true;
}

#ifdef _WIN32

static HKEY hive_of(const char *hive)
{
   return SDL_strcmp(hive, "HKLM") == 0 ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

static WCHAR *wide(const char *text)
{
   return text ? (WCHAR *)SDL_iconv_string("UTF-16LE", "UTF-8", text, SDL_strlen(text) + 1)
      : NULL;
}

static void narrow(const WCHAR *text, char *dst, size_t size)
{
   char *s = SDL_iconv_string("UTF-8", "UTF-16LE", (const char *)text,
         (SDL_wcslen(text) + 1) * sizeof(WCHAR));
   SDL_strlcpy(dst, s ? s : "", size);
   SDL_free(s);
}

bool platform_reg_string(const char *hive, const char *key, const char *value,
      char *dst, size_t size)
{
   WCHAR *k = wide(key), *v = wide(value), buf[2048];
   DWORD bytes = sizeof(buf);
   LSTATUS st;

   dst[0] = '\0';
   st = RegGetValueW(hive_of(hive), k, v, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, buf, &bytes);
   SDL_free(k);
   SDL_free(v);
   if (st != ERROR_SUCCESS)
      return false;
   narrow(buf, dst, size);
   return dst[0] != '\0';
}

bool platform_reg_dword(const char *hive, const char *key, const char *value, unsigned *dst)
{
   WCHAR *k = wide(key), *v = wide(value);
   DWORD d = 0, bytes = sizeof(d);
   LSTATUS st = RegGetValueW(hive_of(hive), k, v, RRF_RT_REG_DWORD, NULL, &d, &bytes);

   SDL_free(k);
   SDL_free(v);
   *dst = d;
   return st == ERROR_SUCCESS;
}

void platform_reg_subkeys(const char *hive, const char *key,
      void (*fn)(const char *name, void *user), void *user)
{
   WCHAR *k = wide(key), name[256];
   HKEY h;
   DWORD i, len;

   if (RegOpenKeyExW(hive_of(hive), k, 0, KEY_READ, &h) != ERROR_SUCCESS)
   {
      SDL_free(k);
      return;
   }
   SDL_free(k);
   for (i = 0;; i++)
   {
      char utf8[512];
      len = SDL_arraysize(name);
      if (RegEnumKeyExW(h, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
         break;
      narrow(name, utf8, sizeof(utf8));
      fn(utf8, user);
   }
   RegCloseKey(h);
}

bool platform_find_steam(struct steam_install *steam)
{
   unsigned account = 0;
   char *p;

   SDL_zerop(steam);
   if (!platform_reg_string("HKCU", "Software\\Valve\\Steam", "SteamPath", steam->root,
            sizeof(steam->root))
         && !platform_reg_string("HKLM", "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath",
            steam->root, sizeof(steam->root)))
      return false;
   /* The registry has it with '/'. */
   for (p = steam->root; *p; p++)
      if (*p == '/')
         *p = '\\';
   if (!platform_reg_string("HKCU", "Software\\Valve\\Steam", "SteamExe", steam->exe,
            sizeof(steam->exe)))
      SDL_snprintf(steam->exe, sizeof(steam->exe), "%s\\steam.exe", steam->root);
   for (p = steam->exe; *p; p++)
      if (*p == '/')
         *p = '\\';
   /* What a Steam that crashed left there isn't so any more. */
   if (!platform_steam_running(steam) || !platform_reg_dword("HKCU",
            "Software\\Valve\\Steam\\ActiveProcess", "ActiveUser", &account))
      account = 0;
   if (!pick_user(steam, account))
   {
      steam->root[0] = '\0';
      return false;
   }
   return true;
}

unsigned platform_steam_pid(const struct steam_install *steam)
{
   unsigned pid = 0;

   return platform_reg_dword("HKCU", "Software\\Valve\\Steam\\ActiveProcess", "pid", &pid)
      && platform_steam_is(pid) ? pid : 0;
}

bool platform_steam_is(unsigned pid)
{
   HANDLE h = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : NULL;
   WCHAR exe[1024];
   DWORD code = 0, size = SDL_arraysize(exe);
   size_t n;
   bool is;

   if (!h)
      return false;
   /* The number may be another's by now. */
   is = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE
      && QueryFullProcessImageNameW(h, 0, exe, &size)
      && (n = wcslen(exe)) >= 9 && _wcsicmp(exe + n - 9, L"steam.exe") == 0;
   CloseHandle(h);
   return is;
}

#else

static bool find_root(struct steam_install *steam, const char *home, const char *rel, bool flatpak)
{
   char path[1024], *real;
   bool fits;

   SDL_snprintf(path, sizeof(path), "%s/%s", home, rel);
   if (!(real = realpath(path, NULL)))
      return false;
   fits = SDL_strlcpy(steam->root, real, sizeof(steam->root)) < sizeof(steam->root);
   free(real);
   steam->flatpak = flatpak;
   return fits && pick_user(steam, 0);
}

bool platform_find_steam(struct steam_install *steam)
{
   char home[1024];

   SDL_zerop(steam);
   platform_home(home, sizeof(home));
   if (!find_root(steam, home, ".steam/steam", false)
         && !find_root(steam, home, ".local/share/Steam", false)
         && !find_root(steam, home, ".var/app/com.valvesoftware.Steam/.local/share/Steam", true))
   {
      steam->root[0] = '\0';
      return false;
   }
   SDL_strlcpy(steam->exe, steam->flatpak ? "flatpak" : "steam", sizeof(steam->exe));
   return true;
}

unsigned platform_steam_pid(const struct steam_install *steam)
{
   char home[1024], path[1024], *text;
   long pid;

   platform_home(home, sizeof(home));
   SDL_snprintf(path, sizeof(path), "%s/.steam/steam.pid", home);
   if (!(text = SDL_LoadFile(path, NULL)))
   {
      SDL_snprintf(path, sizeof(path),
            "%s/.var/app/com.valvesoftware.Steam/.steam/steam.pid", home);
      if (!(text = SDL_LoadFile(path, NULL)))
         return 0;
   }
   pid = SDL_strtol(text, NULL, 10);
   SDL_free(text);
   return pid > 0 && platform_steam_is((unsigned)pid) ? (unsigned)pid : 0;
}

bool platform_steam_is(unsigned pid)
{
   /* 0 would be this process's own group. */
   return pid && kill((pid_t)pid, 0) == 0;
}

#endif

bool platform_steam_running(const struct steam_install *steam)
{
   return platform_steam_pid(steam) != 0;
}

#ifdef _WIN32
/* DWM draws its shadow for a window whose frame reaches into the client
 * area: a one-pixel margin is enough, and SDL keeps the frame itself
 * hidden for a borderless window. */
void platform_window_shadow(void *sdl_window)
{
   HWND hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_window),
         SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
   MARGINS margins = { 1, 1, 1, 1 };

   if (hwnd)
      DwmExtendFrameIntoClientArea(hwnd, &margins);
}
/* The exe's icon at 256 (Windows scales a smaller one up), its colour
 * bitmap read as 32-bit BGRA; an icon without alpha takes it from its
 * mask. */
bool platform_exe_icon(const char *exe, const char *png_path)
{
   WCHAR *w = wide(exe);
   HICON icon = NULL;
   UINT id = 0;
   ICONINFO ii;
   BITMAP bm;
   BITMAPINFO bi;
   SDL_Surface *s = NULL;
   HDC dc;
   bool ok = false;
   int i, n;

   if (!w)
      return false;
   n = (int)PrivateExtractIconsW(w, 0, 256, 256, &icon, &id, 1, 0);
   SDL_free(w);
   if (n != 1 || !icon)
      return false;
   if (!GetIconInfo(icon, &ii))
   {
      DestroyIcon(icon);
      return false;
   }
   if (ii.hbmColor && GetObject(ii.hbmColor, sizeof(bm), &bm)
         && (s = SDL_CreateSurface(bm.bmWidth, bm.bmHeight, SDL_PIXELFORMAT_BGRA32)))
   {
      SDL_zero(bi);
      bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = bm.bmWidth;
      bi.bmiHeader.biHeight = -bm.bmHeight;
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;
      dc = GetDC(NULL);
      if (s->pitch == bm.bmWidth * 4
            && GetDIBits(dc, ii.hbmColor, 0, bm.bmHeight, s->pixels, &bi, DIB_RGB_COLORS))
      {
         Uint8 *p = s->pixels;
         bool alpha = false;
         for (i = 0; i < bm.bmWidth * bm.bmHeight && !alpha; i++)
            alpha = p[i * 4 + 3] != 0;
         if (!alpha && ii.hbmMask)
         {
            Uint8 *mask = SDL_malloc((size_t)bm.bmWidth * bm.bmHeight * 4);
            if (mask && GetDIBits(dc, ii.hbmMask, 0, bm.bmHeight, mask, &bi, DIB_RGB_COLORS))
               for (i = 0; i < bm.bmWidth * bm.bmHeight; i++)
                  p[i * 4 + 3] = mask[i * 4] ? 0 : 255;
            SDL_free(mask);
         }
         ok = SDL_SavePNG(s, png_path);
      }
      ReleaseDC(NULL, dc);
      SDL_DestroySurface(s);
   }
   if (ii.hbmColor)
      DeleteObject(ii.hbmColor);
   if (ii.hbmMask)
      DeleteObject(ii.hbmMask);
   DestroyIcon(icon);
   return ok;
}
#else
void platform_window_shadow(void *sdl_window)
{
}

bool platform_exe_icon(const char *exe, const char *png_path)
{
   return false;
}
#endif

static void run(const struct steam_install *steam, bool shutdown, int port)
{
   const char *args[8];
   char number[16];
   int n = 0;
   SDL_Process *p;

   args[n++] = steam->exe;
   if (steam->flatpak)
   {
      args[n++] = "run";
      args[n++] = "com.valvesoftware.Steam";
   }
   if (shutdown)
      args[n++] = "-shutdown";
   if (port)
   {
      SDL_snprintf(number, sizeof(number), "%d", port);
      args[n++] = "-cef-enable-debugging";
      args[n++] = "-devtools-port";
      args[n++] = number;
   }
   args[n] = NULL;
   /* Not waited for: Steam keeps running after FuseBox closes. */
   if ((p = SDL_CreateProcess(args, false)))
      SDL_DestroyProcess(p);
   else
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Can't run %s: %s", steam->exe, SDL_GetError());
}

void platform_steam_shutdown(const struct steam_install *steam)
{
   run(steam, true, 0);
}

void platform_steam_start(const struct steam_install *steam, int port)
{
   run(steam, false, port);
}

#ifdef _WIN32
void platform_fuseboxes(char *dst, size_t size)
{
   static DWORD pids[8192];
   DWORD bytes = 0, i;

   dst[0] = '\0';
   if (!K32EnumProcesses(pids, sizeof(pids), &bytes))
      return;
   for (i = 0; i < bytes / sizeof(pids[0]); i++)
   {
      HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
      WCHAR exe[1024];
      DWORD count = SDL_arraysize(exe);
      char path[1024];

      if (!p)
         continue;
      if (QueryFullProcessImageNameW(p, 0, exe, &count) && count >= 12
            && _wcsicmp(exe + count - 12, L"\\fusebox.exe") == 0)
      {
         narrow(exe, path, sizeof(path));
         SDL_strlcat(dst, path, size);
         SDL_strlcat(dst, "\n", size);
      }
      CloseHandle(p);
   }
}
#else
void platform_fuseboxes(char *dst, size_t size)
{
   dst[0] = '\0';
}
#endif

/* --- Without a window (background.h) --- */

#ifdef _WIN32
#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define ONE_NAME L"Local\\FuseBoxBackground"

/* "<this exe>" --background */
static void background_command(WCHAR *dst, size_t count)
{
   WCHAR exe[1024];

   exe[0] = 0;
   GetModuleFileNameW(NULL, exe, SDL_arraysize(exe));
   _snwprintf(dst, count, L"\"%ls\" --background", exe);
   dst[count - 1] = 0;
}

void platform_autostart(bool on)
{
   WCHAR want[1100], have[1100];
   DWORD size = sizeof(have) - sizeof(WCHAR);
   HKEY key;

   if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &key,
            NULL) != ERROR_SUCCESS)
      return;
   SDL_zeroa(have);
   if (RegQueryValueExW(key, L"FuseBox", NULL, NULL, (BYTE *)have, &size) != ERROR_SUCCESS)
      have[0] = 0;
   background_command(want, SDL_arraysize(want));
   if (!on && have[0])
      RegDeleteValueW(key, L"FuseBox");
   /* Again when FuseBox's folder was moved. */
   else if (on && wcscmp(have, want) != 0)
      RegSetValueExW(key, L"FuseBox", 0, REG_SZ, (const BYTE *)want,
            (DWORD)((wcslen(want) + 1) * sizeof(WCHAR)));
   RegCloseKey(key);
}

/* Closed by Windows when the process ends, unless it's passed on. */
static HANDLE only;

bool platform_only_background(void)
{
   only = CreateMutexW(NULL, FALSE, ONE_NAME);
   return only && GetLastError() != ERROR_ALREADY_EXISTS;
}

/* Outside: as the desktop's child rather than the caller's, or not at all. Either
 * way without what Steam put in the caller's environment. */
static bool start(WCHAR *command, bool outside)
{
   HANDLE shell = NULL, token = NULL;
   LPPROC_THREAD_ATTRIBUTE_LIST list = NULL;
   STARTUPINFOEXW si;
   PROCESS_INFORMATION pi;
   void *env = NULL;
   SIZE_T size = 0;
   DWORD pid = 0;
   bool as_shells = false, ok = false;

   SDL_zero(si);
   si.StartupInfo.cb = sizeof(si);
   if (outside)
   {
      GetWindowThreadProcessId(GetShellWindow(), &pid);
      shell = pid ? OpenProcess(PROCESS_CREATE_PROCESS, FALSE, pid) : NULL;
      InitializeProcThreadAttributeList(NULL, 1, 0, &size);
      if (shell && (list = SDL_malloc(size)))
         as_shells = InitializeProcThreadAttributeList(list, 1, 0, &size)
            && UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &shell,
                  sizeof(shell), NULL, NULL);
      si.lpAttributeList = list;
   }
   if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &token))
   {
      if (!CreateEnvironmentBlock(&env, token, FALSE))
         env = NULL;
      CloseHandle(token);
   }
   if ((!outside || as_shells) && CreateProcessW(NULL, command, NULL, NULL, FALSE,
            CREATE_UNICODE_ENVIRONMENT | (as_shells ? EXTENDED_STARTUPINFO_PRESENT : 0), env,
            NULL, &si.StartupInfo, &pi))
   {
      ok = true;
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
   }
   if (env)
      DestroyEnvironmentBlock(env);
   if (as_shells)
      DeleteProcThreadAttributeList(list);
   SDL_free(list);
   if (shell)
      CloseHandle(shell);
   return ok;
}

void platform_start_background(bool by_game)
{
   HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, ONE_NAME);
   WCHAR command[1100];

   if (running)
   {
      CloseHandle(running);
      return;
   }
   background_command(command, SDL_arraysize(command));
   start(command, by_game);
}

bool platform_again_background(void)
{
   WCHAR command[1100];

   if (only)
      CloseHandle(only);
   only = NULL;
   background_command(command, SDL_arraysize(command));
   if (start(command, false))
      return true;
   /* This one goes on, then. */
   only = CreateMutexW(NULL, FALSE, ONE_NAME);
   return false;
}

bool platform_start_game(const char *path)
{
   char text[2300];
   size_t n = SDL_strlen(path);
   WCHAR *command;
   bool ok = false;

   /* A folder's separator would escape the quote. */
   while (n > 1 && (path[n - 1] == '\\' || path[n - 1] == '/'))
      n--;
   SDL_snprintf(text, sizeof(text), "\"%sfusebox.exe\" \"%.*s\"", SDL_GetBasePath(), (int)n, path);
   if ((command = wide(text)))
      ok = start(command, true);
   SDL_free(command);
   return ok;
}

#define WM_TRAY (WM_APP + 1)
enum { TRAY_OPEN = 1, TRAY_QUIT };

static struct
{
   SDL_Thread *thread;
   /* Posted once the thread has its window (or has none); to quit. */
   SDL_Semaphore *ready, *quit;
   HWND window;
   NOTIFYICONDATAW icon;
   /* Explorer's message when it (re)starts: icons are to be added again. */
   UINT taskbar_created;
   /* The FuseBox opened from here last. */
   HANDLE opened;
} tray;

struct find_window
{
   const WCHAR *exe;
   /* NULL: any a program shows as its own. */
   const WCHAR *title;
   HWND found;
};

static BOOL CALLBACK on_window(HWND hwnd, LPARAM user)
{
   struct find_window *f = (struct find_window *)user;
   WCHAR text[1024];
   DWORD pid = 0, size = SDL_arraysize(text);
   HANDLE process;

   if (!IsWindowVisible(hwnd) || !GetWindowTextW(hwnd, text, SDL_arraysize(text))
         || (f->title ? wcscmp(text, f->title) != 0 : GetWindow(hwnd, GW_OWNER)
            || (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)))
      return TRUE;
   /* A folder in Explorer can be called that too. */
   GetWindowThreadProcessId(hwnd, &pid);
   if (!(process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)))
      return TRUE;
   if (QueryFullProcessImageNameW(process, 0, text, &size) && _wcsicmp(text, f->exe) == 0)
      f->found = hwnd;
   CloseHandle(process);
   return !f->found;
}

static bool to_front(const WCHAR *exe, const WCHAR *title)
{
   struct find_window f = { exe, title, NULL };

   EnumWindows(on_window, (LPARAM)&f);
   if (!f.found)
      return false;
   if (IsIconic(f.found))
      ShowWindowAsync(f.found, SW_RESTORE);
   SetForegroundWindow(f.found);
   return true;
}

bool platform_open(const char *exe)
{
   WCHAR *path = wide(exe), dir[1024], command[1100], *slash;
   INPUT none;
   STARTUPINFOW si;
   PROCESS_INFORMATION pi;
   bool ok = false;

   if (!path || wcslen(path) >= SDL_arraysize(dir))
   {
      SDL_free(path);
      return false;
   }
   /* Windows lets only who had the last input put a window in front: a mouse
    * move of nothing, which is enough. */
   SDL_zero(none);
   none.type = INPUT_MOUSE;
   SendInput(1, &none, sizeof(none));
   if (to_front(path, NULL))
   {
      SDL_free(path);
      return true;
   }
   /* From its own folder: some look there for their files. */
   wcscpy(dir, path);
   if ((slash = wcsrchr(dir, '\\')))
      *slash = 0;
   _snwprintf(command, SDL_arraysize(command), L"\"%ls\"", path);
   command[SDL_arraysize(command) - 1] = 0;
   SDL_zero(si);
   si.cb = sizeof(si);
   if (CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, slash ? dir : NULL, &si, &pi))
   {
      AllowSetForegroundWindow(pi.dwProcessId);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      ok = true;
   }
   /* One that asks to run as administrator. */
   else
      ok = (INT_PTR)ShellExecuteW(NULL, NULL, path, NULL, slash ? dir : NULL, SW_SHOWNORMAL) > 32;
   SDL_free(path);
   return ok;
}

/* FuseBox's window to the front, or a new one. */
static void tray_open(void)
{
   WCHAR exe[1024], command[1100];
   STARTUPINFOW si;
   PROCESS_INFORMATION pi;

   exe[0] = 0;
   GetModuleFileNameW(NULL, exe, SDL_arraysize(exe));
   if (to_front(exe, L"FuseBox"))
      return;
   /* A double click is two: the first one's window is yet to come. */
   if (tray.opened && WaitForSingleObject(tray.opened, 0) == WAIT_TIMEOUT)
      return;
   if (tray.opened)
      CloseHandle(tray.opened);
   tray.opened = NULL;
   SDL_zero(si);
   si.cb = sizeof(si);
   _snwprintf(command, SDL_arraysize(command), L"\"%ls\"", exe);
   command[SDL_arraysize(command) - 1] = 0;
   if (CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
   {
      AllowSetForegroundWindow(pi.dwProcessId);
      CloseHandle(pi.hThread);
      tray.opened = pi.hProcess;
   }
}

static void tray_menu(HWND hwnd, int x, int y)
{
   HMENU menu = CreatePopupMenu();
   int chosen;

   if (!menu)
      return;
   AppendMenuW(menu, MF_STRING, TRAY_OPEN, L"Open FuseBox");
   AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
   AppendMenuW(menu, MF_STRING, TRAY_QUIT, L"Quit");
   SetMenuDefaultItem(menu, TRAY_OPEN, FALSE);
   /* Without being in front (and the message after), it wouldn't close on a
    * click elsewhere. */
   SetForegroundWindow(hwnd);
   chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, x, y, 0, hwnd,
         NULL);
   PostMessageW(hwnd, WM_NULL, 0, 0);
   DestroyMenu(menu);
   if (chosen == TRAY_OPEN)
      tray_open();
   else if (chosen == TRAY_QUIT)
      PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

static void tray_add(void)
{
   /* Fails at login until there's a taskbar: taskbar_created comes then. */
   if (Shell_NotifyIconW(NIM_ADD, &tray.icon))
      Shell_NotifyIconW(NIM_SETVERSION, &tray.icon);
}

static LRESULT CALLBACK on_tray(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
   if (msg == WM_TRAY)
   {
      switch (LOWORD(lp))
      {
         case NIN_SELECT:
         case NIN_KEYSELECT:
            tray_open();
            break;
         case WM_CONTEXTMENU:
            tray_menu(hwnd, (short)LOWORD(wp), (short)HIWORD(wp));
            break;
      }
      return 0;
   }
   if (msg == WM_DESTROY)
   {
      Shell_NotifyIconW(NIM_DELETE, &tray.icon);
      SDL_SignalSemaphore(tray.quit);
      PostQuitMessage(0);
      return 0;
   }
   if (tray.taskbar_created && msg == tray.taskbar_created)
      tray_add();
   return DefWindowProcW(hwnd, msg, wp, lp);
}

static int SDLCALL tray_thread(void *user)
{
   HINSTANCE instance = GetModuleHandleW(NULL);
   UINT dpi;
   WNDCLASSW wc;
   MSG msg;

   (void)user;
   /* Sharp, though SDL's video (which would see to it) isn't used here. */
   SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
   dpi = GetDpiForSystem();
   SDL_zero(wc);
   wc.lpfnWndProc = on_tray;
   wc.hInstance = instance;
   wc.lpszClassName = L"FuseBoxTray";
   RegisterClassW(&wc);
   tray.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
   /* Never shown; not a message-only one, those don't get taskbar_created. */
   tray.window = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"FuseBox", WS_POPUP, 0, 0,
         0, 0, NULL, NULL, instance, NULL);
   if (tray.window)
   {
      tray.icon.cbSize = sizeof(tray.icon);
      tray.icon.hWnd = tray.window;
      tray.icon.uID = 1;
      tray.icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
      tray.icon.uCallbackMessage = WM_TRAY;
      tray.icon.uVersion = NOTIFYICON_VERSION_4;
      /* The exe's (fusebox.rc). */
      tray.icon.hIcon = LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
            GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), 0);
      wcscpy(tray.icon.szTip, L"FuseBox");
      tray_add();
   }
   SDL_SignalSemaphore(tray.ready);
   if (!tray.window)
      return 0;
   while (GetMessageW(&msg, NULL, 0, 0) > 0)
   {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
   }
   if (tray.icon.hIcon)
      DestroyIcon(tray.icon.hIcon);
   return 0;
}

void platform_tray_start(void)
{
   tray.ready = SDL_CreateSemaphore(0);
   tray.quit = SDL_CreateSemaphore(0);
   /* Its own thread: the background one's is busy with the network. */
   if (tray.ready && tray.quit && (tray.thread = SDL_CreateThread(tray_thread, "tray", NULL)))
      SDL_WaitSemaphore(tray.ready);
}

void platform_tray_stop(void)
{
   if (tray.thread)
   {
      if (tray.window)
         PostMessageW(tray.window, WM_CLOSE, 0, 0);
      SDL_WaitThread(tray.thread, NULL);
   }
   if (tray.opened)
      CloseHandle(tray.opened);
   SDL_DestroySemaphore(tray.ready);
   SDL_DestroySemaphore(tray.quit);
   SDL_zero(tray);
}

bool platform_tray_wait(int ms)
{
   if (tray.quit)
      return SDL_WaitSemaphoreTimeout(tray.quit, ms);
   SDL_Delay(ms);
   return false;
}

/* This program's command line from its nth argument on, quotes and all. */
static const WCHAR *typed_from(int n)
{
   const WCHAR *p = GetCommandLineW();

   for (; n > 0 && *p; n--)
   {
      bool quoted = false;

      for (; *p && (quoted || (*p != ' ' && *p != '\t')); p++)
         if (*p == '\\' && p[1] == '"')
            p++;
         else if (*p == '"')
            quoted = !quoted;
      while (*p == ' ' || *p == '\t')
         p++;
   }
   return p;
}

void platform_arguments(char **argv, int skip, char *dst, size_t size)
{
   (void)argv;
   narrow(typed_from(skip), dst, size);
}

bool platform_run_game(const char *exe, const char *args, const char *dir, char **argv, int skip,
      int *code)
{
   const WCHAR *more = typed_from(skip);
   WCHAR *wexe = wide(exe), *wargs = wide(args), *wdir = dir[0] ? wide(dir) : NULL;
   WCHAR *command = NULL, *params;
   STARTUPINFOW si;
   PROCESS_INFORMATION pi;
   HANDLE process = NULL;
   DWORD result = 0;
   bool ok = false;
   size_t len;

   (void)argv;
   *code = 1;
   SDL_zero(si);
   si.cb = sizeof(si);
   SDL_zero(pi);
   if (wexe && wargs)
   {
      len = wcslen(wexe) + wcslen(wargs) + wcslen(more) + 8;
      command = SDL_malloc(len * sizeof(WCHAR));
   }
   if (command)
   {
      _snwprintf(command, len, L"\"%ls\" %ls%ls%ls", wexe, wargs, *wargs && *more ? L" " : L"", more);
      params = command + wcslen(wexe) + 3;
      /* One that asks to run as administrator, or isn't a program itself, is
       * Windows' to open. */
      if (CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, wdir, &si, &pi))
      {
         CloseHandle(pi.hThread);
         process = pi.hProcess;
         AllowSetForegroundWindow(pi.dwProcessId);
         ok = true;
      }
      else
      {
         SHELLEXECUTEINFOW ex;

         SDL_zero(ex);
         ex.cbSize = sizeof(ex);
         ex.fMask = SEE_MASK_NOCLOSEPROCESS;
         ex.lpFile = wexe;
         ex.lpParameters = params;
         ex.lpDirectory = wdir;
         ex.nShow = SW_SHOWNORMAL;
         if ((ok = ShellExecuteExW(&ex)))
            process = ex.hProcess;
      }
   }
   SDL_free(command);
   SDL_free(wexe);
   SDL_free(wargs);
   SDL_free(wdir);
   if (process)
   {
      WaitForSingleObject(process, INFINITE);
      GetExitCodeProcess(process, &result);
      CloseHandle(process);
   }
   if (ok)
      *code = (int)result;
   return ok;
}

#else

static bool own_exe(char *dst, size_t size)
{
   ssize_t n = readlink("/proc/self/exe", dst, size - 1);

   dst[n > 0 ? n : 0] = '\0';
   return n > 0;
}

void platform_autostart(bool on)
{
   char home[1024], path[1100], exe[1024], text[1400];
   const char *xdg = SDL_getenv("XDG_CONFIG_HOME");

   platform_home(home, sizeof(home));
   if (xdg && xdg[0])
      SDL_snprintf(path, sizeof(path), "%s/autostart", xdg);
   else
      SDL_snprintf(path, sizeof(path), "%s/.config/autostart", home);
   if (on)
      SDL_CreateDirectory(path);
   SDL_strlcat(path, "/fusebox.desktop", sizeof(path));
   if (!on)
   {
      if (platform_exists(path))
         SDL_RemovePath(path);
      return;
   }
   if (!own_exe(exe, sizeof(exe)))
      return;
   SDL_snprintf(text, sizeof(text), "[Desktop Entry]\nType=Application\nName=FuseBox\n"
         "Exec=\"%s\" --background\nNoDisplay=true\n", exe);
   SDL_SaveFile(path, text, SDL_strlen(text));
}

/* Locked by the background one while it runs. */
static int one_file(void)
{
   const char *dir = SDL_getenv("XDG_RUNTIME_DIR");
   char path[1100];

   if (dir && dir[0])
      SDL_snprintf(path, sizeof(path), "%s/fusebox-background", dir);
   else
      SDL_snprintf(path, sizeof(path), "/tmp/fusebox-background-%d", (int)getuid());
   return open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
}

bool platform_only_background(void)
{
   int fd = one_file();

   return fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0;
}

void platform_start_background(bool by_game)
{
   /* Steam follows a game's children wherever they go: systemd starts it
    * outside of them. */
   const char *args[8];
   char exe[1024];
   SDL_Process *p;
   int n = 0, fd = one_file();
   bool runs = fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0;

   if (fd >= 0)
      close(fd);
   if (runs || !own_exe(exe, sizeof(exe)))
      return;
   if (by_game)
   {
      args[n++] = "systemd-run";
      args[n++] = "--user";
      args[n++] = "--quiet";
      args[n++] = "--collect";
   }
   args[n++] = exe;
   args[n++] = "--background";
   args[n] = NULL;
   if ((p = SDL_CreateProcess(args, false)))
      SDL_DestroyProcess(p);
}

bool platform_again_background(void)
{
   return false;
}

bool platform_start_game(const char *path)
{
   char exe[1024];
   const char *args[] = { exe, path, NULL };
   SDL_Process *p = own_exe(exe, sizeof(exe)) ? SDL_CreateProcess(args, false) : NULL;

   if (p)
      SDL_DestroyProcess(p);
   return p != NULL;
}

void platform_arguments(char **argv, int skip, char *dst, size_t size)
{
   size_t n = 0;
   const char *p;

   /* Quoted, as the shell that takes them apart needs them. */
   for (argv += skip; *argv && n + 4 < size; argv++)
   {
      if (n)
         dst[n++] = ' ';
      dst[n++] = '\'';
      for (p = *argv; *p && n + 6 < size; p++)
         if (*p == '\'')
         {
            SDL_memcpy(dst + n, "'\\''", 4);
            n += 4;
         }
         else
            dst[n++] = *p;
      dst[n++] = '\'';
   }
   dst[n] = '\0';
}

bool platform_open(const char *exe)
{
   const char *args[] = { exe, NULL };
   SDL_Process *p = SDL_CreateProcess(args, false);

   if (p)
      SDL_DestroyProcess(p);
   return p != NULL;
}

void platform_tray_start(void)
{
}

void platform_tray_stop(void)
{
}

bool platform_tray_wait(int ms)
{
   SDL_Delay(ms);
   return false;
}

bool platform_run_game(const char *exe, const char *args, const char *dir, char **argv, int skip,
      int *code)
{
   size_t size = SDL_strlen(exe) * 4 + SDL_strlen(args) + 32, n;
   char *command = SDL_malloc(size), **list;
   const char *p;
   int status = 0, count = 0;
   pid_t pid;

   *code = 1;
   while (argv[skip + count])
      count++;
   list = SDL_calloc((size_t)count + 5, sizeof(*list));
   if (!command || !list)
   {
      SDL_free(command);
      SDL_free(list);
      return false;
   }
   /* Through a shell, as Steam would: the arguments are written for one. */
   n = (size_t)SDL_snprintf(command, size, "exec '");
   for (p = exe; *p; p++)
      n += (size_t)SDL_snprintf(command + n, size - n, *p == '\'' ? "'\\''" : "%c", *p);
   SDL_snprintf(command + n, size - n, "' %s \"$@\"", args);
   list[0] = "/bin/sh";
   list[1] = "-c";
   list[2] = command;
   list[3] = "sh";
   SDL_memcpy(list + 4, argv + skip, sizeof(*list) * (size_t)count);
   pid = fork();
   if (pid == 0)
   {
      if (dir[0] && chdir(dir) != 0)
         _exit(127);
      execv(list[0], list);
      _exit(127);
   }
   SDL_free(command);
   SDL_free(list);
   if (pid < 0 || waitpid(pid, &status, 0) < 0)
      return false;
   *code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
   /* A shell's "not found" and "can't be run". */
   return *code != 127 && *code != 126;
}
#endif
