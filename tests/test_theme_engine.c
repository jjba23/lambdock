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

#include "../src/lambdock.h"
#include "../src/ui/theme_engine.h"
#include <gtk/gtk.h>

static void test_theme_engine_load_custom_css(void) {
  LambdockState state = {0};
  state.window = gtk_window_new();
  state.theme = g_strdup("vanilla");
  state.config_path = g_strdup("test_settings.scm");

  load_custom_css(&state);

  // Verify active theme class assigned to window
  g_assert_cmpstr(state.active_theme_class, ==, "theme-vanilla");
  g_assert_true(gtk_widget_has_css_class(state.window, "theme-vanilla"));

  // Switch theme to 'none' and verify cleanup
  g_free(state.theme);
  state.theme = g_strdup("none");
  load_custom_css(&state);

  g_assert_cmpstr(state.active_theme_class, ==, "theme-none");
  g_assert_null(state.base_css_provider);

  g_free(state.theme);
  g_free(state.config_path);
  g_free(state.active_theme_class);
  g_object_ref_sink(state.window);
  g_object_unref(state.window);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  if (!gtk_init_check()) {
    g_print("Skipping theme engine tests: No display server available\n");
    return 77;
  }

  g_test_add_func("/ui/theme_engine/load_custom_css",
                  test_theme_engine_load_custom_css);

  return g_test_run();
}
