// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

/* =========================================================================
 * THE WINDOW TRACKER
 * =========================================================================
 * One tracker per session, shared by every dock: the compositor reports a
 * window, and each dock then decides whether it wants an icon for it, which is
 * why every event below is a broadcast.
 *
 * This file holds both halves of the arrangement, in this order:
 *
 *   1. the broadcast     -- `LambdockToplevel` in, "every dock, now" out. This
 *                           is what the backends call, and the only part they
 *                           share.
 *   2. the dispatch      -- `init_toplevel_tracker()` and friends pick the
 *                           backend this session offers.
 *   3. the Wayland half  -- one registry listener and one foreign-toplevel
 *                           manager, with its proxy lifetimes.
 *   4. public API
 *
 * The X11/EWMH half lives in x11_tracker.c, which fills the same records and
 * calls the same broadcasts; see core/x11_rules.h for the decisions it makes.
 * Nothing here builds a widget, and the decisions ("should this dock hide?",
 * "is this item stale?") live in core/tracker_rules.c, which is unit tested
 * without a display.
 */

#include "toplevel_tracker.h"

#include "app_window.h"
#include "core/tracker_rules.h"
#include "dock_registry.h"
#include "dock_window.h"
#include "guile_bridge.h"
#include "logger.h"
#include "x11_tracker.h"
#include "ui/desktop_entry.h"
#include "ui/dock_item.h"

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif

#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"

/* =========================================================================
 * TYPES
 * =========================================================================
 */

typedef struct ToplevelTracker ToplevelTracker;

/** Which window list this session gave us. */
typedef enum {
  TRACKER_NONE = 0,
  TRACKER_WAYLAND,
  TRACKER_X11,
} TrackerBackend;

/**
 * ToplevelTracker:
 * Everything the Wayland half owns, as one value.
 *
 * There is exactly one instance (see `g_tracker`) because there is exactly one
 * Wayland connection. What changed from the five loose statics this module used
 * to keep is that the fields now have names and a single owner, and that the
 * listeners receive the tracker through Wayland's own `data` pointer instead of
 * reaching for a file-scope variable.
 */
struct ToplevelTracker {
  struct zwlr_foreign_toplevel_manager_v1 *manager;
  uint32_t manager_global; /* registry name of @manager; 0 when unbound */
  struct wl_seat *seat;
  uint32_t seat_global; /* registry name of @seat; 0 when unbound */
  /* The registry listener is installed once and stays installed; see
   * tracker_teardown() for why re-adding it would be wrong. */
  gboolean registry_listener_installed;
  /* How many globals the registry has announced. Non-zero means the registry
   * was dispatched at least once, which is what lets "the manager is missing"
   * be reported as a fact instead of a guess. */
  guint globals_seen;
  gboolean warned_no_seat; /* one diagnostic, not one per click */
  GList *toplevels;        /* LambdockToplevel*, owned here */
};

/* The one tracker. A single named object, rather than a handful of statics,
 * keeps "what does this module own?" answerable at a glance. */
static ToplevelTracker g_tracker;

/* Which backend answered init_toplevel_tracker(); NONE until it has run. */
static TrackerBackend g_backend = TRACKER_NONE;

/* =========================================================================
 * BROADCASTS
 * =========================================================================
 * Every session event means "this, for each dock". Each visitor is a plain
 * function over one dock; `dock_registry_foreach` walks a snapshot, so a
 * visitor may even destroy its own dock without disturbing the walk.
 */

/* Defined below, next to the visitor that needs it. */
static DockItem *create_item_for_app_id(LambdockState *state,
                                        const char *app_id);

/** Payload of the two app-id visitors: the window, and the id it reports now. */
typedef struct {
  LambdockToplevel *window;
  const char *app_id;
} AppIdEvent;

/** Retitles @event->window in one dock. */
static void dock_apply_window_title(LambdockState *state, gpointer data) {
  const LambdockToplevel *top = data;

  DockItem *item = find_dock_item_by_app_id(state, top->app_id);
  if (!item)
    return;

  AppWindow *window = NULL;
  for (const GList *node = item->windows; node != NULL; node = node->next) {
    AppWindow *candidate = node->data;
    if (app_window_matches(candidate, top)) {
      window = candidate;
      break;
    }
  }
  if (!window)
    return;

  /* The session re-announces the title on every change, including changes that
   * do not touch the text we show. Nothing to do means nothing to rebuild: no
   * label, no accessible name, no title tooltip. */
  if (g_strcmp0(window->title, top->title) == 0)
    return;

  app_window_set_title(window, top->title);

  /* Refresh the running state, the accessible name and the window-title
   * label, which all read the title back. */
  update_dock_item_status(item);
}

