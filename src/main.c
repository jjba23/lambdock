#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <libguile.h>

/**
 * Configure Guile's %load-path so it can locate module files on disk.
 */
static void setup_guile_load_path(void) {
  // 1. Add relative path for local development (cwd)
  scm_c_eval_string("(add-to-load-path \"scheme\")");

#ifdef LAMBDOCK_SCHEME_DIR
  // 2. Add system installed path for Guix store execution
  scm_c_eval_string("(add-to-load-path \"" LAMBDOCK_SCHEME_DIR "\")");
#endif
}

/**
 * Initialize Guile runtime context, load (lambdock core), and evaluate dock
 * items.
 */
static void load_guile_core(void) {
  // Ensure %load-path contains ./scheme before resolving modules
  setup_guile_load_path();

  // Load the (lambdock core) module
  scm_c_use_module("lambdock core");

  // Fetch required procedures from (lambdock core)
  SCM init_fn = scm_c_public_ref("lambdock core", "init-dock!");
  SCM get_name_fn = scm_c_public_ref("lambdock core", "dock-item-name");

  // Execute (init-dock!) to receive the list of configured dock items
  SCM items_list = scm_call_0(init_fn);

  // Iterate through returned Scheme list
  while (scm_is_pair(items_list)) {
    SCM item = scm_car(items_list);
    SCM name_scm = scm_call_1(get_name_fn, item);

    char *name = scm_to_utf8_string(name_scm);
    g_print("[Lambdock] Loaded item: %s\n", name);
    free(name); // Always free UTF-8 strings returned by scm_to_utf8_string

    items_list = scm_cdr(items_list);
  }
}

/**
 * GTK Activation Callback
 */
static void activate(GtkApplication *app, gpointer user_data) {
  GtkWidget *window = gtk_application_window_new(app);

  // 1. Wayland Layer-Shell Setup
  gtk_layer_init_for_window(GTK_WINDOW(window));
  gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_TOP);
  gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
  gtk_layer_auto_exclusive_zone_enable(GTK_WINDOW(window));

  // 2. Main Dock Layout Container
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(box, 10);
  gtk_widget_set_margin_end(box, 10);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);

  gtk_window_set_child(GTK_WINDOW(window), box);

  // 3. Initialize Scheme Core Logic
  load_guile_core();

  gtk_window_present(GTK_WINDOW(window));
}

/**
 * Guile Inner Main Callback
 */
static void inner_main(void *data, int argc, char **argv) {
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
