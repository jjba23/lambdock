// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

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
 *
 * The process-wide timers live in one value, so their ownership and their
 * teardown are one thing to look at.
 */
typedef struct {
  guint config_reload;
} MainTimers;

static MainTimers g_timers;

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

/**
 * is_settings_filename:
 * Returns TRUE if filename is "settings.scm" or matches pattern
 * "settings-*.scm".
 */
static gboolean is_settings_filename(const char *filename) {
  if (!filename)
    return FALSE;
  if (g_strcmp0(filename, "settings.scm") == 0)
    return TRUE;
  if (g_str_has_prefix(filename, "settings-") &&
      g_str_has_suffix(filename, ".scm"))
    return TRUE;
  return FALSE;
}

/**
 * get_config_dir_c:
 * Resolves the configuration directory (~/.config/lambdock).
 */
static char *get_config_dir_c(void) {
  const char *config_dir = g_get_user_config_dir();
  return g_build_filename(config_dir, "lambdock", NULL);
}

/**
 * scan_settings_files:
 * Scans ~/.config/lambdock/ for all matching settings files (settings.scm or
 * settings-*.scm). Returns a GList of heap-allocated absolute file path
 * strings.
 */
static GList *scan_settings_files(void) {
  char *config_dir = get_config_dir_c();
  GList *file_list = NULL;

  GDir *dir = g_dir_open(config_dir, 0, NULL);
  if (dir) {
    const char *filename;
    while ((filename = g_dir_read_name(dir)) != NULL) {
      if (is_settings_filename(filename)) {
        char *full_path = g_build_filename(config_dir, filename, NULL);
        file_list = g_list_append(file_list, full_path);
      }
    }
    g_dir_close(dir);
  }
  g_free(config_dir);
  return file_list;
}

/**
 * find_monitor_by_connector:
 * Resolves a monitor connector name (e.g. "DP-1") to the matching GdkMonitor.
 * Returns a new reference (caller must g_object_unref it) or NULL if none match.
 */
static GdkMonitor *find_monitor_by_connector(const char *connector) {
  if (!connector || *connector == '\0')
    return NULL;

#if GTK_CHECK_VERSION(4, 10, 0)
  GdkDisplay *display = gdk_display_get_default();
  if (!display)
    return NULL;

  GListModel *monitors = gdk_display_get_monitors(display);
  if (!monitors)
    return NULL;

  guint n = g_list_model_get_n_items(monitors);
  for (guint i = 0; i < n; i++) {
    GdkMonitor *mon = GDK_MONITOR(g_list_model_get_item(monitors, i));
    if (!mon)
      continue;

    const char *name = gdk_monitor_get_connector(mon);
    if (name && g_strcmp0(name, connector) == 0)
      return mon; /* transfer reference to the caller */
    g_object_unref(mon);
  }
#else
  (void)connector;
#endif
  return NULL;
}

/* =========================================================================
 * GUILE SCHEME <-> C BRIDGE & HOT-RELOAD LOGIC
 * ========================================================================= */

/**
 * on_dock_scroll:
 * Mouse wheel scroll event controller callback handler. Converts mouse wheel
 * motion (dy/dx) into smooth viewport scrolling along the active main dock
 * axis.
 */
static gboolean on_dock_scroll(GtkEventControllerScroll *controller, double dx,
                               double dy, gpointer user_data) {
  (void)controller;
  LambdockState *state = (LambdockState *)user_data;
  if (!state || !state->scrolled_window)
    return FALSE;

  if (state->orientation == GTK_ORIENTATION_HORIZONTAL) {
    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(
        GTK_SCROLLED_WINDOW(state->scrolled_window));
    double delta = (fabs(dx) > fabs(dy)) ? dx : dy;
    double page_size = gtk_adjustment_get_page_size(adj);
    double upper = gtk_adjustment_get_upper(adj);
    double lower = gtk_adjustment_get_lower(adj);
    double max_scroll = upper - page_size;
    double new_val = gtk_adjustment_get_value(adj) + delta * 30.0;
    if (new_val < lower)
      new_val = lower;
    if (new_val > max_scroll)
      new_val = max_scroll;
    gtk_adjustment_set_value(adj, new_val);
    return TRUE;
  } else {
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(state->scrolled_window));
    double page_size = gtk_adjustment_get_page_size(adj);
    double upper = gtk_adjustment_get_upper(adj);
    double lower = gtk_adjustment_get_lower(adj);
    double max_scroll = upper - page_size;
    double new_val = gtk_adjustment_get_value(adj) + dy * 30.0;
    if (new_val < lower)
      new_val = lower;
    if (new_val > max_scroll)
      new_val = max_scroll;
    gtk_adjustment_set_value(adj, new_val);
    return TRUE;
  }
  return FALSE;
}

