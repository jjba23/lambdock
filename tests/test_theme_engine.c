// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/lambdock.h"
#include "../src/ui/theme_engine.h"
#include <gtk/gtk.h>
#include <string.h>

/* ============================================================================
 * THEME REQUEST (pure: same arguments, same value)
 * ============================================================================ */

static void test_theme_request_defaults(void) {
  /* A dock with no theme configured gets vanilla, not an empty class. */
  g_auto(LambdockThemeRequest) request =
      lambdock_theme_request(NULL, FALSE, FALSE, FALSE);

  g_assert_cmpstr(request.theme_name, ==, "vanilla");
  g_assert_cmpstr(request.theme_class, ==, "theme-vanilla");
  g_assert_false(request.disabled);
  g_assert_false(request.taskbar_modifier);
  g_assert_false(request.shelf_modifier);
}

static void test_theme_request_normalises(void) {
  g_auto(LambdockThemeRequest) empty =
      lambdock_theme_request("", FALSE, FALSE, FALSE);
  g_assert_cmpstr(empty.theme_name, ==, "vanilla");

  g_auto(LambdockThemeRequest) unity =
      lambdock_theme_request("unity", FALSE, FALSE, FALSE);
  g_assert_cmpstr(unity.theme_name, ==, "unity");
  g_assert_cmpstr(unity.theme_class, ==, "theme-unity");
  g_assert_false(unity.disabled);
}

static void test_theme_request_none_disables(void) {
  g_auto(LambdockThemeRequest) none =
      lambdock_theme_request("none", TRUE, TRUE, TRUE);

  /* 'none' still names a class (so the old one can be swapped out) but loads
   * nothing, whatever the modifiers say. */
  g_assert_true(none.disabled);
  g_assert_cmpstr(none.theme_class, ==, "theme-none");

  g_auto(LambdockThemeRequest) upper =
      lambdock_theme_request("NONE", FALSE, FALSE, FALSE);
  g_assert_true(upper.disabled);
}

static void test_theme_request_modifiers(void) {
  /* Taskbar mode needs the modifier... */
  g_auto(LambdockThemeRequest) taskbar =
      lambdock_theme_request("vanilla", TRUE, FALSE, FALSE);
  g_assert_true(taskbar.taskbar_modifier);

  /* ...and so do window titles, on their own. */
  g_auto(LambdockThemeRequest) titles =
      lambdock_theme_request("vanilla", FALSE, TRUE, TRUE);
  g_assert_true(titles.taskbar_modifier);
  g_assert_true(titles.shelf_modifier);

  /* Neither means neither. */
  g_auto(LambdockThemeRequest) plain =
      lambdock_theme_request("vanilla", FALSE, FALSE, FALSE);
  g_assert_false(plain.taskbar_modifier);
  g_assert_false(plain.shelf_modifier);
}

/* The cleanup has to be safe to run twice, because ~g_auto~ runs it on the way
 * out even for a request whose strings were already released. */
static void test_theme_request_clear_is_idempotent(void) {
  LambdockThemeRequest request =
      lambdock_theme_request("unity", FALSE, FALSE, FALSE);

  lambdock_theme_request_clear(&request);
  g_assert_null(request.theme_name);
  g_assert_null(request.theme_class);

  lambdock_theme_request_clear(&request);
  lambdock_theme_request_clear(NULL);
}

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

/* A selector list may span several lines. Every selector in it still has to be
 * scoped: an unscoped rule applies to every dock (and to the whole app), which
 * is how one theme's indicator once ended up painted under another's icons. */
static void test_scope_multiline_selectors(void) {
  const char *css = "button.window-focused,\n"
                    "button.window-focused:hover {\n"
                    "  color: red;\n"
                    "}\n";
  char *out = theme_scope_css_selectors(css, "theme-x");

  g_assert_nonnull(strstr(out, ".theme-x button.window-focused, "
                               ".theme-x button.window-focused:hover {"));
  g_assert_null(strstr(out, "\nbutton.window-focused,"));

  g_free(out);
}

static void test_scope_multiline_selectors_with_window(void) {
  const char *css = "window,\n"
                    ".lambdock-window.shelf-mode {\n"
                    "  a: b;\n"
                    "}\n";
  char *out = theme_scope_css_selectors(css, "theme-x");

  g_assert_nonnull(strstr(out, "window.theme-x, "
                               ".theme-x .lambdock-window.shelf-mode {"));
  g_assert_null(strstr(out, "\nwindow,"));
  g_free(out);
}

