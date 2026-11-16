// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "core/tracker_rules.h"
#include "dock_window.h"
#include "accessibility.h"
#include "dock_registry.h"
#include "guile_bridge.h"
#include "lambdock.h"

#include <libguile.h>
#include "logger.h"
#include "toplevel_tracker.h"
#include "ui/shelf.h"
#include "ui/dock_dnd.h"
#include "ui/dock_item.h"
#include "ui/icon_resolver.h"
#include "ui/launchpad.h"
#include "ui/popover_menu.h"
#include "ui/preferences.h"
#include "ui/theme_engine.h"
#include <glib/gi18n.h>
#include <locale.h>
#include <stdlib.h>

/* =========================================================================
 * GLOBAL STATE VARIABLES
 * =========================================================================
 */

/**
 * MainTimers:
 * @config_reload: source id debouncing settings-file changes, 0 when nothing is
 *   scheduled.
 * @state_reload: source id debouncing local-state changes (the Preferences
 *   overrides and the drag-and-drop layout), 0 when nothing is scheduled.
 *
 * The process-wide timers live in one value, so their ownership and their
 * teardown are one thing to look at.
 */
typedef struct {
  guint config_reload;
  guint state_reload;
} MainTimers;

static MainTimers g_timers;

/* The two decisions `main.c` used to make itself -- what a settings file is
 * called, and how a wheel moves the row -- are `rust/src/main_app.rs` now, along
 * with the monitor lookup; they cross the seam by name. */
int is_settings_filename_c(const char *filename);
char *get_config_dir_c(void);
void on_system_motion_changed(GObject *settings, GParamSpec *pspec,
                              gpointer user_data);
void on_export_settings_action(GSimpleAction *action, GVariant *parameter,
                               gpointer user_data);
char *get_state_dir_c(void);
GList *scan_settings_files(void);
gboolean on_dock_scroll(GtkEventControllerScroll *controller, double dx,
                        double dy, gpointer user_data);
GdkMonitor *find_monitor_by_connector(const char *connector);
void create_dock_instance(GtkApplication *app, GdkMonitor *monitor,
                          const char *config_path);
void sync_docks_with_disk(GtkApplication *app);

/**
 * unregister_dock_state:
 * Removes an active LambdockState instance from the dock registry.
 */
void unregister_dock_state(LambdockState *state) {
  dock_registry_remove(state);
}

/* =========================================================================
 * HELPER ROUTINES FOR DIRECTORY DISCOVERY & FILE MATCHING
 * ========================================================================= */

/* =========================================================================
 * GUILE SCHEME <-> C BRIDGE & HOT-RELOAD LOGIC
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * SELECTING DOCKS
 * -------------------------------------------------------------------------
 * Every "reconcile the docks with the world" step has the same shape: pick the
 * registered docks that satisfy some predicate, then destroy them. Both halves
 * are small helpers here, so the policy below reads as policy.
 */

/* =========================================================================
 * REPL HOT-RELOAD vs DISK SYNC
 * ========================================================================= */

/**
 * redraw_active_docks:
 * Re-applies active in-memory Guile state to GTK widgets without re-reading
 * disk files.
 */
static void redraw_visitor(LambdockState *state, gpointer user_data) {
  (void)user_data;
  reload_dock_instance(state);
}

static void redraw_active_docks(void) {
  dock_registry_foreach(redraw_visitor, NULL);
}

static gboolean on_manual_reload_idle(gpointer user_data) {
  (void)user_data;
  LOG_C_INFO("Main", "Redrawing UI from in-memory state...");
  redraw_active_docks();
  return G_SOURCE_REMOVE;
}

/**
 * scm_reload_dock:
 * Scheme procedure (reload-dock!). Redraws UI from current Guile memory state.
 */
static SCM scm_reload_dock(void) {
  g_idle_add(on_manual_reload_idle, NULL);
  return SCM_UNSPECIFIED;
}

/* ---- launchpad primitives -------------------------------------------------
 * The REPL runs on its own thread, so everything below marshals to the GTK
 * thread with g_idle_add and hands it a value it owns.
 */

