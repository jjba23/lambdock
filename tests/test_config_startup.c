// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

/* End-to-end configuration test: the real Scheme modules, the real C bridge.
 *
 * tests/test_guile_bridge.c mocks `(lambdock core)' so it can check every
 * conversion branch. That means nothing there proves the thing users actually
 * depend on: that a settings file plus the two override files in
 * $XDG_STATE_HOME produce the values the dock reads on the first load.
 *
 * This binary writes a settings file and the two override files into a throwaway
 * directory, points XDG_STATE_HOME/HOME/XDG_CONFIG_HOME at it, and then does what
 * main.c does at startup: load the config, then read values through the bridge.
 */

#include "../src/core/config_values.h"
#include "../src/guile_bridge.h"
#include "../src/lambdock.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <libguile.h>
#include <string.h>

static char *test_root = NULL;
static char *config_path = NULL;

static void write_file(const char *path, const char *contents) {
  g_assert_true(g_file_set_contents(path, contents, -1, NULL));
}

static void make_fixtures(void) {
  test_root = g_dir_make_tmp("lambdock-startup-XXXXXX", NULL);
  g_assert_nonnull(test_root);

  char *state_dir = g_build_filename(test_root, "state", "lambdock", NULL);
  char *config_dir = g_build_filename(test_root, "config", "lambdock", NULL);
  g_assert_cmpint(g_mkdir_with_parents(state_dir, 0700), ==, 0);
  g_assert_cmpint(g_mkdir_with_parents(config_dir, 0700), ==, 0);

  char *settings = g_build_filename(config_dir, "settings.scm", NULL);
  write_file(settings,
             "(define dock-position 'bottom)\n"
             "(define dock-theme 'vanilla)\n"
             "(define dock-icon-size 48)\n"
             "(define dock-shelf-margin 24)\n"
             "(define dock-items\n"
             "  (list (app-item #:name \"Files\" #:icon \"system-file-manager\"\n"
             "                   #:exec \"nautilus\")\n"
             "        (app-item #:name \"Firefox\" #:icon \"firefox\"\n"
             "                   #:exec \"firefox\")\n"
             "        (app-item #:name \"Emacs\" #:icon \"emacs\"\n"
             "                   #:exec \"emacs\")))\n");

  /* What the Preferences window and drag and drop leave behind, named after the
   * settings file's stem exactly as the writers name them. */
  char *prefs = g_build_filename(state_dir, "preferences-settings.scm", NULL);
  write_file(prefs, "((dock-theme . macos-mavericks-dark)\n"
                    " (dock-shelf-margin . -18))\n");

  char *layout = g_build_filename(state_dir, "layout-settings.scm", NULL);
  write_file(layout, "((order \"app:Emacs\" \"app:Firefox\" \"app:Files\")\n"
                     " (removed) (added) (views))\n");

  config_path = settings; /* owned by the test */

  g_setenv("XDG_STATE_HOME", g_build_filename(test_root, "state", NULL), TRUE);
  g_setenv("XDG_CONFIG_HOME", g_build_filename(test_root, "config", NULL), TRUE);
  g_setenv("HOME", test_root, TRUE);

  g_free(state_dir);
  g_free(config_dir);
  g_free(prefs);
  g_free(layout);
}

static void check_overrides_apply(void) {
  g_autofree char *theme = get_theme_config_for(config_path);
  g_assert_cmpstr(theme, ==, "macos-mavericks-dark");

  LambdockShelfParams shelf = {0};
  get_shelf_config_for(config_path, &shelf);
  g_assert_cmpint(shelf.margin_px, ==, -18);

  /* The stored order must be the order the dock reads: Emacs, Firefox, Files. */
  SCM get_items = scm_c_public_ref("lambdock core", "get-dock-items");
  SCM items = scm_call_1(get_items, scm_from_utf8_string(config_path));
  g_assert_true(scm_is_pair(items));

  SCM get_name = scm_c_public_ref("lambdock core", "dock-item-name");
  const char *expected[] = {"Emacs", "Firefox", "Files"};
  for (gsize i = 0; i < G_N_ELEMENTS(expected); i++) {
    g_assert_true(scm_is_pair(items));
    g_autofree char *name =
        scm_to_utf8_string(scm_call_1(get_name, scm_car(items)));
    g_assert_cmpstr(name, ==, expected[i]);
    items = scm_cdr(items);
  }
}

static void load_and_check(void *data) {
  (void)data;

  setup_guile_load_path();
  scm_c_use_module("lambdock core");

  /* This is what main.c's reconcile_settings_file() does before spawning a dock. */
  load_dock_config_scm(config_path);

  check_overrides_apply();
}

static void inner_main(void *data, int argc, char **argv) {
  (void)argc;
  (void)argv;

  make_fixtures();
  load_and_check(data);
}

int main(int argc, char **argv) {
  scm_boot_guile(argc, argv, inner_main, NULL);
  return 0;
}
