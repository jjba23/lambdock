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
#include <string.h>

/* ============================================================================
 * SELECTOR SCOPING
 * ============================================================================ */

static void test_scope_null_and_empty(void) {
  char *out = theme_scope_css_selectors(NULL, "theme-x");
  g_assert_cmpstr(out, ==, "");
  g_free(out);

  out = theme_scope_css_selectors("", "theme-x");
  g_assert_cmpstr(out, ==, "");
  g_free(out);
}

static void test_scope_class_selectors(void) {
  /* Class selectors are nested under the scope wrapper. */
  char *out =
      theme_scope_css_selectors(".lambdock-window {\n  color: red;\n}\n",
                                "theme-x");
  g_assert_nonnull(strstr(out, ".theme-x .lambdock-window {"));
  g_assert_nonnull(strstr(out, "color: red;"));
  g_free(out);

  /* Single-line rule bodies are handled too. */
  out = theme_scope_css_selectors(".foo { color: red; }", "theme-x");
  g_assert_nonnull(strstr(out, ".theme-x .foo {"));
  g_free(out);
}

static void test_scope_window_element_boundary(void) {
  /* The `window` element selector is compounded, not nested, and ONLY when
   * `window` is a complete element name -- a selector such as
   * `windowcontrols` must keep its `.theme-x ` prefix. */
  const char *css = "window {\n  background: transparent;\n}\n"
                    "window.background {\n  a: b;\n}\n"
                    "window:hover {\n  c: d;\n}\n"
                    "window[foo] {\n  e: f;\n}\n"
                    "windowcontrols {\n  g: h;\n}\n"
                    ".window {\n  i: j;\n}\n";

  char *out = theme_scope_css_selectors(css, "theme-x");

  g_assert_nonnull(strstr(out, "window.theme-x {"));
  g_assert_nonnull(strstr(out, "window.theme-x.background"));
  g_assert_nonnull(strstr(out, "window.theme-x:hover"));
  g_assert_nonnull(strstr(out, "window.theme-x[foo]"));

  g_assert_nonnull(strstr(out, ".theme-x windowcontrols"));
  g_assert_null(strstr(out, "window.theme-xcontrols"));

  /* `.window` is a class, not the element. */
  g_assert_nonnull(strstr(out, ".theme-x .window"));

  g_free(out);
}

static void test_scope_multiple_selectors(void) {
  char *out = theme_scope_css_selectors("button, .foo, window {\n  a: b;\n}",
                                        "theme-x");
  g_assert_nonnull(strstr(out, ".theme-x button, .theme-x .foo, "
                               "window.theme-x {"));
  g_free(out);
}

static void test_scope_passthrough(void) {
  const char *css = "/* a comment */\n"
                    "@import \"other.css\";\n"
                    "* {\n  margin: 0;\n}\n";
  char *out = theme_scope_css_selectors(css, "theme-x");

  g_assert_nonnull(strstr(out, "/* a comment */"));
  g_assert_nonnull(strstr(out, "@import \"other.css\";"));
  /* Star rules are passed through verbatim. */
  g_assert_nonnull(strstr(out, "* {"));
  g_free(out);
}

/* ============================================================================
 * PROVIDER LIFECYCLE
 * ============================================================================ */

static void test_load_custom_css_guards(void) {
  load_custom_css(NULL);

  LambdockState no_window = {0};
  no_window.theme = g_strdup("vanilla");
  load_custom_css(&no_window);
  g_assert_null(no_window.base_css_provider);
  g_free(no_window.theme);
}

static void test_load_custom_css_default_theme(void) {
  LambdockState state = {0};
  state.window = gtk_window_new();
  state.config_path = g_strdup("test_settings.scm");
  /* An unset theme must fall back to vanilla. */
  state.theme = NULL;

  load_custom_css(&state);

  g_assert_cmpstr(state.active_theme_class, ==, "theme-vanilla");
  g_assert_true(gtk_widget_has_css_class(state.window, "theme-vanilla"));
  g_assert_nonnull(state.base_css_provider);

  /* The shipped stylesheet uses both `window {` and `.lambdock-window {`, so
   * the scoped provider must show both forms of scoping. */
  char *provider_css = gtk_css_provider_to_string(state.base_css_provider);
  g_assert_nonnull(strstr(provider_css, "window.theme-vanilla"));
  g_assert_nonnull(strstr(provider_css, ".theme-vanilla .lambdock-window"));
  g_free(provider_css);

  GdkDisplay *display = gdk_display_get_default();
  gtk_style_context_remove_provider_for_display(
      display, GTK_STYLE_PROVIDER(state.base_css_provider));
  g_clear_object(&state.base_css_provider);
  g_free(state.config_path);
  g_free(state.theme);
  g_free(state.active_theme_class);
  g_object_ref_sink(state.window);
  g_object_unref(state.window);
}

