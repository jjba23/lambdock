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

/**
 * activate:
 * @app: Pointer to the active GtkApplication instance.
 * @user_data: User data parameter (unused here).
 *
 * Primary entry point for UI building. Executed when GtkApplication emits
 * the "activate" signal upon application startup.
 */
static void activate(GtkApplication *app, gpointer user_data) {
  // Silence unused variable warning
  (void)user_data;

  // --- Initialize Guile Scheme Runtime Module ---
  setup_guile_load_path();
  scm_c_use_module("lambdock core"); // Import Scheme core module definitions

  // Load custom icon theme assets & desktop CSS stylesheets
  setup_icon_theme();

  // --- Allocate Application State ---
  // g_new0 allocates memory for 1 LambdockState struct and zero-initializes it
  LambdockState *state = g_new0(LambdockState, 1);

  // Load custom Dock CSS
  load_custom_css();

  // --- Build Main GTK Window & Outer Container ---
  GtkWidget *window = gtk_application_window_new(app);
  state->window = window;

  // Create a horizontal box layout with 8px spacing between child elements
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(box, "rounded-dock");

  // GTK 4 Window Child setup (replaces gtk_container_add from GTK 3)
  gtk_window_set_child(GTK_WINDOW(window), box);

  // Parse Guile configuration and dynamically populate dock buttons into the
  // box
  populate_dock_items(box);

  // --- Register Application Actions (GTK Command Mapping) ---
  // Create an abstract "quit" action attached to the application namespace
  // ("app.quit")
  GSimpleAction *quit_action = g_simple_action_new("quit", NULL);
  g_signal_connect(quit_action, "activate", G_CALLBACK(on_quit_action), app);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(quit_action));

  // --- Construct Context Popover Menu ---
  // Declarative GMenu model definition
  GMenu *menu_model = g_menu_new();
  g_menu_append(menu_model, "Quit",
                "app.quit"); // Maps menu item to "app.quit" action

  // Create popover menu UI widget from the structural model
  GtkWidget *popover =
      gtk_popover_menu_new_from_model(G_MENU_MODEL(menu_model));

  // Decrement reference count on menu_model now that popover widget owns a
  // reference
  g_object_unref(menu_model);

  // Attach popover as child of the main window widget
  gtk_widget_set_parent(popover, window);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover),
                            FALSE); // Clean borderless look

  // Store popover reference in state struct
  state->popover = popover;

  // Connect closed signal so auto-hide timer can resume when menu closes
  g_signal_connect(popover, "closed", G_CALLBACK(on_popover_closed), state);

  // --- Step 6: Attach Right-Click Input Controller ---
  // GTK 4 uses input controllers instead of direct signal handling on widgets
  GtkGesture *right_click_gesture = gtk_gesture_click_new();

  // Configure gesture specifically for secondary mouse button (Right-Click)
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click_gesture),
                                GDK_BUTTON_SECONDARY);

  // Trigger on_right_click callback, passing 'state' as user_data context
  g_signal_connect(right_click_gesture, "pressed", G_CALLBACK(on_right_click),
                   state);

  // Attach gesture controller to the main window widget
  gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(right_click_gesture));

  // --- Load Configuration & Configure Layer Shell ---
  state->auto_hide = get_autohide_config();
  g_print("[Lambdock] state->auto_hide: %d\n", state->auto_hide);

  // Setup Wayland Layer Shell protocols (anchoring, layer depth, margins)
  setup_layer_shell_window(GTK_WINDOW(window), state->auto_hide);

  // Attach enter/leave mouse motion controllers
  setup_dock_motion_controller(state);

  // AUTOMATIC MEMORY CLEANUP:
  // Connects g_free directly to the window's "destroy" signal.
  // 'swapped' causes g_free to receive 'state' as its primary argument when
  // destroyed.
  g_signal_connect_swapped(window, "destroy", G_CALLBACK(g_free), state);

  // Realize and present the window on screen
  gtk_window_present(GTK_WINDOW(window));

  // Initialize Wayland foreign toplevel window state tracker
  init_toplevel_tracker(state);

  // Schedule auto-hide timer countdown if auto-hide option is active
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
