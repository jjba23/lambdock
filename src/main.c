#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <libguile.h>

/* =============================================================================
 * App Launcher Callback
 * =============================================================================
 */

static void on_app_button_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  const char *exec_cmd = (const char *)user_data;

  if (exec_cmd && *exec_cmd != '\0') {
    GError *error = NULL;
    if (!g_spawn_command_line_async(exec_cmd, &error)) {
      g_warning("[Lambdock] Failed to launch '%s': %s", exec_cmd,
                error->message);
      g_clear_error(&error);
    } else {
      g_print("[Lambdock] Launched: %s\n", exec_cmd);
    }
  }
}

/* =============================================================================
 * GTK Widget Factory Helpers
 * =============================================================================
 */

/**
 * Creates an app launcher button with an icon, flat styling, and hover tooltip.
 */
static GtkWidget *create_app_button(const char *name, const char *icon,
                                    const char *exec, int icon_size) {
  GtkWidget *button = gtk_button_new();
  GtkWidget *image = gtk_image_new_from_icon_name(icon);

  gtk_image_set_pixel_size(GTK_IMAGE(image), icon_size);
  gtk_button_set_child(GTK_BUTTON(button), image);
  gtk_widget_add_css_class(button, "flat");

  // Enable hover tooltip showing the app name (fallback to exec)
  if (name && *name != '\0') {
    gtk_widget_set_tooltip_text(button, name);
  } else if (exec && *exec != '\0') {
    gtk_widget_set_tooltip_text(button, exec);
  }

  // Bind click handler. Ownership of duplicated exec string is transferred to
  // g_signal_connect_data and cleaned up with g_free when button is destroyed.
  g_signal_connect_data(button, "clicked", G_CALLBACK(on_app_button_clicked),
                        g_strdup(exec), (GClosureNotify)g_free, 0);

  return button;
}

/**
 * Creates a vertical separator for the dock layout.
 */
static GtkWidget *create_separator(void) {
  GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_margin_start(separator, 4);
  gtk_widget_set_margin_end(separator, 4);
  return separator;
}

/* =============================================================================
 * Guile Integration Logic
 * =============================================================================
 */

static void setup_guile_load_path(void) {
  scm_c_eval_string("(set! %load-path (cons \"scheme\" %load-path))");
#ifdef LAMBDOCK_SCHEME_DIR
  scm_c_eval_string("(set! %load-path (cons \"" LAMBDOCK_SCHEME_DIR
                    "\" %load-path))");
#endif
}

static void populate_dock_items(GtkWidget *box) {
  setup_guile_load_path();
  scm_c_use_module("lambdock core");

  // Resolve procedure references
  SCM init_fn = scm_c_public_ref("lambdock core", "init-dock!");
  SCM get_type_fn = scm_c_public_ref("lambdock core", "dock-item-type");
  SCM get_name_fn = scm_c_public_ref("lambdock core", "dock-item-name");
  SCM get_icon_fn = scm_c_public_ref("lambdock core", "dock-item-icon");
  SCM get_exec_fn = scm_c_public_ref("lambdock core", "dock-item-exec");
  SCM get_icon_sz_fn = scm_c_public_ref("lambdock core", "get-icon-size");

  int icon_size = scm_to_int(scm_call_0(get_icon_sz_fn));
  SCM items_list = scm_call_0(init_fn);

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

/* =============================================================================
 * Window & GTK Initialization
 * =============================================================================
 */

static void setup_layer_shell_window(GtkWindow *window) {
  gtk_layer_init_for_window(window);
  gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_TOP);
  gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
  gtk_layer_auto_exclusive_zone_enable(window);
}

static void activate(GtkApplication *app, gpointer user_data) {
  (void)user_data;
  GtkWidget *window = gtk_application_window_new(app);

  setup_layer_shell_window(GTK_WINDOW(window));

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(box, 10);
  gtk_widget_set_margin_end(box, 10);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);

  gtk_window_set_child(GTK_WINDOW(window), box);

  populate_dock_items(box);

  gtk_window_present(GTK_WINDOW(window));
}

static void inner_main(void *data, int argc, char **argv) {
  (void)data;
  GtkApplication *app =
      gtk_application_new("org.scheme.lambdock", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
}

int main(int argc, char **argv) {
  scm_boot_guile(argc, argv, inner_main, NULL);
  return 0;
}