/**
 * forward declaration of create_dock_instance.
 */
static void create_dock_instance(GtkApplication *app, GdkMonitor *monitor,
                                 const char *config_path);

/* -------------------------------------------------------------------------
 * SELECTING DOCKS
 * -------------------------------------------------------------------------
 * Every "reconcile the docks with the world" step has the same shape: pick the
 * registered docks that satisfy some predicate, then destroy them. Both halves
 * are small helpers here, so the policy below reads as policy.
 */

typedef gboolean (*DockPredicate)(const LambdockState *state, gpointer user);

/** A monitor plus the config path it belongs to. */
typedef struct {
  const char *config_path;
  GdkMonitor *monitor; /* borrowed */
} DockMonitorSelector;

/** Snapshot of the registered docks that satisfy @pred (all when NULL). */
static GPtrArray *docks_matching(DockPredicate pred, gpointer user) {
  GPtrArray *matches = g_ptr_array_new();
  GPtrArray *all = dock_registry_snapshot();

  for (guint i = 0; i < all->len; i++) {
    LambdockState *state = g_ptr_array_index(all, i);
    if (state && (!pred || pred(state, user)))
      g_ptr_array_add(matches, state);
  }
  g_ptr_array_unref(all);

  return matches;
}

/** Number of registered docks that satisfy @pred. */
static guint docks_matching_count(DockPredicate pred, gpointer user) {
  GPtrArray *matches = docks_matching(pred, user);
  guint count = matches->len;

  g_ptr_array_unref(matches);
  return count;
}

/** Destroys every dock in @states and frees the array itself. */
static void destroy_docks(GPtrArray *states) {
  if (!states)
    return;

  for (guint i = 0; i < states->len; i++)
    destroy_dock_instance(g_ptr_array_index(states, i));
  g_ptr_array_unref(states);
}

static gboolean dock_has_no_config(const LambdockState *state, gpointer user) {
  (void)user;
  return state->config_path == NULL;
}

static gboolean dock_has_config(const LambdockState *state, gpointer user) {
  (void)user;
  return state->config_path != NULL;
}

static gboolean dock_is_for_config(const LambdockState *state, gpointer user) {
  return g_strcmp0(state->config_path, (const char *)user) == 0;
}

static gboolean dock_is_for_config_without_monitor(const LambdockState *state,
                                                   gpointer user) {
  return dock_is_for_config(state, user) && state->monitor == NULL;
}

static gboolean dock_is_for_config_on_any_monitor(const LambdockState *state,
                                                  gpointer user) {
  return dock_is_for_config(state, user) && state->monitor != NULL;
}

static gboolean dock_is_on_selector_monitor(const LambdockState *state,
                                            gpointer user) {
  const DockMonitorSelector *selector = user;

  return dock_is_for_config(state, (gpointer)selector->config_path) &&
         state->monitor == selector->monitor;
}

static gboolean dock_is_not_on_selector_monitor(const LambdockState *state,
                                                gpointer user) {
  const DockMonitorSelector *selector = user;

  return dock_is_for_config(state, (gpointer)selector->config_path) &&
         state->monitor != selector->monitor;
}

/** TRUE when @state belongs to @user and its monitor is gone or invalid. */
static gboolean dock_is_stranded(const LambdockState *state, gpointer user) {
  if (!dock_is_for_config(state, user))
    return FALSE;

  return state->monitor == NULL || !is_monitor_valid(state->monitor);
}

/** TRUE when @state's settings file is not in @user (a GList of paths). */
static gboolean dock_config_missing_from(const LambdockState *state,
                                         gpointer user) {
  if (state->config_path == NULL)
    return FALSE;

  for (GList *l = (GList *)user; l != NULL; l = l->next) {
    if (g_strcmp0((const char *)l->data, state->config_path) == 0)
      return FALSE;
  }
  return TRUE;
}

