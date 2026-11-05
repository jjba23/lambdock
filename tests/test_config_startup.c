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
#include "../src/ui/dock_item.h"
#include "../src/ui/shelf.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <libguile.h>
#include <math.h>
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
             "(define dock-item-spacing 7)\n"
             "(define dock-capsule-padding 9)\n"
             "(define dock-shelf-margin 24)\n"
             "(define dock-shelf-front 14)\n"
             "(define dock-reflection 0.3)\n"
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
  write_file(prefs, "((dock-theme . platinum-dark)\n"
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

/* Startup and a hot reload must read the settings through the same reader: when
 * they each read their own subset, the CSS could be built from an icon size the
 * reload had not updated yet, and a key added to one path was silently missing
 * from the other. */
static void check_settings_read_in_one_pass(void) {
  LambdockState state = {0};
  state.config_path = config_path;

  dock_read_settings(&state);

  g_assert_cmpstr(state.theme, ==, "platinum-dark");
  g_assert_cmpint(state.icon_size, ==, 48);
  g_assert_cmpint(state.item_spacing, ==, 7);
  g_assert_cmpint(state.capsule_padding, ==, 9);
  g_assert_false(state.taskbar);
  g_assert_false(state.show_window_titles);
  g_assert_cmpint(state.shelf.margin_px, ==, -18);
  g_assert_cmpint(state.shelf.front_px, ==, 14);

  /* The stylesheet asks the shelf for its band from this icon size, so both
   * have to be current by the time load_custom_css() runs. */
  g_assert_cmpint(lambdock_shelf_geometry_for(&state, state.icon_size).band_px,
                  ==, (int)lround(48 * 0.3));

  g_free(state.theme);
}

static void check_overrides_apply(void) {
  g_autofree char *theme = get_theme_config_for(config_path);
  g_assert_cmpstr(theme, ==, "platinum-dark");

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

/* The row is built after the widgets it goes into, so the pass that derives the
 * layout from the row has to run once the row exists. Before it did, every
 * reflection slot stayed hidden -- the shelf numbers were read correctly but
 * only reached the row on the next reload -- and the icons sat a band lower
 * than the shelf they were configured for. */
static void check_row_layout_is_derived(void) {
  if (!gtk_init_check()) {
    g_print("Skipping the row layout check: no display server available\n");
    return;
  }

  LambdockState state = {0};

  state.config_path = config_path;
  state.edge = GTK_LAYER_SHELL_EDGE_BOTTOM;
  state.orientation = GTK_ORIENTATION_HORIZONTAL;
  state.icon_size = 48;
  state.capsule_padding = 8;
  state.overlay =
      g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  state.bg_capsule =
      g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  state.scrolled_window = g_object_ref_sink(gtk_scrolled_window_new());
  state.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(state.scrolled_window),
                                state.box);

  get_shelf_config_for(config_path, &state.shelf);

  populate_dock_items_for(&state);
  g_assert_cmpint(g_list_length(state.dock_items), >, 0);

  int band = (int)lround(
      (double)state.icon_size *
      lambdock_shelf_params_effective(&state).reflection_ratio);
  g_assert_cmpint(band, >, 0);

  for (GList *l = state.dock_items; l != NULL; l = l->next) {
    DockItem *item = l->data;

    g_assert_nonnull(item->reflection);
    g_assert_true(gtk_widget_get_visible(item->reflection));

    int width = 0;
    int height = 0;
    gtk_widget_get_size_request(item->reflection, &width, &height);
    g_assert_cmpint(width, ==, state.icon_size);
    g_assert_cmpint(height, ==, band);
  }

  clear_favorite_dock_items(&state);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(state.scrolled_window),
                                NULL);
  g_object_unref(state.scrolled_window);
  g_object_unref(state.bg_capsule);
  g_object_unref(state.overlay);
}

static void load_and_check(void *data) {
  (void)data;

  setup_guile_load_path();
  scm_c_use_module("lambdock core");

  /* This is what main.c's reconcile_settings_file() does before spawning a dock. */
  load_dock_config_scm(config_path);

  check_overrides_apply();
  check_settings_read_in_one_pass();
  check_row_layout_is_derived();
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
