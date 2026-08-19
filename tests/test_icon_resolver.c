// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock. If not, see <https://www.gnu.org/licenses/>.

#include "../src/ui/icon_resolver.h"
#include <gtk/gtk.h>

static void test_icon_resolver_null_and_empty(void) {
  GtkIconTheme *theme =
      gtk_icon_theme_get_for_display(gdk_display_get_default());

  GtkWidget *img_null = create_image_for_icon(theme, NULL, 48);
  g_assert_null(img_null);

  GtkWidget *img_empty = create_image_for_icon(theme, "", 48);
  g_assert_null(img_empty);
}

static void test_icon_resolver_fallback_executable(void) {
  GtkIconTheme *theme =
      gtk_icon_theme_get_for_display(gdk_display_get_default());

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

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  if (!gtk_init_check()) {
    g_print("Skipping icon resolver tests: No display server available\n");
    return 77; // GLib/Meson standard exit code for SKIPPED
  }

  g_test_add_func("/ui/icon_resolver/null_empty",
                  test_icon_resolver_null_and_empty);
  g_test_add_func("/ui/icon_resolver/fallback",
                  test_icon_resolver_fallback_executable);

  return g_test_run();
}