/* -------------------------------------------------------------------------
 * DOCK RECONCILIATION
 * -------------------------------------------------------------------------
 */

/** A settings file's monitor policy, resolved once per reconcile pass. */
typedef struct MonitorSelection {
  gboolean all;
  GdkMonitor *target;      /* borrowed */
  char *connector;         /* owned */
} MonitorSelection;

/**
 * resolve_monitor_selection:
 * Turns the settings file's `dock-monitor` string ("all", "default" or a
 * connector name) into a policy. A connector that does not exist falls back to
 * the default monitor, as documented for a typo'd settings value.
 */
static MonitorSelection resolve_monitor_selection(const char *file_path) {
  MonitorSelection selection = {FALSE, NULL, NULL};

  selection.connector = get_monitor_config_for(file_path);
  selection.all = (g_strcmp0(selection.connector, "all") == 0);

  if (!selection.all && selection.connector && *selection.connector != '\0' &&
      g_strcmp0(selection.connector, "default") != 0) {
    selection.target = find_monitor_by_connector(selection.connector);
    if (!selection.target) {
      LOG_C_WARN("Main",
                 "Monitor connector '%s' not found for [%s]; falling back "
                 "to the default monitor.",
                 selection.connector, file_path);
    }
  }

  return selection;
}

/** Reloads the dock for @monitor/@file_path, or spawns it when missing. */
static void reload_or_spawn_dock(GtkApplication *app, GdkMonitor *monitor,
                                 const char *file_path) {
  DockMonitorSelector selector = {file_path, monitor};
  GPtrArray *existing = docks_matching(dock_is_on_selector_monitor, &selector);

  if (existing->len > 0) {
    LambdockState *state = g_ptr_array_index(existing, 0);
    g_ptr_array_unref(existing);
    reload_dock_instance(state);
    return;
  }
  g_ptr_array_unref(existing);

  if (monitor)
    LOG_C_INFO("Main", "New monitor detected. Spawning dock instance (%s)...",
               file_path);
  else
    LOG_C_INFO("Main", "Spawning single dock instance (%s)...", file_path);

  create_dock_instance(app, monitor, file_path);
}

/** `dock-monitor: all` -- one dock per active monitor. */
static void reconcile_all_monitors(GtkApplication *app, const char *file_path,
                                   GListModel *monitors, guint n_monitors) {
  destroy_docks(docks_matching(dock_is_stranded, (gpointer)file_path));

  for (guint i = 0; i < n_monitors; i++) {
    GdkMonitor *monitor = GDK_MONITOR(g_list_model_get_item(monitors, i));
    if (!monitor)
      continue;

    reload_or_spawn_dock(app, monitor, file_path);
    g_object_unref(monitor);
  }
}

/** A named connector: exactly one dock, on that monitor. */
static void reconcile_named_monitor(GtkApplication *app, const char *file_path,
                                    GdkMonitor *target) {
  DockMonitorSelector selector = {file_path, target};

  destroy_docks(docks_matching(dock_is_not_on_selector_monitor, &selector));
  reload_or_spawn_dock(app, target, file_path);
}

/** No monitor in the settings: a single dock that follows no monitor. */
static void reconcile_default_monitor(GtkApplication *app,
                                      const char *file_path) {
  destroy_docks(
      docks_matching(dock_is_for_config_on_any_monitor, (gpointer)file_path));

  if (docks_matching_count(dock_is_for_config_without_monitor,
                           (gpointer)file_path) == 0)
    reload_or_spawn_dock(app, NULL, file_path);
}

/** Picks the monitor policy for one settings file and applies it. */
static void reconcile_settings_file(GtkApplication *app,
                                    const char *file_path) {
  load_dock_config_scm(file_path);

  SCM start_repl_fn = scm_c_public_ref("lambdock core", "start-repl!");
  if (scm_is_true(start_repl_fn))
    scm_call_1(start_repl_fn, scm_from_utf8_string(file_path));

  MonitorSelection selection = resolve_monitor_selection(file_path);

  GdkDisplay *display = gdk_display_get_default();
  GListModel *monitors = display ? gdk_display_get_monitors(display) : NULL;
  guint n_monitors = monitors ? g_list_model_get_n_items(monitors) : 0;

  if (selection.all && monitors && n_monitors > 0)
    reconcile_all_monitors(app, file_path, monitors, n_monitors);
  else if (selection.target)
    reconcile_named_monitor(app, file_path, selection.target);
  else
    reconcile_default_monitor(app, file_path);

  g_free(selection.connector);
}

