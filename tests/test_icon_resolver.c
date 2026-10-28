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
  g_test_add_func("/ui/icon_resolver/file_path",
                  test_icon_resolver_file_path);
  g_test_add_func("/ui/icon_resolver/fallback",
                  test_icon_resolver_fallback_executable);
  g_test_add_func("/ui/icon_resolver/setup_smoke",
                  test_setup_icon_theme_smoke);

  return g_test_run();
}
