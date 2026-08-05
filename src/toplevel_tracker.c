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

// --- Auto-Generated Protocol Header ---
// Generated from wlr-foreign-toplevel-management-unstable-v1.xml during Meson
// build
#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"

/* ============================================================================
 * DATA STRUCTURES & GLOBAL WAYLAND HANDLES
 * ============================================================================
 */

/**
 * WindowTracker:
 * @handle: Pointer to the Wayland protocol instance for a single foreign window
 * surface.
 * @state: Global Lambdock runtime state container pointer.
 * @dock_item: Associated UI icon widget (`DockItem`) on the bar.
 * @app_win: AppWindow wrapper linking the window handle to the dock item.
 * @title: String buffer holding the window's current title.
 * @app_id: Application desktop ID string reported by the compositor.
 *
 * Private context struct attached as user data (`void *data`) to Wayland event
 * listeners.
 */
typedef struct {
  struct zwlr_foreign_toplevel_handle_v1 *handle;
  LambdockState *state;
  DockItem *dock_item;
  AppWindow *app_win;
  char *title;
  char *app_id;
} WindowTracker;

/* Global handles to bound Wayland registry interfaces */
static struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager =
    NULL; // Manages global window events
static struct wl_seat *default_seat =
    NULL; // User seat (keyboard/pointer focus)

/* ============================================================================
 * PUBLIC WINDOW CONTROLLER ACTIONS
 * ============================================================================
 */

/**
 * activate_toplevel_window:
 * @handle: Foreign Wayland window handle to focus.
 *
 * Requests the Wayland compositor to activate (focus and bring to front) the
 * target window using the bound default user seat.
 */
void activate_toplevel_window(struct zwlr_foreign_toplevel_handle_v1 *handle) {
  if (handle && default_seat) {
    zwlr_foreign_toplevel_handle_v1_activate(handle, default_seat);
  }
}

/**
 * close_toplevel_window:
 * @handle: Foreign Wayland window handle to close.
 *
 * Sends a protocol close request to the compositor for the specified window
 * surface.
 */
void close_toplevel_window(struct zwlr_foreign_toplevel_handle_v1 *handle) {
  if (handle) {
    zwlr_foreign_toplevel_handle_v1_close(handle);
  }
}

/* ============================================================================
 * WAYLAND TOPLEVEL WINDOW EVENT LISTENERS
 * ============================================================================
 */

/**
 * toplevel_handle_title:
 * @data: User data pointer pointing to our `WindowTracker` instance.
 * @handle: Wayland handle that fired the event.
 * @title: New window title string sent by compositor.
 *
 * Callback triggered whenever an open window changes its window title label.
 */
static void
toplevel_handle_title(void *data,
                      struct zwlr_foreign_toplevel_handle_v1 *handle,
                      const char *title) {
  (void)handle; // Mark unused parameter
  WindowTracker *tracker = (WindowTracker *)data;
  if (!tracker)
    return;

  if (title) {
    // Free existing string and store newly received title string
    g_free(tracker->title);
    tracker->title = g_strdup(title);

    // Update active window struct title label if already bound to dock item
    if (tracker->app_win) {
      g_free(tracker->app_win->title);
      tracker->app_win->title = g_strdup(title);
    }
  }
}

/**
 * Stub Event Callbacks:
 * The Wayland protocol definition requires providing callback pointers for
 * output enter, output leave, state updates, and done signals. Even if we do
 * not process them, we provide empty handler stubs to satisfy the C listener
 * struct definition.
 */
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

/**
 * toplevel_handle_app_id:
 * @data: Pointer to our `WindowTracker`.
 * @handle: Wayland foreign window handle.
 * @app_id: Application identity string (e.g., "org.gnome.Nautilus").
 *
 * Callback triggered when the compositor identifies the window's desktop
 * app_id. This function matches the window to an existing pinned icon or spawns
 * a dynamic running icon.
 */