static void test_load_custom_css_switching(void) {
  LambdockState state = {0};
  state.window = gtk_window_new();
  state.config_path = g_strdup("test_settings.scm");
  state.theme = g_strdup("vanilla");

  load_custom_css(&state);
  g_assert_true(gtk_widget_has_css_class(state.window, "theme-vanilla"));
  GtkCssProvider *first = state.base_css_provider;
  g_assert_nonnull(first);

  /* Switching themes must drop the previous class and provider. */
  g_free(state.theme);
  state.theme = g_strdup("snow-leopard");
  load_custom_css(&state);

  g_assert_cmpstr(state.active_theme_class, ==, "theme-snow-leopard");
  g_assert_true(gtk_widget_has_css_class(state.window, "theme-snow-leopard"));
  g_assert_false(gtk_widget_has_css_class(state.window, "theme-vanilla"));
  g_assert_true(state.base_css_provider != first);

  GdkDisplay *display = gdk_display_get_default();
  gtk_style_context_remove_provider_for_display(
      display, GTK_STYLE_PROVIDER(state.base_css_provider));
  g_clear_object(&state.base_css_provider);
  g_free(state.config_path);
  g_free(state.theme);
  g_free(state.active_theme_class);
  g_object_ref_sink(state.window);
  g_object_unref(state.window);
}

static void test_load_custom_css_none(void) {
  LambdockState state = {0};
  state.window = gtk_window_new();
  state.config_path = g_strdup("test_settings.scm");
  state.theme = g_strdup("vanilla");

  load_custom_css(&state);
  g_assert_nonnull(state.base_css_provider);

  /* `none` must clear any bound providers. */
  g_free(state.theme);
  state.theme = g_strdup("none");
  load_custom_css(&state);

  g_assert_cmpstr(state.active_theme_class, ==, "theme-none");
  g_assert_null(state.base_css_provider);
  g_assert_null(state.user_css_provider);

  g_free(state.config_path);
  g_free(state.theme);
  g_free(state.active_theme_class);
  g_object_ref_sink(state.window);
  g_object_unref(state.window);
}

static void test_load_custom_css_unknown_theme_falls_back(void) {
  LambdockState state = {0};
  state.window = gtk_window_new();
  state.config_path = g_strdup("test_settings.scm");
  state.theme = g_strdup("definitely-not-a-theme");

  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                        "*Theme resource*not found*");
  load_custom_css(&state);
  g_test_assert_expected_messages();

  g_assert_cmpstr(state.active_theme_class, ==,
                  "theme-definitely-not-a-theme");
  g_assert_nonnull(state.base_css_provider); /* vanilla fallback */

  GdkDisplay *display = gdk_display_get_default();
  gtk_style_context_remove_provider_for_display(
      display, GTK_STYLE_PROVIDER(state.base_css_provider));
  g_clear_object(&state.base_css_provider);
  g_free(state.config_path);
  g_free(state.theme);
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

  g_test_add_func("/ui/theme_engine/scope_null_empty",
                  test_scope_null_and_empty);
  g_test_add_func("/ui/theme_engine/scope_class_selectors",
                  test_scope_class_selectors);
  g_test_add_func("/ui/theme_engine/scope_window_boundary",
                  test_scope_window_element_boundary);
  g_test_add_func("/ui/theme_engine/scope_multiple_selectors",
                  test_scope_multiple_selectors);
  g_test_add_func("/ui/theme_engine/scope_passthrough", test_scope_passthrough);
  g_test_add_func("/ui/theme_engine/load_guards",
                  test_load_custom_css_guards);
  g_test_add_func("/ui/theme_engine/load_default_theme",
                  test_load_custom_css_default_theme);
  g_test_add_func("/ui/theme_engine/load_switching",
                  test_load_custom_css_switching);
  g_test_add_func("/ui/theme_engine/load_none",
                  test_load_custom_css_none);
  g_test_add_func("/ui/theme_engine/load_unknown_falls_back",
                  test_load_custom_css_unknown_theme_falls_back);

  return g_test_run();
}
