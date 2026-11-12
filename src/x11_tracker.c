// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

/* =========================================================================
 * THE X11 (EWMH) BACKEND
 * =========================================================================
 * On Wayland the compositor tells the dock about windows and the layer shell
 * lets it claim an edge. On X11 both of those are *conventions*: the extended
 * window manager hints, which every desktop there implements. This module
 * speaks them, and nothing else:
 *
 *   1. atoms        -- the names EWMH is written in, interned once
 *   2. queries      -- read a property off a window; no decisions
 *   3. the list     -- which windows exist, tracked in a hash table by XID
 *   4. events       -- one Xlib connection of our own, drained on the main loop
 *   5. requests     -- activate, minimize, close: the same three the popover asks
 *   6. the dock     -- its window's type, state, geometry and struts
 *
 * What the property values *mean* is in core/x11_rules.c, where it is unit
 * tested without an X server. A build without X11 support compiles the no-op
 * half at the bottom, which is what keeps dock_window.c free of #ifdefs.
 *
 * Why a private connection: GDK reads the events of its own X connection and
 * does not hand them out, and a dock that wants `PropertyNotify` on the root
 * window cannot ask GDK for it. One extra socket, watched like any other file
 * descriptor, keeps the tracker out of GDK's event path entirely -- the two
 * only meet at a window id.
 */

#include "x11_tracker.h"

#include "core/x11_rules.h"
#include "logger.h"

#include <glib-unix.h>

#if defined(HAVE_X11) && defined(GDK_WINDOWING_X11)

#include <gdk/x11/gdkx.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

/* =========================================================================
 * 1. ATOMS
 * ========================================================================= */

/* Every atom the backend touches, in one table: EWMH's names, plus the two
 * GTK properties that say what application a window belongs to. */
typedef enum {
  ATOM_NET_CLIENT_LIST,
  ATOM_NET_ACTIVE_WINDOW,
  ATOM_NET_CLOSE_WINDOW,
  ATOM_NET_WM_NAME,
  ATOM_NET_WM_STATE,
  ATOM_NET_WM_STATE_HIDDEN,
  ATOM_NET_WM_STATE_SKIP_TASKBAR,
  ATOM_NET_WM_STATE_ABOVE,
  ATOM_NET_WM_STATE_STICKY,
  ATOM_NET_WM_STATE_SKIP_PAGER,
  ATOM_NET_WM_WINDOW_TYPE,
  ATOM_NET_WM_WINDOW_TYPE_DOCK,
  ATOM_NET_WM_PID,
  ATOM_NET_WM_DESKTOP,
  ATOM_NET_WM_STRUT,
  ATOM_NET_WM_STRUT_PARTIAL,
  ATOM_NET_SUPPORTING_WM_CHECK,
  ATOM_GTK_APPLICATION_ID,
  ATOM_UTF8_STRING,
  ATOM_WM_CLASS,
  ATOM_COUNT
} AtomIndex;

static const char *const ATOM_NAMES[ATOM_COUNT] = {
    "_NET_CLIENT_LIST",
    "_NET_ACTIVE_WINDOW",
    "_NET_CLOSE_WINDOW",
    "_NET_WM_NAME",
    "_NET_WM_STATE",
    "_NET_WM_STATE_HIDDEN",
    "_NET_WM_STATE_SKIP_TASKBAR",
    "_NET_WM_STATE_ABOVE",
    "_NET_WM_STATE_STICKY",
    "_NET_WM_STATE_SKIP_PAGER",
    "_NET_WM_WINDOW_TYPE",
    "_NET_WM_WINDOW_TYPE_DOCK",
    "_NET_WM_PID",
    "_NET_WM_DESKTOP",
    "_NET_WM_STRUT",
    "_NET_WM_STRUT_PARTIAL",
    "_NET_SUPPORTING_WM_CHECK",
    "_GTK_APPLICATION_ID",
    "UTF8_STRING",
    "WM_CLASS",
};

static Atom g_atoms[ATOM_COUNT];

/* EWMH's source indications: 2 means "a pager did this", which is what the dock
 * is, and is what lets a window manager apply its focus-stealing policy. */
#define SOURCE_PAGER 2

/* `_NET_WM_STATE' request actions. */
#define STATE_REMOVE 0
#define STATE_ADD 1

/* =========================================================================
 * 2. QUERIES
 * =========================================================================
 * Property reads only. Each returns FALSE when the property is absent, which is
 * the normal answer for most windows and most properties.
 */

typedef struct X11Tracker {
  Display *display; /* our own connection; NULL when the backend is off */
  Window root;
  guint fd_source;      /* the main-loop watch on @display's socket */
  gulong own_pid;       /* every window of ours is skipped by it */
  GHashTable *windows;  /* XID -> LambdockToplevel*, owned here */
  gulong active_window; /* last `_NET_ACTIVE_WINDOW', 0 when nothing is focused */
  gboolean warned_no_wm;
  gboolean started;
} X11Tracker;