static void
toplevel_handle_app_id(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *handle,
                       const char *app_id) {
  WindowTracker *tracker = (WindowTracker *)data;

  if (!tracker || !app_id || *app_id == '\0')
    return; // Ignore invalid or empty app_ids

  // Filter out Lambdock's own windows to avoid the dock tracking itself
  if (g_ascii_strcasecmp(app_id, "lambdock") == 0 ||
      g_ascii_strcasecmp(app_id, "org.jointhefreeworld.lambdock") == 0) {
    return;
  }

  // Check Scheme configuration rules to see if user has excluded this app_id
  if (is_app_excluded(app_id)) {
    LOG_C_INFO("Tracker", "Ignoring excluded app_id: '%s'", app_id);
    return;
  }

  // Store resolved app_id string
  g_free(tracker->app_id);
  tracker->app_id = g_strdup(app_id);

  // Exit if this window is already linked to a dock item
  if (tracker->dock_item != NULL)
    return;

  LambdockState *state = tracker->state;
  if (!state)
    return;

  LOG_C_INFO("Tracker", "App ID resolved: '%s'", app_id);

  // Search for existing pinned launcher matching this app_id
  DockItem *item = find_dock_item_by_app_id(state, app_id);

  if (item != NULL) {
    LOG_C_INFO("Tracker", "Unified window with existing dock item for '%s'",
               app_id);
  } else {
    // If no pinned item matches, create a dynamic running dock launcher button
    LOG_C_INFO("Tracker", "Creating dynamic dock item for running app '%s'",
               app_id);
    item = create_dock_item(state, app_id, app_id, NULL, app_id, FALSE);
  }

  // Bind window handle to DockItem's active window list
  tracker->dock_item = item;
  tracker->app_win = dock_item_add_window(item, handle);

  // Apply title string if title event arrived before app_id event
  if (tracker->title && tracker->app_win) {
    g_free(tracker->app_win->title);
    tracker->app_win->title = g_strdup(tracker->title);
  }
}

/**
 * toplevel_handle_closed:
 * @data: Pointer to `WindowTracker`.
 * @handle: Window handle being destroyed.
 *
 * Callback triggered when an open window is closed by the user or compositor.
 * Handles cleanup of tracking pointers, UI indicator dots, and dynamic dock
 * items.
 */
static void
toplevel_handle_closed(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *handle) {
  WindowTracker *tracker = (WindowTracker *)data;
  if (!tracker)
    return;

  DockItem *item = tracker->dock_item;
  if (item) {
    LOG_C_INFO("Tracker", "Window closed for app_id: '%s'",
               item->app_id ? item->app_id : "unknown");

    // Remove window instance handle from dock item list & update active
    // indicator dots
    dock_item_remove_window(item, handle);
    tracker->app_win = NULL;

    // If an unpinned dynamic application closes all its open windows, destroy
    // its dock icon
    if (item->windows == NULL && !item->is_favorite) {
      if (item->state) {
        item->state->dock_items = g_list_remove(item->state->dock_items, item);
      }
      free_dock_item(item); // Destroy GTK widgets and memory
    }
  }

  // Remove tracker instance from global tracker tracking list
  if (tracker->state) {
    tracker->state->window_trackers =
        g_list_remove(tracker->state->window_trackers, tracker);

    // Automatically slide dock back into view when the last window closes
    if (tracker->state->show_when_clear &&
        tracker->state->window_trackers == NULL) {
      reveal_dock(tracker->state);
    }
  }

  // Destroy Wayland protocol handle context
  if (tracker->handle) {
    zwlr_foreign_toplevel_handle_v1_destroy(tracker->handle);
  }

  // Free allocated string buffers and memory struct
  g_free(tracker->title);
  g_free(tracker->app_id);
  g_free(tracker);
}

/**
 * Wayland Toplevel Listener Function Pointer Table:
 * Maps Wayland wire protocol events to our C callback handlers.
 */
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

/* ============================================================================
 * WAYLAND TOPLEVEL MANAGER LISTENER
 * ============================================================================
 */

/**
 * manager_handle_toplevel:
 * @data: Global LambdockState pointer.
 * @manager: Manager protocol object pointer.
 * @toplevel: Newly created Wayland foreign window handle from compositor.
 *
 * Triggered whenever a new window opens anywhere on the desktop session.
 */
static void
manager_handle_toplevel(void *data,
                        struct zwlr_foreign_toplevel_manager_v1 *manager,
                        struct zwlr_foreign_toplevel_handle_v1 *toplevel) {
  (void)manager; // Mark unused parameter
  LambdockState *state = (LambdockState *)data;
  if (!state)
    return;

  // Allocate a new WindowTracker context instance
  WindowTracker *tracker = g_new0(WindowTracker, 1);
  tracker->handle = toplevel;
  tracker->state = state;

  // Add tracker to global window trackers list
  state->window_trackers = g_list_append(state->window_trackers, tracker);

  // Attach event listener callback table to newly opened window surface
  zwlr_foreign_toplevel_handle_v1_add_listener(toplevel, &toplevel_listener,
                                               tracker);
  // If auto-hide & show-when-clear are enabled, trigger dock hide when first
  // window opens
  if (state->auto_hide && state->show_when_clear &&
      state->window_trackers != NULL) {
    hide_dock(state);
  }
}

/**
 * manager_handle_finished:
 * Triggered if the compositor shuts down foreign window management service.
 */
static void
manager_handle_finished(void *data,
                        struct zwlr_foreign_toplevel_manager_v1 *manager) {
  (void)data;
  (void)manager;
}

/**
 * Foreign Toplevel Manager Listener Table
 */
static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener =
    {
        .toplevel = manager_handle_toplevel,
        .finished = manager_handle_finished,
};

/* ============================================================================
 * WAYLAND GLOBAL REGISTRY BINDING & TRACKER INITIALIZATION
 * ============================================================================
 */

