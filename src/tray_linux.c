#ifndef _WIN32

/* The background one's icon by the clock: a StatusNotifierItem (KDE's, which GNOME
 * shows with its AppIndicator extension, and most other desktops too) and its menu
 * (com.canonical.dbusmenu), over the session's D-Bus. By libdbus, looked for when it
 * runs: what few things of it are needed are declared here. */
#define _GNU_SOURCE
#include "bolt.h"
#include "platform.h"

#include <SDL3/SDL.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define ITEM "org.kde.StatusNotifierItem"
#define WATCHER "org.kde.StatusNotifierWatcher"
#define MENU "com.canonical.dbusmenu"
#define PROPERTIES "org.freedesktop.DBus.Properties"
#define ITEM_PATH "/StatusNotifierItem"
#define MENU_PATH "/MenuBar"

/* Its real one is smaller. */
struct iter
{
   void *opaque[16];
};

enum { CALL = 1, SIGNAL = 4, HANDLED = 0, NOT_HANDLED = 1 };

#define DBUS(X) \
   X(void *, bus_get_private, (int type, void *error)) \
   X(int, bus_request_name, (void *c, const char *name, unsigned flags, void *error)) \
   X(void, bus_add_match, (void *c, const char *rule, void *error)) \
   X(void, connection_set_change_sigpipe, (Uint32 change)) \
   X(void, connection_set_exit_on_disconnect, (void *c, Uint32 on)) \
   X(Uint32, connection_add_filter, (void *c, int (*fn)(void *c, void *m, void *user), void *user, \
      void (*free_user)(void *))) \
   X(Uint32, connection_read_write_dispatch, (void *c, int ms)) \
   X(Uint32, connection_send, (void *c, void *m, Uint32 *serial)) \
   X(void, connection_flush, (void *c)) \
   X(void, connection_close, (void *c)) \
   X(void, connection_unref, (void *c)) \
   X(void *, message_new_method_call, (const char *to, const char *path, const char *interface, \
      const char *method)) \
   X(void *, message_new_method_return, (void *m)) \
   X(void *, message_new_error, (void *m, const char *name, const char *text)) \
   X(void, message_unref, (void *m)) \
   X(int, message_get_type, (void *m)) \
   X(const char *, message_get_interface, (void *m)) \
   X(const char *, message_get_member, (void *m)) \
   X(const char *, message_get_path, (void *m)) \
   X(Uint32, message_get_no_reply, (void *m)) \
   X(void, message_set_auto_start, (void *m, Uint32 on)) \
   X(Uint32, message_iter_init, (void *m, struct iter *it)) \
   X(Uint32, message_iter_next, (struct iter *it)) \
   X(int, message_iter_get_arg_type, (struct iter *it)) \
   X(void, message_iter_recurse, (struct iter *it, struct iter *sub)) \
   X(void, message_iter_get_basic, (struct iter *it, void *value)) \
   X(void, message_iter_init_append, (void *m, struct iter *it)) \
   X(Uint32, message_iter_append_basic, (struct iter *it, int type, const void *value)) \
   X(Uint32, message_iter_append_fixed_array, (struct iter *it, int type, const void *values, int n)) \
   X(Uint32, message_iter_open_container, (struct iter *it, int type, const char *signature, \
      struct iter *sub)) \
   X(Uint32, message_iter_close_container, (struct iter *it, struct iter *sub))

static struct
{
#define X(type, name, args) type (*name) args;
   DBUS(X)
#undef X
} dbus;

static struct
{
   SDL_Thread *thread;
   SDL_Semaphore *quit;
   SDL_AtomicInt stop;
   void *bus;
   char name[64];
   /* The desktop's leave to come to the front, for the click that follows. */
   char token[256];
   /* The FuseBox opened from here last. */
   SDL_Process *opened;
} tray;

/* ---- The window, found by its socket ----------------------------------- */

