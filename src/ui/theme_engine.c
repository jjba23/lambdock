#include "theme_engine.h"
#include "logger.h"

static GtkCssProvider *g_base_css_provider = NULL;
static GtkCssProvider *g_user_css_provider = NULL;

void load_custom_css(LambdockState *state) {
  GdkDisplay *display = gdk_display_get_default();
  if (!display)
    return;

  const char *raw_theme = (state && state->theme) ? state->theme : NULL;
  const char *theme_name =
      (raw_theme && *raw_theme != '\0') ? raw_theme : "vanilla";

  // If theme is 'none', remove and unbind providers
  if (g_ascii_strcasecmp(theme_name, "none") == 0) {
    LOG_C_INFO("UI", "Theme set to 'none'. Removing all GTK CSS providers.");
    if (g_base_css_provider) {
      gtk_style_context_remove_provider_for_display(
          display, GTK_STYLE_PROVIDER(g_base_css_provider));
      g_clear_object(&g_base_css_provider);
    }
    if (g_user_css_provider) {
      gtk_style_context_remove_provider_for_display(
          display, GTK_STYLE_PROVIDER(g_user_css_provider));
      g_clear_object(&g_user_css_provider);
    }
    return;
  }

  // 1. BASE THEME
  if (!g_base_css_provider) {
    g_base_css_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(g_base_css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }

  char *resource_path =
      g_strdup_printf("/org/jointhefreeworld/lambdock/css/%s.css", theme_name);
  if (g_resources_get_info(resource_path, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL,
                           NULL, NULL)) {
    LOG_C_INFO("UI", "Loading base CSS theme from GResource: %s",
               resource_path);
    gtk_css_provider_load_from_resource(g_base_css_provider, resource_path);
  } else {
    LOG_C_WARN("UI", "Theme resource '%s' not found, falling back to vanilla",
               resource_path);
    gtk_css_provider_load_from_resource(
        g_base_css_provider, "/org/jointhefreeworld/lambdock/css/vanilla.css");
  }
  g_free(resource_path);

  // 2. USER OVERRIDE CSS (~/.config/lambdock/styles.css)
  const char *config_dir = g_get_user_config_dir();
  char *user_css_path =
      g_build_filename(config_dir, "lambdock", "styles.css", NULL);

  if (g_file_test(user_css_path, G_FILE_TEST_EXISTS)) {
    if (!g_user_css_provider) {
      g_user_css_provider = gtk_css_provider_new();
      gtk_style_context_add_provider_for_display(
          display, GTK_STYLE_PROVIDER(g_user_css_provider),
          GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    LOG_C_INFO("UI", "Loading user CSS override from: %s", user_css_path);
    gtk_css_provider_load_from_path(g_user_css_provider, user_css_path);
  }
  g_free(user_css_path);
}