static X11Tracker g_x11;

/* Defined with the public API below; the dock's own surface needs it too. */
static gboolean x11_connect(void);

/**
 * get_property:
 * @window: the window to read from.
 * @property: the atom to read.
 * @type: the type the property is expected to have. Xlib only hands over a
 *  property whose type matches, which is why every caller below names one.
 * @data: (out): the property's data, to be XFree()d by the caller.
 * @n_items: (out): how many items of @data there are.
 *
 * Returns: FALSE when the window has no such property, which is the normal
 * answer for most windows and most properties -- not an error.
 */
static gboolean get_property(Window window, Atom property, Atom type,
                             unsigned char **data, unsigned long *n_items) {
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long bytes_after = 0;

  *data = NULL;
  *n_items = 0;

  if (XGetWindowProperty(g_x11.display, window, property, 0, 4096, False, type,
                         &actual_type, &actual_format, n_items, &bytes_after,
                         data) != Success)
    return FALSE;

  /* Asking for a property a window does not have answers "no data", not an
   * error: the pointer comes back NULL and the type is None. */
  if (!*data || actual_type != type) {
    if (*data)
      XFree(*data);
    *data = NULL;
    *n_items = 0;
    return FALSE;
  }

  return *n_items > 0;
}

/** Reads a UTF-8 string property into a newly allocated string. */
static char *get_string_property(Window window, Atom property) {
  unsigned char *data = NULL;
  unsigned long n_items = 0;

  if (!get_property(window, property, g_atoms[ATOM_UTF8_STRING], &data,
                    &n_items)) {
    /* ICCCM's WM_NAME is Latin-1 in the strict reading and UTF-8 in practice;
     * it is a fallback, not the source of truth. */
    if (!get_property(window, property, XA_STRING, &data, &n_items))
      return NULL;
  }

  char *value = g_strndup((const char *)data, (gsize)n_items);
  XFree(data);

  return value;
}

/**
 * get_id_property:
 * Reads a property that is a single id: a window in `_NET_ACTIVE_WINDOW`, a
 * process in `_NET_WM_PID`.
 */
static gboolean get_id_property(Window window, Atom property, Atom type,
                                gulong *value) {
  unsigned char *data = NULL;
  unsigned long n_items = 0;

  if (!get_property(window, property, type, &data, &n_items))
    return FALSE;

  *value = ((unsigned long *)data)[0];
  XFree(data);

  return TRUE;
}

/**
 * get_list_property:
 * Reads a list property (windows, atoms) as the server handed it over, to be
 * XFree()d by the caller. NULL when there is none.
 */
static unsigned long *get_list_property(Window window, Atom property, Atom type,
                                        unsigned long *n_items) {
  unsigned char *data = NULL;

  *n_items = 0;
  if (!get_property(window, property, type, &data, n_items))
    return NULL;

  return (unsigned long *)data;
}

/** TRUE when the atom list in @property holds @atom. */
static gboolean atom_list_has(Window window, Atom property, Atom atom) {
  unsigned long n_atoms = 0;
  Atom *atoms = (Atom *)get_list_property(window, property, XA_ATOM, &n_atoms);
  gboolean found = FALSE;

  for (unsigned long i = 0; i < n_atoms && !found; i++)
    found = atoms[i] == atom;

  if (atoms)
    XFree(atoms);

  return found;
}

/**
 * window_type_is_desktop_chrome:
 * Whether any of the window's declared types is one the desktop owns.
 *
 * The names are read back from the server because the decision table lives in
 * core/x11_rules.c, where it is tested; a second copy here would be the copy
 * nobody updates. This costs one round trip per window, and only for windows
 * the dock has not seen before.
 */
static gboolean window_type_is_desktop_chrome(Window window) {
  unsigned long n_atoms = 0;
  Atom *atoms = (Atom *)get_list_property(window, g_atoms[ATOM_NET_WM_WINDOW_TYPE],
                                          XA_ATOM, &n_atoms);
  gboolean chrome = FALSE;

  for (unsigned long i = 0; i < n_atoms && !chrome; i++) {
    char *name = XGetAtomName(g_x11.display, atoms[i]);
    chrome = lambdock_x11_type_is_desktop_chrome(name);
    if (name)
      XFree(name);
  }

  if (atoms)
    XFree(atoms);

  return chrome;
}

/**
 * window_app_id:
 * The application a window belongs to, in the order that identifies it best:
 * the GTK application id when the toolkit publishes one, then the WM_CLASS
 * class, then its instance name.
 *
 * Returns: (transfer full) (nullable): the id, or NULL when the window has no
 * WM_CLASS at all -- in which case there is nothing to attach it to.
 */
