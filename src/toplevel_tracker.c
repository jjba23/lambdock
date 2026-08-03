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
#include "toplevel_tracker.h"
#include "ui_helpers.h"

/* =========================================================================
 * DATA STRUCTURES & MODULE-LEVEL STATE
 * ========================================================================= */

/**
 * WindowItem:
 * Struct binding a Wayland foreign toplevel window handle to its GTK 4 UI
 * representation.
 */
typedef struct {
  struct zwlr_foreign_toplevel_handle_v1
      *handle;          /* Wayland protocol handle for window */
  LambdockState *state; /* Pointer to global app state */
  GtkWidget *button;    /* GTK button widget on the dock */
  GtkWidget *popover;   /* Right-click context menu popup */
  char *app_id;         /* Application ID string (e.g., "firefox") */
} WindowItem;

/* Global Wayland protocol interfaces bound via registry */
static struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager = NULL;
static struct wl_seat *default_seat = NULL;

/* Dynamic UI state for separating fixed launcher items from active running apps
 */
static GtkWidget *running_apps_separator = NULL;
static int running_app_count = 0;

/* =========================================================================
 * BUTTON CLICK & GESTURE HANDLERS
 * ========================================================================= */

/**
 * on_running_app_clicked:
 * Callback triggered on primary (left) click on a running application dock
 * button. Sends an activation request to the Wayland compositor to bring the
 * window to focus.
 */
static void on_running_app_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  WindowItem *item = (WindowItem *)user_data;

  // Wayland window activation requires both a valid window handle and an active
  // input seat
  if (item && item->handle && default_seat) {
    g_print("[Lambdock Tracker] Focusing window for app_id: '%s'\n",
            item->app_id ? item->app_id : "unknown");

    // Request Wayland compositor to activate/focus window
    zwlr_foreign_toplevel_handle_v1_activate(item->handle, default_seat);
  } else {
    g_warning(
        "[Lambdock Tracker] Cannot focus window: missing handle or seat.\n");
  }
}

/**
 * on_close_app_clicked:
 * Callback triggered by the "Close Window" option inside the right-click
 * popover menu. Requests the Wayland compositor to close the targeted
 * application window.
 */
static void on_close_app_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  WindowItem *item = (WindowItem *)user_data;

  if (item && item->handle) {
    g_print("[Lambdock Tracker] Requesting window close for app_id: '%s'\n",
            item->app_id ? item->app_id : "unknown");

    // Request Wayland compositor to gracefully close target window
    zwlr_foreign_toplevel_handle_v1_close(item->handle);
  } else {
    g_warning("[Lambdock Tracker] Cannot close window: missing handle.\n");
  }
}

/**
 * on_running_app_right_click:
 * Gesture callback triggered on secondary (right) click on a running app
 * button. Increments active popover counters and presents the popover context
 * menu.
 */
static void on_running_app_right_click(GtkGestureClick *gesture, gint n_press,
                                       gdouble x, gdouble y,
                                       gpointer user_data) {
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;
  WindowItem *item = (WindowItem *)user_data;

  if (!item || !item->popover || !item->state)
    return;

  // Increment tracking count if opening a new visible popover menu
  if (!gtk_widget_get_visible(item->popover)) {
    item->state->active_popovers++;
  }

  gtk_popover_popup(GTK_POPOVER(item->popover));
}

/* =========================================================================
 * WAYLAND TOPLEVEL WINDOW EVENT CALLBACKS
 * ========================================================================= */

