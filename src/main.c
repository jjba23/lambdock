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
#include "accessibility.h"
#include "guile_bridge.h"
#include "lambdock.h"
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
 * g_active_states:
 * A GLib doubly-linked list (`GList*`) storing pointers to `LambdockState`
 * instances.
 */
static GList *g_active_states = NULL;

GList *get_active_dock_states(void) { return g_active_states; }

/**
 * g_reload_timer_id:
 * GLib event source ID for debouncing file modification events.
 * 0 means no reload is currently scheduled.
 */
static guint g_reload_timer_id = 0;

/**
 * unregister_dock_state:
 * Removes an active LambdockState instance from g_active_states.
 */
void unregister_dock_state(LambdockState *state) {
  g_active_states = g_list_remove(g_active_states, state);
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

/**
 * sync_docks_with_disk:
 * Reconciles active dock windows with the current state of settings files on
 * disk and active monitor outputs.
 * - Handles creation of new dock instances when settings-*.scm files are added.
 * - Handles destruction of dock instances when settings-*.scm files are deleted
 * or when monitors are removed.
 * - Spawns default fallback dock if and only if NO settings files exist on
 * disk.
 */
static void sync_docks_with_disk(GtkApplication *app) {
  GList *disk_files = scan_settings_files();

  if (disk_files != NULL) {
    // Collect fallback docks to destroy
    GList *to_destroy = NULL;
    for (GList *curr = g_active_states; curr != NULL; curr = curr->next) {
      LambdockState *state = (LambdockState *)curr->data;
      if (state->config_path == NULL) {
        to_destroy = g_list_append(to_destroy, state);
      }
    }
    for (GList *l = to_destroy; l != NULL; l = l->next) {
      LOG_C_INFO("Main",
                 "Settings file(s) detected. Destroying fallback dock.");
      destroy_dock_instance((LambdockState *)l->data);
    }
    g_list_free(to_destroy);
    to_destroy = NULL;

    // Collect docks whose config files were deleted from disk
    for (GList *curr = g_active_states; curr != NULL; curr = curr->next) {
      LambdockState *state = (LambdockState *)curr->data;
      if (state->config_path != NULL) {
        gboolean found = FALSE;
        for (GList *df = disk_files; df != NULL; df = df->next) {
          if (g_strcmp0((const char *)df->data, state->config_path) == 0) {
            found = TRUE;
            break;
          }
        }
        if (!found) {
          to_destroy = g_list_append(to_destroy, state);
        }
      }
    }
    for (GList *l = to_destroy; l != NULL; l = l->next) {
      LambdockState *state = (LambdockState *)l->data;
      LOG_C_INFO("Main", "Config file deleted: %s. Destroying dock instance...",
                 state->config_path);
      destroy_dock_instance(state);
    }
    g_list_free(to_destroy);

    // Spawn new docks or reload existing docks
    for (GList *df = disk_files; df != NULL; df = df->next) {
      const char *file_path = (const char *)df->data;
      load_dock_config_scm(file_path);

      SCM start_repl_fn = scm_c_public_ref("lambdock core", "start-repl!");
      if (scm_is_true(start_repl_fn)) {
        scm_call_1(start_repl_fn, scm_from_utf8_string(file_path));
      }

      char *monitor_target = get_monitor_config_for(file_path);
      GdkDisplay *display = gdk_display_get_default();
      GListModel *monitors = display ? gdk_display_get_monitors(display) : NULL;
      guint n_monitors = monitors ? g_list_model_get_n_items(monitors) : 0;

      gboolean monitor_all = (g_strcmp0(monitor_target, "all") == 0);
      GdkMonitor *target_monitor = NULL;
      if (!monitor_all && monitor_target && *monitor_target != '\0' &&
          g_strcmp0(monitor_target, "default") != 0) {
        target_monitor = find_monitor_by_connector(monitor_target);
        if (!target_monitor) {
          LOG_C_WARN("Main",
                     "Monitor connector '%s' not found for [%s]; falling back "
                     "to the default monitor.",
                     monitor_target, file_path);
        }
      }

      if (monitor_all && monitors && n_monitors > 0) {
        // 1. Destroy docks for this config path whose monitor was
        // unplugged/invalidated
        to_destroy = NULL;
        for (GList *curr = g_active_states; curr != NULL; curr = curr->next) {
          LambdockState *state = (LambdockState *)curr->data;
          if (state->config_path &&
              g_strcmp0(state->config_path, file_path) == 0) {
            if (!state->monitor || !is_monitor_valid(state->monitor)) {
              to_destroy = g_list_append(to_destroy, state);
            }
          }
        }
        for (GList *l = to_destroy; l != NULL; l = l->next) {
          LambdockState *state = (LambdockState *)l->data;
          LOG_C_INFO("Main",
                     "Monitor removed for dock instance [%s]. Destroying...",
                     file_path);
          destroy_dock_instance(state);
        }
        g_list_free(to_destroy);

        // 2. Ensure each active monitor has a dock instance
        for (guint i = 0; i < n_monitors; i++) {
          GdkMonitor *mon = GDK_MONITOR(g_list_model_get_item(monitors, i));
          gboolean active = FALSE;

          for (GList *l = g_active_states; l != NULL; l = l->next) {
            LambdockState *state = (LambdockState *)l->data;
            if (state->config_path &&
                g_strcmp0(state->config_path, file_path) == 0 &&
                state->monitor == mon) {
              active = TRUE;
              reload_dock_instance(state);
              break;
            }
          }

          if (!active) {
            LOG_C_INFO("Main",
                       "New monitor detected. Spawning dock instance for "
                       "monitor %u (%s)...",
                       i, file_path);
            create_dock_instance(app, mon, file_path);
          }
          g_object_unref(mon);
        }
      } else if (target_monitor != NULL) {
        // Destroy docks for this config that are not bound to the target monitor
        to_destroy = NULL;
        for (GList *curr = g_active_states; curr != NULL; curr = curr->next) {
          LambdockState *state = (LambdockState *)curr->data;
          if (state->config_path &&
              g_strcmp0(state->config_path, file_path) == 0 &&
              state->monitor != target_monitor) {
            to_destroy = g_list_append(to_destroy, state);
          }
        }
        for (GList *l = to_destroy; l != NULL; l = l->next) {
          destroy_dock_instance((LambdockState *)l->data);
        }
        g_list_free(to_destroy);
        to_destroy = NULL;

        // Ensure a single dock is bound to the target monitor
        gboolean active = FALSE;
        for (GList *l = g_active_states; l != NULL; l = l->next) {
          LambdockState *state = (LambdockState *)l->data;
          if (state->config_path &&
              g_strcmp0(state->config_path, file_path) == 0 &&
              state->monitor == target_monitor) {
            active = TRUE;
            reload_dock_instance(state);
            break;
          }
        }

        if (!active) {
          LOG_C_INFO("Main",
                     "Spawning dock instance on monitor connector '%s' (%s)...",
                     monitor_target, file_path);
          create_dock_instance(app, target_monitor, file_path);
        }
      } else {
        // Destroy monitor-specific docks if target is no longer "all"
        to_destroy = NULL;
        for (GList *curr = g_active_states; curr != NULL; curr = curr->next) {
          LambdockState *state = (LambdockState *)curr->data;
          if (state->config_path &&
              g_strcmp0(state->config_path, file_path) == 0) {
            if (state->monitor != NULL) {
              to_destroy = g_list_append(to_destroy, state);
            }
          }
        }
        for (GList *l = to_destroy; l != NULL; l = l->next) {
          destroy_dock_instance((LambdockState *)l->data);
        }
        g_list_free(to_destroy);

        // Single default dock instance
        gboolean active = FALSE;
        for (GList *l = g_active_states; l != NULL; l = l->next) {
          LambdockState *state = (LambdockState *)l->data;
          if (state->config_path &&
              g_strcmp0(state->config_path, file_path) == 0 &&
              state->monitor == NULL) {
            active = TRUE;
            reload_dock_instance(state);
            break;
          }
        }

        if (!active) {
          LOG_C_INFO("Main", "Spawning single dock instance (%s)...",
                     file_path);
          create_dock_instance(app, NULL, file_path);
        }
      }
      if (target_monitor)
        g_object_unref(target_monitor);
      free(monitor_target); /* get_monitor_config_for may return scm_to_utf8_string() */
    }
  } else {
    // No settings files exist on disk: destroy all file-backed docks
    GList *to_destroy = NULL;
    for (GList *curr = g_active_states; curr != NULL; curr = curr->next) {
      LambdockState *state = (LambdockState *)curr->data;
      if (state->config_path != NULL) {
        to_destroy = g_list_append(to_destroy, state);
      }
    }
    for (GList *l = to_destroy; l != NULL; l = l->next) {
      destroy_dock_instance((LambdockState *)l->data);
    }
    g_list_free(to_destroy);

    if (g_active_states == NULL) {
      LOG_C_INFO("Main",
                 "No settings files found. Spawning default fallback dock...");
      reload_guile_config();
      create_dock_instance(app, NULL, NULL);
    }
  }

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
static void redraw_active_docks(void) {
  for (GList *l = get_active_dock_states(); l != NULL; l = l->next) {
    LambdockState *state = (LambdockState *)l->data;
    if (state) {
      reload_dock_instance(state);
    }
  }
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
  scm_c_export("reload-dock!", NULL);

  scm_set_current_module(previous);
}

/**
 * on_config_reload_timeout:
 * Debounced callback executed after file changes cease in ~/.config/lambdock.
 */
static gboolean on_config_reload_timeout(gpointer user_data) {
  GtkApplication *app = GTK_APPLICATION(user_data);
  sync_docks_with_disk(app);
  g_reload_timer_id = 0;
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

  if (g_reload_timer_id > 0) {
    g_source_remove(g_reload_timer_id);
  }

  g_reload_timer_id = g_timeout_add(CONFIG_RELOAD_DEBOUNCE_MS,
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

  if (state->window && GTK_IS_WINDOW(state->window)) {
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
static gboolean g_monitor_sync_pending = FALSE;

static gboolean on_monitors_changed_idle(gpointer user_data) {
  g_monitor_sync_pending = FALSE;
  GtkApplication *app = GTK_APPLICATION(user_data);
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

  if (!g_monitor_sync_pending) {
    g_monitor_sync_pending = TRUE;
    g_idle_add(on_monitors_changed_idle, user_data);
  }
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
static void create_dock_instance(GtkApplication *app, GdkMonitor *monitor,
                                 const char *config_path) {
  LambdockState *state = g_new0(LambdockState, 1);
  state->monitor = monitor ? g_object_ref(monitor) : NULL;
  state->config_path = config_path ? g_strdup(config_path) : NULL;

  // Listen for monitor disconnects immediately
  if (state->monitor) {
    g_signal_connect(state->monitor, "invalidate",
                     G_CALLBACK(on_monitor_invalidated), state);
  }

  state->edge = get_position_config_for(state->config_path);

  state->orientation = (state->edge == GTK_LAYER_SHELL_EDGE_LEFT ||
                        state->edge == GTK_LAYER_SHELL_EDGE_RIGHT)
                           ? GTK_ORIENTATION_VERTICAL
                           : GTK_ORIENTATION_HORIZONTAL;

  GtkWidget *window = gtk_application_window_new(app);
  state->window = window;
  /* A window without a title is announced as an unnamed frame; name the dock so
   * a screen reader says what surface the focus just landed on. */
  gtk_window_set_title(GTK_WINDOW(window), _("Lambdock dock"));

  /* Resolve mode flags BEFORE loading CSS so the taskbar modifier stylesheet is
   * applied when taskbar mode or window titles are enabled. */
  state->taskbar = get_taskbar_config_for(state->config_path);
  state->show_window_titles =
      get_show_window_titles_config_for(state->config_path);
  get_shelf_config_for(state->config_path, &state->shelf);

  state->auto_hide = get_autohide_config_for(state->config_path);
  state->track_windows = get_track_windows_config_for(state->config_path);
  state->show_when_clear = get_show_when_clear_config_for(state->config_path);
  state->hover_animate = get_hover_animate_config_for(state->config_path);
  state->reduce_motion =
      get_reduce_motion_config_for(state->config_path);
  state->icon_size = get_icon_size_config_for(state->config_path);
  state->item_spacing = get_item_spacing_config_for(state->config_path);
  state->capsule_padding =
      get_capsule_padding_config_for(state->config_path);

  /* Load CSS theme AFTER window context is created */
  state->theme = get_theme_config_for(state->config_path);
  load_custom_css(state);

  GtkWidget *overlay = gtk_overlay_new();
  gtk_widget_set_overflow(overlay, GTK_OVERFLOW_VISIBLE);
  gtk_window_set_child(GTK_WINDOW(window), overlay);
  state->overlay = overlay;

  GtkWidget *bg_capsule = gtk_box_new(state->orientation, 0);
  gtk_widget_add_css_class(bg_capsule, "lambdock-window");
  state->bg_capsule = bg_capsule;

  /* Wrap the styled capsule in the shelf frame. With the default shelf
   * parameters (no tilt) the frame is a pure pass-through, so the dock looks
   * identical to a plain capsule. */
  GtkWidget *bg_shelf = shelf_frame_new(bg_capsule);
  state->bg_shelf = bg_shelf;
  gtk_widget_set_overflow(bg_shelf, GTK_OVERFLOW_VISIBLE);

  apply_dock_mode_classes(state);

  GtkWidget *box = gtk_box_new(state->orientation, state->item_spacing);
  gtk_widget_set_overflow(box, GTK_OVERFLOW_VISIBLE);
  state->box = box;

  GtkWidget *scrolled_window = gtk_scrolled_window_new();
  state->scrolled_window = scrolled_window;
  gtk_widget_set_overflow(scrolled_window, GTK_OVERFLOW_VISIBLE);

  gtk_scrolled_window_set_propagate_natural_width(
      GTK_SCROLLED_WINDOW(scrolled_window), TRUE);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(scrolled_window), TRUE);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), box);

  if (state->orientation == GTK_ORIENTATION_HORIZONTAL) {
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  } else {
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
  }

  GtkEventController *scroll_ctrl =
      gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect(scroll_ctrl, "scroll", G_CALLBACK(on_dock_scroll), state);
  gtk_widget_add_controller(scrolled_window, scroll_ctrl);

  update_dock_dimensions(state);

  GtkSizeGroupMode mode = (state->orientation == GTK_ORIENTATION_HORIZONTAL)
                              ? GTK_SIZE_GROUP_HORIZONTAL
                              : GTK_SIZE_GROUP_VERTICAL;
  state->size_group = gtk_size_group_new(mode);
  gtk_size_group_add_widget(state->size_group, bg_capsule);
  gtk_size_group_add_widget(state->size_group, scrolled_window);

  gtk_overlay_set_child(GTK_OVERLAY(overlay), bg_shelf);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), scrolled_window);

  gtk_widget_set_overflow(box, GTK_OVERFLOW_VISIBLE);
  gtk_widget_set_overflow(window, GTK_OVERFLOW_VISIBLE);

  populate_dock_items_for(state);

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

  gtk_widget_set_parent(popover, window);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  lambdock_popover_close_on_escape(popover);
  state->popover = popover;

  g_signal_connect(popover, "closed", G_CALLBACK(on_popover_closed), state);

  GtkGesture *right_click_gesture = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click_gesture),
                                GDK_BUTTON_SECONDARY);
  g_signal_connect(right_click_gesture, "pressed", G_CALLBACK(on_right_click),
                   state);
  gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(right_click_gesture));

  setup_layer_shell_window(state);
  setup_dock_motion_controller(state);
  dock_dnd_setup(state);

  /* Keyboard access: arrows walk the icons, Menu opens their context menu,
   * Escape closes whatever the dock has open, Ctrl+Arrow rearranges them. */
  lambdock_dock_attach_key_navigation(window, state);

  /* A dock that starts with reduced motion must not run a first slide. */
  dock_apply_motion_preference(state);

  g_active_states = g_list_append(g_active_states, state);

  g_signal_connect_swapped(window, "destroy", G_CALLBACK(destroy_dock_instance),
                           state);

  gtk_window_present(GTK_WINDOW(window));

  init_toplevel_tracker(state);
  sync_window_trackers(state);

  if (state->auto_hide && state->show_when_clear &&
      state->window_trackers != NULL) {
    hide_dock(state);
  } else {
    schedule_initial_autohide(state);
  }
}

/**
 * on_system_motion_changed:
 * The desktop's "reduce motion" (GTK's `gtk-enable-animations`) can change
 * while the dock is running; every dock re-reads it so the next slide or hover
 * is immediate instead of animated.
 */
static void on_system_motion_changed(GObject *settings, GParamSpec *pspec,
                                     gpointer user_data) {
  (void)settings;
  (void)pspec;
  (void)user_data;

  for (GList *l = g_active_states; l != NULL; l = l->next)
    dock_apply_motion_preference((LambdockState *)l->data);
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
