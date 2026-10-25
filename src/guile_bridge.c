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

#include "guile_bridge.h" // Header file defining function declarations and LambdockState
#include "logger.h"       // Custom logging macros (e.g., LOG_C_INFO)
#include "ui/dock_item.h" // Functions to create visual dock items and separators

/**
 * setup_guile_load_path:
 * Configures GNU Guile's internal directory search path (`%load-path`).
 *
 * Tells the Guile interpreter where to look on disk when a Scheme script
 * requests to load a module (for example, `(use-modules (lambdock core))`).
 */
void setup_guile_load_path(void) {
  // Tell Guile to search the local "scheme" folder relative to the execution
  // path
  scm_c_eval_string("(set! %load-path (cons \"scheme\" %load-path))");

#ifdef LAMBDOCK_SCHEME_DIR
  // If a system installation path was defined at compile-time (e.g.,
  // /usr/share/lambdock), add that directory to Guile's search path as well.
  scm_c_eval_string("(set! %load-path (cons \"" LAMBDOCK_SCHEME_DIR
                    "\" %load-path))");
#endif
}

/* =================================================================================
 * INTERNAL HELPER FUNCTIONS
 * =================================================================================
 * These static helpers simplify getting data out of Guile without duplicating
 * conversion code everywhere.
 * =================================================================================
 */

/**
 * call_core_proc_with_path:
 * Executes a Scheme procedure defined in `(lambdock core)` with an optional
 * config path argument.
 */
static SCM call_core_proc_with_path(const char *func_name, const char *path) {
  SCM fn = scm_c_public_ref("lambdock core", func_name);
  if (!scm_is_true(fn))
    return SCM_BOOL_F;

  if (path && *path != '\0') {
    SCM scm_path = scm_from_utf8_string(path);
    return scm_call_1(fn, scm_path);
  } else {
    return scm_call_0(fn);
  }
}

/**
 * get_guile_string_for:
 * Calls a Scheme function that returns a string or symbol for a given config
 * path, and converts it into a C string.
 */
static char *get_guile_string_for(const char *func_name,
                                  const char *fallback_module,
                                  const char *config_path,
                                  const char *default_val) {
  SCM fn = fallback_module ? scm_c_public_ref(fallback_module, func_name)
                           : scm_c_public_ref("lambdock core", func_name);
  if (!scm_is_true(fn))
    return g_strdup(default_val);

  SCM res = (config_path && *config_path != '\0')
                ? scm_call_1(fn, scm_from_utf8_string(config_path))
                : scm_call_0(fn);

  if (!scm_is_true(res))
    return g_strdup(default_val);

  SCM str_scm = scm_is_symbol(res) ? scm_symbol_to_string(res) : res;

  if (scm_is_string(str_scm)) {
    char *str = scm_to_utf8_string(str_scm);
    if (str && *str != '\0')
      return str;
    free(str);
  }

  return g_strdup(default_val);
}

/**
 * get_guile_bool_for:
 * Calls a Scheme predicate function (ending in `?`) for a path and returns a C
 * boolean.
 */
static gboolean get_guile_bool_for(const char *func_name, const char *path) {
  SCM res = call_core_proc_with_path(func_name, path);
  return scm_is_true(res) ? scm_to_bool(res) : FALSE;
}

/**
 * get_guile_number_for:
 * Calls a Scheme function returning a number for a path. Anything that is not
 * a real number yields @fallback, so a broken/old Scheme side can never push
 * garbage into the dock geometry.
 */
static double get_guile_number_for(const char *func_name, const char *path,
                                   double fallback) {
  SCM fn = scm_c_public_ref("lambdock core", func_name);
  if (!scm_is_true(fn))
    return fallback;

  SCM res = (path && *path != '\0')
                ? scm_call_1(fn, scm_from_utf8_string(path))
                : scm_call_0(fn);

  if (scm_is_real(res) && !scm_is_true(scm_nan_p(res)))
    return scm_to_double(res);

  return fallback;
}

/* --- Clamping helpers ------------------------------------------------------ */

