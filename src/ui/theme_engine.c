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

static char *scope_css_selectors(const char *css_text,
                                 const char *scope_class) {
  if (!css_text || *css_text == '\0')
    return g_strdup("");

  GString *result = g_string_new("");
  gchar **lines = g_strsplit(css_text, "\n", -1);
  gboolean in_ruleset = FALSE;

  for (int i = 0; lines[i] != NULL; i++) {
    char *line = lines[i];
    char *trimmed = g_strstrip(g_strdup(line));

    // Pass empty lines, comments, and @-rules through unchanged
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
          g_string_append_printf(result, ".%s %s", scope_class, sel);
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

void load_custom_css(LambdockState *state) {
  if (!state || !state->window)
    return;

  GdkDisplay *display = gdk_display_get_default();
  if (!display)
    return;

  const char *raw_theme = state->theme ? state->theme : "vanilla";
  const char *theme_name = (*raw_theme != '\0') ? raw_theme : "vanilla";

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
  if (state->user_css_provider) {
    gtk_style_context_remove_provider_for_display(
        display, GTK_STYLE_PROVIDER(state->user_css_provider));
    g_clear_object(&state->user_css_provider);
  }

  if (g_ascii_strcasecmp(theme_name, "none") == 0) {
    LOG_C_INFO("UI", "Theme set to 'none' for [%s].",
               state->config_path ? state->config_path : "default");
    return;
  }

  // Read raw CSS from GResource bundle
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
    char *scoped_css = scope_css_selectors(raw_css, theme_class);

    state->base_css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(state->base_css_provider, scoped_css);

    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(state->base_css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    LOG_C_INFO("UI", "Loaded & scoped CSS theme [%s] for [%s]", theme_name,
               state->config_path ? state->config_path : "default");

    g_free(scoped_css);
    g_bytes_unref(data);
  }
  g_free(resource_path);

  // USER OVERRIDE CSS (~/.config/lambdock/styles.css)
  const char *config_dir = g_get_user_config_dir();
  char *user_css_path =
      g_build_filename(config_dir, "lambdock", "styles.css", NULL);

  if (g_file_test(user_css_path, G_FILE_TEST_EXISTS)) {
    char *user_raw = NULL;
    if (g_file_get_contents(user_css_path, &user_raw, NULL, NULL)) {
      char *scoped_user_css = scope_css_selectors(user_raw, theme_class);

      state->user_css_provider = gtk_css_provider_new();
      gtk_css_provider_load_from_string(state->user_css_provider,
                                        scoped_user_css);

      gtk_style_context_add_provider_for_display(
          display, GTK_STYLE_PROVIDER(state->user_css_provider),
          GTK_STYLE_PROVIDER_PRIORITY_USER);

      LOG_C_INFO("UI", "Loaded & scoped user CSS override for [%s]",
                 state->config_path ? state->config_path : "default");

      g_free(scoped_user_css);
      g_free(user_raw);
    }
  }
  g_free(user_css_path);
}
