// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock. If not, see <https://www.gnu.org/licenses/>.

#include "toplevel_tracker.h"
#include "dock_window.h"
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
    }
  }
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
      item = create_dock_item(state, app_id, app_id, NULL, app_id, FALSE, TRUE);
    }

    AppWindow *win = dock_item_add_window(item, handle);
    if (top->title && win) {
      g_free(win->title);
      win->title = g_strdup(top->title);
    }

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
    if (!state)
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

static void
manager_handle_finished(void *data,
                        struct zwlr_foreign_toplevel_manager_v1 *manager) {
  (void)data;
  (void)manager;
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
        zwlr_foreign_toplevel_manager_v1_add_listener(toplevel_manager,
                                                      &manager_listener, NULL);
        LOG_C_INFO("Tracker", "Bound foreign toplevel manager successfully.");
      }
    }
  } else if (g_strcmp0(interface, wl_seat_interface.name) == 0) {
    if (!default_seat) {
      default_seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
      LOG_C_INFO("Tracker", "Bound default Wayland seat successfully.");
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
        .global_remove = NULL,
    };

    wl_registry_add_listener(registry, &registry_listener, NULL);
    tracker_initialized = TRUE;
    LOG_C_INFO("Tracker", "Initialized global toplevel tracker listener.");
  }
}

void sync_window_trackers(LambdockState *state) {
  if (!state)
    return;

  for (GList *l = g_master_toplevels; l != NULL; l = l->next) {
    MasterToplevel *top = (MasterToplevel *)l->data;
    if (!top || !top->app_id)
      continue;

    if (is_app_excluded(top->app_id, state->config_path))
      continue;

    DockItem *item = find_dock_item_by_app_id(state, top->app_id);
    if (!item) {
      item = create_dock_item(state, top->app_id, top->app_id, NULL,
                              top->app_id, FALSE, TRUE);
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
