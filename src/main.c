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
#include "toplevel_tracker.h"
#include "ui_helpers.h"

/* =========================================================================
 * GTK APPLICATION ACTIVATION (UI CONSTRUCTION & SETUP)
 * ========================================================================= */

/* =========================================================================
 * GTK APPLICATION ACTIVATION (UI CONSTRUCTION & SETUP)
 * ========================================================================= */

/**
 * activate:
 * @app: Pointer to active GtkApplication context.
 * @user_data: Generic user context (unused).
 *
 * Activation signal entry point. Evaluates Scheme configurations, determines
 * dynamic screen edge anchor and box orientation, constructs window layout, and
 * applies Wayland Layer Shell settings.
 */
static void activate(GtkApplication *app, gpointer user_data) {
  (void)user_data;

  // --- Initialize Guile Scheme Runtime Module ---
  setup_guile_load_path();
  scm_c_use_module("lambdock core");

  // Load custom icon theme assets & desktop CSS stylesheets
  setup_icon_theme();

  // Allocate state container struct
  LambdockState *state = g_new0(LambdockState, 1);

  load_custom_css();

  // --- Parse Position Configuration ---
  // Read edge setting from Guile Scheme ("top", "bottom", "left", "right")
  state->edge = get_position_config();

  // Infer GTK orientation: side docks are VERTICAL, top/bottom docks are
  // HORIZONTAL
  state->orientation = (state->edge == GTK_LAYER_SHELL_EDGE_LEFT ||
                        state->edge == GTK_LAYER_SHELL_EDGE_RIGHT)
                           ? GTK_ORIENTATION_VERTICAL
                           : GTK_ORIENTATION_HORIZONTAL;

  // --- Build Main GTK Window & Outer Container ---
  GtkWidget *window = gtk_application_window_new(app);
  state->window = window;

  // Instantiate GTK Box layout dynamic to orientation with 8px item spacing
  GtkWidget *box = gtk_box_new(state->orientation, 8);
  gtk_widget_add_css_class(box, "rounded-dock");

  // Set box as root child widget of main GTK Window
  gtk_window_set_child(GTK_WINDOW(window), box);

  // Parse Scheme items and populate box, passing dock orientation for
  // separators
  populate_dock_items(box, state->orientation);

  // --- Register Application Actions (GTK Command Mapping) ---
  GSimpleAction *quit_action = g_simple_action_new("quit", NULL);
  g_signal_connect(quit_action, "activate", G_CALLBACK(on_quit_action), app);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(quit_action));

  // --- Construct Context Popover Menu ---
  GMenu *menu_model = g_menu_new();
  g_menu_append(menu_model, "Quit", "app.quit");

  GtkWidget *popover =
      gtk_popover_menu_new_from_model(G_MENU_MODEL(menu_model));
  g_object_unref(menu_model);

  gtk_widget_set_parent(popover, window);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);

  state->popover = popover;

  g_signal_connect(popover, "closed", G_CALLBACK(on_popover_closed), state);

  // --- Attach Right-Click Input Controller ---
  GtkGesture *right_click_gesture = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click_gesture),
                                GDK_BUTTON_SECONDARY);
  g_signal_connect(right_click_gesture, "pressed", G_CALLBACK(on_right_click),
                   state);
  gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(right_click_gesture));

  // --- Load Remaining Configurations & Configure Layer Shell ---
  state->auto_hide = get_autohide_config();
  g_print("[Lambdock] state->auto_hide: %d, state->edge: %d\n",
          state->auto_hide, state->edge);

  // Apply Wayland Layer Shell settings using state container
  setup_layer_shell_window(state);

  // Attach pointer enter/leave motion controllers
  setup_dock_motion_controller(state);

  // Free state memory on window destruction
  g_signal_connect_swapped(window, "destroy", G_CALLBACK(g_free), state);

  // Render window surface on display
  gtk_window_present(GTK_WINDOW(window));

  // Initialize Wayland foreign toplevel window tracker
  init_toplevel_tracker(state);

  // Schedule auto-hide timer if active
  schedule_initial_autohide(state);
}

/* =========================================================================
 * GUILE INNER MAIN & GTK APPLICATION RUNNER
 * ========================================================================= */

/**
 * inner_main:
 * @data: Context data passed from scm_boot_guile.
 * @argc: Command line argument count.
 * @argv: Command line argument vector array.
 *
 * Callback executed by internal runtime when environment is ready.
 * Instantiates GtkApplication and enters GLib's main event loop.
 */
static void inner_main(void *data, int argc, char **argv) {
  // Silence unused variable warning
  (void)data;

  // Create GtkApplication with unique reverse-DNS application ID
  GtkApplication *app =
      gtk_application_new("org.scheme.lambdock", G_APPLICATION_DEFAULT_FLAGS);

  // Connect application activation signal to UI setup handler
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

  // Run application event processing loop (blocks until app exits)
  g_application_run(G_APPLICATION(app), argc, argv);

  // Release application reference to clean memory on exit
  g_object_unref(app);
}

/* =========================================================================
 * PROGRAM ENTRY POINT
 * ========================================================================= */

/**
 * main:
 * Entry point executed by operating system kernel on launch.
 */
int main(int argc, char **argv) {
// Optional preprocessor flag to configure default GTK Graphics Scene Kit
// renderer backend
#ifdef DEFAULT_GSK_RENDERER
  g_setenv("GSK_RENDERER", DEFAULT_GSK_RENDERER, FALSE);
#endif

  // Hand over control to Guile environment initialization, which then invokes
  // 'inner_main'
  scm_boot_guile(argc, argv, inner_main, NULL);

  return 0; // Return execution status code to OS
}