static void
toplevel_handle_title(void *data,
                      struct zwlr_foreign_toplevel_handle_v1 *handle,
                      const char *title) {
  (void)data;
  (void)handle;
  (void)title;
  /* Unused: Can be extended to update tooltip titles on hover */
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

/**
 * toplevel_handle_app_id:
 * Event callback triggered when the Wayland compositor resolves or changes an
 * app_id for an open window (e.g., "org.gnome.Terminal").
 *
 * This function builds the corresponding GTK button, popover context menu, and
 * adds the running app icon to the dock UI.
 */
static void
toplevel_handle_app_id(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *handle,
                       const char *app_id) {
  (void)handle;
  WindowItem *item = (WindowItem *)data;

  if (!item || !app_id || *app_id == '\0')
    return;

  // If button was already constructed, update string reference and exit
  if (item->button != NULL) {
    g_free(item->app_id);
    item->app_id = g_strdup(app_id);
    return;
  }

  item->app_id = g_strdup(app_id);
  g_print("[Lambdock Tracker] App ID resolved: '%s'\n", app_id);

  if (!item->state || !item->state->window)
    return;

  GtkWidget *box = gtk_window_get_child(GTK_WINDOW(item->state->window));
  if (!box || !GTK_IS_BOX(box))
    return;

  // Create launcher button widget using app_id for icon lookup
  item->button = create_app_button(app_id, app_id, "", 32);
  if (item->button) {
    gtk_widget_add_css_class(item->button, "running-app");

    // Connect left-click button signal to focus window handler
    g_signal_connect(item->button, "clicked",
                     G_CALLBACK(on_running_app_clicked), item);

    // --- Context Menu Construction ---
    item->popover = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(item->popover), FALSE);
    gtk_widget_set_parent(item->popover, item->button);

    // Resume auto-hide timers when context menu closes
    g_signal_connect(item->popover, "closed", G_CALLBACK(on_popover_closed),
                     item->state);

    // Populate popover with a "Close Window" button
    GtkWidget *close_button = gtk_button_new_with_label("Close Window");
    gtk_widget_add_css_class(close_button, "flat");
    gtk_popover_set_child(GTK_POPOVER(item->popover), close_button);

    // Action handlers for popover button
    g_signal_connect(close_button, "clicked", G_CALLBACK(on_close_app_clicked),
                     item);
    g_signal_connect_swapped(close_button, "clicked",
                             G_CALLBACK(gtk_popover_popdown), item->popover);

    // Attach right-click input gesture controller
    GtkGesture *right_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click),
                                  GDK_BUTTON_SECONDARY);
    g_signal_connect(right_click, "pressed",
                     G_CALLBACK(on_running_app_right_click), item);
    gtk_widget_add_controller(item->button, GTK_EVENT_CONTROLLER(right_click));

    // --- Dynamic UI Separator Handling ---
    // If this is the first running app detected, insert visual separator first
    if (running_apps_separator == NULL) {
      GtkOrientation sep_orient =
          (item->state->orientation == GTK_ORIENTATION_HORIZONTAL)
              ? GTK_ORIENTATION_VERTICAL
              : GTK_ORIENTATION_HORIZONTAL;

      running_apps_separator = create_separator(sep_orient);
      gtk_box_append(GTK_BOX(box), running_apps_separator);
      g_print("[Lambdock Tracker] Inserted running apps separator.\n");
    }

    running_app_count++;

    // Append newly constructed app button to main dock box layout
    gtk_box_append(GTK_BOX(box), item->button);
    g_print("[Lambdock Tracker] Added running app button to dock for: '%s'\n",
            app_id);
  }
}

/**
 * toplevel_handle_closed:
 * Event callback triggered when a monitored window closes in Wayland.
 * Removes button from dock UI, frees associated GTK widgets, and cleans heap
 * memory.
 */
static void
toplevel_handle_closed(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *handle) {
  (void)handle;
  WindowItem *item = (WindowItem *)data;
  if (!item)
    return;

  g_print("[Lambdock Tracker] Window closed: '%s'\n",
          item->app_id ? item->app_id : "unknown");

  if (item->button && item->state && item->state->window) {
    GtkWidget *box = gtk_window_get_child(GTK_WINDOW(item->state->window));
    if (box && GTK_IS_BOX(box)) {

      // GTK 4 Requirement: Detach parent/child relationship before removing
      // widget
      if (item->popover != NULL) {
        gtk_widget_unparent(item->popover);
        item->popover = NULL;
      }

      // Remove button widget from GTK Box layout container
      gtk_box_remove(GTK_BOX(box), item->button);
      item->button = NULL;

      running_app_count--;

      // If no running apps remain, remove visual separator bar from dock
      if (running_app_count <= 0 && running_apps_separator != NULL) {
        gtk_box_remove(GTK_BOX(box), running_apps_separator);
        running_apps_separator = NULL;
        running_app_count = 0;
        g_print("[Lambdock Tracker] Removed running apps separator.\n");
      }
    }
  }

  // Destroy Wayland foreign toplevel handle context
  if (item->handle) {
    zwlr_foreign_toplevel_handle_v1_destroy(item->handle);
  }

  // Memory cleanup for dynamically allocated C strings and struct
  g_free(item->app_id);
  g_free(item);
}