static double clamp_double(double value, double low, double high) {
  if (value < low)
    return low;
  if (value > high)
    return high;
  return value;
}

static int clamp_int(double value, int low, int high) {
  int v = (int)round(value);
  if (v < low)
    return low;
  if (v > high)
    return high;
  return v;
}

/* =================================================================================
 * PUBLIC BRIDGE API
 * =================================================================================
 */

/** Checks if dock auto-hiding is enabled for a specific config path. */
gboolean get_autohide_config_for(const char *config_path) {
  return get_guile_bool_for("get-dock-auto-hide?", config_path);
}

/** Checks if the dock should remain visible when clear for a specific config
 * path. */
gboolean get_show_when_clear_config_for(const char *config_path) {
  return get_guile_bool_for("get-dock-show-when-clear?", config_path);
}

/** Checks if hover animation is enabled for a specific config path. */
gboolean get_hover_animate_config_for(const char *config_path) {
  return get_guile_bool_for("get-dock-hover-animate?", config_path);
}

/** Reads icon size for a specific config path. */
int get_icon_size_config_for(const char *config_path) {
  SCM res = call_core_proc_with_path("get-dock-icon-size", config_path);
  if (scm_is_true(res) && (scm_is_integer(res) || scm_is_number(res))) {
    int sz = scm_to_int(res);
    if (sz > 0)
      return sz;
  }
  return DEFAULT_ICON_SIZE;
}

/**
 * get_item_spacing_config_for:
 * Reads `dock-item-spacing` (px between dock items) for a config path,
 * clamped so a typo cannot collapse or explode the dock.
 */
int get_item_spacing_config_for(const char *config_path) {
  SCM res = call_core_proc_with_path("get-dock-item-spacing", config_path);
  if (scm_is_true(res) && (scm_is_integer(res) || scm_is_number(res))) {
    int spacing = scm_to_int(res);
    if (spacing < 0)
      return 0;
    if (spacing > 64)
      return 64;
    return spacing;
  }
  return DOCK_ITEM_SPACING_PX;
}

/** Checks if Guile REPL is enabled for a specific config path. */
gboolean get_repl_config_for(const char *config_path) {
  return get_guile_bool_for("get-dock-enable-repl?", config_path);
}

/** Retrieves active CSS theme name for a specific config path. */
char *get_theme_config_for(const char *config_path) {
  return get_guile_string_for("get-dock-theme", NULL, config_path, "vanilla");
}

gboolean get_track_windows_config_for(const char *config_path) {
  return get_guile_bool_for("get-dock-track-windows?", config_path);
}

/** Returns TRUE when the dock should render as a full-width taskbar. */
gboolean get_taskbar_config_for(const char *config_path) {
  char *mode =
      get_guile_string_for("get-dock-mode", NULL, config_path, "dock");
  gboolean taskbar = (g_ascii_strcasecmp(mode, "taskbar") == 0);
  free(mode);
  return taskbar;
}

/** Returns TRUE when running app items should show their window title. */
gboolean get_show_window_titles_config_for(const char *config_path) {
  return get_guile_bool_for("get-dock-show-window-titles?", config_path);
}

/**
 * get_shelf_config_for:
 * Resolves every shelf/reflection parameter for a dock instance.
 *
 * The Scheme side already merges "explicit setting > special-mode preset >
 * default"; this function only clamps the results so that hand-written or
 * hostile values (negative padding, absurd tilt, NaN...) can never break the
 * layout maths.
 */