/** Mirrors @top's minimized/focused state onto the window one dock tracks. */
static void dock_apply_window_state(LambdockState *state, gpointer data) {
  const LambdockToplevel *top = data;

  DockItem *item = find_dock_item_by_app_id(state, top->app_id);
  if (!item)
    return;

  for (GList *node = item->windows; node != NULL; node = node->next) {
    AppWindow *window = node->data;
    if (!app_window_matches(window, top))
      continue;

    app_window_set_state(window, top->minimized, top->activated);

    /* Minimize to dock is only half done if the icon does not show it: dim the
     * item when all of its windows are tucked away (two CSS toggles, no text). */
    dock_item_refresh_window_state(item);
    return;
  }
}

/**
 * dock_detach_previous_app_id:
 * Detaches @event->window from the item that was tracking its previous app id.
 *
 * Applications that load a profile late (Electron apps, browsers) change app id
 * after their window exists. Without this the old item keeps a stale AppWindow
 * and a permanent running indicator.
 */
static void dock_detach_previous_app_id(LambdockState *state, gpointer data) {
  const AppIdEvent *event = data;

  DockItem *item = find_dock_item_by_app_id(state, event->window->app_id);
  if (!item)
    return;

  dock_item_remove_window(item, event->window);

  /* A pinned item stays; an item that only existed because a window did goes
   * away with it. */
  if (lambdock_tracker_item_should_be_dropped(item->windows != NULL,
                                              item->is_favorite)) {
    state->dock_items = g_list_remove(state->dock_items, item);
    free_dock_item(item);
  }
}

/** Attaches @event->window to the item for its current app id. */
static void dock_attach_current_app_id(LambdockState *state, gpointer data) {
  const AppIdEvent *event = data;
  LambdockToplevel *top = event->window;
  const char *app_id = event->app_id;

  /* A dock that does not track windows gets no running-app items here, exactly
   * as sync_window_trackers() gives it none on reload. Without this gate the
   * two paths disagreed and a reload silently removed what a live event had
   * just added. */
  if (!state->track_windows)
    return;

  /* An excluded app is the one case where the dock deliberately does nothing
   * at all, so it stays its own early return. */
  if (is_app_excluded(app_id, state->config_path)) {
    LOG_C_INFO("Tracker", "Ignoring excluded app_id '%s' for [%s]", app_id,
               state->config_path ? state->config_path : "default");
    return;
  }

  DockItem *item = find_dock_item_by_app_id(state, app_id);
  if (!item)
    item = create_item_for_app_id(state, app_id);

  AppWindow *window = dock_item_add_window(item, top);
  app_window_set_state(window, top->minimized, top->activated);
  app_window_set_title(window, top->title);

  if (!g_list_find(state->window_trackers, top))
    state->window_trackers = g_list_append(state->window_trackers, top);

  update_dock_item_status(item);

  if (lambdock_tracker_should_hide(state->auto_hide, state->show_when_clear,
                                   state->window_trackers != NULL))
    hide_dock(state);
}

/** Forgets @top's window in one dock, because the session closed it. */
static void dock_forget_closed_window(LambdockState *state, gpointer data) {
  const LambdockToplevel *top = data;

  /* Mirror of the gate above: a dock that never attached this window has no
   * item to detach it from, and no window_trackers entry either. */
  if (!state->track_windows)
    return;

  DockItem *item = find_dock_item_by_app_id(state, top->app_id);
  if (item) {
    dock_item_remove_window(item, top);

    if (lambdock_tracker_item_should_be_dropped(item->windows != NULL,
                                                item->is_favorite)) {
      state->dock_items = g_list_remove(state->dock_items, item);
      free_dock_item(item);
    }
  }

  state->window_trackers = g_list_remove(state->window_trackers, top);

  if (lambdock_tracker_should_reveal(state->show_when_clear,
                                     state->window_trackers != NULL))
    reveal_dock(state);
}

/** Drops every tracked window of one dock, because the tracker went away. */
static void dock_forget_all_windows(LambdockState *state, gpointer data) {
  (void)data;

  clear_tracked_windows(state);
}

