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
 * call_core_proc:
 * Executes a 0-argument Scheme procedure defined in the `(lambdock core)`
 * module.
 *
 * @param func_name The name of the Scheme function to call (e.g.,
 * "get-dock-auto-hide?").
 * @return SCM object containing the return value from Scheme, or SCM_BOOL_F
 * (#f) if missing.
 */
static SCM call_core_proc(const char *func_name) {
  // Look up the public procedure inside the Scheme module "(lambdock core)"
  SCM fn = scm_c_public_ref("lambdock core", func_name);

  // If the function exists, call it with zero arguments `scm_call_0()`;
  // otherwise return False
  return scm_is_true(fn) ? scm_call_0(fn) : SCM_BOOL_F;
}

/**
 * get_guile_string:
 * Calls a Scheme function that returns a string or symbol, and converts it into
 * a C string.
 *
 * @param func_name        The Scheme procedure to call.
 * @param fallback_module Optional alternate module name (defaults to "lambdock
 * core" if NULL).
 * @param default_val      Fallback string returned if the Scheme lookup fails
 * or returns empty.
 * @return Dynamically allocated C string pointer (must be freed by caller using
 * g_free / free).
 */
static char *get_guile_string(const char *func_name,
                              const char *fallback_module,
                              const char *default_val) {
  // Resolve procedure reference from target module
  SCM fn = fallback_module ? scm_c_public_ref(fallback_module, func_name)
                           : scm_c_public_ref("lambdock core", func_name);
  if (!scm_is_true(fn))
    return g_strdup(
        default_val); // Return heap copy of default if function doesn't exist

  // Execute procedure
  SCM res = scm_call_0(fn);
  if (!scm_is_true(res))
    return g_strdup(default_val);

  // Scheme symbols (e.g., 'bottom) and strings (e.g., "bottom") are
  // converted to text
  SCM str_scm = scm_is_symbol(res) ? scm_symbol_to_string(res) : res;

  // Convert Guile SCM string into a UTF-8 C string buffer
  if (scm_is_string(str_scm)) {
    char *str = scm_to_utf8_string(str_scm);
    if (str && *str != '\0')
      return str; // Return valid non-empty string
    free(str);    // Clean up empty string allocation before falling back
  }

  return g_strdup(default_val);
}

/**
 * get_guile_bool:
 * Calls a Scheme predicate function (ending in `?`) and returns a C boolean.
 *
 * @param func_name Name of procedure returning Scheme `#t` (true) or `#f`
 * (false).
 * @return C boolean: TRUE or FALSE.
 */
static gboolean get_guile_bool(const char *func_name) {
  SCM res = call_core_proc(func_name);
  // `scm_is_true()` checks if value is not Scheme `#f`; `scm_to_bool()`
  // converts it to C
  return scm_is_true(res) ? scm_to_bool(res) : FALSE;
}

/* =================================================================================
 * PUBLIC BRIDGE API
 * =================================================================================
 * Functions exposed to the rest of the C application to inspect Scheme
 * settings.
 * =================================================================================
 */

/** Checks if dock auto-hiding is enabled in Scheme configuration. */
gboolean get_autohide_config(void) {
  return get_guile_bool("get-dock-auto-hide?");
}

/** Checks if the dock should remain visible when no application windows are
 * open. */
gboolean get_show_when_clear_config(void) {
  return get_guile_bool("get-dock-show-when-clear?");
}

/** Checks if macOS-style hover magnification animation is enabled. */
gboolean get_hover_animate_config(void) {
  return get_guile_bool("get-dock-hover-animate?");
}

/**
 * get_icon_size_config:
 * Reads the configured base icon pixel size from Scheme (`get-dock-icon-size`).
 *
 * @return Integer pixel dimension (e.g., 48). Falls back to 48 if invalid or <=
 * 0.
 */
int get_icon_size_config(void) {
  SCM res = call_core_proc("get-dock-icon-size");

  // Verify that the return value is valid and is a numeric type
  if (scm_is_true(res) && (scm_is_integer(res) || scm_is_number(res))) {
    int sz = scm_to_int(res);
    if (sz > 0)
      return sz; // Valid positive icon dimension
  }
  return DEFAULT_ICON_SIZE;
}

/** Checks if the live Guile REPL (interactive Unix socket console) is enabled.
 */
gboolean get_repl_config(void) {
  return get_guile_bool("get-dock-enable-repl?");
}

/** Retrieves active CSS theme name string (e.g., "vanilla"). */
char *get_theme_config(void) {
  return get_guile_string("get-dock-theme", NULL, "vanilla");
}

/** Retrieves target monitor screen configuration string (e.g., "default",
 * "all", "DP-1"). */
char *get_monitor_config(void) {
  return get_guile_string("get-dock-monitor", NULL, "default");
}

/** Retrieves the absolute file path to the user's `settings.scm` configuration
 * file. */
char *get_config_path_c(void) {
  return get_guile_string("get-config-path", "lambdock loader", NULL);
}

/**
 * get_position_config:
 * Reads screen placement string/symbol ('top, 'bottom, 'left, 'right) from
 * Scheme and converts it into GTK Layer Shell's `GtkLayerShellEdge` C enum
 * type.
 *
 * @return GtkLayerShellEdge enum indicating screen attachment border.
 */
GtkLayerShellEdge get_position_config(void) {
  char *pos_str = get_guile_string("get-dock-position", NULL, "bottom");
  GtkLayerShellEdge edge = GTK_LAYER_SHELL_EDGE_BOTTOM; // Default edge

  if (g_strcmp0(pos_str, "top") == 0)
    edge = GTK_LAYER_SHELL_EDGE_TOP;
  else if (g_strcmp0(pos_str, "left") == 0)
    edge = GTK_LAYER_SHELL_EDGE_LEFT;
  else if (g_strcmp0(pos_str, "right") == 0)
    edge = GTK_LAYER_SHELL_EDGE_RIGHT;

  free(pos_str); // Free temporary allocated string buffer
  return edge;
}

/**
 * reload_guile_config:
 * Re-evaluates the Scheme settings file on the fly by invoking `(init-dock!)`.
 * Used during dynamic hot-reloading without restarting the application process.
 */
void reload_guile_config(void) {
  LOG_C_INFO("GuileBridge", "Re-evaluating Scheme configuration file...");
  SCM init_fn = scm_c_public_ref("lambdock core", "init-dock!");
  if (scm_is_true(init_fn))
    scm_call_0(init_fn);
}

/**
 * is_app_excluded:
 * Checks if a given application identifier or executable command is listed in
 * the user's Scheme exclusion list (`get-dock-exclude-apps`).
 *
 * @param app_id Application identifier string (e.g., "org.gnome.Terminal").
 * @return TRUE if app is in exclusion list; FALSE otherwise.
 */
gboolean is_app_excluded(const char *app_id) {
  if (!app_id || *app_id == '\0')
    return FALSE;

  // Retrieve Scheme list of excluded app strings/symbols
  SCM list = call_core_proc("get-dock-exclude-apps");

  // Traverse the Scheme linked list loop until empty (`pair?` checks for list
  // nodes)
  while (scm_is_pair(list)) {
    SCM elem = scm_car(list); // `car` fetches current node element
    SCM str_scm = scm_is_symbol(elem) ? scm_symbol_to_string(elem) : elem;

    if (scm_is_string(str_scm)) {
      char *str = scm_to_utf8_string(str_scm);
      // Case-insensitive comparison between exclusion entry and app_id
      gboolean match = (g_ascii_strcasecmp(str, app_id) == 0);
      free(str); // Clean up temporary C string

      if (match)
        return TRUE; // Found in blacklist
    }
    list = scm_cdr(list); // `cdr` advances to next list node
  }
  return FALSE; // Not excluded
}

/**
 * populate_dock_items:
 * Reads launcher items configured in Scheme (`get-dock-items`), parses each
 * record's properties, and constructs corresponding GTK visual widgets inside
 * the C state.
 *
 * @param state Pointer to global Lambdock application state wrapper.
 */
void populate_dock_items(LambdockState *state) {
  if (!state || !state->box)
    return;

  // Retrieve configured items list and helper procedure handles from Scheme
  SCM items_list = call_core_proc("get-dock-items");
  SCM get_type_fn = scm_c_public_ref("lambdock core", "dock-item-type");
  SCM get_name_fn = scm_c_public_ref("lambdock core", "dock-item-name");
  SCM get_icon_fn = scm_c_public_ref("lambdock core", "dock-item-icon");
  SCM get_exec_fn = scm_c_public_ref("lambdock core", "dock-item-exec");

  // Determine separator bar orientation based on overall dock layout direction
  GtkOrientation sep_orient = (state->orientation == GTK_ORIENTATION_HORIZONTAL)
                                  ? GTK_ORIENTATION_VERTICAL
                                  : GTK_ORIENTATION_HORIZONTAL;

  // Iterate over each item record node in the Scheme list
  while (scm_is_pair(items_list)) {
    SCM item = scm_car(items_list); // Extract current item record

    // Get item type symbol ('app or 'separator) and convert to C string
    char *type_str =
        scm_to_utf8_string(scm_symbol_to_string(scm_call_1(get_type_fn, item)));

    if (g_strcmp0(type_str, "app") == 0) {
      // Extract application launcher record attributes
      char *name = scm_to_utf8_string(scm_call_1(get_name_fn, item));
      char *icon = scm_to_utf8_string(scm_call_1(get_icon_fn, item));
      char *exec = scm_to_utf8_string(scm_call_1(get_exec_fn, item));

      // Create C launcher button only if name/icon/exec are not excluded
      if (!is_app_excluded(name) && !is_app_excluded(icon) &&
          !is_app_excluded(exec)) {
        create_dock_item(state, name, icon, exec, name, TRUE);
        LOG_C_INFO("GuileBridge", "Created favorite item: %s (%s)", name, exec);
      } else {
        LOG_C_INFO("GuileBridge", "Skipping excluded launcher item: %s", name);
      }

      // Free temporary attribute string allocations
      free(name);
      free(icon);
      free(exec);
    } else if (g_strcmp0(type_str, "separator") == 0) {
      // Instantiate visual separator line element
      create_separator_dock_item(state, sep_orient);
      LOG_C_INFO("GuileBridge", "Created layout separator");
    }

    free(type_str);                   // Clean up type string
    items_list = scm_cdr(items_list); // Move to next item in Scheme list
  }
}
