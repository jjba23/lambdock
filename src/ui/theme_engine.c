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

#include "theme_engine.h"
#include "../lambdock.h"
#include "../logger.h"
#include <gtk/gtk.h>

/* ============================================================================
 * SELECTOR SCOPING UTILITIES
 * ============================================================================
 */

/**
 * scope_css_selectors:
 * Transforms raw CSS rules to be scoped strictly under a target class wrapper.
 *
 * Examples with scope_class = "theme-unity":
 * - ".lambdock-window"  =>  ".theme-unity .lambdock-window"
 * - "window"            =>  "window.theme-unity"
 * - "window.background" =>  "window.theme-unity.background"
 */
char *theme_scope_css_selectors(const char *css_text,
                                const char *scope_class) {
  if (!css_text || *css_text == '\0')
    return g_strdup("");

  GString *result = g_string_new("");
  gchar **lines = g_strsplit(css_text, "\n", -1);
  gboolean in_ruleset = FALSE;

  for (int i = 0; lines[i] != NULL; i++) {
    char *line = lines[i];
    char *trimmed = g_strstrip(g_strdup(line));

    // Pass empty lines, comments, and top-level @-rules through unchanged
    if (strlen(trimmed) == 0 || g_str_has_prefix(trimmed, "/*") ||
        g_str_has_prefix(trimmed, "@") || g_str_has_prefix(trimmed, "*")) {
      g_string_append_printf(result, "%s\n", line);
      g_free(trimmed);
      continue;
    }

    if (!in_ruleset && strchr(line, '{')) {
      gchar **parts = g_strsplit(line, "{", 2);
      gchar **selectors = g_strsplit(parts[0], ",", -1);

      for (int j = 0; selectors[j] != NULL; j++) {
        char *sel = g_strstrip(selectors[j]);
        if (strlen(sel) > 0) {
          if (j > 0)
            g_string_append(result, ", ");

          // Compound the 'window' element selector directly with the scope
          // class, but only when 'window' is a whole element (e.g. "window",
          // "window.background"), never a different element whose name merely
          // starts with it (e.g. "windowcontrols"). The prefix check must come
          // first: reading sel[6] unconditionally overflows short selectors.
          if (g_str_has_prefix(sel, "window")) {
            char after = sel[6];
            if (after == '\0' || after == '.' || after == ':' ||
                after == '[' || g_ascii_isspace(after)) {
              g_string_append_printf(result, "window.%s%s", scope_class,
                                     sel + 6);
            } else {
              g_string_append_printf(result, ".%s %s", scope_class, sel);
            }
          } else {
            g_string_append_printf(result, ".%s %s", scope_class, sel);
          }
        }
      }
      g_string_append(result, " {\n");

      if (parts[1] && strchr(parts[1], '}')) {
        g_string_append_printf(result, "%s\n", parts[1]);
        in_ruleset = FALSE;
      } else {
        in_ruleset = TRUE;
      }

      g_strfreev(parts);
      g_strfreev(selectors);
    } else {
      if (strchr(line, '}')) {
        in_ruleset = FALSE;
      }
      g_string_append_printf(result, "%s\n", line);
    }
    g_free(trimmed);
  }

  g_strfreev(lines);
  return g_string_free(result, FALSE);
}

/* ============================================================================
 * HELPER: CSS PROVIDER GENERATOR
 * ============================================================================
 */

static GtkCssProvider *create_scoped_provider(GdkDisplay *display,
                                              const char *raw_css,
                                              const char *scope_class,
                                              guint priority) {
  if (!raw_css || *raw_css == '\0')
    return NULL;

  char *scoped_css = theme_scope_css_selectors(raw_css, scope_class);
  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, scoped_css);

  gtk_style_context_add_provider_for_display(
      display, GTK_STYLE_PROVIDER(provider), priority);

  g_free(scoped_css);
  return provider;
}

/* ============================================================================
 * PUBLIC ENGINE INTERFACE
 * ============================================================================
 */

