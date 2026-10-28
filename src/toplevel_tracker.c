// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "toplevel_tracker.h"
#include "dock_window.h"
#include "ui/desktop_entry.h"
#include "guile_bridge.h"
#include "logger.h"
#include "ui/dock_item.h"
#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>

#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"

typedef struct {
  struct zwlr_foreign_toplevel_handle_v1 *handle;
  char *title;
  char *app_id;
} MasterToplevel;

static struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager = NULL;
static struct wl_seat *default_seat = NULL;
static gboolean tracker_initialized = FALSE;
static GList *g_master_toplevels = NULL;

/* Registry global names of the bound manager/seat, needed to recognise their
 * removal in registry_handle_global_remove(). 0 means "not bound". */
static uint32_t toplevel_manager_global = 0;
static uint32_t default_seat_global = 0;

void activate_toplevel_window(struct zwlr_foreign_toplevel_handle_v1 *handle) {
  if (handle && default_seat) {
    zwlr_foreign_toplevel_handle_v1_activate(handle, default_seat);
  }
}

void close_toplevel_window(struct zwlr_foreign_toplevel_handle_v1 *handle) {
  if (handle) {
    zwlr_foreign_toplevel_handle_v1_close(handle);
  }
}

/* ============================================================================
 * WAYLAND TOPLEVEL WINDOW EVENT LISTENERS
 * ============================================================================
 */

static void
toplevel_handle_title(void *data,
                      struct zwlr_foreign_toplevel_handle_v1 *handle,
                      const char *title) {
  MasterToplevel *top = (MasterToplevel *)data;
  if (!top || !title)
    return;

  g_free(top->title);
  top->title = g_strdup(title);

  // Broadcast title update across all active docks
  for (GList *l = get_active_dock_states(); l != NULL; l = l->next) {
    LambdockState *state = (LambdockState *)l->data;
    if (!state)
      continue;

    DockItem *item =
        find_dock_item_by_app_id(state, top->app_id ? top->app_id : "");
    if (item) {
      for (GList *w = item->windows; w != NULL; w = w->next) {
        AppWindow *win = (AppWindow *)w->data;
        if (win && win->handle == handle) {
          g_free(win->title);
          win->title = g_strdup(title);
        }
      }
      /* Refresh the XP-style window-title label, if enabled. */
      update_dock_item_status(item);
    }
  }
}

/**
 * create_item_for_app_id:
 * Builds the dock item for an application that is running but not pinned.
 *
 * The compositor gives an app id, which is not a name people want to read
 * ("org.gnome.SystemMonitor") and not always an icon either. The desktop entry
 * cache turns it into the application's localized name, icon and command; when
 * there is no desktop file at all, the id itself is cleaned up into a name
 * ("org.gnome.SystemMonitor" -> "System Monitor") and used as the icon lookup,
 * since icon themes often name the icon after the app id.
 */
static DockItem *create_item_for_app_id(LambdockState *state,
                                        const char *app_id) {
  const LambdockDesktopEntry *entry = lambdock_desktop_entry_lookup(app_id);

  char *name = entry ? g_strdup(entry->name) : lambdock_app_id_to_name(app_id);
  char *icon = entry ? g_strdup(entry->icon) : g_strdup(app_id);
  char *exec = (entry && entry->exec) ? g_strdup(entry->exec) : NULL;

  if (entry) {
    LOG_C_INFO("Tracker", "App id '%s' resolved to %s (%s)", app_id, entry->id,
               entry->name);
  } else {
    LOG_C_INFO("Tracker", "App id '%s' has no desktop entry; showing it as '%s'",
               app_id, name);
  }

  DockItem *item =
      create_dock_item(state, name, icon, exec, app_id, FALSE, TRUE);

  g_free(name);
  g_free(icon);
  g_free(exec);
  return item;
}