/* The block body still passes through, including a one-line rule. */
static void test_scope_multiline_body_kept(void) {
  const char *css = ".a,\n.b { color: red; }\n.c {\n  d: e;\n}\n";
  char *out = theme_scope_css_selectors(css, "theme-x");

  g_assert_nonnull(strstr(out, ".theme-x .a, .theme-x .b { color: red; }"));
  g_assert_nonnull(strstr(out, ".theme-x .c {"));
  g_assert_nonnull(strstr(out, "  d: e;"));
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
  /* Hold a reference so the new provider cannot reuse the old object's address:
   * pointer identity is the point of this assertion, and the allocator is free
   * to hand the just-freed block straight back. */
  GtkCssProvider *first =
      state.base_css_provider ? g_object_ref(state.base_css_provider) : NULL;
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
  g_object_unref(first);
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

/* ============================================================================
 * SHIPPED STYLESHEETS: PARSE VALIDITY
 * ============================================================================ */

static void on_css_parse_error(GtkCssProvider *provider, GtkCssSection *section,
                               const GError *error, gpointer user_data) {
  (void)provider;
  (void)section;
  guint *count = (guint *)user_data;
  (*count)++;
  g_test_message("CSS parse error: %s",
                 (error && error->message) ? error->message : "unknown");
}

/* Every CSS resource we ship (themes + the taskbar modifier) must parse with
 * zero errors once scoped, so a typo cannot silently break a theme at runtime. */
static void test_all_shipped_css_parse(void) {
  char **entries = g_resources_enumerate_children(
      "/org/jointhefreeworld/lambdock/css", G_RESOURCE_LOOKUP_FLAGS_NONE,
      NULL);
  g_assert_nonnull(entries);

  guint stylesheets = 0;
  for (int i = 0; entries[i] != NULL; i++) {
    if (!g_str_has_suffix(entries[i], ".css"))
      continue;

    char *path =
        g_strdup_printf("/org/jointhefreeworld/lambdock/css/%s", entries[i]);
    GBytes *data =
        g_resources_lookup_data(path, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
    g_assert_nonnull(data);

    gsize size = 0;
    const char *raw = g_bytes_get_data(data, &size);
    char *scoped = theme_scope_css_selectors(raw, "theme-test");

    GtkCssProvider *provider = gtk_css_provider_new();
    guint errors = 0;
    gulong handler = g_signal_connect(provider, "parsing-error",
                                      G_CALLBACK(on_css_parse_error), &errors);
    gtk_css_provider_load_from_string(provider, scoped);
    g_signal_handler_disconnect(provider, handler);

    if (errors > 0)
      g_test_message("stylesheet %s produced %u parse error(s)", entries[i],
                     errors);
    g_assert_cmpuint(errors, ==, 0);

    g_object_unref(provider);
    g_free(scoped);
    g_bytes_unref(data);
    g_free(path);
    stylesheets++;
  }
  g_strfreev(entries);

  /* We ship several themes plus the taskbar modifier. */
  g_assert_cmpuint(stylesheets, >=, 6);
}

/* ============================================================================
 * TASKBAR / WINDOW-TITLE MODIFIER
 * ============================================================================ */

static void test_load_custom_css_taskbar_modifier(void) {
  LambdockState state = {0};
  state.window = gtk_window_new();
  state.config_path = g_strdup("test_settings.scm");
  state.theme = g_strdup("vanilla");
  state.taskbar = TRUE;

  load_custom_css(&state);
  g_assert_nonnull(state.taskbar_css_provider);

  char *css = gtk_css_provider_to_string(state.taskbar_css_provider);
  /* GTK normalizes the compound class selector to `.taskbar.lambdock-window`. */
  g_assert_nonnull(strstr(css, ".theme-vanilla .taskbar.lambdock-window"));
  g_free(css);

  /* Disabling taskbar mode drops the modifier on reload. */
  state.taskbar = FALSE;
  state.show_window_titles = FALSE;
  load_custom_css(&state);
  g_assert_null(state.taskbar_css_provider);

  /* Enabling window titles alone is enough to load it. */
  state.show_window_titles = TRUE;
  load_custom_css(&state);
  g_assert_nonnull(state.taskbar_css_provider);

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

  g_test_add_func("/ui/theme_engine/request_defaults",
                  test_theme_request_defaults);
  g_test_add_func("/ui/theme_engine/request_normalises",
                  test_theme_request_normalises);
  g_test_add_func("/ui/theme_engine/request_none",
                  test_theme_request_none_disables);
  g_test_add_func("/ui/theme_engine/request_modifiers",
                  test_theme_request_modifiers);
  g_test_add_func("/ui/theme_engine/request_clear_idempotent",
                  test_theme_request_clear_is_idempotent);
  g_test_add_func("/ui/theme_engine/scope_null_empty",
                  test_scope_null_and_empty);
  g_test_add_func("/ui/theme_engine/scope_class_selectors",
                  test_scope_class_selectors);
  g_test_add_func("/ui/theme_engine/scope_window_boundary",
                  test_scope_window_element_boundary);
  g_test_add_func("/ui/theme_engine/scope_multiple_selectors",
                  test_scope_multiple_selectors);
  g_test_add_func("/ui/theme_engine/scope_multiline_selectors",
                  test_scope_multiline_selectors);
  g_test_add_func("/ui/theme_engine/scope_multiline_with_window",
                  test_scope_multiline_selectors_with_window);
  g_test_add_func("/ui/theme_engine/scope_multiline_body_kept",
                  test_scope_multiline_body_kept);
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
  g_test_add_func("/ui/theme_engine/all_shipped_css_parse",
                  test_all_shipped_css_parse);
  g_test_add_func("/ui/theme_engine/load_taskbar_modifier",
                  test_load_custom_css_taskbar_modifier);

  return g_test_run();
}
