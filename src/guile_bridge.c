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

void setup_guile_load_path(void) {
  // Execute Scheme code string directly in Guile interpreter to update
  // %load-path
  scm_c_eval_string("(set! %load-path (cons \"scheme\" %load-path))");
#ifdef LAMBDOCK_SCHEME_DIR
  scm_c_eval_string("(set! %load-path (cons \"" LAMBDOCK_SCHEME_DIR
                    "\" %load-path))");
#endif
}

gboolean get_autohide_config(void) {
  // Retrieve reference to public Scheme function inside module '(lambdock
  // core)'
  SCM get_autohide_fn =
      scm_c_public_ref("lambdock core", "get-dock-auto-hide?");

  if (scm_is_true(get_autohide_fn)) {
    // Invoke Scheme function with 0 parameters -> returns Guile SCM object
    SCM res = scm_call_0(get_autohide_fn);
    // Convert Scheme boolean type to GTK C gboolean type
    return scm_to_bool(res);
  }

  return FALSE;
}

GtkLayerShellEdge get_position_config(void) {
  SCM get_pos_fn = scm_c_public_ref("lambdock core", "get-dock-position");

  if (scm_is_true(get_pos_fn)) {
    SCM res = scm_call_0(get_pos_fn);

    // Scheme symbols must be converted to Scheme strings before dynamic
    // conversion to C
    SCM str_scm = scm_is_symbol(res) ? scm_symbol_to_string(res) : res;
    char *pos_str = scm_to_utf8_string(str_scm);

    GtkLayerShellEdge edge = GTK_LAYER_SHELL_EDGE_BOTTOM;

    if (g_strcmp0(pos_str, "top") == 0) {
      edge = GTK_LAYER_SHELL_EDGE_TOP;
    } else if (g_strcmp0(pos_str, "left") == 0) {
      edge = GTK_LAYER_SHELL_EDGE_LEFT;
    } else if (g_strcmp0(pos_str, "right") == 0) {
      edge = GTK_LAYER_SHELL_EDGE_RIGHT;
    }

    // MEMORY MANAGEMENT: Free heap buffer allocated by scm_to_utf8_string
    free(pos_str);
    return edge;
  }

  return GTK_LAYER_SHELL_EDGE_BOTTOM;
}

char *get_theme_config(void) {
  SCM get_theme_fn = scm_c_public_ref("lambdock core", "get-dock-theme");

  if (scm_is_true(get_theme_fn)) {
    SCM res = scm_call_0(get_theme_fn);

    // Convert symbol to string if needed
    SCM str_scm = scm_is_symbol(res) ? scm_symbol_to_string(res) : res;

    // Guard against #f or non-string SCM values
    if (scm_is_string(str_scm)) {
      char *theme_str = scm_to_utf8_string(str_scm);

      if (g_strcmp0(theme_str, "snow-leopard") == 0) {
        return theme_str;
      }
      free(theme_str);
    }
  }

  return g_strdup("vanilla");
}

void populate_dock_items(LambdockState *state) {
  if (!state || !state->box)
    return;

  // Lookup module function handles
  SCM dock_items_fn = scm_c_public_ref("lambdock core", "get-dock-items");
  SCM get_type_fn = scm_c_public_ref("lambdock core", "dock-item-type");
  SCM get_name_fn = scm_c_public_ref("lambdock core", "dock-item-name");
  SCM get_icon_fn = scm_c_public_ref("lambdock core", "dock-item-icon");
  SCM get_exec_fn = scm_c_public_ref("lambdock core", "dock-item-exec");

  // Call Scheme procedure to retrieve launcher item list
  SCM items_list = scm_call_0(dock_items_fn);

  GtkOrientation sep_orient = (state->orientation == GTK_ORIENTATION_HORIZONTAL)
                                  ? GTK_ORIENTATION_VERTICAL
                                  : GTK_ORIENTATION_HORIZONTAL;

  // Iterate over Scheme LISP linked list (pair checking)
  while (scm_is_pair(items_list)) {
    SCM item = scm_car(items_list); // Get head element (car)

    SCM type_scm = scm_call_1(get_type_fn, item);
    SCM type_str_scm = scm_symbol_to_string(type_scm);
    char *type_str = scm_to_utf8_string(type_str_scm);

    if (g_strcmp0(type_str, "app") == 0) {
      // Extract properties and convert SCM objects to heap C strings
      char *name = scm_to_utf8_string(scm_call_1(get_name_fn, item));
      char *icon = scm_to_utf8_string(scm_call_1(get_icon_fn, item));
      char *exec = scm_to_utf8_string(scm_call_1(get_exec_fn, item));

      // Instantiate unified favorite dock item
      create_dock_item(state, name, icon, exec, name, TRUE);

      g_print("[Lambdock] Created unified favorite item: %s (%s)\n", name,
              exec);

      // Clean up heap string allocations
      free(name);
      free(icon);
      free(exec);
    } else if (g_strcmp0(type_str, "separator") == 0) {
      GtkWidget *sep = create_separator(sep_orient);
      gtk_box_append(GTK_BOX(state->box), sep);
      g_print("[Lambdock] Created separator\n");
    }

    free(type_str);
    items_list = scm_cdr(items_list); // Move to next tail pair node (cdr)
  }
}