static char *window_app_id(Window window) {
  char *gtk_id = get_string_property(window, g_atoms[ATOM_GTK_APPLICATION_ID]);
  if (gtk_id && *gtk_id != '\0')
    return gtk_id;
  g_free(gtk_id);

  unsigned char *class_data = NULL;
  unsigned long n_items = 0;
  if (!get_property(window, g_atoms[ATOM_WM_CLASS], XA_STRING, &class_data,
                    &n_items))
    return NULL;

  /* WM_CLASS is two NUL-terminated strings: instance, then class. The property
   * comes from another client, so every read is bounded by the length the
   * server reported rather than by the NULs it was supposed to contain. */
  const char *instance = (const char *)class_data;
  gsize instance_len = strnlen(instance, (gsize)n_items);
  gsize klass_len = 0;
  const char *klass = NULL;

  if (instance_len + 1 < (gsize)n_items) {
    klass = instance + instance_len + 1;
    klass_len = strnlen(klass, (gsize)n_items - instance_len - 1);
  }

  char *app_id = (klass_len > 0) ? g_strndup(klass, klass_len)
                                 : g_strndup(instance, instance_len);
  XFree(class_data);

  return app_id;
}

/** The window's title, or NULL while it has not set one. */
static char *window_title(Window window) {
  char *title = get_string_property(window, g_atoms[ATOM_NET_WM_NAME]);

  /* WM_NAME is the ICCCM fallback for toolkits that set nothing else. */
  if (!title)
    title = get_string_property(window, XA_WM_NAME);

  return title;
}

static LambdockX11WindowState window_state(Window window) {
  return lambdock_x11_window_state(
      atom_list_has(window, g_atoms[ATOM_NET_WM_STATE],
                    g_atoms[ATOM_NET_WM_STATE_HIDDEN]),
      window, g_x11.active_window);
}

/* =========================================================================
 * 3. THE WINDOW LIST
 * =========================================================================
 * One record per window, keyed by XID. Everything below is a diff: the list
 * arrives whole, and the tracker adds what is new, drops what has gone and
 * leaves the rest alone -- the property events keep those current.
 */

/** Reads everything the dock wants out of @window into a new record. */
static LambdockToplevel *tracked_window_new(Window window) {
  LambdockToplevel *top = lambdock_toplevel_new();

  top->x11_window = window;
  top->title = window_title(window);

  LambdockX11WindowState state = window_state(window);
  top->minimized = state.minimized;
  top->activated = state.activated;

  return top;
}

/** Drops one window: the docks forget it, then the record goes. */
static void tracked_window_drop(gulong xid) {
  LambdockToplevel *top = g_hash_table_lookup(g_x11.windows, GSIZE_TO_POINTER(xid));
  if (!top)
    return;

  LOG_C_INFO("Tracker", "Window closed globally: '%s'",
             top->app_id ? top->app_id : "unknown");

  lambdock_toplevel_withdraw(top);
  g_hash_table_remove(g_x11.windows, GSIZE_TO_POINTER(xid));
  lambdock_toplevel_free(top);
}

/**
 * tracked_window_consider:
 * Starts tracking @window when the dock should list it.
 *
 * The three properties EWMH spells "do not list me" are read here, in the order
 * that is cheapest to bail out of: the type list, the taskbar flag, and the pid
 * that says the window is ours.
 */
static void tracked_window_consider(Window window) {
  if (g_hash_table_contains(g_x11.windows, GSIZE_TO_POINTER((gsize)window)))
    return;

  gboolean desktop_chrome = window_type_is_desktop_chrome(window);

  gboolean skip_taskbar = atom_list_has(
      window, g_atoms[ATOM_NET_WM_STATE], g_atoms[ATOM_NET_WM_STATE_SKIP_TASKBAR]);

  gulong pid = 0;
  gboolean own_window =
      get_id_property(window, g_atoms[ATOM_NET_WM_PID], XA_CARDINAL, &pid) &&
      pid == g_x11.own_pid;

  g_autofree char *app_id = window_app_id(window);

  if (!lambdock_x11_shows_window(desktop_chrome, skip_taskbar, own_window,
                                 app_id != NULL))
    return;

  LambdockToplevel *top = tracked_window_new(window);
  top->app_id = g_steal_pointer(&app_id);

  g_hash_table_insert(g_x11.windows, GSIZE_TO_POINTER((gsize)window), top);

  /* Title and state changes on this window are what keep the icon current. */
  XSelectInput(g_x11.display, window,
               PropertyChangeMask | StructureNotifyMask);

  lambdock_toplevel_deliver(top);
}