void get_shelf_config_for(const char *config_path, LambdockShelfParams *out) {
  if (!out)
    return;

  LambdockShelfParams defaults = lambdock_shelf_params_default();
  *out = defaults;

  out->tilt_deg = clamp_double(
      get_guile_number_for("get-dock-shelf-tilt", config_path,
                           defaults.tilt_deg),
      -SHELF_TILT_MAX_DEG, SHELF_TILT_MAX_DEG);

  double perspective = get_guile_number_for("get-dock-shelf-perspective",
                                            config_path, defaults.perspective);
  out->perspective =
      clamp_double(perspective, SHELF_PERSPECTIVE_MIN, SHELF_PERSPECTIVE_MAX);

  /* The margin may be negative: that is how the icons overhang the shelf. */
  out->margin_px = clamp_int(
      get_guile_number_for("get-dock-shelf-margin", config_path,
                           (double)defaults.margin_px),
      -SHELF_EXTRA_MAX_PX, SHELF_EXTRA_MAX_PX);

  out->front_px = clamp_int(
      get_guile_number_for("get-dock-shelf-front", config_path,
                           (double)defaults.front_px),
      0, SHELF_EXTRA_MAX_PX);

  out->reflection_ratio = clamp_double(
      get_guile_number_for("get-dock-reflection", config_path,
                           defaults.reflection_ratio),
      0.0, SHELF_REFLECTION_RATIO_MAX);

  out->reflection_intensity = clamp_double(
      get_guile_number_for("get-dock-reflection-intensity", config_path,
                           defaults.reflection_intensity),
      0.0, 1.0);

  out->reflection_blur_ratio = clamp_double(
      get_guile_number_for("get-dock-reflection-blur", config_path,
                           defaults.reflection_blur_ratio),
      0.0, SHELF_REFLECTION_BLUR_MAX);
}

/** Retrieves target monitor screen configuration string for a specific config
 * path. */
char *get_monitor_config_for(const char *config_path) {
  return get_guile_string_for("get-dock-monitor", NULL, config_path, "default");
}

/** Retrieves default config path. */
char *get_config_path_c(void) {
  return get_guile_string_for("get-config-path", "lambdock loader", NULL, NULL);
}

/** Reads screen placement edge enum for a specific config path. */
GtkLayerShellEdge get_position_config_for(const char *config_path) {
  char *pos_str =
      get_guile_string_for("get-dock-position", NULL, config_path, "bottom");
  GtkLayerShellEdge edge = GTK_LAYER_SHELL_EDGE_BOTTOM;

  if (g_strcmp0(pos_str, "top") == 0)
    edge = GTK_LAYER_SHELL_EDGE_TOP;
  else if (g_strcmp0(pos_str, "left") == 0)
    edge = GTK_LAYER_SHELL_EDGE_LEFT;
  else if (g_strcmp0(pos_str, "right") == 0)
    edge = GTK_LAYER_SHELL_EDGE_RIGHT;

  free(pos_str);
  return edge;
}

/**
 * load_dock_config_scm:
 * Invokes Scheme procedure `(load-dock-config! file_path)` to load a specific
 * file.
 */
void load_dock_config_scm(const char *file_path) {
  if (!file_path)
    return;
  SCM load_fn = scm_c_public_ref("lambdock core", "load-dock-config!");
  if (scm_is_true(load_fn)) {
    scm_call_1(load_fn, scm_from_utf8_string(file_path));
  }
}

/** Re-evaluates default Scheme configuration. */
void reload_guile_config(void) {
  LOG_C_INFO("GuileBridge", "Re-evaluating Scheme configuration file...");
  SCM init_fn = scm_c_public_ref("lambdock core", "init-dock!");
  if (scm_is_true(init_fn))
    scm_call_0(init_fn);
}

/** Checks app exclusion list for a specific configuration path context. */
gboolean is_app_excluded(const char *app_id, const char *config_path) {
  if (!app_id || *app_id == '\0')
    return FALSE;

  SCM list = call_core_proc_with_path("get-dock-exclude-apps", config_path);
  while (scm_is_pair(list)) {
    SCM elem = scm_car(list);
    SCM str_scm = scm_is_symbol(elem) ? scm_symbol_to_string(elem) : elem;

    if (scm_is_string(str_scm)) {
      char *str = scm_to_utf8_string(str_scm);
      gboolean match = (g_ascii_strcasecmp(str, app_id) == 0);
      free(str);

      if (match)
        return TRUE;
    }
    list = scm_cdr(list);
  }
  return FALSE;
}