/**
 * create_item_for_app_id:
 * Builds the dock item for an application that is running but not pinned.
 *
 * The session gives an app id, which is not a name people want to read
 * ("org.gnome.SystemMonitor") and not always an icon either. The desktop entry
 * cache turns it into the application's localized name, icon and command; when
 * there is no desktop file at all, the id itself is cleaned up into a name
 * ("org.gnome.SystemMonitor" -> "System Monitor") and used as the icon lookup,
 * since icon themes often name the icon after the app id.
 */
static DockItem *create_item_for_app_id(LambdockState *state,
                                        const char *app_id) {
  const LambdockDesktopEntry *entry = lambdock_desktop_entry_lookup(app_id);

  g_autofree char *name =
      entry ? g_strdup(entry->name) : lambdock_app_id_to_name(app_id);
  g_autofree char *icon = entry ? g_strdup(entry->icon) : g_strdup(app_id);
  g_autofree char *exec = (entry && entry->exec) ? g_strdup(entry->exec) : NULL;

  if (entry) {
    LOG_C_INFO("Tracker", "App id '%s' resolved to %s (%s)", app_id, entry->id,
               entry->name);
  } else {
    LOG_C_INFO("Tracker",
               "App id '%s' has no desktop entry; showing it as '%s'", app_id,
               name);
  }

  return create_dock_item(state, name, icon, exec, app_id, FALSE, TRUE);
}

/* =========================================================================
 * THE BROADCAST API
 * =========================================================================
 * What both backends call. Values in, every dock updated, no proxy touched.
 */

LambdockToplevel *lambdock_toplevel_new(void) {
  return g_new0(LambdockToplevel, 1);
}

void lambdock_toplevel_free(LambdockToplevel *window) {
  if (!window)
    return;

  g_free(window->title);
  g_free(window->app_id);
  g_free(window);
}

void lambdock_toplevel_set_app_id(LambdockToplevel *window,
                                  const char *app_id) {
  if (!window || lambdock_tracker_ignores_app_id(app_id))
    return;

  /* Detach from the previous id before recording the new one: the detach has to
   * look up the old item by the old app id, which is about to be overwritten. */
  if (window->app_id && g_strcmp0(window->app_id, app_id) != 0) {
    AppIdEvent detach_event = {.window = window, .app_id = window->app_id};
    dock_registry_foreach(dock_detach_previous_app_id, &detach_event);
  }

  g_free(window->app_id);
  window->app_id = g_strdup(app_id);

  LOG_C_INFO("Tracker", "App ID resolved globally: '%s'", app_id);

  AppIdEvent attach_event = {.window = window, .app_id = window->app_id};
  dock_registry_foreach(dock_attach_current_app_id, &attach_event);
}

void lambdock_toplevel_set_title(LambdockToplevel *window, const char *title) {
  if (!window || !title)
    return;

  if (g_strcmp0(window->title, title) == 0)
    return;

  g_free(window->title);
  window->title = g_strdup(title);

  dock_registry_foreach(dock_apply_window_title, window);
}

void lambdock_toplevel_set_state(LambdockToplevel *window, gboolean minimized,
                                 gboolean activated) {
  if (!window)
    return;

  window->minimized = minimized;
  window->activated = activated;

  dock_registry_foreach(dock_apply_window_state, window);
}

void lambdock_toplevel_deliver(LambdockToplevel *window) {
  if (!window || !window->app_id)
    return;

  AppIdEvent attach_event = {.window = window, .app_id = window->app_id};
  dock_registry_foreach(dock_attach_current_app_id, &attach_event);
  dock_registry_foreach(dock_apply_window_title, window);
}

void lambdock_toplevel_withdraw(LambdockToplevel *window) {
  if (!window)
    return;

  dock_registry_foreach(dock_forget_closed_window, window);
}

/* =========================================================================
 * BACKEND DISPATCH
 * =========================================================================
 * One session, one window list. The two halves are compiled in separately, and
 * a session that offers neither is a session where the dock shows its pinned
 * items and says why.
 */

/** True when this record came from the Wayland backend. */
static gboolean toplevel_is_wayland(const LambdockToplevel *window) {
  return window && window->handle != NULL;
}

/* =========================================================================
 * THE WAYLAND HALF
 * =========================================================================
 * Registry binding, the manager, and the listeners. Everything above deals in
 * LambdockToplevel values; only this section touches wl_proxy.
 */

/** Frees every window record, destroying its proxy first. */
static GList *master_toplevels_free(GList *toplevels) {
  for (GList *node = toplevels; node != NULL; node = node->next) {
    LambdockToplevel *top = node->data;
    if (!top)
      continue;

    if (top->handle)
      zwlr_foreign_toplevel_handle_v1_destroy(top->handle);
    lambdock_toplevel_free(top);
  }

  g_list_free(toplevels);
  return NULL;
}