/** The whole window list, read at once: adds, drops, leaves the rest alone. */
static void sync_window_list(void) {
  unsigned long n_windows = 0;
  Window *windows =
      (Window *)get_list_property(g_x11.root, g_atoms[ATOM_NET_CLIENT_LIST],
                                  XA_WINDOW, &n_windows);

  if (!windows) {
    /* No `_NET_CLIENT_LIST' means no EWMH window manager: an X server alone
     * lists nothing. Say so once, and keep showing the pinned items. */
    if (!g_x11.warned_no_wm) {
      g_x11.warned_no_wm = TRUE;
      LOG_C_WARN("Tracker",
                 "This X11 session has no EWMH window manager: running "
                 "applications and their window menus will not appear.");
    }
  }

  /* A window that left the list is gone, whichever way it went. */
  GList *known = g_hash_table_get_keys(g_x11.windows);
  for (GList *node = known; node != NULL; node = node->next) {
    gboolean still_listed = FALSE;
    for (unsigned long i = 0; i < n_windows && !still_listed; i++)
      still_listed = windows[i] == (Window)GPOINTER_TO_SIZE(node->data);

    if (!still_listed)
      tracked_window_drop(GPOINTER_TO_SIZE(node->data));
  }
  g_list_free(known);

  for (unsigned long i = 0; i < n_windows; i++)
    tracked_window_consider(windows[i]);

  if (windows)
    XFree(windows);
}

/** `_NET_ACTIVE_WINDOW` changed: one window gained the focus, one lost it. */
static void sync_active_window(void) {
  gulong active = 0;
  get_id_property(g_x11.root, g_atoms[ATOM_NET_ACTIVE_WINDOW], XA_WINDOW,
                  &active);
  g_x11.active_window = active;

  GList *known = g_hash_table_get_keys(g_x11.windows);
  for (GList *node = known; node != NULL; node = node->next) {
    LambdockToplevel *top =
        g_hash_table_lookup(g_x11.windows, node->data);
    if (!top)
      continue;

    LambdockX11WindowState state = window_state(top->x11_window);
    lambdock_toplevel_set_state(top, state.minimized, state.activated);
  }
  g_list_free(known);
}

/** Re-reads the properties that change while a window is open. */
static void sync_window_properties(Window window, Atom property) {
  LambdockToplevel *top =
      g_hash_table_lookup(g_x11.windows, GSIZE_TO_POINTER((gsize)window));
  if (!top)
    return;

  if (property == g_atoms[ATOM_NET_WM_NAME] || property == XA_WM_NAME) {
    g_autofree char *title = window_title(window);
    lambdock_toplevel_set_title(top, title);
    return;
  }

  if (property == g_atoms[ATOM_NET_WM_STATE]) {
    LambdockX11WindowState state = window_state(window);
    lambdock_toplevel_set_state(top, state.minimized, state.activated);
    return;
  }

  /* A browser that loads its profile late changes the application it belongs
   * to after its window exists: the id has to be re-read, not remembered. */
  if (property == g_atoms[ATOM_WM_CLASS] ||
      property == g_atoms[ATOM_GTK_APPLICATION_ID]) {
    g_autofree char *app_id = window_app_id(window);
    lambdock_toplevel_set_app_id(top, app_id);
  }
}

/* =========================================================================
 * 4. EVENTS
 * =========================================================================
 * One file descriptor, drained whenever the X server has something to say.
 */

static gboolean on_x11_events(gint fd, GIOCondition condition, gpointer data) {
  (void)fd;
  (void)condition;
  (void)data;

  /* Xlib queues what it has read, not what has arrived: drain the socket and
   * the queue together, so a burst of window events is one pass. */
  XEvent event;
  while (XPending(g_x11.display)) {
    XNextEvent(g_x11.display, &event);

    switch (event.type) {
    case PropertyNotify:
      if (event.xproperty.window == g_x11.root) {
        if (event.xproperty.atom == g_atoms[ATOM_NET_CLIENT_LIST])
          sync_window_list();
        else if (event.xproperty.atom == g_atoms[ATOM_NET_ACTIVE_WINDOW])
          sync_active_window();
        break;
      }

      sync_window_properties(event.xproperty.window, event.xproperty.atom);
      break;

    case DestroyNotify:
      tracked_window_drop(event.xdestroywindow.window);
      break;

    default:
      break;
    }
  }

  return G_SOURCE_CONTINUE;
}

/**
 * x11_error_handler:
 * Counts a window that vanished between the list and the read as "gone".
 *
 * Reading a property of a window that has just been destroyed is a race the
 * server reports as BadWindow, and Xlib's default handler prints it and keeps
 * going -- a line of noise per closed window, which is exactly what a tracker
 * reading the window list produces. Every other error goes to whatever handler
 * was installed before this one (GDK's, in practice), so this cannot swallow a
 * real problem.
 */