/**
 * sync_docks_with_disk:
 * Reconciles active dock windows with the settings files on disk and the active
 * monitor outputs: spawns docks for new settings files, destroys docks whose
 * file or monitor disappeared, and keeps one fallback dock when no settings
 * file exists at all.
 */
static void sync_docks_with_disk(GtkApplication *app) {
  GList *disk_files = scan_settings_files();

  if (disk_files == NULL) {
    destroy_docks(docks_matching(dock_has_config, NULL));

    if (dock_registry_is_empty()) {
      LOG_C_INFO("Main",
                 "No settings files found. Spawning default fallback dock...");
      reload_guile_config();
      create_dock_instance(app, NULL, NULL);
    }
    return;
  }

  LOG_C_INFO("Main", "Settings file(s) detected. Destroying fallback dock.");
  destroy_docks(docks_matching(dock_has_no_config, NULL));
  destroy_docks(docks_matching(dock_config_missing_from, disk_files));

  for (GList *df = disk_files; df != NULL; df = df->next)
    reconcile_settings_file(app, (const char *)df->data);

  g_list_free_full(disk_files, g_free);
}
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
 * Sets up directory-level watcher (`GFileMonitor`) on ~/.config/lambdock/
 * to detect addition, deletion, and modification of configuration files.
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
}

/* =========================================================================
 * MONITOR HOTPLUG LISTENER SETUP
 * ========================================================================= */

/**
 * on_monitor_invalidated:
 * Called synchronously by GDK when a monitor is disconnected.
 * Unbinds gtk-layer-shell from the output immediately to prevent a Wayland
 * protocol abort.
 */
static void on_monitor_invalidated(GdkMonitor *monitor, gpointer user_data) {
  LambdockState *state = (LambdockState *)user_data;
  (void)monitor;
  if (!state || state->is_destroying)
    return;

  LOG_C_INFO("Main",
             "Monitor invalidated for dock instance [%s]. Unbinding layer "
             "shell output...",
             state->config_path ? state->config_path : "default");

  if (state->window && GTK_IS_WINDOW(state->window) &&
      gtk_layer_is_layer_window(GTK_WINDOW(state->window))) {
    // Unbind layer shell surface from the dead output before dispatch cycle
    // fails
    gtk_layer_set_monitor(GTK_WINDOW(state->window), NULL);
  }
}

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

/* =========================================================================
 * GTK APPLICATION ACTIVATION (UI CONSTRUCTION & SETUP)
 * ========================================================================= */

/**
 * create_dock_instance:
 * Constructs and displays a single dock window on a specific monitor output for
 * a config_path.
 * @param app Pointer to active GtkApplication instance.
 * @param monitor GdkMonitor target (or NULL for default monitor).
 * @param config_path Absolute path to the settings file for this dock instance.
 */
/* -------------------------------------------------------------------------
 * DOCK INSTANCE CONSTRUCTION
 * -------------------------------------------------------------------------
 * Creating a dock is a sequence of independent steps: read the settings, build
 * the widget tree, build the menu, install the input, then show it. Each step is
 * its own function so a step can be read (and changed) without holding the whole
 * 170-line construction in your head.
 */

/** Reads everything the dock needs out of the settings file into @state. */
static void dock_state_read_config(LambdockState *state) {
  /* Mode flags are resolved BEFORE the CSS is loaded, so the taskbar modifier
   * stylesheet is applied when taskbar mode or window titles are enabled. */
  state->taskbar = get_taskbar_config_for(state->config_path);
  state->show_window_titles =
      get_show_window_titles_config_for(state->config_path);
  get_shelf_config_for(state->config_path, &state->shelf);

  state->auto_hide = get_autohide_config_for(state->config_path);
  state->track_windows = get_track_windows_config_for(state->config_path);
  state->show_when_clear = get_show_when_clear_config_for(state->config_path);
  state->hover_animate = get_hover_animate_config_for(state->config_path);
  state->reduce_motion = get_reduce_motion_config_for(state->config_path);
  state->icon_size = get_icon_size_config_for(state->config_path);
  state->item_spacing = get_item_spacing_config_for(state->config_path);
  state->capsule_padding = get_capsule_padding_config_for(state->config_path);
}