/**
 * tracker_teardown:
 * Takes the Wayland half down because the compositor withdrew the
 * foreign-toplevel manager (a "finished" event or removal of the registry
 * global). Every handle we hold is invalid from then on, so drop the
 * bookkeeping, destroy the manager proxy and make each dock forget its tracked
 * windows.
 *
 * The registry listener deliberately stays installed: the registry itself is
 * still alive and re-announces the global if the compositor brings it back,
 * which re-binds through registry_handle_global(). Re-adding the listener here
 * would instead deliver every subsequent global twice.
 */
static void tracker_teardown(ToplevelTracker *tracker) {
  tracker->toplevels = master_toplevels_free(tracker->toplevels);

  if (tracker->manager) {
    zwlr_foreign_toplevel_manager_v1_destroy(tracker->manager);
    tracker->manager = NULL;
  }
  tracker->manager_global = 0;

  dock_registry_foreach(dock_forget_all_windows, NULL);
}

/* ---- handle events: one adapter per session event ---- */

static void
toplevel_handle_title(void *data,
                      struct zwlr_foreign_toplevel_handle_v1 *handle,
                      const char *title) {
  (void)handle;

  lambdock_toplevel_set_title(data, title);
}

static void
toplevel_handle_app_id(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *handle,
                       const char *app_id) {
  (void)handle;

  lambdock_toplevel_set_app_id(data, app_id);
}

static void
toplevel_handle_closed(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *handle) {
  LambdockToplevel *top = data;
  (void)handle;

  if (!top)
    return;

  LOG_C_INFO("Tracker", "Window closed globally: '%s'",
             top->app_id ? top->app_id : "unknown");

  lambdock_toplevel_withdraw(top);

  g_tracker.toplevels = g_list_remove(g_tracker.toplevels, top);
  if (top->handle)
    zwlr_foreign_toplevel_handle_v1_destroy(top->handle);
  top->handle = NULL;
  lambdock_toplevel_free(top);
}

/* Events this dock has no use for. Kept as named no-ops instead of NULL so the
 * listener table below reads as a complete description of the protocol. */

static void
toplevel_handle_parent(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *handle,
                       struct zwlr_foreign_toplevel_handle_v1 *parent) {
  (void)data;
  (void)handle;
  (void)parent;
}

static void
toplevel_handle_output_enter(void *data,
                             struct zwlr_foreign_toplevel_handle_v1 *handle,
                             struct wl_output *output) {
  (void)data;
  (void)handle;
  (void)output;
}

static void
toplevel_handle_output_leave(void *data,
                             struct zwlr_foreign_toplevel_handle_v1 *handle,
                             struct wl_output *output) {
  (void)data;
  (void)handle;
  (void)output;
}

/** Reads the two flags the dock cares about out of the compositor's array. */
static void master_toplevel_read_state(gboolean *minimized, gboolean *activated,
                                       const struct wl_array *states) {
  *minimized = FALSE;
  *activated = FALSE;

  const uint32_t *entry;
  wl_array_for_each(entry, states) {
    switch (*entry) {
    case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED:
      *minimized = TRUE;
      break;
    case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:
      *activated = TRUE;
      break;
    default:
      /* Maximized and fullscreen do not change what a click should do. */
      break;
    }
  }
}

static void
toplevel_handle_state(void *data,
                      struct zwlr_foreign_toplevel_handle_v1 *handle,
                      struct wl_array *state) {
  (void)handle;

  if (!data || !state)
    return;

  gboolean minimized = FALSE;
  gboolean activated = FALSE;
  master_toplevel_read_state(&minimized, &activated, state);

  lambdock_toplevel_set_state(data, minimized, activated);
}

static void
toplevel_handle_done(void *data,
                     struct zwlr_foreign_toplevel_handle_v1 *handle) {
  (void)data;
  (void)handle;
}

/* Bound at version 1, so `.parent` (since 3) is never delivered today. It is
 * filled in anyway: a future version bump must not hand libwayland a NULL
 * handler for an opcode a compositor may send (that aborts the process). */
static const struct zwlr_foreign_toplevel_handle_v1_listener toplevel_listener =
    {
        .title = toplevel_handle_title,
        .app_id = toplevel_handle_app_id,
        .output_enter = toplevel_handle_output_enter,
        .output_leave = toplevel_handle_output_leave,
        .state = toplevel_handle_state,
        .done = toplevel_handle_done,
        .closed = toplevel_handle_closed,
        .parent = toplevel_handle_parent,
};