static XErrorHandler g_previous_error_handler = NULL;

static int x11_error_handler(Display *display, XErrorEvent *event) {
  switch (event->error_code) {
  case BadWindow:
  case BadDrawable:
  case BadMatch:
  case BadAtom:
  case BadValue:
    return 0;
  default:
    break;
  }

  if (g_previous_error_handler)
    return g_previous_error_handler(display, event);

  return 0;
}

/* =========================================================================
 * 5. REQUESTS
 * =========================================================================
 * The three things the popover and the dock's click policy ask for.
 */

/** Sends one root-window client message, the way EWMH asks for a request. */
static void send_root_message(Window target, Atom message, long arg0,
                              long arg1, long arg2, long arg3) {
  XEvent event = {0};

  event.xclient.type = ClientMessage;
  event.xclient.window = target;
  event.xclient.message_type = message;
  event.xclient.format = 32;
  event.xclient.data.l[0] = arg0;
  event.xclient.data.l[1] = arg1;
  event.xclient.data.l[2] = arg2;
  event.xclient.data.l[3] = arg3;

  XSendEvent(g_x11.display, g_x11.root, False,
             SubstructureRedirectMask | SubstructureNotifyMask, &event);
  XFlush(g_x11.display);
}

/** Adds or removes one `_NET_WM_STATE' flag on a window. */
static void request_state_change(Window window, long action, Atom state,
                                 Atom state2) {
  send_root_message(window, g_atoms[ATOM_NET_WM_STATE], action, (long)state,
                    (long)state2, SOURCE_PAGER);
}

void x11_tracker_activate(LambdockToplevel *window) {
  if (!window || window->x11_window == 0 || !g_x11.display)
    return;

  /* A minimized window has to be restored as well as raised: the two requests
   * are independent in EWMH, so asking for focus alone would leave a
   * tucked-away window tucked away. */
  request_state_change(window->x11_window, STATE_REMOVE,
                       g_atoms[ATOM_NET_WM_STATE_HIDDEN], None);

  send_root_message(window->x11_window, g_atoms[ATOM_NET_ACTIVE_WINDOW],
                    SOURCE_PAGER, CurrentTime, 0, 0);
}

void x11_tracker_minimize(LambdockToplevel *window) {
  if (!window || window->x11_window == 0 || !g_x11.display)
    return;

  /* WM_CHANGE_STATE is the ICCCM request every window manager implements,
   * reparenting or not; EWMH's hidden flag is advisory on top of it. */
  XIconifyWindow(g_x11.display, window->x11_window,
                 DefaultScreen(g_x11.display));
  XFlush(g_x11.display);
}

void x11_tracker_close(LambdockToplevel *window) {
  if (!window || window->x11_window == 0 || !g_x11.display)
    return;

  /* The same request the window's own close button makes: the application gets
   * to save its work, and can even cancel. */
  send_root_message(window->x11_window, g_atoms[ATOM_NET_CLOSE_WINDOW],
                    CurrentTime, SOURCE_PAGER, 0, 0);
}

/* =========================================================================
 * 6. THE DOCK'S OWN SURFACE
 * =========================================================================
 * On Wayland the layer shell does this. Here the dock sets its own geometry and
 * asks the window manager, through a strut, not to put anything on top of it.
 */

/** The dock's window id, or 0 when it is not an X11 surface. */
static Window dock_xid(const LambdockState *state) {
  if (!state || !state->window || !GTK_IS_WINDOW(state->window))
    return 0;

  GtkNative *native = gtk_widget_get_native(state->window);
  GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
  if (!surface || !GDK_IS_X11_SURFACE(surface))
    return 0;

  /* Deprecated in GTK 4.18 without a replacement: an X11 window id is the one
   * handle a strut, a move and a client message are all addressed to, and GDK
   * offers no other way to name the window it created. */
  Window xid = 0;
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  xid = gdk_x11_surface_get_xid(GDK_X11_SURFACE(surface));
  G_GNUC_END_IGNORE_DEPRECATIONS

  return xid;
}

/**
 * dock_actual_geometry:
 * Where the dock's window really is, in root coordinates.
 *
 * XGetGeometry answers in the window's parent's coordinates, and a window
 * manager reparents top-level windows into a frame, so the position is
 * translated to the root rather than read off the geometry call.
 */