/**
 * registry_handle_global:
 * @data: Global LambdockState pointer.
 * @registry: Wayland registry object.
 * @name: Numeric ID assigned to global object by compositor.
 * @interface: Name string of interface (e.g.,
 * "zwlr_foreign_toplevel_manager_v1").
 * @version: Protocol version.
 *
 * Listener callback fired when enumerating system global interfaces exported by
 * the compositor.
 */
static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version) {
  (void)version; // Suppress warning
  LambdockState *state = (LambdockState *)data;

  // Look for foreign window management interface protocol
  if (g_strcmp0(interface, zwlr_foreign_toplevel_manager_v1_interface.name) ==
      0) {
    toplevel_manager = wl_registry_bind(
        registry, name, &zwlr_foreign_toplevel_manager_v1_interface, 1);

    if (toplevel_manager) {
      zwlr_foreign_toplevel_manager_v1_add_listener(toplevel_manager,
                                                    &manager_listener, state);
      LOG_C_INFO("Tracker", "Bound foreign toplevel manager successfully.");
    }
  }
  // Look for default Wayland input seat interface (for window focus control)
  else if (g_strcmp0(interface, wl_seat_interface.name) == 0) {
    if (!default_seat) {
      default_seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
      LOG_C_INFO("Tracker", "Bound default Wayland seat successfully.");
    }
  }
}

/**
 * init_toplevel_tracker:
 * @state: Pointer to global Lambdock application state struct.
 *
 * Hooks into GDK's active Wayland display connection and registers registry
 * listeners to begin receiving foreign window events.
 */
void init_toplevel_tracker(LambdockState *state) {
  if (!state)
    return;

  // Get active GDK display instance
  GdkDisplay *display = gdk_display_get_default();

  // Verify that active graphics display backend is Wayland (and not X11)
  if (display && GDK_IS_WAYLAND_DISPLAY(display)) {
    // Extract underlying C `wl_display` handle from GDK wrapper
    struct wl_display *wl_display = gdk_wayland_display_get_wl_display(display);
    struct wl_registry *registry = wl_display_get_registry(wl_display);

    static const struct wl_registry_listener registry_listener = {
        .global = registry_handle_global,
        .global_remove = NULL,
    };

    // Attach registry listener to discover global protocol capabilities
    wl_registry_add_listener(registry, &registry_listener, state);
    LOG_C_INFO("Tracker", "Initialized toplevel tracker listener.");
  }
}

/* ============================================================================
 * TRACKER SYNC & CLEANUP HELPERS
 * ============================================================================
 */

/**
 * sync_window_trackers:
 * @state: Global Lambdock runtime state container.
 *
 * Re-evaluates all tracked windows against current dock state. Called after
 * hot-reloading or re-populating dock layout items to ensure window handles
 * remain mapped to dock buttons.
 */
void sync_window_trackers(LambdockState *state) {
  if (!state)
    return;

  for (GList *l = state->window_trackers; l != NULL; l = l->next) {
    WindowTracker *tracker = (WindowTracker *)l->data;
    if (!tracker || !tracker->app_id)
      continue;

    // Skip excluded apps
    if (is_app_excluded(tracker->app_id))
      continue;

    // Search if a dock item already exists matching tracker->app_id
    DockItem *item = find_dock_item_by_app_id(state, tracker->app_id);
    if (!item) {
      // Create new dynamic dock launcher if item is missing
      item = create_dock_item(state, tracker->app_id, tracker->app_id, NULL,
                              tracker->app_id, FALSE);
    }

    tracker->dock_item = item;

    // Check if app_win is already in item->windows list
    gboolean already_added = FALSE;
    for (GList *w = item->windows; w != NULL; w = w->next) {
      AppWindow *win = (AppWindow *)w->data;
      if (win->handle == tracker->handle) {
        tracker->app_win = win;
        already_added = TRUE;
        break;
      }
    }

    // Attach window handle to item if not already linked
    if (!already_added) {
      tracker->app_win = dock_item_add_window(item, tracker->handle);
      if (tracker->title && tracker->app_win) {
        g_free(tracker->app_win->title);
        tracker->app_win->title = g_strdup(tracker->title);
      }
    }
  }
}

/**
 * clear_window_tracker_dock_references:
 * @state: Pointer to global Lambdock state.
 *
 * Safely unlinks dock item references from window trackers without destroying
 * the Wayland window handles. Used before clearing UI dock layout items during
 * hot-reload.
 */
void clear_window_tracker_dock_references(LambdockState *state) {
  if (!state)
    return;
  for (GList *l = state->window_trackers; l != NULL; l = l->next) {
    WindowTracker *tracker = (WindowTracker *)l->data;
    if (tracker) {
      tracker->dock_item = NULL;
      tracker->app_win = NULL;
    }
  }
}
