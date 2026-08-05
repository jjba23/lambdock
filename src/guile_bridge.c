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
#include "logger.h"
#include "ui/dock_item.h"

void setup_guile_load_path(void) {
  scm_c_eval_string("(set! %load-path (cons \"scheme\" %load-path))");
#ifdef LAMBDOCK_SCHEME_DIR
  scm_c_eval_string("(set! %load-path (cons \"" LAMBDOCK_SCHEME_DIR
                    "\" %load-path))");
#endif
}

/* =========================================================================
 * Generic Guile Helpers
 * ========================================================================= */

static SCM call_core_proc(const char *func_name) {
  SCM fn = scm_c_public_ref("lambdock core", func_name);
  return scm_is_true(fn) ? scm_call_0(fn) : SCM_BOOL_F;
}

static char *get_guile_string(const char *func_name,
                              const char *fallback_module,
                              const char *default_val) {
  SCM fn = fallback_module ? scm_c_public_ref(fallback_module, func_name)
                           : scm_c_public_ref("lambdock core", func_name);
  if (!scm_is_true(fn))
    return g_strdup(default_val);

  SCM res = scm_call_0(fn);
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

static gboolean get_guile_bool(const char *func_name) {
  SCM res = call_core_proc(func_name);
  return scm_is_true(res) ? scm_to_bool(res) : FALSE;
}

/* =========================================================================
 * Public Bridge API
 * ========================================================================= */

gboolean get_autohide_config(void) {
  return get_guile_bool("get-dock-auto-hide?");
}
gboolean get_repl_config(void) {
  return get_guile_bool("get-dock-enable-repl?");
}

char *get_theme_config(void) {
  return get_guile_string("get-dock-theme", NULL, "vanilla");
}
char *get_monitor_config(void) {
  return get_guile_string("get-dock-monitor", NULL, "default");
}
char *get_config_path_c(void) {
  return get_guile_string("get-config-path", "lambdock loader", NULL);
}

GtkLayerShellEdge get_position_config(void) {
  char *pos_str = get_guile_string("get-dock-position", NULL, "bottom");
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

void reload_guile_config(void) {
  LOG_C_INFO("GuileBridge", "Re-evaluating Scheme configuration file...");
  SCM init_fn = scm_c_public_ref("lambdock core", "init-dock!");
  if (scm_is_true(init_fn))
    scm_call_0(init_fn);
}

gboolean is_app_excluded(const char *app_id) {
  if (!app_id || *app_id == '\0')
    return FALSE;

  SCM list = call_core_proc("get-dock-exclude-apps");
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

void populate_dock_items(LambdockState *state) {
  if (!state || !state->box)
    return;

  SCM items_list = call_core_proc("get-dock-items");
  SCM get_type_fn = scm_c_public_ref("lambdock core", "dock-item-type");
  SCM get_name_fn = scm_c_public_ref("lambdock core", "dock-item-name");
  SCM get_icon_fn = scm_c_public_ref("lambdock core", "dock-item-icon");
  SCM get_exec_fn = scm_c_public_ref("lambdock core", "dock-item-exec");

  GtkOrientation sep_orient = (state->orientation == GTK_ORIENTATION_HORIZONTAL)
                                  ? GTK_ORIENTATION_VERTICAL
                                  : GTK_ORIENTATION_HORIZONTAL;

  while (scm_is_pair(items_list)) {
    SCM item = scm_car(items_list);
    char *type_str =
        scm_to_utf8_string(scm_symbol_to_string(scm_call_1(get_type_fn, item)));

    if (g_strcmp0(type_str, "app") == 0) {
      char *name = scm_to_utf8_string(scm_call_1(get_name_fn, item));
      char *icon = scm_to_utf8_string(scm_call_1(get_icon_fn, item));
      char *exec = scm_to_utf8_string(scm_call_1(get_exec_fn, item));

      if (!is_app_excluded(name) && !is_app_excluded(icon) &&
          !is_app_excluded(exec)) {
        create_dock_item(state, name, icon, exec, name, TRUE);
        LOG_C_INFO("GuileBridge", "Created favorite item: %s (%s)", name, exec);
      } else {
        LOG_C_INFO("GuileBridge", "Skipping excluded launcher item: %s", name);
      }

      free(name);
      free(icon);
      free(exec);
    } else if (g_strcmp0(type_str, "separator") == 0) {
      create_separator_dock_item(state, sep_orient);
      LOG_C_INFO("GuileBridge", "Created layout separator");
    }

    free(type_str);
    items_list = scm_cdr(items_list);
  }
}