/** The scrolling content viewport: the item box inside a scroller. */
static GtkWidget *dock_viewport_build(LambdockState *state) {
  GtkWidget *scrolled_window = gtk_scrolled_window_new();
  gtk_widget_set_overflow(scrolled_window, GTK_OVERFLOW_VISIBLE);
  gtk_scrolled_window_set_propagate_natural_width(
      GTK_SCROLLED_WINDOW(scrolled_window), TRUE);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(scrolled_window), TRUE);

  state->box = gtk_box_new(state->orientation, state->item_spacing);
  gtk_widget_set_overflow(state->box, GTK_OVERFLOW_VISIBLE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window),
                                state->box);

  /* The viewport only ever scrolls along the dock's main axis. */
  if (state->orientation == GTK_ORIENTATION_HORIZONTAL)
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  else
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);

  GtkEventController *scroll_ctrl =
      gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect(scroll_ctrl, "scroll", G_CALLBACK(on_dock_scroll), state);
  gtk_widget_add_controller(scrolled_window, scroll_ctrl);

  return scrolled_window;
}

/** The capsule (and its shelf frame) inside the window's overlay. */
static void dock_capsule_build(LambdockState *state) {
  GtkWidget *overlay = gtk_overlay_new();
  gtk_widget_set_overflow(overlay, GTK_OVERFLOW_VISIBLE);
  gtk_window_set_child(GTK_WINDOW(state->window), overlay);
  state->overlay = overlay;

  GtkWidget *bg_capsule = gtk_box_new(state->orientation, 0);
  gtk_widget_add_css_class(bg_capsule, "lambdock-window");
  state->bg_capsule = bg_capsule;

  /* Wrap the styled capsule in the shelf frame. With the default shelf
   * parameters (no tilt) the frame is a pure pass-through. */
  state->bg_shelf = shelf_frame_new(bg_capsule);
  gtk_widget_set_overflow(state->bg_shelf, GTK_OVERFLOW_VISIBLE);

  apply_dock_mode_classes(state);
}

/** Assembles window -> overlay -> [shelf frame( capsule ), scroller]. */
static void dock_widgets_build(LambdockState *state) {
  dock_capsule_build(state);

  GtkWidget *scrolled_window = dock_viewport_build(state);
  state->scrolled_window = scrolled_window;

  update_dock_dimensions(state);

  GtkSizeGroupMode mode = (state->orientation == GTK_ORIENTATION_HORIZONTAL)
                              ? GTK_SIZE_GROUP_HORIZONTAL
                              : GTK_SIZE_GROUP_VERTICAL;
  state->size_group = gtk_size_group_new(mode);
  gtk_size_group_add_widget(state->size_group, state->bg_capsule);
  gtk_size_group_add_widget(state->size_group, scrolled_window);

  gtk_overlay_set_child(GTK_OVERLAY(state->overlay), state->bg_shelf);
  gtk_overlay_add_overlay(GTK_OVERLAY(state->overlay), scrolled_window);

  gtk_widget_set_overflow(state->box, GTK_OVERFLOW_VISIBLE);
  gtk_widget_set_overflow(state->window, GTK_OVERFLOW_VISIBLE);
}

/** The window's own context menu: Preferences (for this dock), About, Quit. */
static void dock_menu_popover_build(LambdockState *state) {
  GMenu *menu_model = g_menu_new();

  /* Preferences carries the dock's settings path as its action target: with
   * several settings files in use, the menu must open the pane for the dock it
   * was right-clicked on, not for an arbitrary one. */
  GMenuItem *prefs_item = g_menu_item_new(_("Preferences…"), NULL);
  g_menu_item_set_action_and_target(prefs_item, "app.preferences", "s",
                                    state->config_path ? state->config_path
                                                       : "");
  g_menu_append_item(menu_model, prefs_item);
  g_object_unref(prefs_item);

  g_menu_append(menu_model, _("About Lambdock"), "app.about");
  g_menu_append(menu_model, _("Quit"), "app.quit");

  GtkWidget *popover =
      gtk_popover_menu_new_from_model(G_MENU_MODEL(menu_model));
  g_object_unref(menu_model);

  gtk_widget_set_parent(popover, state->window);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  lambdock_popover_close_on_escape(popover);

  g_signal_connect(popover, "closed", G_CALLBACK(on_popover_closed), state);
  state->popover = popover;
}

