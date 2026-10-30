// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/guile_bridge.h"
#include "../src/lambdock.h"
#include "../src/ui/preferences.h"
#include <glib.h>
#include <gtk/gtk.h>
#include <libguile.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* The numeric spin button sits beside its slider in the same control box; the
 * slider is the keyed widget, so this is how the test reaches the precise
 * input that pairs with it. */
static GtkWidget *spin_for(GtkWidget *scale) {
  if (!scale)
    return NULL;

  GtkWidget *parent = gtk_widget_get_parent(scale);
  if (!parent)
    return NULL;

  for (GtkWidget *child = gtk_widget_get_first_child(parent); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (GTK_IS_SPIN_BUTTON(child))
      return child;
  }
  return NULL;
}

static gboolean quit_loop(gpointer data) {
  g_main_loop_quit((GMainLoop *)data);
  return G_SOURCE_REMOVE;
}

/* Removes the temporary tree the test created. Leaving a
 * `lambdock-prefs-test-*' directory behind is not just untidy: Guix's
 * install-license-files phase looks for a sibling directory whose name starts
 * with the package name, and would mistake it for the source. */
static void remove_tree(const char *path) {
  GDir *dir = g_dir_open(path, 0, NULL);
  if (dir != NULL) {
    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
      char *child = g_build_filename(path, name, NULL);
      if (g_file_test(child, G_FILE_TEST_IS_DIR))
        remove_tree(child);
      else
        unlink(child);
      g_free(child);
    }
    g_dir_close(dir);
  }
  rmdir(path);
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
    remove_tree(dir);
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

  /* Every numeric row has a spin button for an exact value, including one that
   * the slider's step (2 px here) never produces. It starts in sync with the
   * slider and drives the very same apply path. */
  GtkWidget *icon_spin = spin_for(icon_size);
  g_assert_nonnull(icon_spin);
  g_assert_true(GTK_IS_SPIN_BUTTON(icon_spin));
  g_assert_cmpfloat_with_epsilon(gtk_spin_button_get_value(GTK_SPIN_BUTTON(icon_spin)),
                                 64.0, 1e-9);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(icon_spin), 63.0);
  pump_main_loop(400);
  g_assert_cmpint(get_icon_size_config_for(config_path), ==, 63);

  /* The auto-hide checkbox is a boolean control. */
  GtkWidget *auto_hide = find_by_key(content, "dock-auto-hide?");
  g_assert_nonnull(auto_hide);
  g_assert_true(GTK_IS_SWITCH(auto_hide));
  gtk_switch_set_active(GTK_SWITCH(auto_hide), TRUE);
  pump_main_loop(400);
  g_assert_true(get_autohide_config_for(config_path));

  /* A switch has no text of its own, so the form labels it for screen readers,
   * and the accessibility section carries its own reduced-motion switch. */
  g_assert_null(gtk_test_accessible_check_property(
      GTK_ACCESSIBLE(auto_hide), GTK_ACCESSIBLE_PROPERTY_LABEL, "Auto-hide"));
  g_assert_null(gtk_test_accessible_check_property(
      GTK_ACCESSIBLE(icon_size), GTK_ACCESSIBLE_PROPERTY_LABEL, "Icon size"));
  g_assert_null(gtk_test_accessible_check_property(
      GTK_ACCESSIBLE(icon_spin), GTK_ACCESSIBLE_PROPERTY_LABEL,
      "Icon size, exact value"));

  GtkWidget *reduce_motion = find_by_key(content, "dock-reduce-motion?");
  g_assert_nonnull(reduce_motion);
  g_assert_true(GTK_IS_SWITCH(reduce_motion));
  g_assert_null(gtk_test_accessible_check_property(
      GTK_ACCESSIBLE(reduce_motion), GTK_ACCESSIBLE_PROPERTY_LABEL,
      "Reduce motion"));
  gtk_switch_set_active(GTK_SWITCH(reduce_motion), TRUE);
  pump_main_loop(400);
  g_assert_true(get_reduce_motion_config_for(config_path));

  /* The theme dropdown carries a symbol. Pick macOS Mavericks by name, so
   * adding a theme to the form's list cannot silently break this test. */
  GtkWidget *theme = find_by_key(content, "dock-theme");
  g_assert_nonnull(theme);
  g_assert_true(GTK_IS_DROP_DOWN(theme));
  GListModel *theme_model = gtk_drop_down_get_model(GTK_DROP_DOWN(theme));
  guint mavericks_index = G_MAXUINT;
  for (guint i = 0; i < g_list_model_get_n_items(theme_model); i++) {
    g_autoptr(GtkStringObject) item = g_list_model_get_item(theme_model, i);
    if (g_strcmp0(gtk_string_object_get_string(item), "macOS Mavericks") == 0) {
      mavericks_index = i;
      break;
    }
  }
  g_assert_cmpuint(mavericks_index, <, G_MAXUINT);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(theme), mavericks_index);
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

  /* The launchpad section is wired to the launchpad settings too. */
  GtkWidget *launchpad_icon = find_by_key(content, "dock-launchpad-icon-size");
  g_assert_nonnull(launchpad_icon);
  gtk_range_set_value(GTK_RANGE(launchpad_icon), 96.0);
  pump_main_loop(400);
  g_assert_cmpint(get_launchpad_icon_size_config_for(config_path), ==, 96);

  GtkWidget *launchpad_blur = find_by_key(content, "dock-launchpad-blur?");
  g_assert_nonnull(launchpad_blur);
  gtk_switch_set_active(GTK_SWITCH(launchpad_blur), TRUE);
  pump_main_loop(400);
  g_assert_true(get_launchpad_blur_config_for(config_path));

  /* The launchpad's close button is a two-choice row, and it starts on the left
   * (the documented default). */
  GtkWidget *close_side =
      find_by_key(content, "dock-launchpad-close-side");
  g_assert_nonnull(close_side);
  g_assert_true(GTK_IS_DROP_DOWN(close_side));
  g_assert_false(get_launchpad_close_on_right_config_for(config_path));
  gtk_drop_down_set_selected(GTK_DROP_DOWN(close_side), 1);
  pump_main_loop(400);
  g_assert_true(get_launchpad_close_on_right_config_for(config_path));

  /* The "Dock items" card adds a widget to the dock: the item goes into the
   * layout override (never into the settings file), and the dock's items grow. */
  SCM items_fn = scm_c_public_ref("lambdock core", "get-dock-items");
  SCM items_before = scm_call_1(items_fn, scm_from_utf8_string(config_path));
  guint before = scm_to_uint(scm_length(items_before));

  GtkWidget *add_item = find_by_key(content, "dock-add-item");
  g_assert_nonnull(add_item);
  g_assert_true(GTK_IS_DROP_DOWN(add_item));

  /* Select "RAM usage" (the third row of the dropdown) and press Add. */
  gtk_drop_down_set_selected(GTK_DROP_DOWN(add_item), 2);
  GtkWidget *add_button =
      find_by_key(content, "dock-add-item-button");
  g_assert_nonnull(add_button);
  g_assert_true(GTK_IS_BUTTON(add_button));
  /* A click, as far as the button is concerned. gtk_widget_activate() does not
   * emit "clicked" for a GtkButton, so the signal is the honest way to press
   * it from a test. */
  g_signal_emit_by_name(add_button, "clicked");
  pump_main_loop(200);

  SCM items_after = scm_call_1(items_fn, scm_from_utf8_string(config_path));
  g_assert_cmpuint(scm_to_uint(scm_length(items_after)), ==, before + 1);

  /* The new item is the RAM gauge, and it was stored as a layout preset. */
  SCM last = scm_car(scm_reverse(items_after));
  SCM key_fn = scm_c_public_ref("lambdock core", "dock-item-key");
  char *last_key = scm_to_utf8_string(scm_call_1(key_fn, last));
  g_assert_cmpstr(last_key, ==, "dynamic:RAM Usage");
  free(last_key);

  char *layout_file = g_build_filename(dir, "lambdock", "layout-settings.scm",
                                       NULL);
  char *layout_text = NULL;
  g_assert_true(g_file_get_contents(layout_file, &layout_text, NULL, NULL));
  g_assert_nonnull(strstr(layout_text, "preset"));
  g_assert_nonnull(strstr(layout_text, "ram"));
  g_free(layout_text);
  g_free(layout_file);

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
  g_assert_nonnull(strstr(contents, "dock-launchpad-icon-size"));
  g_assert_nonnull(strstr(contents, "dock-launchpad-blur?"));
  g_assert_nonnull(strstr(contents, "dock-launchpad-close-side"));
  g_assert_nonnull(strstr(contents, "dock-reduce-motion?"));
  g_free(contents);

  /* Reset forgets the overrides; reloading restores the settings file. */
  reset_dock_preferences(config_path);
  load_dock_config_scm(config_path);
  g_assert_cmpint(get_icon_size_config_for(config_path), ==, 48);
  g_assert_false(get_autohide_config_for(config_path));

  free(prefs_path);
  gtk_window_destroy(GTK_WINDOW(window));
  remove_tree(dir);
  g_free(config_path);
  g_free(dir);

  g_print("# all preferences assertions passed\n");
}

int main(int argc, char **argv) {
  scm_boot_guile(argc, argv, inner_test_preferences, NULL);
  return 0;
}