static gboolean dock_actual_geometry(Window xid, int *x, int *y, int *width,
                                     int *height) {
  Window returned_root = None;
  int window_x = 0;
  int window_y = 0;
  unsigned int window_width = 0;
  unsigned int window_height = 0;
  unsigned int border = 0;
  unsigned int depth = 0;

  if (!XGetGeometry(g_x11.display, xid, &returned_root, &window_x, &window_y,
                    &window_width, &window_height, &border, &depth))
    return FALSE;

  int root_x = 0;
  int root_y = 0;
  Window child = None;
  if (!XTranslateCoordinates(g_x11.display, xid, g_x11.root, 0, 0, &root_x,
                             &root_y, &child))
    return FALSE;

  *x = root_x;
  *y = root_y;
  *width = (int)window_width;
  *height = (int)window_height;

  return TRUE;
}

/** Whether the strut last published is the one @placement describes. */
static gboolean strut_published(const LambdockX11Placement *placement) {
  static LambdockX11Placement published;
  static gboolean published_once = FALSE;

  if (published_once && published.strut_left == placement->strut_left &&
      published.strut_right == placement->strut_right &&
      published.strut_top == placement->strut_top &&
      published.strut_bottom == placement->strut_bottom &&
      published.strut_bottom_start_x == placement->strut_bottom_start_x &&
      published.strut_bottom_end_x == placement->strut_bottom_end_x &&
      published.strut_top_start_x == placement->strut_top_start_x &&
      published.strut_top_end_x == placement->strut_top_end_x &&
      published.strut_left_start_y == placement->strut_left_start_y &&
      published.strut_left_end_y == placement->strut_left_end_y &&
      published.strut_right_start_y == placement->strut_right_start_y &&
      published.strut_right_end_y == placement->strut_right_end_y)
    return TRUE;

  published = *placement;
  published_once = TRUE;
  return FALSE;
}

/** Writes an array of longs as a 32-bit property. */
static void set_cardinal_list(Window window, Atom property, const long *values,
                              int count) {
  XChangeProperty(g_x11.display, window, property, XA_CARDINAL, 32,
                  PropModeReplace, (const unsigned char *)values, count);
}

static void set_atom_value(Window window, Atom property, Atom value) {
  XChangeProperty(g_x11.display, window, property, XA_ATOM, 32,
                  PropModeReplace, (const unsigned char *)&value, 1);
}

/**
 * dock_set_window_state:
 * Declares the dock's window as what it is: a dock, above the applications,
 * pinned to every desktop, and absent from taskbars and pagers.
 *
 * The flags are published twice on purpose. As a property, this is what the
 * window manager reads when it manages the window -- and GTK writes its own
 * `_NET_WM_STATE' when it maps a toplevel, so the property is set again once the
 * window is mapped, or GTK's empty list would be the last word. As a client
 * message, it is what EWMH specifies for a window that is already mapped, which
 * is also the case for a dock a reload re-presented.
 */
static void dock_set_window_state(Window xid, gboolean mapped) {
  set_atom_value(xid, g_atoms[ATOM_NET_WM_WINDOW_TYPE],
                 g_atoms[ATOM_NET_WM_WINDOW_TYPE_DOCK]);

  Atom states[4] = {g_atoms[ATOM_NET_WM_STATE_STICKY],
                    g_atoms[ATOM_NET_WM_STATE_ABOVE],
                    g_atoms[ATOM_NET_WM_STATE_SKIP_TASKBAR],
                    g_atoms[ATOM_NET_WM_STATE_SKIP_PAGER]};
  XChangeProperty(g_x11.display, xid, g_atoms[ATOM_NET_WM_STATE], XA_ATOM, 32,
                  PropModeReplace, (const unsigned char *)states, 4);

  /* 0xFFFFFFFF is "on all desktops", the same as the sticky flag above for
   * window managers that read one and not the other. */
  const long all_desktops = 0xFFFFFFFF;
  set_cardinal_list(xid, g_atoms[ATOM_NET_WM_DESKTOP], &all_desktops, 1);

  if (mapped) {
    /* A managed window changes its state through the window manager, two flags
     * per message, which is also how a running dock re-asserts itself after a
     * reload re-presented it. */
    request_state_change(xid, STATE_ADD, g_atoms[ATOM_NET_WM_STATE_STICKY],
                         g_atoms[ATOM_NET_WM_STATE_ABOVE]);
    request_state_change(xid, STATE_ADD, g_atoms[ATOM_NET_WM_STATE_SKIP_TASKBAR],
                         g_atoms[ATOM_NET_WM_STATE_SKIP_PAGER]);
  }

  XFlush(g_x11.display);
}