/* Wayland Listener Struct: Maps Wayland event signals to C callback functions
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

/* =========================================================================
 * WAYLAND PROTOCOL MANAGER & REGISTRY LISTENERS
 * ========================================================================= */

/**
 * manager_handle_toplevel:
 * Triggered by Wayland compositor whenever a new top-level window is created.
 */
static void
manager_handle_toplevel(void *data,
                        struct zwlr_foreign_toplevel_manager_v1 *manager,
                        struct zwlr_foreign_toplevel_handle_v1 *toplevel) {
  (void)manager;
  LambdockState *state = (LambdockState *)data;
  if (!state)
    return;

  // Allocate tracking item struct for newly opened window
  WindowItem *item = g_new0(WindowItem, 1);
  item->handle = toplevel;
  item->state = state;

  // Subscribe to window event listeners for this specific window instance
  zwlr_foreign_toplevel_handle_v1_add_listener(toplevel, &toplevel_listener,
                                               item);
}

static void
manager_handle_finished(void *data,
                        struct zwlr_foreign_toplevel_manager_v1 *manager) {
  (void)data;
  (void)manager;
}

/* Listener structure for foreign toplevel manager global events */
static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener =
    {
        .toplevel = manager_handle_toplevel,
        .finished = manager_handle_finished,
};

/**
 * registry_handle_global:
 * Callback triggered during initial Wayland handshake. Listens for exposed
 * global interfaces provided by the Wayland compositor (such as foreign
 * toplevel manager and default seat).
 */
static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version) {
  (void)version;
  LambdockState *state = (LambdockState *)data;

  // Check if interface matches Foreign Toplevel Management protocol
  if (g_strcmp0(interface, zwlr_foreign_toplevel_manager_v1_interface.name) ==
      0) {
    toplevel_manager = wl_registry_bind(
        registry, name, &zwlr_foreign_toplevel_manager_v1_interface, 1);

    if (toplevel_manager) {
      zwlr_foreign_toplevel_manager_v1_add_listener(toplevel_manager,
                                                    &manager_listener, state);
      g_print(
          "[Lambdock Tracker] Bound foreign toplevel manager successfully.\n");
    }

    // Check if interface matches default input Seat interface
  } else if (g_strcmp0(interface, wl_seat_interface.name) == 0) {
    if (!default_seat) {
      default_seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
      g_print("[Lambdock Tracker] Bound default Wayland seat successfully.\n");
    }
  }
}

/* =========================================================================
 * TRACKER INITIALIZATION ENTRY POINT
 * ========================================================================= */

/**
 * init_toplevel_tracker:
 * @state: Pointer to LambdockState context struct.
 *
 * Fetches underlying Wayland display connection from GTK's GdkDisplay, queries
 * the Wayland registry, and attaches event listeners to track system
 * application windows.
 */
void init_toplevel_tracker(LambdockState *state) {
  if (!state)
    return;

  GdkDisplay *display = gdk_display_get_default();

  // Verify running graphics backend is native Wayland
  if (display && GDK_IS_WAYLAND_DISPLAY(display)) {
    // Extract native low-level 'wl_display' handle from GDK display wrapper
    struct wl_display *wl_display = gdk_wayland_display_get_wl_display(display);

    // Obtain global object registry from Wayland compositor display
    struct wl_registry *registry = wl_display_get_registry(wl_display);

    static const struct wl_registry_listener registry_listener = {
        .global = registry_handle_global,
        .global_remove = NULL,
    };

    // Attach registry listener to bind to global interfaces
    wl_registry_add_listener(registry, &registry_listener, state);
    g_print("[Lambdock Tracker] Initialized toplevel tracker listener.\n");
  }
}
