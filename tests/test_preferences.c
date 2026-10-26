// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY ; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock. If not, see <https://www.gnu.org/licenses/>.

#include "../src/guile_bridge.h"
#include "../src/lambdock.h"
#include "../src/ui/preferences.h"
#include <glib.h>
#include <gtk/gtk.h>
#include <libguile.h>
#include <stdlib.h>
#include <string.h>

/* Drives the real preferences form against the real Scheme modules: build the
 * content for a temporary settings file, flip a switch / drag a slider / pick a
 * theme, let the debounce fire, and assert that the ordinary `set-dock-*!'
 * setters ran and the GUI override file was written. This is the wiring between
 * the widgets and the Scheme layer, which the pure-Scheme tests cannot see. */

static GtkWidget *find_by_key(GtkWidget *root, const char *key) {
  if (!root)
    return NULL;

  const char *stored = g_object_get_data(G_OBJECT(root), "lambdock-key");
  if (stored != NULL && g_strcmp0(stored, key) == 0)
    return root;

  for (GtkWidget *child = gtk_widget_get_first_child(root); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_by_key(child, key);
    if (found)
      return found;
  }
  return NULL;
}

static gboolean quit_loop(gpointer data) {
  g_main_loop_quit((GMainLoop *)data);
  return G_SOURCE_REMOVE;
}

/* Runs the default main context for MS milliseconds, which is long enough for
 * the preferences' apply debounce to fire. */
static void pump_main_loop(guint ms) {
  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  g_timeout_add(ms, quit_loop, loop);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
}

static void inner_test_preferences(void *data, int argc, char **argv) {
  (void)data;

  g_test_init(&argc, &argv, NULL);

  char *dir = g_dir_make_tmp("lambdock-prefs-test-XXXXXX", NULL);
  g_assert_nonnull(dir);
  g_setenv("XDG_STATE_HOME", dir, TRUE);

  /* The tests run from the project source root, the same way the Guile bridge
   * resolves the `scheme' directory. Load the real modules -- the fake ones in
   * test_guile_bridge would not exercise set-dock-preference!. */
  scm_c_eval_string("(set! %load-path (cons \"scheme\" %load-path))");
  scm_c_use_module("lambdock core");

  char *config_path = g_build_filename(dir, "settings.scm", NULL);
  g_assert_true(g_file_set_contents(config_path,
                                    "(define dock-icon-size 48)\n"
                                    "(define dock-auto-hide? #f)\n"
                                    "(define dock-theme 'vanilla)\n",
                                    -1, NULL));

  SCM load_fn = scm_c_public_ref("lambdock core", "load-dock-config!");
  scm_call_1(load_fn, scm_from_utf8_string(config_path));

  g_assert_cmpint(get_icon_size_config_for(config_path), ==, 48);
  g_assert_false(get_autohide_config_for(config_path));

  if (!gtk_init_check()) {
    g_test_message("No display server: skipping preferences widget tests.");
    g_free(config_path);
    g_free(dir);
    return;
  }

  GtkWidget *window = gtk_window_new();
  GtkWidget *content = lambdock_preferences_content_for(config_path);
  g_assert_nonnull(content);
  gtk_window_set_child(GTK_WINDOW(window), content);

  /* The icon-size slider is an integer control: dragging it to 64 must reach
   * the Scheme setter after the debounce. */
  GtkWidget *icon_size = find_by_key(content, "dock-icon-size");
  g_assert_nonnull(icon_size);
  g_assert_true(GTK_IS_RANGE(icon_size));
  gtk_range_set_value(GTK_RANGE(icon_size), 64.0);
  pump_main_loop(400);
  g_assert_cmpint(get_icon_size_config_for(config_path), ==, 64);

  /* The auto-hide checkbox is a boolean control. */
  GtkWidget *auto_hide = find_by_key(content, "dock-auto-hide?");
  g_assert_nonnull(auto_hide);
  g_assert_true(GTK_IS_SWITCH(auto_hide));
  gtk_switch_set_active(GTK_SWITCH(auto_hide), TRUE);
  pump_main_loop(400);
  g_assert_true(get_autohide_config_for(config_path));

  /* The theme dropdown carries a symbol. Index 9 is macOS Mavericks in the
   * form's theme list. */
  GtkWidget *theme = find_by_key(content, "dock-theme");
  g_assert_nonnull(theme);
  g_assert_true(GTK_IS_DROP_DOWN(theme));
  gtk_drop_down_set_selected(GTK_DROP_DOWN(theme), 9);
  pump_main_loop(400);
  char *applied_theme = get_theme_config_for(config_path);
  g_assert_cmpstr(applied_theme, ==, "macos-mavericks");
  free(applied_theme);

  /* A floating-point control (the reflection band). */
  GtkWidget *reflection = find_by_key(content, "dock-reflection");
  g_assert_nonnull(reflection);
  g_assert_true(GTK_IS_RANGE(reflection));
  gtk_range_set_value(GTK_RANGE(reflection), 0.3);
  pump_main_loop(400);
  LambdockShelfParams shelf;
  get_shelf_config_for(config_path, &shelf);
  g_assert_cmpfloat_with_epsilon(shelf.reflection_ratio, 0.3, 1e-6);

  /* Every edit was remembered in the GUI override file. */
  SCM path_fn = scm_c_public_ref("lambdock core", "dock-preferences-path");
  SCM path_scm = scm_call_1(path_fn, scm_from_utf8_string(config_path));
  char *prefs_path = scm_to_utf8_string(path_scm);
  char *contents = NULL;
  g_assert_true(g_file_get_contents(prefs_path, &contents, NULL, NULL));
  g_assert_nonnull(strstr(contents, "dock-icon-size"));
  g_assert_nonnull(strstr(contents, "dock-auto-hide?"));
  g_assert_nonnull(strstr(contents, "dock-theme"));
  g_assert_nonnull(strstr(contents, "dock-reflection"));
  g_free(contents);

  /* Reset forgets the overrides; reloading restores the settings file. */
  reset_dock_preferences(config_path);
  load_dock_config_scm(config_path);
  g_assert_cmpint(get_icon_size_config_for(config_path), ==, 48);
  g_assert_false(get_autohide_config_for(config_path));

  free(prefs_path);
  gtk_window_destroy(GTK_WINDOW(window));
  g_free(config_path);
  g_free(dir);

  g_print("# all preferences assertions passed\n");
}

int main(int argc, char **argv) {
  scm_boot_guile(argc, argv, inner_test_preferences, NULL);
  return 0;
}