/** Publishes the reservation so the work area stops at the dock. */
static void dock_set_struts(Window xid, const LambdockX11Placement *placement) {
  long partial[12] = {
      placement->strut_left,
      placement->strut_right,
      placement->strut_top,
      placement->strut_bottom,
      placement->strut_left_start_y,
      placement->strut_left_end_y,
      placement->strut_right_start_y,
      placement->strut_right_end_y,
      placement->strut_top_start_x,
      placement->strut_top_end_x,
      placement->strut_bottom_start_x,
      placement->strut_bottom_end_x,
  };

  set_cardinal_list(xid, g_atoms[ATOM_NET_WM_STRUT_PARTIAL], partial, 12);

  /* The four-value version is the older hint, for window managers that never
   * learned the partial one; the ranges above are simply not expressible. */
  long struts[4] = {placement->strut_left, placement->strut_right,
                    placement->strut_top, placement->strut_bottom};
  set_cardinal_list(xid, g_atoms[ATOM_NET_WM_STRUT], struts, 4);

  XFlush(g_x11.display);
}

/**
 * dock_monitor_rect:
 * The monitor the dock belongs to, in root coordinates.
 *
 * GTK reports monitor geometry in application pixels and X in device pixels;
 * the scale factor is the bridge between them. It is 1 on every X11 session
 * without fractional scaling, which is almost all of them.
 *
 * shortcut: fractional scaling is rounded to the integer scale factor here, so
 * a dock on a 1.5x monitor can be off by a couple of pixels. Upgrade when GTK
 * exposes a monitor geometry in device pixels.
 */
static gboolean dock_monitor_rect(const LambdockState *state,
                                  LambdockX11Rect *rect) {
  GdkDisplay *display = gdk_display_get_default();
  if (!display)
    return FALSE;

  GdkMonitor *monitor = is_monitor_valid(state->monitor) ? state->monitor : NULL;
  GdkMonitor *owned = NULL;

  if (!monitor) {
    GtkNative *native = gtk_widget_get_native(state->window);
    GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
    if (surface)
      monitor = gdk_display_get_monitor_at_surface(display, surface);
  }

  if (!monitor) {
    GListModel *monitors = gdk_display_get_monitors(display);
    if (monitors && g_list_model_get_n_items(monitors) > 0)
      owned = monitor = g_list_model_get_item(monitors, 0);
  }

  if (!monitor)
    return FALSE;

  GdkRectangle geometry;
  gdk_monitor_get_geometry(monitor, &geometry);
  int scale = gdk_monitor_get_scale_factor(monitor);

  rect->x = geometry.x * scale;
  rect->y = geometry.y * scale;
  rect->width = geometry.width * scale;
  rect->height = geometry.height * scale;

  if (owned)
    g_object_unref(owned);

  return TRUE;
}

void lambdock_x11_dock_apply(LambdockState *state) {
  if (!state || !state->overlay || !x11_connect())
    return;

  Window xid = dock_xid(state);
  if (!xid)
    return;

  /* Typed before the window manager sees the window, and typed again once it is
   * mapped, because mapping is when GTK writes its own window state. */
  gboolean mapped = gtk_widget_get_mapped(state->window);
  if (state->x11_window != xid || mapped) {
    state->x11_window = xid;
    state->x11_placed = FALSE;
    dock_set_window_state(xid, mapped);
  }

  LambdockX11Rect monitor = {0};
  if (!dock_monitor_rect(state, &monitor)) {
    monitor = (LambdockX11Rect){0, 0, DisplayWidth(g_x11.display, 0),
                                DisplayHeight(g_x11.display, 0)};
  }
  LambdockX11Rect root = {0, 0, DisplayWidth(g_x11.display, 0),
                          DisplayHeight(g_x11.display, 0)};

  /* The overlay's size requests already say how big the dock wants to be: the
   * row plus the room it magnifies into, or -1 for a taskbar, which spans its
   * monitor. Reading them back keeps one source of truth for the dock's size. */
  gboolean horizontal =
      (state->orientation == GTK_ORIENTATION_HORIZONTAL);
  int requested_main = -1;
  int requested_cross = -1;
  gtk_widget_get_size_request(state->overlay,
                              horizontal ? &requested_main : &requested_cross,
                              horizontal ? &requested_cross : &requested_main);
  int cross_px = requested_cross > 0 ? requested_cross : dock_cross_axis_dim(state);

  LambdockX11Placement placement = lambdock_x11_placement_compute(
      state->edge, root, monitor, requested_main, cross_px,
      dock_capsule_cross_dim(state));

  /* Compare against where the window actually *is*, not against what was asked
   * for last time. GTK sizes and moves its toplevels itself -- on the first
   * frames of a window, and whenever a size request changes -- so a dock that
   * only remembered its own last request could be moved once and then quietly
   * left wherever GTK put it. Reading the window back makes each call
   * self-correcting, and a window that is already in the right place costs two
   * round trips and no move. */
  int actual_x = 0;
  int actual_y = 0;
  int actual_width = 0;
  int actual_height = 0;
  gboolean geometry_known = dock_actual_geometry(xid, &actual_x, &actual_y,
                                                &actual_width, &actual_height);

  gboolean moved = !geometry_known || actual_x != placement.x ||
                   actual_y != placement.y || actual_width != placement.width ||
                   actual_height != placement.height;

  if (moved) {
    XMoveResizeWindow(g_x11.display, xid, placement.x, placement.y,
                      (unsigned)placement.width, (unsigned)placement.height);
    XFlush(g_x11.display);
  }

  if (moved || !state->x11_placed || state->x11_rect_x != placement.x ||
      state->x11_rect_y != placement.y ||
      state->x11_rect_width != placement.width ||
      state->x11_rect_height != placement.height)
    LOG_C_DEBUG("X11", "dock placed at %d,%d %dx%d (was %d,%d %dx%d)",
                placement.x, placement.y, placement.width, placement.height,
                actual_x, actual_y, actual_width, actual_height);

  state->x11_placed = TRUE;
  state->x11_rect_x = placement.x;
  state->x11_rect_y = placement.y;
  state->x11_rect_width = placement.width;
  state->x11_rect_height = placement.height;

  /* The strut follows the *capsule*, not the window: the room the icons
   * magnify into is the dock's, not the desktop's. Publishing it on every call
   * keeps it right across a mode switch, and it is one property write. */
  if (moved || !strut_published(&placement))
    dock_set_struts(xid, &placement);
}

