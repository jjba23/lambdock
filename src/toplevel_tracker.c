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

#include "toplevel_tracker.h"
#include "ui_helpers.h"
#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"
#include <gdk/wayland/gdkwayland.h>

typedef struct {
  struct zwlr_foreign_toplevel_handle_v1 *handle;
  LambdockState *state;
  GtkWidget *button;
  char *app_id;
} WindowItem;

static struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager = NULL;

// -----------------------------------------------------------------
// Window Event Callbacks
// -----------------------------------------------------------------
static void
toplevel_handle_app_id(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *handle,
                       const char *app_id) {
  (void)handle;
  WindowItem *item = (WindowItem *)data;
  if (!app_id || *app_id == '\0')
    return;

  item->app_id = g_strdup(app_id);
  g_print("[Lambdock] Window created: %s\n", app_id);

  // Get the main GTK horizontal box container
  GtkWidget *box = gtk_window_get_child(GTK_WINDOW(item->state->window));

  // Create dock item using your existing helper
  item->button = create_app_button(app_id, app_id, "", 32);
  gtk_widget_add_css_class(item->button, "running-app");

  // Add button to dock container
  gtk_box_append(GTK_BOX(box), item->button);
}

static void
toplevel_handle_closed(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *handle) {
  (void)handle;
  WindowItem *item = (WindowItem *)data;
  g_print("[Lambdock] Window closed: %s\n",
          item->app_id ? item->app_id : "unknown");

  if (item->button) {
    GtkWidget *box = gtk_window_get_child(GTK_WINDOW(item->state->window));
    gtk_box_remove(GTK_BOX(box), item->button);
  }

  zwlr_foreign_toplevel_handle_v1_destroy(item->handle);
  g_free(item->app_id);
  g_free(item);
}

static const struct zwlr_foreign_toplevel_handle_v1_listener toplevel_listener =
    {
        .title = NULL,
        .app_id = toplevel_handle_app_id,
        .output_enter = NULL,
        .output_leave = NULL,
        .state = NULL,
        .done = NULL,
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

  WindowItem *item = g_new0(WindowItem, 1);
  item->handle = toplevel;
  item->state = state;

  zwlr_foreign_toplevel_handle_v1_add_listener(toplevel, &toplevel_listener,
                                               item);
}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener =
    {
        .toplevel = manager_handle_toplevel,
        .finished = NULL,
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
    zwlr_foreign_toplevel_manager_v1_add_listener(toplevel_manager,
                                                  &manager_listener, state);
  }
}

// -----------------------------------------------------------------
// Initialize Tracker
// -----------------------------------------------------------------
void init_toplevel_tracker(LambdockState *state) {
  GdkDisplay *display = gdk_display_get_default();

  if (GDK_IS_WAYLAND_DISPLAY(display)) {
    struct wl_display *wl_display = gdk_wayland_display_get_wl_display(display);
    struct wl_registry *registry = wl_display_get_registry(wl_display);

    static const struct wl_registry_listener registry_listener = {
        .global = registry_handle_global,
        .global_remove = NULL,
    };

    wl_registry_add_listener(registry, &registry_listener, state);
  } else {
    g_warning("[Lambdock] Not running on Wayland display. Foreign toplevel "
              "tracking disabled.");
  }
}
