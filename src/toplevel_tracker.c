// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock.If not, see <https://www.gnu.org/licenses/>.

#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>

#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"

#include "dock_window.h"
#include "toplevel_tracker.h"
#include "ui_helpers.h"

typedef struct {
  struct zwlr_foreign_toplevel_handle_v1 *handle;
  LambdockState *state;
  GtkWidget *button;
  GtkWidget *popover; // <-- Track popover reference on WindowItem
  char *app_id;
} WindowItem;

static struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager = NULL;
static struct wl_seat *default_seat = NULL;

// -----------------------------------------------------------------
// Dynamic Separator State
// -----------------------------------------------------------------
static GtkWidget *running_apps_separator = NULL;
static int running_app_count = 0;

// -----------------------------------------------------------------
// Button Click Handlers
// -----------------------------------------------------------------

// Left Click: Focus Window
static void on_running_app_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  WindowItem *item = (WindowItem *)user_data;

  if (item && item->handle && default_seat) {
    g_print("[Lambdock Tracker] Focusing window for app_id: '%s'\n",
            item->app_id ? item->app_id : "unknown");

    zwlr_foreign_toplevel_handle_v1_activate(item->handle, default_seat);
  } else {
    g_warning(
        "[Lambdock Tracker] Cannot focus window: missing handle or seat.\n");
  }
}

// Right Click Action: Close Window
static void on_close_app_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  WindowItem *item = (WindowItem *)user_data;

  if (item && item->handle) {
    g_print("[Lambdock Tracker] Requesting window close for app_id: '%s'\n",
            item->app_id ? item->app_id : "unknown");

    zwlr_foreign_toplevel_handle_v1_close(item->handle);
  } else {
    g_warning("[Lambdock Tracker] Cannot close window: missing handle.\n");
  }
}

// Right Click Gesture Callback on Running App Icon
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

  if (!gtk_widget_get_visible(item->popover)) {
    item->state->active_popovers++;
  }

  gtk_popover_popup(GTK_POPOVER(item->popover));
}

// -----------------------------------------------------------------
// Window Event Callbacks
// -----------------------------------------------------------------

static void
toplevel_handle_title(void *data,
                      struct zwlr_foreign_toplevel_handle_v1 *handle,
                      const char *title) {
  (void)data;
  (void)handle;
  (void)title;
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
  (void)handle;
  WindowItem *item = (WindowItem *)data;

  if (!item || !app_id || *app_id == '\0')
    return;

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

  item->button = create_app_button(app_id, app_id, "", 32);
  if (item->button) {
    gtk_widget_add_css_class(item->button, "running-app");

    // Left-click signal -> focus window
    g_signal_connect(item->button, "clicked",
                     G_CALLBACK(on_running_app_clicked), item);

    // -------------------------------------------------------------
    // Right-Click Menu Popover Setup
    // -------------------------------------------------------------
    item->popover = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(item->popover), FALSE);
    gtk_widget_set_parent(item->popover, item->button);

    // Connect "closed" signal so auto-hide resumes when menu is closed
    g_signal_connect(item->popover, "closed", G_CALLBACK(on_popover_closed),
                     item->state);

    GtkWidget *close_button = gtk_button_new_with_label("Close Window");
    gtk_widget_add_css_class(close_button, "flat");
    gtk_popover_set_child(GTK_POPOVER(item->popover), close_button);

    // Close button action
    g_signal_connect(close_button, "clicked", G_CALLBACK(on_close_app_clicked),
                     item);
    g_signal_connect_swapped(close_button, "clicked",
                             G_CALLBACK(gtk_popover_popdown), item->popover);

    // Attach right-click gesture controller
    GtkGesture *right_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click),
                                  GDK_BUTTON_SECONDARY);
    g_signal_connect(right_click, "pressed",
                     G_CALLBACK(on_running_app_right_click), item);
    gtk_widget_add_controller(item->button, GTK_EVENT_CONTROLLER(right_click));

    // -------------------------------------------------------------
    // Add separator BEFORE appending the first running application
    // -------------------------------------------------------------
    if (running_apps_separator == NULL) {
      running_apps_separator = create_separator();
      gtk_box_append(GTK_BOX(box), running_apps_separator);
      g_print("[Lambdock Tracker] Inserted running apps separator.\n");
    }

    running_app_count++;

    gtk_box_append(GTK_BOX(box), item->button);
    g_print("[Lambdock Tracker] Added running app button to dock for: '%s'\n",
            app_id);
  }
}

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

      if (item->popover != NULL) {
        gtk_widget_unparent(item->popover);
        item->popover = NULL;
      }

      gtk_box_remove(GTK_BOX(box), item->button);
      item->button = NULL;

      running_app_count--;

      // Remove separator when NO running applications remain
      if (running_app_count <= 0 && running_apps_separator != NULL) {
        gtk_box_remove(GTK_BOX(box), running_apps_separator);
        running_apps_separator = NULL;
        running_app_count = 0;
        g_print("[Lambdock Tracker] Removed running apps separator.\n");
      }
    }
  }

  if (item->handle) {
    zwlr_foreign_toplevel_handle_v1_destroy(item->handle);
  }

  g_free(item->app_id);
  g_free(item);
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

// -----------------------------------------------------------------
// Protocol Manager Listeners
// -----------------------------------------------------------------
static void
manager_handle_toplevel(void *data,
                        struct zwlr_foreign_toplevel_manager_v1 *manager,
                        struct zwlr_foreign_toplevel_handle_v1 *toplevel) {
  (void)manager;
  LambdockState *state = (LambdockState *)data;
  if (!state)
    return;

  WindowItem *item = g_new0(WindowItem, 1);
  item->handle = toplevel;
  item->state = state;

  zwlr_foreign_toplevel_handle_v1_add_listener(toplevel, &toplevel_listener,
                                               item);
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
  } else if (g_strcmp0(interface, wl_seat_interface.name) == 0) {
    if (!default_seat) {
      default_seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
      g_print("[Lambdock Tracker] Bound default Wayland seat successfully.\n");
    }
  }
}

// -----------------------------------------------------------------
// Initialize Tracker
// -----------------------------------------------------------------
void init_toplevel_tracker(LambdockState *state) {
  if (!state)
    return;

  GdkDisplay *display = gdk_display_get_default();
  if (display && GDK_IS_WAYLAND_DISPLAY(display)) {
    struct wl_display *wl_display = gdk_wayland_display_get_wl_display(display);
    struct wl_registry *registry = wl_display_get_registry(wl_display);

    static const struct wl_registry_listener registry_listener = {
        .global = registry_handle_global,
        .global_remove = NULL,
    };

    wl_registry_add_listener(registry, &registry_listener, state);
    g_print("[Lambdock Tracker] Initialized toplevel tracker listener.\n");
  }
}