void lambdock_x11_dock_forget(LambdockState *state) {
  if (!state)
    return;

  state->x11_window = 0;
  state->x11_placed = FALSE;
}

/* =========================================================================
 * PUBLIC API
 * =========================================================================
 */

gboolean lambdock_x11_session(void) {
  GdkDisplay *display = gdk_display_get_default();

  return display != NULL && GDK_IS_X11_DISPLAY(display);
}

static void intern_atoms(void) {
  for (int i = 0; i < ATOM_COUNT; i++)
    g_atoms[i] = XInternAtom(g_x11.display, ATOM_NAMES[i], False);
}

/**
 * x11_connect:
 * Opens the private connection and interns the atoms, once.
 *
 * Deliberately separate from starting the tracker: the dock's own window has to
 * be typed and placed from the moment it is mapped, which is before the tracker
 * is asked for the window list -- and a session whose window list cannot be read
 * still gets a dock that reserves its edge.
 *
 * Returns: TRUE when @g_x11.display is usable.
 */
static gboolean x11_connect(void) {
  if (g_x11.display)
    return TRUE;

  GdkDisplay *display = gdk_display_get_default();
  if (!display || !GDK_IS_X11_DISPLAY(display))
    return FALSE;

  /* A connection of our own: GDK reads the events of its own and hands none of
   * them out, and this backend is built on `PropertyNotify` on the root window.
   * The X server allows as many connections as needed, and window ids are
   * server-wide, so the dock's own surface can still be moved from here. */
  g_x11.display = XOpenDisplay(gdk_display_get_name(display));
  if (!g_x11.display)
    return FALSE;

  g_x11.root = DefaultRootWindow(g_x11.display);
  g_x11.own_pid = (gulong)getpid();
  g_x11.windows = g_hash_table_new(g_direct_hash, g_direct_equal);

  g_previous_error_handler = XSetErrorHandler(x11_error_handler);

  intern_atoms();
  return TRUE;
}

gboolean x11_tracker_start(GError **error) {
  if (g_x11.started)
    return TRUE;

  if (!x11_connect()) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Could not open the X display.");
    return FALSE;
  }

  XSelectInput(g_x11.display, g_x11.root, PropertyChangeMask);
  XFlush(g_x11.display);

  sync_active_window();
  sync_window_list();

  g_x11.fd_source = g_unix_fd_add(ConnectionNumber(g_x11.display), G_IO_IN,
                                  on_x11_events, NULL);
  g_x11.started = TRUE;

  LOG_C_INFO("Tracker", "Initialized the X11 (EWMH) window list.");
  return TRUE;
}

void x11_tracker_replay(void) {
  if (!g_x11.display)
    return;

  GList *known = g_hash_table_get_values(g_x11.windows);
  for (GList *node = known; node != NULL; node = node->next)
    lambdock_toplevel_deliver(node->data);
  g_list_free(known);
}

#else /* no X11 in this build */

gboolean lambdock_x11_session(void) { return FALSE; }

gboolean x11_tracker_start(GError **error) {
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      "This build has no X11 support.");
  return FALSE;
}

void x11_tracker_replay(void) {}

void x11_tracker_activate(LambdockToplevel *window) { (void)window; }

void x11_tracker_minimize(LambdockToplevel *window) { (void)window; }

void x11_tracker_close(LambdockToplevel *window) { (void)window; }

void lambdock_x11_dock_apply(LambdockState *state) { (void)state; }

void lambdock_x11_dock_forget(LambdockState *state) { (void)state; }

#endif