/** Shows the launchpad of the first active dock (the one a user would see). */
static gboolean show_launchpad_idle(gpointer user_data) {
  (void)user_data;

  g_autoptr(GPtrArray) docks = dock_registry_snapshot();
  if (docks->len > 0)
    lambdock_launchpad_show(g_ptr_array_index(docks, 0));

  return G_SOURCE_REMOVE;
}

static gboolean hide_launchpad_idle(gpointer user_data) {
  (void)user_data;

  lambdock_launchpad_hide();
  return G_SOURCE_REMOVE;
}

/** @data is the strdup'd query; owned and freed here. */
static gboolean launchpad_search_idle(gpointer data) {
  g_autofree char *query = data;

  lambdock_launchpad_set_query(query);
  return G_SOURCE_REMOVE;
}

static SCM scm_show_launchpad(void) {
  g_idle_add(show_launchpad_idle, NULL);
  return SCM_UNSPECIFIED;
}

static SCM scm_hide_launchpad(void) {
  g_idle_add(hide_launchpad_idle, NULL);
  return SCM_UNSPECIFIED;
}

static SCM scm_launchpad_search(SCM query) {
  char *text = scm_to_utf8_string(query);
  g_idle_add(launchpad_search_idle, text); /* the idle takes ownership */
  return SCM_UNSPECIFIED;
}

/**
 * setup_guile_primitives:
 * Exposes C functions to the Guile Scheme runtime engine.
 */
static void setup_guile_primitives(void) {
  /* Define the primitive *inside* (lambdock core): `scm_c_use_module' only
   * imports that module into the current one, so defining without switching
   * first would put `reload-dock!' in (guile-user) -- where the setters, which
   * resolve it with `module-ref' on (lambdock core), would never find it and
   * the dock would not redraw after `(set-dock-...!)'. */
  SCM module = scm_c_resolve_module("lambdock core");
  SCM previous = scm_set_current_module(module);

  scm_c_define_gsubr("reload-dock!", 0, 0, 0,
                     (scm_t_subr)(intptr_t)scm_reload_dock);
  scm_c_define_gsubr("show-launchpad!", 0, 0, 0,
                     (scm_t_subr)(intptr_t)scm_show_launchpad);
  scm_c_define_gsubr("hide-launchpad!", 0, 0, 0,
                     (scm_t_subr)(intptr_t)scm_hide_launchpad);
  scm_c_define_gsubr("launchpad-search!", 1, 0, 0,
                     (scm_t_subr)(intptr_t)scm_launchpad_search);
  scm_c_export("reload-dock!", "show-launchpad!", "hide-launchpad!",
               "launchpad-search!", NULL);

  scm_set_current_module(previous);
}

/**
 * on_config_reload_timeout:
 * Debounced callback executed after file changes cease in ~/.config/lambdock.
 */
static gboolean on_config_reload_timeout(gpointer user_data) {
  GtkApplication *app = GTK_APPLICATION(user_data);

  /* The source is gone once we return; forget the id so the next file event
   * cannot remove an unrelated (recycled) source. */
  g_timers.config_reload = 0;
  sync_docks_with_disk(app);
  return G_SOURCE_REMOVE;
}

/**
 * state_reload_visitor:
 * Re-reads one dock's settings and redraws it.
 *
 * A local-state file is not a settings file: it is replayed *on top of* one, by
 * the Scheme loader. Reading the settings file again is therefore what makes
 * the new preferences or layout visible -- the in-memory configuration the
 * setters already updated is not the whole story, because the state files are
 * also written by other tools (a reconfigured home, a second dock, a hand
 * edit).
 */
static void state_reload_visitor(LambdockState *state, gpointer user_data) {
  (void)user_data;
  if (!state)
    return;

  if (state->config_path && *state->config_path != '\0')
    load_dock_config_scm(state->config_path);

  reload_dock_instance(state);
}

/** Debounced redraw after the local state directory settled. */
static gboolean on_state_reload_timeout(gpointer user_data) {
  (void)user_data;

  g_timers.state_reload = 0;
  dock_registry_foreach(state_reload_visitor, NULL);
  return G_SOURCE_REMOVE;
}