static void
toplevel_handle_app_id(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *handle,
                       const char *app_id) {
  MasterToplevel *top = (MasterToplevel *)data;
  if (!top || !app_id || *app_id == '\0')
    return;

  if (g_ascii_strcasecmp(app_id, "lambdock") == 0 ||
      g_ascii_strcasecmp(app_id, "org.jointhefreeworld.lambdock") == 0) {
    return;
  }

  /* If the app_id changed, detach this handle from any item that was tracking
   * it under the previous id. Otherwise that item keeps a stale AppWindow and a
   * permanent running indicator (and leaks the AppWindow). */
  if (top->app_id && g_strcmp0(top->app_id, app_id) != 0) {
    for (GList *l = get_active_dock_states(); l != NULL; l = l->next) {
      LambdockState *state = (LambdockState *)l->data;
      if (!state)
        continue;

      DockItem *old_item = find_dock_item_by_app_id(state, top->app_id);
      if (old_item) {
        dock_item_remove_window(old_item, handle);
        if (old_item->windows == NULL && !old_item->is_favorite) {
          state->dock_items = g_list_remove(state->dock_items, old_item);
          free_dock_item(old_item);
        }
      }
    }
  }

  g_free(top->app_id);
  top->app_id = g_strdup(app_id);

  LOG_C_INFO("Tracker", "App ID resolved globally: '%s'", app_id);

  // Broadcast window creation across all active docks
  for (GList *l = get_active_dock_states(); l != NULL; l = l->next) {
    LambdockState *state = (LambdockState *)l->data;
    if (!state)
      continue;

    if (is_app_excluded(app_id, state->config_path)) {
      LOG_C_INFO("Tracker", "Ignoring excluded app_id '%s' for [%s]", app_id,
                 state->config_path ? state->config_path : "default");
      continue;
    }

    DockItem *item = find_dock_item_by_app_id(state, app_id);
    if (!item) {
      item = create_item_for_app_id(state, app_id);
    }

    AppWindow *win = dock_item_add_window(item, handle);
    if (top->title && win) {
      g_free(win->title);
      win->title = g_strdup(top->title);
    }
    update_dock_item_status(item);

    // Register handle in state window_trackers
    if (!g_list_find(state->window_trackers, handle)) {
      state->window_trackers = g_list_append(state->window_trackers, handle);
    }

    if (state->auto_hide && state->show_when_clear &&
        state->window_trackers != NULL) {
      hide_dock(state);
    }
  }
}

