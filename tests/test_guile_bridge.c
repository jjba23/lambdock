#include "../src/guile_bridge.h"
#include <gtk/gtk.h>
#include <libguile.h>

static void inner_test_guile_bridge(void *data, int argc, char **argv) {
  (void)data;
  (void)argc;
  (void)argv;

  // Evaluate basic Scheme definitions into the running Guile runtime
  scm_c_eval_string("(define-module (lambdock core)\n"
                    "  #:export (get-dock-auto-hide? get-dock-icon-size "
                    "get-dock-exclude-apps))\n"
                    "(define (get-dock-auto-hide? . args) #t)\n"
                    "(define (get-dock-icon-size . args) 64)\n"
                    "(define (get-dock-exclude-apps . args) '(\"bad-app\" "
                    "\"tracker-daemon\"))\n");

  // Test C bridge boolean reader
  gboolean autohide = get_autohide_config_for("test_config.scm");
  g_assert_true(autohide);

  // Test C bridge integer reader
  int icon_sz = get_icon_size_config_for("test_config.scm");
  g_assert_cmpint(icon_sz, ==, 64);

  // Test exclusion list matching
  g_assert_true(is_app_excluded("bad-app", "test_config.scm"));
  g_assert_true(
      is_app_excluded("TRACKER-DAEMON", "test_config.scm")); // Case insensitive
  g_assert_false(is_app_excluded("allowed-app", "test_config.scm"));
}

int main(int argc, char **argv) {
  gtk_init();
  g_test_init(&argc, &argv, NULL);

  // Boot Guile environment and run integration assertions inside inner main
  scm_boot_guile(argc, argv, inner_test_guile_bridge, NULL);
  return 0;
}