/* A message to it has it come to the front: on Wayland nobody else can do that. */
static struct
{
   SDL_Window *window;
   SDL_Thread *thread;
   int fd;
   struct sockaddr_un at;
} win = { .fd = -1 };

/* Of this copy of FuseBox. */
static void window_address(struct sockaddr_un *at)
{
   const char *run = SDL_getenv("XDG_RUNTIME_DIR"), *base = SDL_GetBasePath();
   Uint32 copy = base ? SDL_crc32(0, base, SDL_strlen(base)) : 0;

   SDL_zerop(at);
   at->sun_family = AF_UNIX;
   if (run && run[0])
      SDL_snprintf(at->sun_path, sizeof(at->sun_path), "%s/fusebox-window-%08x", run, (unsigned)copy);
   else
      SDL_snprintf(at->sun_path, sizeof(at->sun_path), "/tmp/fusebox-window-%d-%08x", (int)getuid(),
            (unsigned)copy);
}

/* Asks the window to the front (token NULL: only looks for it). False if there's
 * none. */
static bool ask_window(const struct sockaddr_un *at, const char *token)
{
   int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
   bool ok = fd >= 0 && connect(fd, (const struct sockaddr *)at, sizeof(*at)) == 0;

   /* One that doesn't take it has been asked already. */
   if (ok && token && send(fd, token, SDL_strlen(token) + 1, MSG_NOSIGNAL) < 0)
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The window wasn't asked to the front");
   if (fd >= 0)
      close(fd);
   return ok;
}

static void SDLCALL to_front(void *user)
{
   char *token = user;
   const char *driver = SDL_GetCurrentVideoDriver();
   SDL_WindowFlags flags = win.window ? SDL_GetWindowFlags(win.window) : 0;

   if (win.window && token[0] && driver && SDL_strcmp(driver, "wayland") == 0)
   {
      /* SDL uses the token, and can undo minimizing there, only as a window is shown. */
      if (!(flags & SDL_WINDOW_INPUT_FOCUS))
      {
         SDL_HideWindow(win.window);
         SDL_setenv_unsafe("XDG_ACTIVATION_TOKEN", token, 1);
         SDL_ShowWindow(win.window);
      }
   }
   else if (win.window)
   {
      if (flags & SDL_WINDOW_MINIMIZED)
         SDL_RestoreWindow(win.window);
      SDL_RaiseWindow(win.window);
   }
   SDL_free(token);
}

static int SDLCALL window_thread(void *user)
{
   char token[sizeof(tray.token) + 1];
   ssize_t n;

   (void)user;
   /* 0: shut down. */
   while ((n = recv(win.fd, token, sizeof(token) - 1, 0)) > 0)
   {
      char *copy;

      token[n] = '\0';
      if ((copy = SDL_strdup(token)) && !SDL_RunOnMainThread(to_front, copy, false))
         SDL_free(copy);
   }
   return 0;
}