/* ---- manager events ---- */

static void
manager_handle_toplevel(void *data,
                        struct zwlr_foreign_toplevel_manager_v1 *manager,
                        struct zwlr_foreign_toplevel_handle_v1 *toplevel) {
  ToplevelTracker *tracker = data;
  (void)manager;

  if (!tracker || !toplevel)
    return;

  LambdockToplevel *top = lambdock_toplevel_new();
  top->handle = toplevel;
  tracker->toplevels = g_list_append(tracker->toplevels, top);

  zwlr_foreign_toplevel_handle_v1_add_listener(toplevel, &toplevel_listener,
                                               top);
}

static void
manager_handle_finished(void *data,
                        struct zwlr_foreign_toplevel_manager_v1 *manager) {
  (void)manager;

  LOG_C_INFO("Tracker", "Foreign toplevel manager finished; tearing down.");
  tracker_teardown(data);
}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener =
    {
        .toplevel = manager_handle_toplevel,
        .finished = manager_handle_finished,
};

/* ---- registry events ---- */

/** Binds the manager global and starts listening for the windows it reports. */
static void tracker_bind_manager(ToplevelTracker *tracker,
                                 struct wl_registry *registry, uint32_t name) {
  tracker->manager = wl_registry_bind(
      registry, name, &zwlr_foreign_toplevel_manager_v1_interface, 1);

  /* No early return with an error to report: the caller is a Wayland event, and
   * a failed bind only means this compositor will not be tracked. */
  if (!tracker->manager)
    return;

  tracker->manager_global = name;
  zwlr_foreign_toplevel_manager_v1_add_listener(tracker->manager,
                                                &manager_listener, tracker);
  LOG_C_INFO("Tracker", "Bound foreign toplevel manager successfully.");
}

/** Binds the seat used to activate windows. */
static void tracker_bind_seat(ToplevelTracker *tracker,
                              struct wl_registry *registry, uint32_t name) {
  tracker->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
  if (!tracker->seat)
    return;

  tracker->seat_global = name;
  LOG_C_INFO("Tracker", "Bound default Wayland seat successfully.");
}

static void registry_handle_global_remove(void *data,
                                          struct wl_registry *registry,
                                          uint32_t name) {
  ToplevelTracker *tracker = data;
  (void)registry;

  if (tracker->manager && name == tracker->manager_global) {
    LOG_C_INFO("Tracker",
               "Foreign toplevel manager global removed; tearing down.");
    tracker_teardown(tracker);
    return;
  }

  if (tracker->seat && name == tracker->seat_global) {
    wl_proxy_destroy((struct wl_proxy *)tracker->seat);
    tracker->seat = NULL;
    tracker->seat_global = 0;
  }
}

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version) {
  ToplevelTracker *tracker = data;
  (void)version;

  if (!tracker || !interface)
    return;

  tracker->globals_seen++;

  if (g_strcmp0(interface, zwlr_foreign_toplevel_manager_v1_interface.name) ==
      0) {
    if (!tracker->manager)
      tracker_bind_manager(tracker, registry, name);
    return;
  }

  if (g_strcmp0(interface, wl_seat_interface.name) == 0 && !tracker->seat)
    tracker_bind_seat(tracker, registry, name);
}

/**
 * tracker_warn_if_untrackable:
 * Says once, and only when it is a fact, that this compositor exposes no
 * foreign-toplevel manager.
 *
 * "A fact" means the registry has announced at least one global: before that
 * the registry has not been dispatched yet, and a missing manager would only be
 * a guess. GNOME/Mutter is the compositor people hit this on -- the dock still
 * runs there, it just cannot show running applications.
 */
