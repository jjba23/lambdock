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
#include "ui/dock_item.h"
#include "ui/icon_resolver.h"
#include "ui/popover_menu.h"
#include "ui/theme_engine.h"

/* =========================================================================
 * GLOBAL STATE VARIABLES
 * =========================================================================
 */

/**
 * g_active_states:
 * A GLib doubly-linked list (`GList*`) storing pointers to `LambdockState`
 * instances.
 */
static GList *g_active_states = NULL;

/**
 * g_reload_timer_id:
 * GLib event source ID for debouncing file modification events.
 * 0 means no reload is currently scheduled.
 */
static guint g_reload_timer_id = 0;

/* =========================================================================
 * GUILE SCHEME <-> C BRIDGE & HOT-RELOAD LOGIC
 * ========================================================================= */

/**
 * on_manual_reload_idle:
 * GTK Idle callback function executed on the main UI thread.
 * * WHY IS THIS NEEDED?
 * In GTK applications, UI updates MUST happen on the main event thread. If an
 * event comes from another thread (like Guile or a background process), we
 * schedule it on the main thread loop using `g_idle_add()`.
 *
 * @param user_data Unused pointer passed from GLib event loop.
 * @return G_SOURCE_REMOVE (false) to automatically unregister this idle handler
 * after 1 run.
 */
static gboolean on_manual_reload_idle(gpointer user_data) {
  (void)user_data; // Silence compiler warning regarding unused parameters
  LOG_C_INFO("Main", "Triggering manual UI reload...");

  // Iterate over every active dock window instance in our global list
  for (GList *l = g_active_states; l != NULL; l = l->next) {
    LambdockState *state = (LambdockState *)l->data;
    reload_dock_instance(state); // Re-creates widgets according to new settings
  }

  return G_SOURCE_REMOVE; // Don't repeat this function
}

/**
 * trigger_dock_reload:
 * Schedules a UI refresh on the GTK main loop thread as soon as it is idle.
 */
static void trigger_dock_reload(void) {
  g_idle_add(on_manual_reload_idle, NULL);
}

/**
 * scm_reload_dock:
 * C implementation of the `(reload-dock!)` Scheme function.
 * * `SCM` is the primitive C type used by GNU Guile to represent any Scheme
 * value (strings, numbers, functions, symbols, booleans).
 * * @return SCM_UNSPECIFIED equivalent to Scheme's `#<unspecified>`.
 */
static SCM scm_reload_dock(void) {
  trigger_dock_reload();
  return SCM_UNSPECIFIED;
}

/**
 * setup_guile_primitives:
 * Exposes C functions to the Guile Scheme runtime engine.
 * This allows Scheme scripts to call C functions directly!
 */
static void setup_guile_primitives(void) {
  // Enter the Scheme module domain "(lambdock core)"
  scm_c_use_module("lambdock core");

  // Register `scm_reload_dock` as the Scheme function `reload-dock!`
  // Arguments: Name, req_args, opt_args, rst_args, function_pointer
  scm_c_define_gsubr("reload-dock!", 0, 0, 0,
                     (scm_t_subr)(intptr_t)scm_reload_dock);

  // Make `reload-dock!` available publicly to other Scheme files
  scm_c_export("reload-dock!", NULL);
}

/**
 * on_config_reload_timeout:
 * Callback executed 150ms after config file writes cease.
 * * CONCEPT: Debouncing
 * When saving a file in text editors (Vim, VS Code), multiple file write events
 * trigger in milliseconds. Debouncing waits until editing stops before
 * reloading.
 */
static gboolean on_config_reload_timeout(gpointer user_data) {
  (void)user_data;

  // Re-evaluate Scheme environment (re-reads settings.scm)
  reload_guile_config();

  // Refresh UI widgets across all active dock instances
  for (GList *l = g_active_states; l != NULL; l = l->next) {
    LambdockState *state = (LambdockState *)l->data;
    reload_dock_instance(state);
  }

  g_reload_timer_id = 0;  // Reset timer ID back to 0
  return G_SOURCE_REMOVE; // Remove timer source from GLib loop
}

/**
 * on_file_changed_cb:
 * Event handler triggered automatically by OS file-system monitors whenever
 * `settings.scm` or `styles.css` are modified, created, or saved.
 */