void load_custom_css(LambdockState *state) {
  if (!state || !state->window)
    return;

  GdkDisplay *display = gdk_display_get_default();
  if (!display)
    return;

  const char *raw_theme =
      (state->theme && *state->theme != '\0') ? state->theme : "vanilla";
  const char *theme_name =
      (raw_theme && *raw_theme != '\0') ? raw_theme : "vanilla";

  // Formulate unique CSS class name for this theme (e.g. "theme-unity")
  char *theme_class = g_strdup_printf("theme-%s", theme_name);

  // Clean up previous theme classes on the window widget
  if (state->active_theme_class) {
    gtk_widget_remove_css_class(state->window, state->active_theme_class);
    g_free(state->active_theme_class);
  }
  state->active_theme_class = theme_class;
  gtk_widget_add_css_class(state->window, theme_class);

  // Unbind previous providers from display if present
  if (state->base_css_provider) {
    gtk_style_context_remove_provider_for_display(
        display, GTK_STYLE_PROVIDER(state->base_css_provider));
    g_clear_object(&state->base_css_provider);
  }
  if (state->taskbar_css_provider) {
    gtk_style_context_remove_provider_for_display(
        display, GTK_STYLE_PROVIDER(state->taskbar_css_provider));
    g_clear_object(&state->taskbar_css_provider);
  }
  if (state->user_css_provider) {
    gtk_style_context_remove_provider_for_display(
        display, GTK_STYLE_PROVIDER(state->user_css_provider));
    g_clear_object(&state->user_css_provider);
  }

  // Early return if theme is set to 'none'
  if (g_ascii_strcasecmp(theme_name, "none") == 0) {
    LOG_C_INFO("UI", "Theme set to 'none' for [%s].",
               state->config_path ? state->config_path : "default");
    return;
  }

  // --------------------------------------------------------------------------
  // 1a. TASKBAR / WINDOW-TITLE MODIFIER (loaded before the theme so theme rules
  // can still override equal-specificity modifiers)
  // --------------------------------------------------------------------------
  if (state->taskbar || state->show_window_titles) {
    GBytes *mod = g_resources_lookup_data(
        "/org/jointhefreeworld/lambdock/css/taskbar.css",
        G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
    if (mod) {
      gsize mod_size = 0;
      const char *mod_css = g_bytes_get_data(mod, &mod_size);
      state->taskbar_css_provider = create_scoped_provider(
          display, mod_css, theme_class,
          GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      LOG_C_INFO("UI", "Loaded taskbar/window-title modifier for [%s]",
                 state->config_path ? state->config_path : "default");
      g_bytes_unref(mod);
    }
  }

  // --------------------------------------------------------------------------
  // 1. BASE THEME (Loaded from GResource Bundle)
  // --------------------------------------------------------------------------
  char *resource_path =
      g_strdup_printf("/org/jointhefreeworld/lambdock/css/%s.css", theme_name);
  GBytes *data = g_resources_lookup_data(resource_path,
                                         G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);

  if (!data) {
    LOG_C_WARN("UI", "Theme resource '%s' not found, falling back to vanilla",
               resource_path);
    g_free(resource_path);
    resource_path = g_strdup("/org/jointhefreeworld/lambdock/css/vanilla.css");
    data = g_resources_lookup_data(resource_path, G_RESOURCE_LOOKUP_FLAGS_NONE,
                                   NULL);
  }

  if (data) {
    gsize size = 0;
    const char *raw_css = g_bytes_get_data(data, &size);

    state->base_css_provider = create_scoped_provider(
        display, raw_css, theme_class, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    LOG_C_INFO("UI", "Loaded & scoped CSS theme [%s] for [%s]", theme_name,
               state->config_path ? state->config_path : "default");

    g_bytes_unref(data);
  }
  g_free(resource_path);

  // --------------------------------------------------------------------------
  // 2. USER OVERRIDE CSS (~/.config/lambdock/styles.css)
  // --------------------------------------------------------------------------
  const char *config_dir = g_get_user_config_dir();
  char *user_css_path =
      g_build_filename(config_dir, "lambdock", "styles.css", NULL);

  if (g_file_test(user_css_path, G_FILE_TEST_EXISTS)) {
    char *user_raw = NULL;
    if (g_file_get_contents(user_css_path, &user_raw, NULL, NULL)) {
      state->user_css_provider = create_scoped_provider(
          display, user_raw, theme_class, GTK_STYLE_PROVIDER_PRIORITY_USER);

      LOG_C_INFO("UI", "Loaded & scoped user CSS override for [%s]",
                 state->config_path ? state->config_path : "default");

      g_free(user_raw);
    }
  }
  g_free(user_css_path);
}