static void tracker_warn_if_untrackable(const ToplevelTracker *tracker) {
  static const ToplevelTracker *warned_about = NULL;

  if (tracker->manager || tracker->globals_seen == 0 || warned_about == tracker)
    return;

  warned_about = tracker;
  LOG_C_WARN("Tracker",
             "This compositor does not implement wlr-foreign-toplevel: "
             "running applications and their window menus will not appear.");
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

/** Starts the Wayland half. FALSE with @error when there is no registry. */
static gboolean wayland_tracker_start(GError **error) {
  if (g_tracker.registry_listener_installed)
    return TRUE;

#ifdef GDK_WINDOWING_WAYLAND
  GdkDisplay *display = gdk_display_get_default();
  struct wl_display *wayland_display =
      gdk_wayland_display_get_wl_display(display);
  if (!wayland_display) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "The Wayland display is not available yet.");
    return FALSE;
  }

  struct wl_registry *registry = wl_display_get_registry(wayland_display);
  if (!registry) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Could not get the Wayland registry.");
    return FALSE;
  }

  wl_registry_add_listener(registry, &registry_listener, &g_tracker);
  g_tracker.registry_listener_installed = TRUE;

  LOG_C_INFO("Tracker", "Initialized global toplevel tracker listener.");
  return TRUE;
#else
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      "This build has no Wayland backend.");
  return FALSE;
#endif
}

/* =========================================================================
 * PUBLIC API
 * =========================================================================
 */

gboolean init_toplevel_tracker(GError **error) {
  if (g_backend != TRACKER_NONE)
    return TRUE;

  GdkDisplay *display = gdk_display_get_default();
  if (!display) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "No display connection; window tracking disabled.");
    return FALSE;
  }

#ifdef GDK_WINDOWING_WAYLAND
  if (GDK_IS_WAYLAND_DISPLAY(display)) {
    if (!wayland_tracker_start(error))
      return FALSE;

    g_backend = TRACKER_WAYLAND;
    return TRUE;
  }
#endif

#ifdef GDK_WINDOWING_X11
  if (GDK_IS_X11_DISPLAY(display)) {
    if (!x11_tracker_start(error))
      return FALSE;

    g_backend = TRACKER_X11;
    return TRUE;
  }
#endif

  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      "This session exposes no window list; window tracking "
                      "disabled.");
  return FALSE;
}

void sync_window_trackers(LambdockState *state) {
  if (!state || !state->track_windows)
    return;

  switch (g_backend) {
  case TRACKER_WAYLAND:
    tracker_warn_if_untrackable(&g_tracker);

    for (GList *node = g_tracker.toplevels; node != NULL; node = node->next) {
      LambdockToplevel *top = node->data;

      /* Skip what the compositor has not finished describing: a window with no
       * app id yet has no icon to attach to. */
      if (!top || !top->app_id)
        continue;

      lambdock_toplevel_deliver(top);
    }
    break;

  case TRACKER_X11:
    x11_tracker_replay();
    break;

  case TRACKER_NONE:
  default:
    break;
  }
}

void activate_toplevel_window(LambdockToplevel *window) {
  if (!window)
    return;

  if (!toplevel_is_wayland(window)) {
    x11_tracker_activate(window);
    return;
  }

  /* A minimized window has to be un-minimized as well as activated: the
   * protocol treats the two requests independently, so asking for focus alone
   * would leave a tucked-away window tucked away on some compositors. */
  zwlr_foreign_toplevel_handle_v1_unset_minimized(window->handle);

  /* activate() needs the seat the request is made for. A compositor that
   * advertises no wl_seat (or withdrew it) can only be half-asked, so say so
   * once instead of pretending the click did something. */
  if (!g_tracker.seat) {
    if (!g_tracker.warned_no_seat) {
      g_tracker.warned_no_seat = TRUE;
      LOG_C_WARN("Tracker",
                 "No wl_seat on this session: the compositor cannot be asked "
                 "to focus a window.");
    }
    return;
  }

  zwlr_foreign_toplevel_handle_v1_activate(window->handle, g_tracker.seat);
}

void minimize_toplevel_window(LambdockToplevel *window) {
  if (!window)
    return;

  if (!toplevel_is_wayland(window)) {
    x11_tracker_minimize(window);
    return;
  }

  zwlr_foreign_toplevel_handle_v1_set_minimized(window->handle);
}

void close_toplevel_window(LambdockToplevel *window) {
  if (!window)
    return;

  if (!toplevel_is_wayland(window)) {
    x11_tracker_close(window);
    return;
  }

  zwlr_foreign_toplevel_handle_v1_close(window->handle);
}

/*
 * Both of the following are no-ops on purpose. `state->window_trackers` holds
 * borrowed records, which the tracker owns and frees when a window closes or
 * the manager goes away. dock_window.c's destroy path walks that list and calls
 * free_window_tracker() for each entry, so these must not free what the tracker
 * still needs.
 */
void clear_window_tracker_dock_references(LambdockState *state) { (void)state; }

void free_window_tracker(void *tracker_ptr) { (void)tracker_ptr; }