/* Helper function to safely convert Guile SCM string/symbol/#f to C char* */
static char *scm_to_c_string_or_null(SCM val) {
  if (!scm_is_true(val))
    return NULL;
  SCM str_scm = scm_is_symbol(val) ? scm_symbol_to_string(val) : val;
  if (scm_is_string(str_scm)) {
    char *str = scm_to_utf8_string(str_scm);
    if (str && *str != '\0')
      return str;
    free(str);
  }
  return NULL;
}

/** Reads launcher items for state->config_path and populates dock. */
void populate_dock_items_for(LambdockState *state) {
  if (!state || !state->box)
    return;

  SCM items_list =
      call_core_proc_with_path("get-dock-items", state->config_path);
  SCM get_type_fn = scm_c_public_ref("lambdock core", "dock-item-type");
  SCM get_name_fn = scm_c_public_ref("lambdock core", "dock-item-name");
  SCM get_icon_fn = scm_c_public_ref("lambdock core", "dock-item-icon");
  SCM get_exec_fn = scm_c_public_ref("lambdock core", "dock-item-exec");
  SCM get_poll_fn = scm_c_public_ref("lambdock core", "dock-item-poll-fn");
  SCM get_poll_interval_fn =
      scm_c_public_ref("lambdock core", "dock-item-poll-interval");
  SCM get_hover_animate_fn =
      scm_c_public_ref("lambdock core", "dock-item-hover-animate?");
  SCM get_path_fn = scm_c_public_ref("lambdock core", "dock-item-path");
  SCM get_view_fn = scm_c_public_ref("lambdock core", "dock-item-view-mode");

  GtkOrientation sep_orient = (state->orientation == GTK_ORIENTATION_HORIZONTAL)
                                  ? GTK_ORIENTATION_VERTICAL
                                  : GTK_ORIENTATION_HORIZONTAL;

  while (scm_is_pair(items_list)) {
    SCM item = scm_car(items_list);

    char *type_str =
        scm_to_utf8_string(scm_symbol_to_string(scm_call_1(get_type_fn, item)));
    gboolean hover_anim = scm_is_true(scm_call_1(get_hover_animate_fn, item));

    if (g_strcmp0(type_str, "app") == 0) {
      char *name = scm_to_c_string_or_null(scm_call_1(get_name_fn, item));
      char *icon = scm_to_c_string_or_null(scm_call_1(get_icon_fn, item));
      char *exec = scm_to_c_string_or_null(scm_call_1(get_exec_fn, item));

      if (!is_app_excluded(name, state->config_path) &&
          !is_app_excluded(icon, state->config_path) &&
          !is_app_excluded(exec, state->config_path)) {
        create_dock_item(state, name, icon, exec, name, TRUE, hover_anim);
        LOG_C_INFO("GuileBridge", "Created favorite item: %s (%s)",
                   name ? name : "unnamed", exec ? exec : "no-exec");
      } else {
        LOG_C_INFO("GuileBridge", "Skipping excluded launcher item: %s",
                   name ? name : "");
      }

      free(name);
      free(icon);
      free(exec);
    } else if (g_strcmp0(type_str, "dynamic") == 0) {
      char *name = scm_to_c_string_or_null(scm_call_1(get_name_fn, item));
      char *icon = scm_to_c_string_or_null(scm_call_1(get_icon_fn, item));
      char *exec = scm_to_c_string_or_null(scm_call_1(get_exec_fn, item));
      SCM poll_fn = scm_call_1(get_poll_fn, item);
      SCM interval_scm = scm_call_1(get_poll_interval_fn, item);
      guint interval_ms =
          (scm_is_integer(interval_scm) || scm_is_number(interval_scm))
              ? scm_to_uint(interval_scm)
              : 1000;

      if (!is_app_excluded(name, state->config_path) &&
          !is_app_excluded(exec, state->config_path)) {
        create_dynamic_dock_item(state, name, icon, exec, poll_fn, interval_ms,
                                 hover_anim);
        LOG_C_INFO(
            "GuileBridge", "Created dynamic item: %s (%s, poll interval: %dms)",
            name ? name : "unnamed", exec ? exec : "no-exec", interval_ms);
      }

      free(name);
      free(icon);
      free(exec);
    } else if (g_strcmp0(type_str, "separator") == 0) {
      create_separator_dock_item(state, sep_orient);
      LOG_C_INFO("GuileBridge", "Created layout separator");
    } else if (g_strcmp0(type_str, "stack") == 0) {
      char *name = scm_to_c_string_or_null(scm_call_1(get_name_fn, item));
      char *icon = scm_to_c_string_or_null(scm_call_1(get_icon_fn, item));
      char *path = scm_to_c_string_or_null(scm_call_1(get_path_fn, item));
      char *view = scm_to_c_string_or_null(scm_call_1(get_view_fn, item));

      create_stack_dock_item(state, name, icon, path, view, hover_anim);

      free(name);
      free(icon);
      free(path);
      free(view);
    } else if (g_strcmp0(type_str, "launchpad") == 0) {
      char *name = scm_to_c_string_or_null(scm_call_1(get_name_fn, item));
      char *icon = scm_to_c_string_or_null(scm_call_1(get_icon_fn, item));

      create_launchpad_dock_item(state, name, icon, hover_anim);

      free(name);
      free(icon);
    }

    free(type_str);
    items_list = scm_cdr(items_list);
  }
}

