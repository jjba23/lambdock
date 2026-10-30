// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/ui/icon_resolver.h"
#include <glib/gstdio.h>
#include <gtk/gtk.h>

/* A valid 1x1 transparent PNG, used to exercise the absolute-file-path branch. */
static const char *PNG_1X1_BASE64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR42mNkYPhfDwAChwGA"
    "60e6kgAAAABJRU5ErkJggg==";

static GtkIconTheme *default_theme(void) {
  return gtk_icon_theme_get_for_display(gdk_display_get_default());
}

static void test_icon_resolver_null_and_empty(void) {
  GtkIconTheme *theme = default_theme();

  GtkWidget *img_null = create_image_for_icon(theme, NULL, 48);
  g_assert_null(img_null);

  GtkWidget *img_empty = create_image_for_icon(theme, "", 48);
  g_assert_null(img_empty);
}

static void test_icon_resolver_theme_icon(void) {
  GtkIconTheme *theme = default_theme();

  if (!gtk_icon_theme_has_icon(theme, "utilities-terminal")) {
    g_test_skip("icon theme does not provide utilities-terminal");
    return;
  }

  GtkWidget *img = create_image_for_icon(theme, "utilities-terminal", 40);
  g_assert_nonnull(img);
  g_assert_true(GTK_IS_IMAGE(img));
  g_assert_cmpint(gtk_image_get_pixel_size(GTK_IMAGE(img)), ==, 40);

  g_object_ref_sink(img);
  g_object_unref(img);
}

static void test_icon_resolver_resource_uri(void) {
  GtkIconTheme *theme = default_theme();
  const char *uri = "resource:///org/jointhefreeworld/lambdock/img/agnus-dei.png";

  GtkWidget *img = create_image_for_icon(theme, uri, 24);
  g_assert_nonnull(img);
  g_assert_true(GTK_IS_IMAGE(img));
  g_assert_cmpint(gtk_image_get_pixel_size(GTK_IMAGE(img)), ==, 24);

  g_object_ref_sink(img);
  g_object_unref(img);
}

static void test_icon_resolver_embedded_resource(void) {
  GtkIconTheme *theme = default_theme();
  const char *path = "/org/jointhefreeworld/lambdock/img/agnus-dei.png";

  GtkWidget *img = create_image_for_icon(theme, path, 32);
  g_assert_nonnull(img);
  g_assert_true(GTK_IS_IMAGE(img));
  g_assert_cmpint(gtk_image_get_pixel_size(GTK_IMAGE(img)), ==, 32);

  g_object_ref_sink(img);
  g_object_unref(img);
}

static void test_icon_resolver_file_path(void) {
  GtkIconTheme *theme = default_theme();

  gsize len = 0;
  guchar *png = g_base64_decode(PNG_1X1_BASE64, &len);
  g_assert_nonnull(png);

  char *dir = g_dir_make_tmp("lambdock-icons-XXXXXX", NULL);
  g_assert_nonnull(dir);
  char *file = g_build_filename(dir, "icon.png", NULL);
  g_assert_true(g_file_set_contents(file, (const char *)png, (gssize)len, NULL));
  g_free(png);

  GtkWidget *img = create_image_for_icon(theme, file, 16);
  g_assert_nonnull(img);
  g_assert_true(GTK_IS_IMAGE(img));
  g_assert_cmpint(gtk_image_get_pixel_size(GTK_IMAGE(img)), ==, 16);

  g_object_ref_sink(img);
  g_object_unref(img);

  g_remove(file);
  g_rmdir(dir);
  g_free(file);
  g_free(dir);
}

static void test_icon_resolver_fallback_executable(void) {
  GtkIconTheme *theme = default_theme();

  // Tell GLib test runner to expect the fallback warning instead of crashing
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                        "*Failed to resolve icon*");

  GtkWidget *img =
      create_image_for_icon(theme, "this-icon-does-not-exist-xyz", 32);
  g_assert_nonnull(img);
  g_assert_true(GTK_IS_IMAGE(img));
  g_assert_cmpint(gtk_image_get_pixel_size(GTK_IMAGE(img)), ==, 32);

  // Verify that the expected warning was indeed caught
  g_test_assert_expected_messages();

  g_object_ref_sink(img);
  g_object_unref(img);
}

static void test_setup_icon_theme_smoke(void) {
  /* Must add search paths / resolve a theme name without crashing. */
  setup_icon_theme();
}

/* Removes a throwaway directory tree (the tests that build one need their own
 * copy: this suite must not depend on the stack or preferences helpers). */
