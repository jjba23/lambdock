#include "guile_bridge.h"
#include "ui_helpers.h"

void setup_guile_load_path(void) {
  scm_c_eval_string("(set! %load-path (cons \"scheme\" %load-path))");
#ifdef LAMBDOCK_SCHEME_DIR
  scm_c_eval_string("(set! %load-path (cons \"" LAMBDOCK_SCHEME_DIR
                    "\" %load-path))");
#endif
}

gboolean get_autohide_config(void) {
  SCM get_autohide_fn =
      scm_c_public_ref("lambdock core", "get-dock-auto-hide?");
  if (scm_is_true(get_autohide_fn)) {
    SCM res = scm_call_0(get_autohide_fn);
    return scm_to_bool(res);
  }
  return FALSE;
}

void populate_dock_items(GtkWidget *box) {
  SCM init_fn = scm_c_public_ref("lambdock core", "init-dock!");
  SCM get_type_fn = scm_c_public_ref("lambdock core", "dock-item-type");
  SCM get_name_fn = scm_c_public_ref("lambdock core", "dock-item-name");
  SCM get_icon_fn = scm_c_public_ref("lambdock core", "dock-item-icon");
  SCM get_exec_fn = scm_c_public_ref("lambdock core", "dock-item-exec");
  SCM get_icon_sz_fn = scm_c_public_ref("lambdock core", "get-icon-size");

  SCM items_list = scm_call_0(init_fn);
  int icon_size = scm_to_int(scm_call_0(get_icon_sz_fn));

  while (scm_is_pair(items_list)) {
    SCM item = scm_car(items_list);

    SCM type_scm = scm_call_1(get_type_fn, item);
    SCM type_str_scm = scm_symbol_to_string(type_scm);
    char *type_str = scm_to_utf8_string(type_str_scm);

    if (g_strcmp0(type_str, "app") == 0) {
      char *name = scm_to_utf8_string(scm_call_1(get_name_fn, item));
      char *icon = scm_to_utf8_string(scm_call_1(get_icon_fn, item));
      char *exec = scm_to_utf8_string(scm_call_1(get_exec_fn, item));

      GtkWidget *btn = create_app_button(name, icon, exec, icon_size);
      gtk_box_append(GTK_BOX(box), btn);

      g_print("[Lambdock] Created app button: %s (%s)\n", name, exec);

      free(name);
      free(icon);
      free(exec);
    } else if (g_strcmp0(type_str, "separator") == 0) {
      GtkWidget *sep = create_separator();
      gtk_box_append(GTK_BOX(box), sep);
      g_print("[Lambdock] Created separator\n");
    }

    free(type_str);
    items_list = scm_cdr(items_list);
  }
}
