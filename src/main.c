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

/* =========================================================================
 * GTK APPLICATION ACTIVATION (UI CONSTRUCTION & SETUP)
 * ========================================================================= */

/**
 * activate:
 * @app: Pointer to active GtkApplication instance.
 * @user_data: Generic context pointer (unused).
 *
 * GTK activation signal handler. Initializes Guile, constructs GTK 4 widget
 * hierarchy, attaches input event controllers, configures Layer Shell, and
 * launches Wayland tracking.
 */
static void activate(GtkApplication *app, gpointer user_data) {
  (void)user_data;

  LOG_C_INFO("Main", "Initializing Guile engine & themes...");
  // --- Guile Engine & Theme Initialization ---
  setup_guile_load_path();
  scm_c_use_module("lambdock core");

  SCM init_fn = scm_c_public_ref("lambdock core", "init-dock!");
  scm_call_0(init_fn);

  setup_icon_theme();

  // g_new0 allocates zero-initialized heap memory for custom structures
  LambdockState *state = g_new0(LambdockState, 1);

  state->theme = get_theme_config();

  load_custom_css(state);

  // Parse Scheme edge setting ("top", "bottom", "left", "right")
  state->edge = get_position_config();

  // Infer orientation: side attachment requires VERTICAL box layout
  state->orientation = (state->edge == GTK_LAYER_SHELL_EDGE_LEFT ||
                        state->edge == GTK_LAYER_SHELL_EDGE_RIGHT)
                           ? GTK_ORIENTATION_VERTICAL
                           : GTK_ORIENTATION_HORIZONTAL;

  // --- Construct Main GTK Window & Outer Container ---
  GtkWidget *window = gtk_application_window_new(app);
  state->window = window;

  GtkWidget *box = gtk_box_new(state->orientation, 8);
  gtk_widget_add_css_class(box, "lambdock-window");
  gtk_window_set_child(GTK_WINDOW(window), box);
  state->box = box;

  // Read favorite launchers from Guile and populate layout box
  populate_dock_items(state);

  // --- Register GTK Application Actions ---
  GSimpleAction *quit_action = g_simple_action_new("quit", NULL);
  g_signal_connect(quit_action, "activate", G_CALLBACK(on_quit_action), app);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(quit_action));

  // --- Background Context Popover Menu ---
  GMenu *menu_model = g_menu_new();
  g_menu_append(menu_model, "Quit", "app.quit");

  GtkWidget *popover =
      gtk_popover_menu_new_from_model(G_MENU_MODEL(menu_model));
  g_object_unref(menu_model); // Decrement reference count to avoid memory leak

  gtk_widget_set_parent(popover, window);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);

  state->popover = popover;
  g_signal_connect(popover, "closed", G_CALLBACK(on_popover_closed), state);

  // Right-click input controller for background popover menu
  GtkGesture *right_click_gesture = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click_gesture),
                                GDK_BUTTON_SECONDARY);
  g_signal_connect(right_click_gesture, "pressed", G_CALLBACK(on_right_click),
                   state);
  gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(right_click_gesture));

  // --- Configuration & Wayland Protocol Attachment ---
  state->auto_hide = get_autohide_config();
  LOG_C_INFO("Main", "State configured (auto_hide: %d, edge: %d)",
             state->auto_hide, state->edge);

  setup_layer_shell_window(state);
  setup_dock_motion_controller(state);

  // g_signal_connect_swapped flips signal data parameters so
  // free_lambdock_state receives 'state'
  g_signal_connect_swapped(window, "destroy", G_CALLBACK(free_lambdock_state),
                           state);

  // Render GTK window surface
  gtk_window_present(GTK_WINDOW(window));

  // Initialize Wayland foreign toplevel window tracker
  init_toplevel_tracker(state);
  schedule_initial_autohide(state);
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
  GtkApplication *app =
      gtk_application_new("org.scheme.lambdock", G_APPLICATION_DEFAULT_FLAGS);

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
