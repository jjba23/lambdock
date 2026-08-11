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

void load_custom_css(LambdockState *state) {
  if (!state || !state->window)
    return;

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  GtkStyleContext *context = gtk_widget_get_style_context(state->window);
  const char *raw_theme = (state && state->theme) ? state->theme : NULL;
  const char *theme_name =
      (raw_theme && *raw_theme != '\0') ? raw_theme : "vanilla";

  // If theme is 'none', remove and unbind providers
  if (g_ascii_strcasecmp(theme_name, "none") == 0) {
    LOG_C_INFO("UI",
               "Theme set to 'none' for [%s]. Removing GTK CSS providers.",
               state->config_path ? state->config_path : "default");
    if (state->base_css_provider) {
      gtk_style_context_remove_provider(
          context, GTK_STYLE_PROVIDER(state->base_css_provider));
      g_clear_object(&state->base_css_provider);
    }
    if (state->user_css_provider) {
      gtk_style_context_remove_provider(
          context, GTK_STYLE_PROVIDER(state->user_css_provider));
      g_clear_object(&state->user_css_provider);
    }
    return;
  }

  // BASE THEME (Attached strictly to this dock window context)
  if (!state->base_css_provider) {
    state->base_css_provider = gtk_css_provider_new();
    gtk_style_context_add_provider(context,
                                   GTK_STYLE_PROVIDER(state->base_css_provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }

  char *resource_path =
      g_strdup_printf("/org/jointhefreeworld/lambdock/css/%s.css", theme_name);
  if (g_resources_get_info(resource_path, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL,
                           NULL, NULL)) {
    LOG_C_INFO("UI", "Loading base CSS theme for [%s] from GResource: %s",
               state->config_path ? state->config_path : "default",
               resource_path);
    gtk_css_provider_load_from_resource(state->base_css_provider,
                                        resource_path);
  } else {
    LOG_C_WARN("UI", "Theme resource '%s' not found, falling back to vanilla",
               resource_path);
    gtk_css_provider_load_from_resource(
        state->base_css_provider,
        "/org/jointhefreeworld/lambdock/css/vanilla.css");
  }
  g_free(resource_path);

  // USER OVERRIDE CSS (~/.config/lambdock/styles.css)
  const char *config_dir = g_get_user_config_dir();
  char *user_css_path =
      g_build_filename(config_dir, "lambdock", "styles.css", NULL);

  if (g_file_test(user_css_path, G_FILE_TEST_EXISTS)) {
    if (!state->user_css_provider) {
      state->user_css_provider = gtk_css_provider_new();
      gtk_style_context_add_provider(
          context, GTK_STYLE_PROVIDER(state->user_css_provider),
          GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    G_GNUC_END_IGNORE_DEPRECATIONS
    LOG_C_INFO("UI", "Loading user CSS override for [%s] from: %s",
               state->config_path ? state->config_path : "default",
               user_css_path);
    gtk_css_provider_load_from_path(state->user_css_provider, user_css_path);
  }
  g_free(user_css_path);
}
