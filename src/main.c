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

#include "dock_window.h"
#include "guile_bridge.h"
#include "lambdock.h"
#include "logger.h"
#include "toplevel_tracker.h"
#include "ui_helpers.h"

/* Global list of active LambdockState instances and file monitor handle */
static GList *g_active_states = NULL;
static guint g_reload_timer_id = 0;

/**
 * on_config_reload_timeout:
 * Debounce callback executed 150ms after file writes complete.
 */
static gboolean on_config_reload_timeout(gpointer user_data) {
  (void)user_data;

  // 1. Re-evaluate Scheme environment
  reload_guile_config();

  // 2. Refresh UI across all running dock instances
  for (GList *l = g_active_states; l != NULL; l = l->next) {
    LambdockState *state = (LambdockState *)l->data;
    reload_dock_instance(state);
  }

  g_reload_timer_id = 0;
  return G_SOURCE_REMOVE;
}

/**
 * on_file_changed_cb:
 * Callback triggered by GFileMonitor when settings.scm or styles.css are
 * edited.
 */
static void on_file_changed_cb(GFileMonitor *monitor, GFile *file,
                               GFile *other_file, GFileMonitorEvent event_type,
                               gpointer user_data) {
  (void)monitor;
  (void)file;
  (void)other_file;
  (void)user_data;

  if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
      event_type == G_FILE_MONITOR_EVENT_CHANGED ||
      event_type == G_FILE_MONITOR_EVENT_CREATED) {

    // Debounce rapid editor write events (150ms grace period)
    if (g_reload_timer_id > 0) {
      g_source_remove(g_reload_timer_id);
    }
    g_reload_timer_id = g_timeout_add(150, on_config_reload_timeout, NULL);
  }
}

/**
 * setup_config_file_monitors:
 * Binds GFileMonitor listeners to settings.scm and styles.css.
 */
static void setup_config_file_monitors(void) {
  char *config_path = get_config_path_c();
  if (config_path) {
    GFile *scheme_file = g_file_new_for_path(config_path);
    GFileMonitor *scheme_mon =
        g_file_monitor_file(scheme_file, G_FILE_MONITOR_NONE, NULL, NULL);

    if (scheme_mon) {
      g_signal_connect(scheme_mon, "changed", G_CALLBACK(on_file_changed_cb),
                       NULL);
      LOG_C_INFO("Main", "Watching Scheme config for live hot-reload: %s",
                 config_path);
    }
    g_object_unref(scheme_file);
    g_free(config_path);
  }

  const char *config_dir = g_get_user_config_dir();
  char *user_css_path =
      g_build_filename(config_dir, "lambdock", "styles.css", NULL);
  GFile *css_file = g_file_new_for_path(user_css_path);
  GFileMonitor *css_mon =
      g_file_monitor_file(css_file, G_FILE_MONITOR_NONE, NULL, NULL);

  if (css_mon) {
    g_signal_connect(css_mon, "changed", G_CALLBACK(on_file_changed_cb), NULL);
    LOG_C_INFO("Main", "Watching user CSS for live hot-reload: %s",
               user_css_path);
  }
  g_object_unref(css_file);
  g_free(user_css_path);
}

/* =========================================================================
 * GTK APPLICATION ACTIVATION (UI CONSTRUCTION & SETUP)
 * ========================================================================= */

/**
 * create_dock_instance:
 * Helper to construct a single dock window on a specific monitor context.
 */
