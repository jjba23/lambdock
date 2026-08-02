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

#include "guile_bridge.h"
#include "ui_helpers.h"

/* =========================================================================
 * GUILE ENVIRONMENT SETUP
 * ========================================================================= */

/**
 * setup_guile_load_path:
 *
 * Configures Guile's internal '%load-path' list so the Scheme evaluator can
 * locate and load external Lambdock Scheme (.scm) modules during runtime.
 */
void setup_guile_load_path(void) {
  // Execute Guile Scheme code directly from C string evaluation.
  // Appends relative "./scheme" directory to Scheme's module search path.
  scm_c_eval_string("(set! %load-path (cons \"scheme\" %load-path))");

// If build macro predefines absolute installed scheme directory path
#ifdef LAMBDOCK_SCHEME_DIR
  scm_c_eval_string("(set! %load-path (cons \"" LAMBDOCK_SCHEME_DIR
                    "\" %load-path))");
#endif
}

/* =========================================================================
 * CONFIGURATION READING
 * ========================================================================= */

/**
 * get_autohide_config:
 *
 * Calls the Scheme procedure 'get-dock-auto-hide?' defined in module '(lambdock
 * core)' to fetch the auto-hide user setting.
 *
 * Returns: gboolean ('TRUE' or 'FALSE') translated from Scheme boolean.
 */
gboolean get_autohide_config(void) {
  // Look up public procedure reference in module '(lambdock core)'
  SCM get_autohide_fn =
      scm_c_public_ref("lambdock core", "get-dock-auto-hide?");

  // Verify function pointer exists and is non-false/defined in Scheme context
  if (scm_is_true(get_autohide_fn)) {
    // Invoke Scheme function with zero arguments -> returns SCM value
    SCM res = scm_call_0(get_autohide_fn);

    // Convert Guile Scheme boolean SCM object to standard C/GTK gboolean type
    return scm_to_bool(res);
  }

  return FALSE; // Fallback default if module or function is missing
}

/* =========================================================================
 * GUILE TO GTK DOCK BUILDER
 * ========================================================================= */

/**
 * populate_dock_items:
 * @box: Pointer to target GTK 4 Box layout widget.
 *
 * Evaluates Scheme module functions to get dock item definitions, iterates
 * through returned LISP list, instantiates GTK widgets (buttons, separators),
 * and appends them to the dock GTK Box layout container.
 */
void populate_dock_items(GtkWidget *box) {
  // --- Look up required Guile Scheme functions from module '(lambdock core)'
  SCM init_fn = scm_c_public_ref("lambdock core", "init-dock!");
  SCM get_type_fn = scm_c_public_ref("lambdock core", "dock-item-type");
  SCM get_name_fn = scm_c_public_ref("lambdock core", "dock-item-name");
  SCM get_icon_fn = scm_c_public_ref("lambdock core", "dock-item-icon");
  SCM get_exec_fn = scm_c_public_ref("lambdock core", "dock-item-exec");
  SCM get_icon_sz_fn = scm_c_public_ref("lambdock core", "get-icon-size");

  // Execute 'init-dock!' procedure to get the list of item objects from Scheme
  SCM items_list = scm_call_0(init_fn);

  // Read preferred icon size from Scheme configuration and convert to C int
  int icon_size = scm_to_int(scm_call_0(get_icon_sz_fn));

  // --- Iterate over Scheme LISP list of launchers ---
  // scm_is_pair checks if 'items_list' is a non-empty Scheme pair/list node
  while (scm_is_pair(items_list)) {
    // Extract head element (car) of the current list node
    SCM item = scm_car(items_list);

    // Get item type (returns Scheme Symbol, e.g., 'app or 'separator)
    SCM type_scm = scm_call_1(get_type_fn, item);

    // Convert Scheme Symbol -> Scheme String -> Dynamic C Heap String
    SCM type_str_scm = scm_symbol_to_string(type_scm);
    char *type_str = scm_to_utf8_string(type_str_scm);

    // --- Handle Application Launchers ---
    if (g_strcmp0(type_str, "app") == 0) {
      // Extract properties from Scheme item object and convert to C heap
      // strings
      char *name = scm_to_utf8_string(scm_call_1(get_name_fn, item));
      char *icon = scm_to_utf8_string(scm_call_1(get_icon_fn, item));
      char *exec = scm_to_utf8_string(scm_call_1(get_exec_fn, item));

      // Construct GTK launcher button widget using helper function
      GtkWidget *btn = create_app_button(name, icon, exec, icon_size);

      // Append constructed button to GTK 4 Box container
      // GTK_BOX macro safe-casts GtkWidget pointer to GtkBox pointer
      gtk_box_append(GTK_BOX(box), btn);

      g_print("[Lambdock] Created app button: %s (%s)\n", name, exec);

      // MEMORY MANAGEMENT: free() heap buffers created by scm_to_utf8_string
      free(name);
      free(icon);
      free(exec);

      // --- Handle Visual Separators ---
    } else if (g_strcmp0(type_str, "separator") == 0) {
      GtkWidget *sep = create_separator();
      gtk_box_append(GTK_BOX(box), sep);
      g_print("[Lambdock] Created separator\n");
    }

    // MEMORY MANAGEMENT: Free the type string buffer for this iteration
    free(type_str);

    // Advance list iterator to next node (cdr = tail of list)
    items_list = scm_cdr(items_list);
  }
}
