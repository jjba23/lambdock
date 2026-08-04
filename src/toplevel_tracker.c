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

#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>

#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"

#include "dock_window.h"
#include "logger.h"
#include "toplevel_tracker.h"
#include "ui_helpers.h"

/**
 * WindowTracker:
 * Internal callback tracking context mapping a Wayland protocol handle to its
 * DockItem.
 */
typedef struct {
  struct zwlr_foreign_toplevel_handle_v1 *handle;
  LambdockState *state;
  DockItem *dock_item;
  AppWindow *app_win;
} WindowTracker;

/* Global Wayland interface handles */
static struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager = NULL;
static struct wl_seat *default_seat = NULL;

void activate_toplevel_window(struct zwlr_foreign_toplevel_handle_v1 *handle) {
  if (handle && default_seat) {
    // Send Wayland protocol request to activate window surface
    zwlr_foreign_toplevel_handle_v1_activate(handle, default_seat);
  }
}

void close_toplevel_window(struct zwlr_foreign_toplevel_handle_v1 *handle) {
  if (handle) {
    // Send Wayland protocol request to close window surface
    zwlr_foreign_toplevel_handle_v1_close(handle);
  }
}

/* =========================================================================
 * WAYLAND TOPLEVEL WINDOW EVENT LISTENERS
 * ========================================================================= */

static void
toplevel_handle_title(void *data,
                      struct zwlr_foreign_toplevel_handle_v1 *handle,
                      const char *title) {
  (void)handle;
  WindowTracker *tracker = (WindowTracker *)data;

  // Dynamically update stored window title string when application window title
  // changes
  if (tracker && tracker->app_win && title && *title != '\0') {
    g_free(tracker->app_win->title);
    tracker->app_win->title = g_strdup(title);
  }
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

static void
toplevel_handle_app_id(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *handle,
                       const char *app_id) {
  WindowTracker *tracker = (WindowTracker *)data;

  if (!tracker || !app_id || *app_id == '\0')
    return;

  // Ignore Lambdock's own windows (e.g. About Dialog, main window)
  if (g_ascii_strcasecmp(app_id, "lambdock") == 0 ||
      g_ascii_strcasecmp(app_id, "org.jointhefreeworld.lambdock") == 0) {
    return;
  }

  if (tracker->dock_item != NULL)
    return;

  LambdockState *state = tracker->state;
  if (!state)
    return;

  LOG_C_INFO("Tracker", "App ID resolved: '%s'\n", app_id);

  // Search if a favorite dock item already exists matching this app_id
  DockItem *item = find_dock_item_by_app_id(state, app_id);

  if (item != NULL) {
    LOG_C_INFO("Tracker", "Unified window with existing dock item for '%s'",
               app_id);
  } else {
    // If no favorite item matches, instantiate dynamic running app dock item
    LOG_C_INFO("Tracker", "Creating dynamic dock item for running app '%s'\n",
               app_id);
    item = create_dock_item(state, app_id, app_id, NULL, app_id, FALSE);
  }

  // Bind window handle to DockItem window list
  tracker->dock_item = item;
  tracker->app_win = dock_item_add_window(item, handle);
}

static void
toplevel_handle_closed(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *handle) {
  WindowTracker *tracker = (WindowTracker *)data;
  if (!tracker)
    return;

  DockItem *item = tracker->dock_item;
  if (item) {
    LOG_C_INFO("Tracker", "Window closed for app_id: '%s'\n",
               item->app_id ? item->app_id : "unknown");

    // Remove window instance handle from item
    dock_item_remove_window(item, handle);

    // If an unpinned running application closes all instances, clean it up
    if (item->windows == NULL && !item->is_favorite) {
      if (item->state) {
        item->state->dock_items = g_list_remove(item->state->dock_items, item);
      }
      // free_dock_item safely unparents popover FIRST, then removes the button
      free_dock_item(item);
    }
  }

  // Destroy protocol handle context
  if (tracker->handle) {
    zwlr_foreign_toplevel_handle_v1_destroy(tracker->handle);
  }

  g_free(tracker);
}

/* Wayland Event Listener table */
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
  (void)manager;
  LambdockState *state = (LambdockState *)data;
  if (!state)
    return;

  WindowTracker *tracker = g_new0(WindowTracker, 1);
  tracker->handle = toplevel;
  tracker->state = state;

  // Attach event listener callback table to newly opened window surface
  zwlr_foreign_toplevel_handle_v1_add_listener(toplevel, &toplevel_listener,
                                               tracker);
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
  LambdockState *state = (LambdockState *)data;

  // Check for foreign toplevel manager interface
  if (g_strcmp0(interface, zwlr_foreign_toplevel_manager_v1_interface.name) ==
      0) {
    toplevel_manager = wl_registry_bind(
        registry, name, &zwlr_foreign_toplevel_manager_v1_interface, 1);

    if (toplevel_manager) {
      zwlr_foreign_toplevel_manager_v1_add_listener(toplevel_manager,
                                                    &manager_listener, state);
      LOG_C_INFO("Tracker", "Bound foreign toplevel manager successfully.");
    }
  } else if (g_strcmp0(interface, wl_seat_interface.name) == 0) {
    if (!default_seat) {
      default_seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
      LOG_C_INFO("Tracker", "Bound default Wayland seat successfully.");
    }
  }
}

void init_toplevel_tracker(LambdockState *state) {
  if (!state)
    return;

  GdkDisplay *display = gdk_display_get_default();

  // Verify active graphics backend is Wayland
  if (display && GDK_IS_WAYLAND_DISPLAY(display)) {
    struct wl_display *wl_display = gdk_wayland_display_get_wl_display(display);
    struct wl_registry *registry = wl_display_get_registry(wl_display);

    static const struct wl_registry_listener registry_listener = {
        .global = registry_handle_global,
        .global_remove = NULL,
    };

    wl_registry_add_listener(registry, &registry_listener, state);
    LOG_C_INFO("Tracker", "Initialized toplevel tracker listener.\n");
  }
}
