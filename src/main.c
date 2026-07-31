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

#include "dock_window.h"
#include "guile_bridge.h"
#include "lambdock.h"
#include "ui_helpers.h"

static void activate(GtkApplication *app, gpointer user_data) {
  (void)user_data;

  setup_guile_load_path();
  scm_c_use_module("lambdock core");

  setup_icon_theme();

  LambdockState *state = g_new0(LambdockState, 1);

  GtkWidget *window = gtk_application_window_new(app);
  state->window = window;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_window_set_child(GTK_WINDOW(window), box);

  // Populate dock items & initialize Guile configuration
  populate_dock_items(box);

  // -------------------------------------------------------------
  // Register "app.quit" action
  // -------------------------------------------------------------
  GSimpleAction *quit_action = g_simple_action_new("quit", NULL);
  g_signal_connect(quit_action, "activate", G_CALLBACK(on_quit_action), app);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(quit_action));

  // -------------------------------------------------------------
  // Build the GMenu model & GtkPopoverMenu
  // -------------------------------------------------------------
  GMenu *menu_model = g_menu_new();
  g_menu_append(menu_model, "Quit", "app.quit");

  GtkWidget *popover =
      gtk_popover_menu_new_from_model(G_MENU_MODEL(menu_model));
  g_object_unref(menu_model);

  gtk_widget_set_parent(popover, window);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);

  // Store reference in state
  state->popover = popover;

  // Connect "closed" signal so auto-hide resumes when menu is closed
  g_signal_connect(popover, "closed", G_CALLBACK(on_popover_closed), state);

  // -------------------------------------------------------------
  // Attach Secondary (Right) Click Gesture to the window
  // -------------------------------------------------------------
  GtkGesture *right_click_gesture = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click_gesture),
                                GDK_BUTTON_SECONDARY);
  // Pass 'state' as user_data instead of 'popover'
  g_signal_connect(right_click_gesture, "pressed", G_CALLBACK(on_right_click),
                   state);
  gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(right_click_gesture));
  // -------------------------------------------------------------

  // Fetch loaded configuration
  state->auto_hide = get_autohide_config();
  g_print("[Lambdock] state->auto_hide: %d\n", state->auto_hide);

  // Configure layer-shell window & event handlers
  setup_layer_shell_window(GTK_WINDOW(window), state->auto_hide);
  setup_dock_motion_controller(state);

  g_signal_connect_swapped(window, "destroy", G_CALLBACK(g_free), state);

  gtk_window_present(GTK_WINDOW(window));

  schedule_initial_autohide(state);
}

static void inner_main(void *data, int argc, char **argv) {
  (void)data;
  GtkApplication *app =
      gtk_application_new("org.scheme.lambdock", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
}

int main(int argc, char **argv) {
#ifdef DEFAULT_GSK_RENDERER
  g_setenv("GSK_RENDERER", DEFAULT_GSK_RENDERER, FALSE);
#endif
  scm_boot_guile(argc, argv, inner_main, NULL);
  return 0;
}