/** Right click, drag and drop, keyboard navigation, motion tracking. */
static void dock_input_install(LambdockState *state) {
  GtkGesture *right_click_gesture = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click_gesture),
                                GDK_BUTTON_SECONDARY);
  g_signal_connect(right_click_gesture, "pressed", G_CALLBACK(on_right_click),
                   state);
  gtk_widget_add_controller(state->window,
                            GTK_EVENT_CONTROLLER(right_click_gesture));

  setup_dock_motion_controller(state);
  dock_dnd_setup(state);

  /* Keyboard access: arrows walk the icons, Menu opens their context menu,
   * Escape closes whatever the dock has open, Ctrl+Arrow rearranges them. */
  lambdock_dock_attach_key_navigation(state->window, state);
}

/** Shows the dock and puts it under window tracking / auto-hide. */
static void dock_present(LambdockState *state) {
  setup_layer_shell_window(state);

  /* A dock that starts with reduced motion must not run a first slide. */
  dock_apply_motion_preference(state);

  dock_registry_add(state);
  g_signal_connect_swapped(state->window, "destroy",
                           G_CALLBACK(destroy_dock_instance), state);

  gtk_window_present(GTK_WINDOW(state->window));

  /* Window tracking is a bonus, not a requirement: report why it is unavailable
   * and carry on with the pinned items. */
  g_autoptr(GError) tracker_error = NULL;
  if (!init_toplevel_tracker(&tracker_error))
    LOG_C_WARN("Tracker", "window tracking unavailable: %s",
               tracker_error->message);

  sync_window_trackers(state);

  if (state->auto_hide && state->show_when_clear &&
      state->window_trackers != NULL)
    hide_dock(state);
  else
    schedule_initial_autohide(state);
}

static void create_dock_instance(GtkApplication *app, GdkMonitor *monitor,
                                 const char *config_path) {
  LambdockState *state = g_new0(LambdockState, 1);

  state->monitor = monitor ? g_object_ref(monitor) : NULL;
  state->config_path = config_path ? g_strdup(config_path) : NULL;

  /* Listen for this monitor disappearing out from under the dock. */
  if (state->monitor) {
    g_signal_connect(state->monitor, "invalidate",
                     G_CALLBACK(on_monitor_invalidated), state);
  }

  state->edge = get_position_config_for(state->config_path);
  state->orientation = (state->edge == GTK_LAYER_SHELL_EDGE_LEFT ||
                        state->edge == GTK_LAYER_SHELL_EDGE_RIGHT)
                           ? GTK_ORIENTATION_VERTICAL
                           : GTK_ORIENTATION_HORIZONTAL;

  dock_state_read_config(state);

  state->window = gtk_application_window_new(app);
  /* A window without a title is announced as an unnamed frame; name the dock so
   * a screen reader says what surface the focus just landed on. */
  gtk_window_set_title(GTK_WINDOW(state->window), _("Lambdock dock"));

  /* The theme stylesheet needs the window's CSS context, hence this order. */
  state->theme = get_theme_config_for(state->config_path);
  load_custom_css(state);

  dock_widgets_build(state);
  populate_dock_items_for(state);
  dock_menu_popover_build(state);
  dock_input_install(state);
  dock_present(state);
}

/**
 * on_system_motion_changed:
 * The desktop's "reduce motion" (GTK's `gtk-enable-animations`) can change
 * while the dock is running; every dock re-reads it so the next slide or hover
 * is immediate instead of animated.
 */
static void apply_motion_visitor(LambdockState *state, gpointer user_data) {
  (void)user_data;
  dock_apply_motion_preference(state);
}

static void on_system_motion_changed(GObject *settings, GParamSpec *pspec,
                                     gpointer user_data) {
  (void)settings;
  (void)pspec;
  (void)user_data;

  dock_registry_foreach(apply_motion_visitor, NULL);
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