/* =================================================================================
 * DOCK LAYOUT OVERRIDES (drag and drop persistence)
 * =================================================================================
 * Drag and drop edits the *running* dock; these three calls hand the edit to
 * `(lambdock layout)`, which records it in the override file and replays it over
 * the configured items on the next load.
 * =================================================================================
 */

static SCM scm_from_optional_string(const char *value) {
  return (value && *value != '\0') ? scm_from_utf8_string(value)
                                   : scm_from_utf8_string("");
}

void dock_layout_add_item(const char *config_path, const char *kind,
                          const char *name, const char *icon, const char *arg,
                          const char *view) {
  if (!config_path || *config_path == '\0')
    return;

  SCM fn = scm_c_public_ref("lambdock core", "dock-layout-add!");
  if (!scm_is_true(fn))
    return;

  SCM args =
      scm_cons(scm_from_utf8_string(config_path),
               scm_list_5(scm_from_optional_string(kind),
                          scm_from_optional_string(name),
                          scm_from_optional_string(icon),
                          scm_from_optional_string(arg),
                          scm_from_optional_string(view)));

  scm_apply_0(fn, args);
}

void dock_layout_remove_item(const char *config_path, const char *key) {
  if (!config_path || *config_path == '\0' || !key)
    return;

  SCM fn = scm_c_public_ref("lambdock core", "dock-layout-remove!");
  if (!scm_is_true(fn))
    return;

  scm_call_2(fn, scm_from_utf8_string(config_path),
             scm_from_utf8_string(key));
}

void dock_layout_set_order(const char *config_path, GList *keys) {
  if (!config_path || *config_path == '\0')
    return;

  SCM fn = scm_c_public_ref("lambdock core", "dock-layout-set-order!");
  if (!scm_is_true(fn))
    return;

  SCM list = SCM_EOL;
  for (GList *l = keys; l != NULL; l = l->next) {
    if (l->data)
      list = scm_cons(scm_from_utf8_string((const char *)l->data), list);
  }
  list = scm_reverse_x(list, SCM_EOL);

  scm_call_2(fn, scm_from_utf8_string(config_path), list);
}

void dock_layout_set_view(const char *config_path, const char *key,
                          const char *view) {
  if (!config_path || *config_path == '\0' || !key)
    return;

  SCM fn = scm_c_public_ref("lambdock core", "dock-layout-set-view!");
  if (!scm_is_true(fn))
    return;

  SCM view_symbol = scm_string_to_symbol(
      scm_from_utf8_string((view && *view != '\0') ? view : "grid"));

  scm_call_3(fn, scm_from_utf8_string(config_path),
             scm_from_utf8_string(key), view_symbol);
}