void platform_window_start(void *sdl_window)
{
   window_address(&win.at);
   /* Another window answers already. */
   if (ask_window(&win.at, NULL) || (win.fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0)
      return;
   /* One that crashed left its. */
   unlink(win.at.sun_path);
   win.window = sdl_window;
   if (bind(win.fd, (struct sockaddr *)&win.at, sizeof(win.at)) == 0
         && (win.thread = SDL_CreateThread(window_thread, "window", NULL)))
      return;
   SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Can't take messages at %s", win.at.sun_path);
   close(win.fd);
   win.fd = -1;
}

void platform_window_stop(void)
{
   win.window = NULL;
   if (win.fd < 0)
      return;
   unlink(win.at.sun_path);
   shutdown(win.fd, SHUT_RDWR);
   SDL_WaitThread(win.thread, NULL);
   close(win.fd);
   win.fd = -1;
}

/* FuseBox's window to the front, or a new one. */
static void tray_open(void)
{
   struct sockaddr_un at;
   char exe[1100];
   const char *args[] = { exe, NULL };
   SDL_Environment *env;
   SDL_PropertiesID props;

   window_address(&at);
   if (ask_window(&at, tray.token))
   {
      tray.token[0] = '\0';
      return;
   }
   /* A double click is two: the first one's window is yet to come. */
   if (tray.opened && !SDL_WaitProcess(tray.opened, false, NULL))
      return;
   SDL_DestroyProcess(tray.opened);
   SDL_snprintf(exe, sizeof(exe), "%s" FUSEBOX_EXE, SDL_GetBasePath());
   env = SDL_CreateEnvironment(true);
   if (tray.token[0])
      SDL_SetEnvironmentVariable(env, "XDG_ACTIVATION_TOKEN", tray.token, true);
   tray.token[0] = '\0';
   props = SDL_CreateProperties();
   SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
   tray.opened = platform_process(args, props);
   SDL_DestroyProperties(props);
   SDL_DestroyEnvironment(env);
}

/* ---- What's written into messages --------------------------------------- */

static void put_text(struct iter *it, int type, const char *text)
{
   dbus.message_iter_append_basic(it, type, &text);
}

static void put_number(struct iter *it, int type, Uint32 n)
{
   dbus.message_iter_append_basic(it, type, &n);
}

static void begin(struct iter *it, int type, const char *signature, struct iter *sub)
{
   dbus.message_iter_open_container(it, type, signature, sub);
}

static void end(struct iter *it, struct iter *sub)
{
   dbus.message_iter_close_container(it, sub);
}

static void put_empty(struct iter *it, const char *signature)
{
   struct iter list;

   begin(it, 'a', signature, &list);
   end(it, &list);
}

/* a(iiay): the icon in the sizes panels have. */
static void put_icons(struct iter *it)
{
   static const int sizes[] = { 16, 22, 24, 32, 48, 64 };
   struct iter list, one, bytes;
   size_t i;
   int j;

   begin(it, 'a', "(iiay)", &list);
   for (i = 0; i < SDL_arraysize(sizes); i++)
   {
      int n = sizes[i] * sizes[i] * 4;
      Uint8 *p = SDL_malloc((size_t)n);

      if (!p)
         continue;
      bolt_icon(p, sizes[i]);
      /* RGBA as ARGB. */
      for (j = 0; j < n; j += 4)
      {
         Uint8 a = p[j + 3];
         SDL_memmove(p + j + 1, p + j, 3);
         p[j] = a;
      }
      begin(&list, 'r', NULL, &one);
      put_number(&one, 'i', (Uint32)sizes[i]);
      put_number(&one, 'i', (Uint32)sizes[i]);
      begin(&one, 'a', "y", &bytes);
      dbus.message_iter_append_fixed_array(&bytes, 'y', &p, n);
      end(&one, &bytes);
      end(&list, &one);
      SDL_free(p);
   }
   end(it, &list);
}

static const struct property
{
   const char *interface, *name, *signature, *text;
   Uint32 number;
} properties[] = {
   { ITEM, "Category", "s", "ApplicationStatus", 0 },
   { ITEM, "Id", "s", "fusebox", 0 },
   { ITEM, "Title", "s", "FuseBox", 0 },
   { ITEM, "Status", "s", "Active", 0 },
   { ITEM, "WindowId", "i", NULL, 0 },
   { ITEM, "IconThemePath", "s", "", 0 },
   { ITEM, "IconName", "s", "", 0 },
   { ITEM, "IconPixmap", "a(iiay)", NULL, 0 },
   { ITEM, "OverlayIconName", "s", "", 0 },
   { ITEM, "AttentionIconName", "s", "", 0 },
   { ITEM, "AttentionMovieName", "s", "", 0 },
   { ITEM, "ToolTip", "(sa(iiay)ss)", NULL, 0 },
   { ITEM, "ItemIsMenu", "b", NULL, 0 },
   { ITEM, "Menu", "o", MENU_PATH, 0 },
   { MENU, "Version", "u", NULL, 3 },
   { MENU, "TextDirection", "s", "ltr", 0 },
   { MENU, "Status", "s", "normal", 0 },
   { MENU, "IconThemePath", "as", NULL, 0 },
};

/* As a variant. */
static void put_property(struct iter *it, const struct property *p)
{
   struct iter v, sub;

   begin(it, 'v', p->signature, &v);
   if (p->text)
      put_text(&v, p->signature[0], p->text);
   else if (!p->signature[1])
      put_number(&v, p->signature[0], p->number);
   else if (p->signature[0] == '(')
   {
      begin(&v, 'r', NULL, &sub);
      put_text(&sub, 's', "");
      put_empty(&sub, "(iiay)");
      put_text(&sub, 's', "FuseBox");
      put_text(&sub, 's', "");
      end(&v, &sub);
   }
   else if (p->signature[1] == '(')
      put_icons(&v);
   else
      put_empty(&v, "s");
   end(it, &v);
}

/* The menu's items by their ids; 0 is the menu. */
enum { MENU_ALL, MENU_OPEN, MENU_LINE, MENU_QUIT, MENU_ITEMS };

static const char *const items[MENU_ITEMS][2] = {
   { "children-display", "submenu" },
   { "label", "Open FuseBox" },
   { "type", "separator" },
   { "label", "Quit" },
};

/* a{sv}: what's not said is as it is for most (enabled, visible). */
static void put_item_properties(struct iter *it, int id)
{
   struct iter list, entry, v;

   begin(it, 'a', "{sv}", &list);
   begin(&list, 'e', NULL, &entry);
   put_text(&entry, 's', items[id][0]);
   begin(&entry, 'v', "s", &v);
   put_text(&v, 's', items[id][1]);
   end(&entry, &v);
   end(&list, &entry);
   end(it, &list);
}

/* (ia{sv}av) */
static void put_item(struct iter *it, int id, int depth)
{
   struct iter item, children, v;
   int i;

   begin(it, 'r', NULL, &item);
   put_number(&item, 'i', (Uint32)id);
   put_item_properties(&item, id);
   begin(&item, 'a', "v", &children);
   for (i = MENU_ALL + 1; id == MENU_ALL && depth != 0 && i < MENU_ITEMS; i++)
   {
      begin(&children, 'v', "(ia{sv}av)", &v);
      put_item(&v, i, 0);
      end(&children, &v);
   }
   end(&item, &children);
   end(it, &item);
}

/* ---- What's read from them ----------------------------------------------- */

static bool read_text(struct iter *it, const char **text)
{
   if (dbus.message_iter_get_arg_type(it) != 's')
      return false;
   dbus.message_iter_get_basic(it, text);
   dbus.message_iter_next(it);
   return true;
}

static bool read_int(struct iter *it, int *n)
{
   Sint32 value;

   if (dbus.message_iter_get_arg_type(it) != 'i')
      return false;
   dbus.message_iter_get_basic(it, &value);
   dbus.message_iter_next(it);
   *n = value;
   return true;
}

/* isvu */
static void menu_event(struct iter *it)
{
   const char *what;
   int id;

   if (!read_int(it, &id) || !read_text(it, &what) || SDL_strcmp(what, "clicked") != 0)
      return;
   if (id == MENU_OPEN)
      tray_open();
   else if (id == MENU_QUIT)
   {
      SDL_SignalSemaphore(tray.quit);
      SDL_SetAtomicInt(&tray.stop, 1);
   }
}

/* ---- Calls ---------------------------------------------------------------- */

/* For those that ask what there is rather than know it. */
static const char item_described[] =
   "<node><interface name='" ITEM "'>"
   "<property name='Category' type='s' access='read'/>"
   "<property name='Id' type='s' access='read'/>"
   "<property name='Title' type='s' access='read'/>"
   "<property name='Status' type='s' access='read'/>"
   "<property name='WindowId' type='i' access='read'/>"
   "<property name='IconThemePath' type='s' access='read'/>"
   "<property name='IconName' type='s' access='read'/>"
   "<property name='IconPixmap' type='a(iiay)' access='read'/>"
   "<property name='OverlayIconName' type='s' access='read'/>"
   "<property name='AttentionIconName' type='s' access='read'/>"
   "<property name='AttentionMovieName' type='s' access='read'/>"
   "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
   "<property name='ItemIsMenu' type='b' access='read'/>"
   "<property name='Menu' type='o' access='read'/>"
   "<method name='Activate'><arg type='i'/><arg type='i'/></method>"
   "<method name='SecondaryActivate'><arg type='i'/><arg type='i'/></method>"
   "<method name='ContextMenu'><arg type='i'/><arg type='i'/></method>"
   "<method name='Scroll'><arg type='i'/><arg type='s'/></method>"
   "<method name='ProvideXdgActivationToken'><arg type='s'/></method>"
   "</interface></node>";

static const char menu_described[] =
   "<node><interface name='" MENU "'>"
   "<property name='Version' type='u' access='read'/>"
   "<property name='TextDirection' type='s' access='read'/>"
   "<property name='Status' type='s' access='read'/>"
   "<property name='IconThemePath' type='as' access='read'/>"
   "<method name='GetLayout'><arg type='i'/><arg type='i'/><arg type='as'/>"
   "<arg type='u' direction='out'/><arg type='(ia{sv}av)' direction='out'/></method>"
   "<method name='GetGroupProperties'><arg type='ai'/><arg type='as'/>"
   "<arg type='a(ia{sv})' direction='out'/></method>"
   "<method name='GetProperty'><arg type='i'/><arg type='s'/>"
   "<arg type='v' direction='out'/></method>"
   "<method name='Event'><arg type='i'/><arg type='s'/><arg type='v'/><arg type='u'/></method>"
   "<method name='EventGroup'><arg type='a(isvu)'/><arg type='ai' direction='out'/></method>"
   "<method name='AboutToShow'><arg type='i'/><arg type='b' direction='out'/></method>"
   "<method name='AboutToShowGroup'><arg type='ai'/><arg type='ai' direction='out'/>"
   "<arg type='ai' direction='out'/></method>"
   "</interface></node>";

/* Each writes the answer to out; false if there's none to be given. */
static bool on_properties(const char *member, struct iter *in, struct iter *out)
{
   const char *interface, *name = NULL;
   struct iter list, entry;
   bool all = SDL_strcmp(member, "GetAll") == 0, found = false;
   size_t i;

   if (!read_text(in, &interface)
         || (!all && (SDL_strcmp(member, "Get") != 0 || !read_text(in, &name))))
      return false;
   if (all)
      begin(out, 'a', "{sv}", &list);
   for (i = 0; i < SDL_arraysize(properties); i++)
   {
      const struct property *p = &properties[i];

      if (SDL_strcmp(p->interface, interface) != 0 || (!all && SDL_strcmp(p->name, name) != 0))
         continue;
      found = true;
      if (!all)
      {
         put_property(out, p);
         continue;
      }
      begin(&list, 'e', NULL, &entry);
      put_text(&entry, 's', p->name);
      put_property(&entry, p);
      end(&list, &entry);
   }
   if (all)
      end(out, &list);
   return found || all;
}

static bool on_item(const char *member, struct iter *in)
{
   const char *token;

   if (SDL_strcmp(member, "Activate") == 0)
      tray_open();
   else if (SDL_strcmp(member, "ProvideXdgActivationToken") == 0 && read_text(in, &token))
      SDL_strlcpy(tray.token, token, sizeof(tray.token));
   else
      /* The desktop shows the menu by itself. */
      return SDL_strcmp(member, "SecondaryActivate") == 0 || SDL_strcmp(member, "ContextMenu") == 0
         || SDL_strcmp(member, "Scroll") == 0;
   return true;
}

static bool on_menu(const char *member, struct iter *in, struct iter *out)
{
   struct iter list, sub;
   const char *name;
   int id, depth;

   if (SDL_strcmp(member, "GetLayout") == 0)
   {
      if (!read_int(in, &id) || !read_int(in, &depth) || id < 0 || id >= MENU_ITEMS)
         return false;
      put_number(out, 'u', 1);
      put_item(out, id, depth);
   }
   else if (SDL_strcmp(member, "GetGroupProperties") == 0)
   {
      bool wanted[MENU_ITEMS] = { false }, any = false;

      if (dbus.message_iter_get_arg_type(in) != 'a')
         return false;
      dbus.message_iter_recurse(in, &sub);
      while (read_int(&sub, &id))
      {
         any = true;
         if (id >= 0 && id < MENU_ITEMS)
            wanted[id] = true;
      }
      begin(out, 'a', "(ia{sv})", &list);
      for (id = 0; id < MENU_ITEMS; id++)
         if (wanted[id] || !any)
         {
            begin(&list, 'r', NULL, &sub);
            put_number(&sub, 'i', (Uint32)id);
            put_item_properties(&sub, id);
            end(&list, &sub);
         }
      end(out, &list);
   }
   else if (SDL_strcmp(member, "GetProperty") == 0)
   {
      if (!read_int(in, &id) || !read_text(in, &name) || id < 0 || id >= MENU_ITEMS
            || SDL_strcmp(name, items[id][0]) != 0)
         return false;
      begin(out, 'v', "s", &sub);
      put_text(&sub, 's', items[id][1]);
      end(out, &sub);
   }
   else if (SDL_strcmp(member, "Event") == 0)
      menu_event(in);
   else if (SDL_strcmp(member, "EventGroup") == 0)
   {
      if (dbus.message_iter_get_arg_type(in) != 'a')
         return false;
      dbus.message_iter_recurse(in, &list);
      for (; dbus.message_iter_get_arg_type(&list) == 'r'; dbus.message_iter_next(&list))
      {
         dbus.message_iter_recurse(&list, &sub);
         menu_event(&sub);
      }
      put_empty(out, "i");
   }
   else if (SDL_strcmp(member, "AboutToShow") == 0)
      put_number(out, 'b', 0);
   else if (SDL_strcmp(member, "AboutToShowGroup") == 0)
   {
      put_empty(out, "i");
      put_empty(out, "i");
   }
   else
      return false;
   return true;
}

/* Tells the desktop of the icon. */
static void tray_add(void)
{
   void *m = dbus.message_new_method_call(WATCHER, "/StatusNotifierWatcher", WATCHER,
         "RegisterStatusNotifierItem");
   struct iter it;

   if (!m)
      return;
   dbus.message_set_auto_start(m, 0);
   dbus.message_iter_init_append(m, &it);
   put_text(&it, 's', tray.name);
   dbus.connection_send(tray.bus, m, NULL);
   dbus.message_unref(m);
}

static int on_signal(void *m, const char *interface, const char *member)
{
   const char *text;
   struct iter in;

   /* The desktop's part came, at login maybe after this, or came back. Said are
    * its name, its old owner and its new one. */
   if (SDL_strcmp(interface, "org.freedesktop.DBus") == 0
         && SDL_strcmp(member, "NameOwnerChanged") == 0 && dbus.message_iter_init(m, &in)
         && read_text(&in, &text) && SDL_strcmp(text, WATCHER) == 0 && read_text(&in, &text)
         && read_text(&in, &text) && text[0])
      tray_add();
   return NOT_HANDLED;
}

static int on_message(void *c, void *m, void *user)
{
   const char *interface = dbus.message_get_interface(m), *member = dbus.message_get_member(m);
   const char *path = dbus.message_get_path(m);
   struct iter in, out;
   void *reply;
   bool ok;

   (void)user;
   if (!interface || !member || !path)
      return NOT_HANDLED;
   if (dbus.message_get_type(m) == SIGNAL)
      return on_signal(m, interface, member);
   if (dbus.message_get_type(m) != CALL || !(reply = dbus.message_new_method_return(m)))
      return NOT_HANDLED;
   /* With no arguments there's nothing to be read. */
   dbus.message_iter_init(m, &in);
   dbus.message_iter_init_append(reply, &out);
   if (SDL_strcmp(interface, PROPERTIES) == 0)
      ok = on_properties(member, &in, &out);
   else if (SDL_strcmp(interface, ITEM) == 0)
      ok = on_item(member, &in);
   else if (SDL_strcmp(interface, MENU) == 0)
      ok = on_menu(member, &in, &out);
   else if (SDL_strcmp(interface, "org.freedesktop.DBus.Introspectable") == 0)
   {
      bool menu = SDL_strcmp(path, MENU_PATH) == 0;

      ok = menu || SDL_strcmp(path, ITEM_PATH) == 0;
      put_text(&out, 's', menu ? menu_described : item_described);
   }
   else
   {
      dbus.message_unref(reply);
      return NOT_HANDLED;
   }
   if (!ok)
   {
      dbus.message_unref(reply);
      reply = dbus.message_new_error(m, "org.freedesktop.DBus.Error.InvalidArgs", "No such thing");
   }
   if (reply && !dbus.message_get_no_reply(m))
      dbus.connection_send(c, reply, NULL);
   if (reply)
      dbus.message_unref(reply);
   return HANDLED;
}

/* ---- Its thread ------------------------------------------------------------ */

/* Never unloaded: libdbus can't be. */
static bool load_dbus(void)
{
   SDL_SharedObject *lib = SDL_LoadObject("libdbus-1.so.3");

   if (!lib)
      return false;
#define X(type, name, args) \
   if (!(dbus.name = (type (*) args)SDL_LoadFunction(lib, "dbus_" #name))) \
      return false;
   DBUS(X)
#undef X
   return true;
}

static int SDLCALL tray_thread(void *user)
{
   (void)user;
   /* Programs opened from here would have it ignored too. */
   dbus.connection_set_change_sigpipe(0);
   /* 0: the session's. */
   if (!(tray.bus = dbus.bus_get_private(0, NULL)))
      return 0;
   dbus.connection_set_exit_on_disconnect(tray.bus, 0);
   SDL_snprintf(tray.name, sizeof(tray.name), ITEM "-%d-1", (int)getpid());
   dbus.bus_request_name(tray.bus, tray.name, 0, NULL);
   dbus.connection_add_filter(tray.bus, on_message, NULL, NULL);
   dbus.bus_add_match(tray.bus, "type='signal',sender='org.freedesktop.DBus',"
         "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" WATCHER "'", NULL);
   tray_add();
   while (!SDL_GetAtomicInt(&tray.stop) && dbus.connection_read_write_dispatch(tray.bus, 1000))
      if (tray.opened && SDL_WaitProcess(tray.opened, false, NULL))
      {
         SDL_DestroyProcess(tray.opened);
         tray.opened = NULL;
      }
   dbus.connection_flush(tray.bus);
   dbus.connection_close(tray.bus);
   dbus.connection_unref(tray.bus);
   return 0;
}

void platform_tray_start(void)
{
   tray.quit = SDL_CreateSemaphore(0);
   SDL_SetAtomicInt(&tray.stop, 0);
   /* Its own thread: the background one's is busy with the network. */
   if (tray.quit && load_dbus())
      tray.thread = SDL_CreateThread(tray_thread, "tray", NULL);
}

void platform_tray_stop(void)
{
   SDL_SetAtomicInt(&tray.stop, 1);
   if (tray.thread)
      SDL_WaitThread(tray.thread, NULL);
   SDL_DestroyProcess(tray.opened);
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

#endif