static void on_file_changed_cb(GFileMonitor *monitor, GFile *file,
                               GFile *other_file, GFileMonitorEvent event_type,
                               gpointer user_data) {
  // Silence unused variable warnings
  (void)monitor;
  (void)file;
  (void)other_file;
  (void)user_data;

  // Check if event corresponds to file edit/write events
  if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
      event_type == G_FILE_MONITOR_EVENT_CHANGED ||
      event_type == G_FILE_MONITOR_EVENT_CREATED) {

    // If a timer was already pending, cancel it (user is still typing/saving)
    if (g_reload_timer_id > 0) {
      g_source_remove(g_reload_timer_id);
    }

    // Schedule timeout callback to run after 150ms of silence
    g_reload_timer_id = g_timeout_add(150, on_config_reload_timeout, NULL);
  }
}

/**
 * setup_config_file_monitors:
 * Sets up background OS watchers (`GFileMonitor`) on the user's configuration
 * files:
 * 1. Scheme Settings File (`settings.scm`)
 * 2. User Styling File (`~/.config/lambdock/styles.css`)
 */
static void setup_config_file_monitors(void) {
  // --- Watch settings.scm ---
  char *config_path = get_config_path_c();
  if (config_path) {
    GFile *scheme_file = g_file_new_for_path(config_path);
    GFileMonitor *scheme_mon =
        g_file_monitor_file(scheme_file, G_FILE_MONITOR_NONE, NULL, NULL);

    if (scheme_mon) {
      // Connect file change signal to our handler
      g_signal_connect(scheme_mon, "changed", G_CALLBACK(on_file_changed_cb),
                       NULL);
      LOG_C_INFO("Main", "Watching Scheme config for live hot-reload: %s",
                 config_path);
    }
    g_object_unref(scheme_file); // Clean up GFile reference
    g_free(config_path);         // Free allocated path string memory
  }

  // --- Watch styles.css ---
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
 * Constructs and displays a single dock window on a specific monitor output.
 * * @param app Pointer to active GtkApplication instance.
 * @param monitor GdkMonitor target (or NULL for default monitor).
 */
static void create_dock_instance(GtkApplication *app, GdkMonitor *monitor) {
  // Allocate zero-initialized memory block for this dock's state wrapper
  LambdockState *state = g_new0(LambdockState, 1);
  state->monitor = monitor;
  state->theme = get_theme_config();

  // Load custom user CSS styles into GTK CSS Provider
  load_custom_css(state);

  // Read dock screen placement from Guile config ('top, 'bottom, 'left, 'right)
  state->edge = get_position_config();

  // Set orientation to Vertical if docked on left/right edges, else Horizontal
  state->orientation = (state->edge == GTK_LAYER_SHELL_EDGE_LEFT ||
                        state->edge == GTK_LAYER_SHELL_EDGE_RIGHT)
                           ? GTK_ORIENTATION_VERTICAL
                           : GTK_ORIENTATION_HORIZONTAL;

  // Create GTK Application Window
  GtkWidget *window = gtk_application_window_new(app);
  state->window = window;

  // Create primary container box widget inside the window (8px spacing)
  GtkWidget *box = gtk_box_new(state->orientation, 8);
  gtk_widget_add_css_class(box, "lambdock-window");
  gtk_window_set_child(GTK_WINDOW(window), box);
  state->box = box;

  // Read Scheme configuration parameters
  state->auto_hide = get_autohide_config();
  state->show_when_clear = get_show_when_clear_config();
  state->hover_animate = get_hover_animate_config();
  state->icon_size = get_icon_size_config();

  // Create dock items (launchers, separators, active app icons)
  populate_dock_items(state);

  // --- Right-Click Context Popover Menu ---
  GMenu *menu_model = g_menu_new();
  g_menu_append(menu_model, "About Lambdock", "app.about");
  g_menu_append(menu_model, "Quit", "app.quit");

  GtkWidget *popover =
      gtk_popover_menu_new_from_model(G_MENU_MODEL(menu_model));
  g_object_unref(menu_model); // Unref model once popover borrows it

  gtk_widget_set_parent(popover, window);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover),
                            FALSE); // Clean borderless look
  state->popover = popover;
  g_signal_connect(popover, "closed", G_CALLBACK(on_popover_closed), state);

  // --- Mouse Click Gesture Controllers ---
  GtkGesture *right_click_gesture = gtk_gesture_click_new();
  // Listen specifically for Right Mouse Button (Secondary)
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click_gesture),
                                GDK_BUTTON_SECONDARY);
  g_signal_connect(right_click_gesture, "pressed", G_CALLBACK(on_right_click),
                   state);
  gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(right_click_gesture));

  // --- Wayland Layer Shell Configuration ---
  setup_layer_shell_window(state);     // Pin window to Wayland desktop layer
  setup_dock_motion_controller(state); // Handle mouse hover show/hide

  // Add state pointer to global tracking list for hot-reloading
  g_active_states = g_list_append(g_active_states, state);

  // When window is destroyed, free allocated memory automatically
  g_signal_connect_swapped(window, "destroy", G_CALLBACK(free_lambdock_state),
                           state);

  // Present window to desktop compositor
  gtk_window_present(GTK_WINDOW(window));

  // Track running applications to display indicators under active icons
  init_toplevel_tracker(state);

  // Start initial auto-hide countdown if enabled
  schedule_initial_autohide(state);
}