/** Debounces local-state events: a write is several events on some backends. */
static void on_state_changed_cb(GFileMonitor *monitor, GFile *file,
                                GFile *other_file, GFileMonitorEvent event_type,
                                gpointer user_data) {
  (void)monitor;
  (void)file;
  (void)other_file;
  (void)event_type;
  (void)user_data;

  g_clear_handle_id(&g_timers.state_reload, g_source_remove);
  g_timers.state_reload =
      g_timeout_add(CONFIG_RELOAD_DEBOUNCE_MS, on_state_reload_timeout, NULL);
}

/**
 * on_file_changed_cb:
 * Event handler triggered whenever files/directories in ~/.config/lambdock are
 * modified, created, or deleted.
 */
static void on_file_changed_cb(GFileMonitor *monitor, GFile *file,
                               GFile *other_file, GFileMonitorEvent event_type,
                               gpointer user_data) {
  (void)monitor;
  (void)file;
  (void)other_file;
  (void)event_type;

  /* Debounce: restart the countdown on every event. */
  g_clear_handle_id(&g_timers.config_reload, g_source_remove);
  g_timers.config_reload = g_timeout_add(CONFIG_RELOAD_DEBOUNCE_MS,
                                         on_config_reload_timeout, user_data);
}

/**
 * setup_config_file_monitors:
 * Sets up directory-level watchers (`GFileMonitor`) on ~/.config/lambdock/ for
 * settings files, and on ~/.local/state/lambdock/ for the local state they are
 * layered with.
 *
 * Both are watched for the same reason: what the dock draws is the settings file
 * *plus* the preferences and layout overrides, and a change to either half is
 * something the user expects to see. The state directory is created first, since
 * a dock that has never been edited has not made one yet.
 *
 * The monitors are deliberately left referenced: they are process-lifetime
 * objects whose signal is the only thing that keeps them doing their job.
 */
static void setup_config_file_monitors(GtkApplication *app) {
  char *config_dir = get_config_dir_c();
  GFile *dir_file = g_file_new_for_path(config_dir);
  GFileMonitor *dir_mon =
      g_file_monitor_directory(dir_file, G_FILE_MONITOR_NONE, NULL, NULL);

  if (dir_mon) {
    g_signal_connect(dir_mon, "changed", G_CALLBACK(on_file_changed_cb), app);
    LOG_C_INFO("Main", "Watching directory for config changes: %s", config_dir);
  }
  g_object_unref(dir_file);
  g_free(config_dir);

  char *state_dir = get_state_dir_c();
  g_mkdir_with_parents(state_dir, 0700);

  GFile *state_file = g_file_new_for_path(state_dir);
  GFileMonitor *state_mon =
      g_file_monitor_directory(state_file, G_FILE_MONITOR_NONE, NULL, NULL);

  if (state_mon) {
    g_signal_connect(state_mon, "changed", G_CALLBACK(on_state_changed_cb),
                     NULL);
    LOG_C_INFO("Main", "Watching directory for local state changes: %s",
               state_dir);
  }
  g_object_unref(state_file);
  g_free(state_dir);
}

/* =========================================================================
 * MONITOR HOTPLUG LISTENER SETUP
 * ========================================================================= */

/**
 * on_monitors_changed_cb:
 * Triggered whenever the Wayland compositor adds or removes display monitor
 * outputs.
 */
/*
 * Monitor reconciliation is deferred to an idle callback: creating/destroying
 * layer-shell surfaces (and iterating the monitor model in is_monitor_valid)
 * while the GListModel is still emitting items-changed is reentrancy-prone.
 */
/* Id of the pending monitor-reconcile idle, or 0 when none is queued. Keeping
 * the id (instead of a separate boolean) means the "already queued" test and
 * the cancellation use the same value. */
static guint g_monitor_sync_idle_id = 0;

static gboolean on_monitors_changed_idle(gpointer user_data) {
  GtkApplication *app = GTK_APPLICATION(user_data);

  g_monitor_sync_idle_id = 0;
  LOG_C_INFO("Main", "MONITORS CHANGED: reconciling docks");
  sync_docks_with_disk(app);
  LOG_C_INFO("Main", "MONITORS CHANGED: reconciliation done");
  return G_SOURCE_REMOVE;
}