static void remove_tree(const char *path) {
  GDir *dir = g_dir_open(path, 0, NULL);
  if (dir != NULL) {
    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
      char *child = g_build_filename(path, name, NULL);
      if (g_file_test(child, G_FILE_TEST_IS_DIR))
        remove_tree(child);
      else
        g_unlink(child);
      g_free(child);
    }
    g_dir_close(dir);
  }
  g_rmdir(path);
}

/* The search paths are built from explicit inputs, so the rule "system dirs,
 * then the user's own, only if they exist" can be checked without touching the
 * process environment. */
static void test_search_paths(void) {
  char *root = g_dir_make_tmp("lambdock-icons-paths-XXXXXX", NULL);
  g_assert_nonnull(root);

  char *system_a = g_build_filename(root, "a", NULL);
  char *system_b = g_build_filename(root, "b", NULL);
  char *system_a_icons = g_build_filename(system_a, "icons", NULL);
  g_assert_cmpint(g_mkdir_with_parents(system_a_icons, 0755), ==, 0);
  g_assert_cmpint(g_mkdir_with_parents(system_b, 0755), ==, 0);

  char *home = g_build_filename(root, "home", NULL);
  char *xdg_icons = g_build_filename(home, ".local", "share", "icons", NULL);
  char *legacy_icons = g_build_filename(home, ".icons", NULL);
  g_assert_cmpint(g_mkdir_with_parents(legacy_icons, 0755), ==, 0);

  const char *dirs[] = {system_a, system_b, NULL};

  /* b has no icons/ subdirectory, and the XDG one does not exist yet: only the
   * existing directories are listed, in search order. */
  g_auto(GStrv) paths = lambdock_icon_search_paths_build(dirs, home);
  g_assert_cmpuint(g_strv_length(paths), ==, 2);
  g_assert_cmpstr(paths[0], ==, system_a_icons);
  g_assert_cmpstr(paths[1], ==, legacy_icons);

  /* Creating the XDG directory slots it in before the legacy one. */
  g_assert_cmpint(g_mkdir_with_parents(xdg_icons, 0755), ==, 0);
  g_auto(GStrv) with_xdg = lambdock_icon_search_paths_build(dirs, home);
  g_assert_cmpuint(g_strv_length(with_xdg), ==, 3);
  g_assert_cmpstr(with_xdg[0], ==, system_a_icons);
  g_assert_cmpstr(with_xdg[1], ==, xdg_icons);
  g_assert_cmpstr(with_xdg[2], ==, legacy_icons);

  /* No inputs at all is an empty list, not a NULL one. */
  g_auto(GStrv) none = lambdock_icon_search_paths_build(NULL, NULL);
  g_assert_nonnull(none);
  g_assert_cmpuint(g_strv_length(none), ==, 0);
  g_auto(GStrv) no_home = lambdock_icon_search_paths_build(dirs, "");
  g_assert_cmpuint(g_strv_length(no_home), ==, 1);

  /* And the process-wide list only names directories that exist. */
  g_auto(GStrv) live = lambdock_icon_search_paths_new();
  g_assert_nonnull(live);
  for (int i = 0; live[i] != NULL; i++)
    g_assert_true(g_file_test(live[i], G_FILE_TEST_IS_DIR));

  g_free(system_a);
  g_free(system_b);
  g_free(system_a_icons);
  g_free(home);
  g_free(xdg_icons);
  g_free(legacy_icons);

  remove_tree(root);
  g_free(root);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  if (!gtk_init_check()) {
    g_print("Skipping icon resolver tests: No display server available\n");
    return 77; // GLib/Meson standard exit code for SKIPPED
  }

  g_test_add_func("/ui/icon_resolver/null_empty",
                  test_icon_resolver_null_and_empty);
  g_test_add_func("/ui/icon_resolver/theme_icon",
                  test_icon_resolver_theme_icon);
  g_test_add_func("/ui/icon_resolver/resource_uri",
                  test_icon_resolver_resource_uri);
  g_test_add_func("/ui/icon_resolver/embedded_resource",
                  test_icon_resolver_embedded_resource);
  g_test_add_func("/ui/icon_resolver/search_paths", test_search_paths);
  g_test_add_func("/ui/icon_resolver/file_path",
                  test_icon_resolver_file_path);
  g_test_add_func("/ui/icon_resolver/fallback",
                  test_icon_resolver_fallback_executable);
  g_test_add_func("/ui/icon_resolver/setup_smoke",
                  test_setup_icon_theme_smoke);

  return g_test_run();
}