static void create_dock_instance(GtkApplication *app, GdkMonitor *monitor) {
  LambdockState *state = g_new0(LambdockState, 1);
  state->monitor = monitor;
  state->theme = get_theme_config();

  load_custom_css(state);

  state->edge = get_position_config();
  state->orientation = (state->edge == GTK_LAYER_SHELL_EDGE_LEFT ||
                        state->edge == GTK_LAYER_SHELL_EDGE_RIGHT)
                           ? GTK_ORIENTATION_VERTICAL
                           : GTK_ORIENTATION_HORIZONTAL;

  GtkWidget *window = gtk_application_window_new(app);
  state->window = window;

  GtkWidget *box = gtk_box_new(state->orientation, 8);
  gtk_widget_add_css_class(box, "lambdock-window");
  gtk_window_set_child(GTK_WINDOW(window), box);
  state->box = box;

  populate_dock_items(state);

  GMenu *menu_model = g_menu_new();
  g_menu_append(menu_model, "About Lambdock", "app.about");
  g_menu_append(menu_model, "Quit", "app.quit");

  GtkWidget *popover =
      gtk_popover_menu_new_from_model(G_MENU_MODEL(menu_model));
  g_object_unref(menu_model);

  gtk_widget_set_parent(popover, window);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  state->popover = popover;
  g_signal_connect(popover, "closed", G_CALLBACK(on_popover_closed), state);

  GtkGesture *right_click_gesture = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click_gesture),
                                GDK_BUTTON_SECONDARY);
  g_signal_connect(right_click_gesture, "pressed", G_CALLBACK(on_right_click),
                   state);
  gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(right_click_gesture));

  state->auto_hide = get_autohide_config();
  setup_layer_shell_window(state);
  setup_dock_motion_controller(state);

  // Track active state globally for hot-reloading
  g_active_states = g_list_append(g_active_states, state);

  g_signal_connect_swapped(window, "destroy", G_CALLBACK(free_lambdock_state),
                           state);

  gtk_window_present(GTK_WINDOW(window));
  init_toplevel_tracker(state);
  schedule_initial_autohide(state);
}

static void activate(GtkApplication *app, gpointer user_data) {
  (void)user_data;

  LOG_C_INFO("Main", "Initializing Guile engine & multi-monitor manager...");
  setup_guile_load_path();
  scm_c_use_module("lambdock core");

  SCM init_fn = scm_c_public_ref("lambdock core", "init-dock!");
  scm_call_0(init_fn);

  setup_icon_theme();

  GSimpleAction *about_action = g_simple_action_new("about", NULL);
  g_signal_connect(about_action, "activate", G_CALLBACK(on_about_action), app);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(about_action));

  GSimpleAction *quit_action = g_simple_action_new("quit", NULL);
  g_signal_connect(quit_action, "activate", G_CALLBACK(on_quit_action), app);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(quit_action));

  // Multi-Monitor resolution
  char *monitor_target = get_monitor_config();
  GdkDisplay *display = gdk_display_get_default();
  GListModel *monitors = gdk_display_get_monitors(display);
  guint n_monitors = g_list_model_get_n_items(monitors);

  if (g_strcmp0(monitor_target, "all") == 0) {
    for (guint i = 0; i < n_monitors; i++) {
      GdkMonitor *mon = GDK_MONITOR(g_list_model_get_item(monitors, i));
      create_dock_instance(app, mon);
      g_object_unref(mon);
    }
  } else {
    create_dock_instance(app, NULL);
  }

  g_free(monitor_target);

  // Initialize file monitoring for hot-reloading
  setup_config_file_monitors();
}

/* =========================================================================
 * PROGRAM ENTRY POINT
 * ========================================================================= */

/**
 * inner_main:
 * Callback invoked once Guile runtime environment initialization completes.
 */
static void inner_main(void *data, int argc, char **argv) {
  (void)data;

  // Construct GtkApplication instance with reverse-DNS application ID
  GtkApplication *app = gtk_application_new("org.jointhefreeworld.lambdock",
                                            G_APPLICATION_DEFAULT_FLAGS);

  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  g_application_run(G_APPLICATION(app), argc, argv); // Blocks until app exits
  g_object_unref(app);
}

int main(int argc, char **argv) {
#ifdef DEFAULT_GSK_RENDERER
  g_setenv("GSK_RENDERER", DEFAULT_GSK_RENDERER, FALSE);
#endif

  // Hand over execution control to Guile runtime, which calls 'inner_main'
  scm_boot_guile(argc, argv, inner_main, NULL);
  return 0;
}