/**
 * activate:
 * Core setup callback triggered when `GtkApplication` starts up.
 */
static void activate(GtkApplication *app, gpointer user_data) {
  (void)user_data;

  LOG_C_INFO("Main", "Initializing Guile engine & multi-monitor manager...");

  // Initialize Guile load paths so it can locate core Scheme modules
  setup_guile_load_path();
  scm_c_use_module("lambdock core");

  // Register custom C primitives into Scheme
  setup_guile_primitives();

  // Call Scheme initialization entry point `(init-dock!)`
  SCM init_fn = scm_c_public_ref("lambdock core", "init-dock!");
  scm_call_0(init_fn);

  // Start live REPL (Read-Eval-Print-Loop) if enabled in settings
  SCM start_repl_fn = scm_c_public_ref("lambdock core", "start-repl!");
  if (scm_is_true(start_repl_fn)) {
    scm_call_0(start_repl_fn);
  }

  // Initialize GTK desktop icon resolver
  setup_icon_theme();

  // --- Global Application Menu Actions ---
  GSimpleAction *about_action = g_simple_action_new("about", NULL);
  g_signal_connect(about_action, "activate", G_CALLBACK(on_about_action), app);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(about_action));

  GSimpleAction *quit_action = g_simple_action_new("quit", NULL);
  g_signal_connect(quit_action, "activate", G_CALLBACK(on_quit_action), app);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(quit_action));

  // --- Multi-Monitor Resolution & Window Spawning ---
  char *monitor_target =
      get_monitor_config(); // Read 'all or specific display name
  GdkDisplay *display = gdk_display_get_default();
  GListModel *monitors = gdk_display_get_monitors(display);
  guint n_monitors = g_list_model_get_n_items(monitors);

  if (g_strcmp0(monitor_target, "all") == 0) {
    // If setting is 'all, spawn a dock window on every connected monitor
    for (guint i = 0; i < n_monitors; i++) {
      GdkMonitor *mon = GDK_MONITOR(g_list_model_get_item(monitors, i));
      create_dock_instance(app, mon);
      g_object_unref(mon); // Drop local reference owner
    }
  } else {
    // Single-monitor configuration default
    create_dock_instance(app, NULL);
  }

  g_free(monitor_target);

  // Initialize live file monitoring for dynamic hot-reloading
  setup_config_file_monitors();
}

/* =========================================================================
 * PROGRAM ENTRY POINT
 * ========================================================================= */

/**
 * inner_main:
 * Continuation function invoked once the Guile Scheme interpreter environment
 * finishes booting up completely.
 */
static void inner_main(void *data, int argc, char **argv) {
  (void)data;

  // Construct GtkApplication instance using unique reverse-DNS identifier
  GtkApplication *app = gtk_application_new("org.jointhefreeworld.lambdock",
                                            G_APPLICATION_DEFAULT_FLAGS);

  // Connect GTK 'activate' signal to our setup function
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

  // Hand execution over to GTK main loop (Blocks until application exits)
  g_application_run(G_APPLICATION(app), argc, argv);

  // Cleanup application reference upon quit
  g_object_unref(app);
}

/**
 * main:
 * Standard C binary main entry point.
 */
int main(int argc, char **argv) {
  // Override or enforce default GTK Graphics Renderer backend if defined
#ifdef DEFAULT_GSK_RENDERER
  g_setenv("GSK_RENDERER", DEFAULT_GSK_RENDERER, FALSE);
#endif
  // Initializes the GNU Guile Scheme engine runtime, initializes garbage
  // collection, and hands control over to `inner_main`.
  scm_boot_guile(argc, argv, inner_main, NULL);

  return 0; // Return success exit status code
}