static void on_monitors_changed_cb(GListModel *monitors, guint position,
                                   guint removed, guint added,
                                   gpointer user_data) {
  (void)monitors;
  LOG_C_INFO("Main", "MONITORS CHANGED: position=%u removed=%u added=%u",
             position, removed, added);

  if (g_monitor_sync_idle_id == 0)
    g_monitor_sync_idle_id =
        g_idle_add(on_monitors_changed_idle, user_data);
}

/**
 * setup_monitor_listeners:
 * Hooks into GdkDisplay monitor model signals to detect monitor hotplug events.
 */
static void setup_monitor_listeners(GtkApplication *app) {
  GdkDisplay *display = gdk_display_get_default();
  if (display) {
    GListModel *monitors = gdk_display_get_monitors(display);
    if (monitors) {
      g_signal_connect(monitors, "items-changed",
                       G_CALLBACK(on_monitors_changed_cb), app);
      LOG_C_INFO("Main",
                 "Registered listener for display monitor hotplug events.");
    }
  }
}

/**
 * load_overlay_css:
 * Loads the theme independent overlay stylesheet (stack popovers, the
 * launchpad and drag feedback) for the whole display. Overlays are not part of
 * any dock capsule, so they must not be scoped to a theme class.
 */
static void load_overlay_css(void) {
  GdkDisplay *display = gdk_display_get_default();
  if (!display)
    return;

  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(
      provider, "/org/jointhefreeworld/lambdock/css/overlays.css");

  gtk_style_context_add_provider_for_display(
      display, GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
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

  // Initialize GTK desktop icon resolver
  setup_icon_theme();

  // Theme independent styling for stacks, the launchpad and drag feedback
  load_overlay_css();

  // Follow the desktop's reduced-motion setting while the dock runs
  lambdock_watch_system_motion(G_CALLBACK(on_system_motion_changed), NULL);

  // --- Global Application Menu Actions ---
  GSimpleAction *about_action = g_simple_action_new("about", NULL);
  g_signal_connect(about_action, "activate", G_CALLBACK(on_about_action), app);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(about_action));
  g_object_unref(about_action); /* the action map holds its own reference */

  GSimpleAction *quit_action = g_simple_action_new("quit", NULL);
  g_signal_connect(quit_action, "activate", G_CALLBACK(on_quit_action), app);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(quit_action));
  g_object_unref(quit_action); /* the action map holds its own reference */

  /* Preferences takes the dock's settings path as a string target. */
  GSimpleAction *prefs_action =
      g_simple_action_new("preferences", G_VARIANT_TYPE_STRING);
  g_signal_connect(prefs_action, "activate", G_CALLBACK(on_preferences_action),
                   app);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(prefs_action));
  g_object_unref(prefs_action); /* the action map holds its own reference */

  /* Same shape: the dock menu passes the settings path it belongs to. */
  GSimpleAction *export_action =
      g_simple_action_new("export-settings", G_VARIANT_TYPE_STRING);
  g_signal_connect(export_action, "activate",
                   G_CALLBACK(on_export_settings_action), app);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(export_action));
  g_object_unref(export_action);

  // Reconcile disk state to spawn docks for all matching settings*.scm files,
  // or fallback if none
  sync_docks_with_disk(app);

  // Initialize live directory monitoring for dynamic hot-reloading
  // (add/delete/modify)
  setup_config_file_monitors(app);

  // Attach display monitor hotplug listener
  setup_monitor_listeners(app);
}

/* =========================================================================
 * PROGRAM ENTRY POINT
 * ========================================================================= */

/**
 * on_app_shutdown:
 * Releases the preferences windows when the application is tearing down.
 */
static void on_app_shutdown(GApplication *application, gpointer user_data) {
  (void)application;
  (void)user_data;
  lambdock_preferences_shutdown();
}

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
  g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);

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

  // Initialize i18n & gettext routing
  setlocale(LC_ALL, "");
#ifdef LOCALEDIR
  bindtextdomain("lambdock", LOCALEDIR);
#endif
  bind_textdomain_codeset("lambdock", "UTF-8");
  textdomain("lambdock");

  // Initializes the GNU Guile Scheme engine runtime, initializes garbage
  // collection, and hands control over to `inner_main`.
  scm_boot_guile(argc, argv, inner_main, NULL);

  return 0; // Return success exit status code
}
