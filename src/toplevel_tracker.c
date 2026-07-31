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

#include "toplevel_tracker.h"
#include "ui_helpers.h"

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

// Stub callbacks to prevent Wayland NULL-pointer crash (SIGABRT)
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

  // Prevent duplicate button creation if app_id is sent multiple times
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
      gtk_box_remove(GTK_BOX(box), item->button);
    }
  }

  if (item->handle) {
    zwlr_foreign_toplevel_handle_v1_destroy(item->handle);
  }

  g_free(item->app_id);
  g_free(item);
}

// Ensure all handlers are non-NULL
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
