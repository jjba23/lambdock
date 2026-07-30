#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <libguile.h>

static void debug_guile_load_path(void) {
  char cwd[1024];
  if (getcwd(cwd, sizeof(cwd))) {
    g_print("[DEBUG] Current CWD: %s\n", cwd);
  }

  g_print("[DEBUG] Guile %%load-path:\n");
  scm_c_eval_string(
      "(for-each (lambda (p) (format #t \"  - ~a~%\" p)) %load-path)");

  g_print("[DEBUG] File lookup for 'lambdock/core.scm':\n");
  scm_c_eval_string("(let ((path (%search-load-path \"lambdock/core.scm\")))\n"
                    "  (if path\n"
                    "      (format #t \"  [FOUND] -> ~a~%\" path)\n"
                    "      (format #t \"  [NOT FOUND] 'lambdock/core.scm' not "
                    "in %%load-path!~%\")))");
}

static void try_load_module_safely(const char *module_name) {
  // Safely evaluate module loading inside Scheme with exception catching
  char eval_buf[512];
  snprintf(eval_buf, sizeof(eval_buf),
           "(catch #t\n"
           "  (lambda () (use-modules (%s)) (display \"[DEBUG] Module loaded "
           "successfully!\\n\"))\n"
           "  (lambda (key . args)\n"
           "    (format #t \"[DEBUG] Exception loading module: key=~a "
           "args=~a~%\" key args)))",
           module_name);

  scm_c_eval_string(eval_buf);
}

static void check_file_access(void) {
  const char *dev_path = "scheme/lambdock/core.scm";
  if (access(dev_path, F_OK) == 0) {
    g_print("[DEBUG] File '%s' exists on disk.\n", dev_path);
  } else {
    g_print("[DEBUG] File '%s' DOES NOT exist (relative to CWD)!\n", dev_path);
  }

#ifdef LAMBDOCK_SCHEME_DIR
  char installed_path[1024];
  snprintf(installed_path, sizeof(installed_path), "%s/lambdock/core.scm",
           LAMBDOCK_SCHEME_DIR);
  if (access(installed_path, F_OK) == 0) {
    g_print("[DEBUG] Installed file '%s' exists.\n", installed_path);
  } else {
    g_print("[DEBUG] Installed file '%s' DOES NOT exist!\n", installed_path);
  }
#endif
}

/**
 * Configure Guile's %load-path so it can locate module files on disk.
 */
static void load_guile_core(void) {
  // 1. Setup paths
  scm_c_eval_string("(set! %load-path (cons \"scheme\" %load-path))");
#ifdef LAMBDOCK_SCHEME_DIR
  scm_c_eval_string("(set! %load-path (cons \"" LAMBDOCK_SCHEME_DIR
                    "\" %load-path))");
#endif

  // 2. Run Diagnostics
  check_file_access();
  debug_guile_load_path();

  // 3. Attempt Module Load with Exception Catching
  try_load_module_safely("lambdock core");

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