static void
toplevel_handle_closed(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *handle) {
  MasterToplevel *top = (MasterToplevel *)data;
  if (!top)
    return;

  LOG_C_INFO("Tracker", "Window closed globally: '%s'",
             top->app_id ? top->app_id : "unknown");

  // Broadcast window close across all active docks
  for (GList *l = get_active_dock_states(); l != NULL; l = l->next) {
    LambdockState *state = (LambdockState *)l->data;
    if (!state || !state->track_windows)
      continue;

    if (top->app_id) {
      DockItem *item = find_dock_item_by_app_id(state, top->app_id);
      if (item) {
        dock_item_remove_window(item, handle);

        if (item->windows == NULL && !item->is_favorite) {
          state->dock_items = g_list_remove(state->dock_items, item);
          free_dock_item(item);
        }
      }
    }

    state->window_trackers = g_list_remove(state->window_trackers, handle);

    if (state->show_when_clear && state->window_trackers == NULL) {
      reveal_dock(state);
    }
  }

  g_master_toplevels = g_list_remove(g_master_toplevels, top);
  zwlr_foreign_toplevel_handle_v1_destroy(handle);
  g_free(top->title);
  g_free(top->app_id);
  g_free(top);
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

static void
toplevel_handle_state(void *data,
                      struct zwlr_foreign_toplevel_handle_v1 *handle,
                      struct wl_array *state) {
  (void)data;
  (void)handle;
  (void)state;
}

static void
toplevel_handle_done(void *data,
                     struct zwlr_foreign_toplevel_handle_v1 *handle) {
  (void)data;
  (void)handle;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener toplevel_listener =
    {
        .title = toplevel_handle_title,
        .app_id = toplevel_handle_app_id,
        .output_enter = toplevel_handle_output_enter,
        .output_leave = toplevel_handle_output_leave,
        .state = toplevel_handle_state,
        .done = toplevel_handle_done,
        .closed = toplevel_handle_closed,
};

static void
manager_handle_toplevel(void *data,
                        struct zwlr_foreign_toplevel_manager_v1 *manager,
                        struct zwlr_foreign_toplevel_handle_v1 *toplevel) {
  (void)data;
  (void)manager;

  MasterToplevel *top = g_new0(MasterToplevel, 1);
  top->handle = toplevel;
  g_master_toplevels = g_list_append(g_master_toplevels, top);

  zwlr_foreign_toplevel_handle_v1_add_listener(toplevel, &toplevel_listener,
                                               top);
}

/* Free all MasterToplevel bookkeeping, destroying the handle proxies first. */
static void master_toplevels_free(void) {
  for (GList *l = g_master_toplevels; l != NULL; l = l->next) {
    MasterToplevel *top = (MasterToplevel *)l->data;
    if (!top)
      continue;

    if (top->handle)
      zwlr_foreign_toplevel_handle_v1_destroy(top->handle);
    g_free(top->title);
    g_free(top->app_id);
    g_free(top);
  }
  g_list_free(g_master_toplevels);
  g_master_toplevels = NULL;
}

/*
 * tracker_teardown:
 * Called when the compositor withdraws the foreign-toplevel manager (either an
 * explicit "finished" event or removal of the registry global). Every handle we
 * held is now invalid, so drop the bookkeeping, destroy the manager proxy and
 * make each dock forget its tracked windows/handles. Keeping tracker_initialized
 * FALSE lets a future global re-bind via registry_handle_global. */
static void tracker_teardown(void) {
  master_toplevels_free();

  if (toplevel_manager) {
    zwlr_foreign_toplevel_manager_v1_destroy(toplevel_manager);
    toplevel_manager = NULL;
  }
  toplevel_manager_global = 0;
  tracker_initialized = FALSE;

  for (GList *l = get_active_dock_states(); l != NULL; l = l->next) {
    clear_tracked_windows((LambdockState *)l->data);
  }
}

static void
manager_handle_finished(void *data,
                        struct zwlr_foreign_toplevel_manager_v1 *manager) {
  (void)data;
  (void)manager;
  LOG_C_INFO("Tracker", "Foreign toplevel manager finished; tearing down.");
  tracker_teardown();
}

static void registry_handle_global_remove(void *data,
                                          struct wl_registry *registry,
                                          uint32_t name) {
  (void)data;
  (void)registry;

  if (toplevel_manager && name == toplevel_manager_global) {
    LOG_C_INFO("Tracker",
               "Foreign toplevel manager global removed; tearing down.");
    tracker_teardown();
  } else if (default_seat && name == default_seat_global) {
    wl_proxy_destroy((struct wl_proxy *)default_seat);
    default_seat = NULL;
    default_seat_global = 0;
  }
}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener =
    {
        .toplevel = manager_handle_toplevel,
        .finished = manager_handle_finished,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version) {
  (void)version;
  (void)data;

  if (g_strcmp0(interface, zwlr_foreign_toplevel_manager_v1_interface.name) ==
      0) {
    if (!toplevel_manager) {
      toplevel_manager = wl_registry_bind(
          registry, name, &zwlr_foreign_toplevel_manager_v1_interface, 1);

      if (toplevel_manager) {
        toplevel_manager_global = name;
        zwlr_foreign_toplevel_manager_v1_add_listener(toplevel_manager,
                                                      &manager_listener, NULL);
        LOG_C_INFO("Tracker", "Bound foreign toplevel manager successfully.");
      }
    }
  } else if (g_strcmp0(interface, wl_seat_interface.name) == 0) {
    if (!default_seat) {
      default_seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
      if (default_seat) {
        default_seat_global = name;
        LOG_C_INFO("Tracker", "Bound default Wayland seat successfully.");
      }
    }
  }
}

void init_toplevel_tracker(LambdockState *state) {
  (void)state;
  GdkDisplay *display = gdk_display_get_default();

  if (display && GDK_IS_WAYLAND_DISPLAY(display) && !tracker_initialized) {
    struct wl_display *wl_display = gdk_wayland_display_get_wl_display(display);
    struct wl_registry *registry = wl_display_get_registry(wl_display);

    static const struct wl_registry_listener registry_listener = {
        .global = registry_handle_global,
        .global_remove = registry_handle_global_remove,
    };

    wl_registry_add_listener(registry, &registry_listener, NULL);
    tracker_initialized = TRUE;
    LOG_C_INFO("Tracker", "Initialized global toplevel tracker listener.");
  }
}

void sync_window_trackers(LambdockState *state) {
  if (!state || !state->track_windows)
    return;

  for (GList *l = g_master_toplevels; l != NULL; l = l->next) {
    MasterToplevel *top = (MasterToplevel *)l->data;
    if (!top || !top->app_id)
      continue;

    if (is_app_excluded(top->app_id, state->config_path))
      continue;

    DockItem *item = find_dock_item_by_app_id(state, top->app_id);
    if (!item) {
      item = create_item_for_app_id(state, top->app_id);
    }

    dock_item_add_window(item, top->handle);
    if (!g_list_find(state->window_trackers, top->handle)) {
      state->window_trackers =
          g_list_append(state->window_trackers, top->handle);
    }
  }
}

void clear_window_tracker_dock_references(LambdockState *state) { (void)state; }

void free_window_tracker(void *tracker_ptr) { (void)tracker_ptr; }
