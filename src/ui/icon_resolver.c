// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "icon_resolver.h"
#include "../logger.h"

/* ============================================================================
 * HELPER FUNCTIONS
 * ============================================================================
 */

/**
 * get_gsettings_icon_theme:
 *
 * Fallback helper to query the GNOME GSettings / dconf backend for the desktop
 * interface icon theme name ('org.gnome.desktop.interface icon-theme').
 *
 * Returns: An allocated string containing the theme name, or NULL if
 * unavailable/empty. caller must free returned string with g_free().
 */
static char *get_gsettings_icon_theme(void) {
  GSettingsSchemaSource *schema_source = g_settings_schema_source_get_default();
  if (!schema_source) {
    LOG_C_WARN(
        "UI",
        "GSettingsSchemaSource default source is NULL. Cannot read gsettings.");
    return NULL;
  }

  GSettingsSchema *schema = g_settings_schema_source_lookup(
      schema_source, "org.gnome.desktop.interface", TRUE);

  if (!schema) {
    LOG_C_WARN(
        "UI",
        "GSettings schema 'org.gnome.desktop.interface' not found on system.");
    return NULL;
  }

  GSettings *settings = g_settings_new("org.gnome.desktop.interface");
  char *theme_name = g_settings_get_string(settings, "icon-theme");

  g_object_unref(settings);
  g_settings_schema_unref(schema);

  if (theme_name && *theme_name == '\0') {
    g_free(theme_name);
    return NULL;
  }

  return theme_name;
}

/* ============================================================================
 * ICON CREATION & RESOLUTION
 * ============================================================================
 */

/**
 * create_image_for_icon:
 * @theme: Pointer to the GTK icon theme instance for the active display.
 * @icon: String identifier, file path, or resource URI for the requested icon.
 * @icon_size: Pixel size (width/height) to render the target icon image (e.g.,
 * 32).
 *
 * Attempts to locate and build a GTK image widget for a given icon identifier.
 */
GtkWidget *create_image_for_icon(GtkIconTheme *theme, const char *icon,
                                 int icon_size) {
  // --------------------------------------------------------------------------
  // Defensive Guard Check for NULL or Empty Icon String
  // --------------------------------------------------------------------------
  if (!icon || *icon == '\0') {
    LOG_C_DEBUG("UI", "create_image_for_icon: Passed NULL or empty icon name.");
    return NULL;
  }

  LOG_C_DEBUG("UI", "Resolving icon: '%s' (target size: %dpx)", icon,
              icon_size);

  // --------------------------------------------------------------------------
  // Step 1: GLib Resource Bundle Check
  // --------------------------------------------------------------------------
  if (g_str_has_prefix(icon, "resource://")) {
    LOG_C_DEBUG("UI", "Attempting load from resource scheme: %s", icon);
    GtkWidget *img = gtk_image_new_from_resource(icon + 11);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  } else if (g_str_has_prefix(icon, "/org/jointhefreeworld/lambdock/")) {
    if (g_resources_get_info(icon, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL, NULL,
                             NULL)) {
      LOG_C_DEBUG("UI", "Found embedded internal resource: %s", icon);
      GtkWidget *img = gtk_image_new_from_resource(icon);
      gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
      return img;
    }
  }

  // --------------------------------------------------------------------------
  // Step 2: Absolute File Path Check
  // --------------------------------------------------------------------------
  if (g_file_test(icon, G_FILE_TEST_EXISTS)) {
    LOG_C_DEBUG("UI", "Found direct file path on disk: %s", icon);
    GtkWidget *img = gtk_image_new_from_file(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  // --------------------------------------------------------------------------
  // Step 3: Standard Icon Theme Lookup
  // --------------------------------------------------------------------------
  gboolean has_icon = gtk_icon_theme_has_icon(theme, icon);
  LOG_C_DEBUG("UI", "gtk_icon_theme_has_icon(theme, '%s'): %s", icon,
              has_icon ? "YES" : "NO");

  if (has_icon) {
    LOG_C_DEBUG("UI", "Resolved '%s' successfully via GTK Icon Theme!", icon);
    GtkWidget *img = gtk_image_new_from_icon_name(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  // --------------------------------------------------------------------------
  // Step 4: System XDG Pixmap Directory Search
  // --------------------------------------------------------------------------
  const char *const *sys_dirs = g_get_system_data_dirs();
  for (int i = 0; sys_dirs[i] != NULL; i++) {
    char *pixmap_png = g_strdup_printf("%s/pixmaps/%s.png", sys_dirs[i], icon);

    if (g_file_test(pixmap_png, G_FILE_TEST_EXISTS)) {
      LOG_C_DEBUG("UI", "Resolved '%s' in pixmaps folder: %s", icon,
                  pixmap_png);
      GtkWidget *img = gtk_image_new_from_file(pixmap_png);
      gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
      g_free(pixmap_png);
      return img;
    }
    g_free(pixmap_png);
  }

  // --------------------------------------------------------------------------
  // Step 5: Symbolic Icon Variant Fallback
  // --------------------------------------------------------------------------
  char *symbolic_name = g_strdup_printf("%s-symbolic", icon);
  gboolean has_symbolic = gtk_icon_theme_has_icon(theme, symbolic_name);
  LOG_C_DEBUG("UI", "gtk_icon_theme_has_icon(theme, '%s'): %s", symbolic_name,
              has_symbolic ? "YES" : "NO");

  if (has_symbolic) {
    LOG_C_DEBUG("UI", "Resolved '%s' using symbolic fallback: %s", icon,
                symbolic_name);
    GtkWidget *img = gtk_image_new_from_icon_name(symbolic_name);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    g_free(symbolic_name);
    return img;
  }
  g_free(symbolic_name);

  // --------------------------------------------------------------------------
  // Step 6: Fallback (Generic Binary Executable Icon)
  // --------------------------------------------------------------------------
  LOG_C_WARN(
      "UI",
      "Failed to resolve icon '%s'. Falling back to 'application-x-executable'",
      icon);
  GtkWidget *img = gtk_image_new_from_icon_name("application-x-executable");
  gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
  return img;
}

/* ============================================================================
 * ICON THEME SYSTEM INITIALIZATION
 * ============================================================================
 */

/**
 * setup_icon_theme:
 *
 * Discovers and registers icon directories into the GTK icon search path, and
 * ensures a valid GTK icon theme name is configured (falling back to
 * GSettings/dconf only if necessary).
 */
char **lambdock_icon_search_paths_build(const char *const *system_data_dirs,
                                        const char *home) {
  GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);

  /* Order matters: the first directory an icon is found in wins, and a user's
   * own icons should shadow the system's, so they come last. */
  for (int i = 0; system_data_dirs && system_data_dirs[i] != NULL; i++) {
    char *icons_dir = g_build_filename(system_data_dirs[i], "icons", NULL);
    if (g_file_test(icons_dir, G_FILE_TEST_IS_DIR))
      g_ptr_array_add(paths, icons_dir);
    else
      g_free(icons_dir);
  }

  if (home && *home != '\0') {
    char *user_icons = g_build_filename(home, ".local", "share", "icons", NULL);
    if (g_file_test(user_icons, G_FILE_TEST_IS_DIR))
      g_ptr_array_add(paths, user_icons);
    else
      g_free(user_icons);

    char *legacy_icons = g_build_filename(home, ".icons", NULL);
    if (g_file_test(legacy_icons, G_FILE_TEST_IS_DIR))
      g_ptr_array_add(paths, legacy_icons);
    else
      g_free(legacy_icons);
  }

  /* NULL-terminated, the way GtkIconTheme's API wants a list. */
  g_ptr_array_add(paths, NULL);

  return (char **)g_ptr_array_free(paths, FALSE);
}

char **lambdock_icon_search_paths_new(void) {
  return lambdock_icon_search_paths_build(g_get_system_data_dirs(),
                                          g_get_home_dir());
}

void setup_icon_theme(void) {
  GdkDisplay *display = gdk_display_get_default();
  if (!display) {
    LOG_C_ERROR("UI", "setup_icon_theme: No default GdkDisplay detected!");
    return;
  }

  GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);
  LOG_C_INFO("UI", "Initializing Icon Theme system for display context %p...",
             (void *)display);

  /* Step 1 and 2: the search paths, built as a value and then applied. */
  g_auto(GStrv) paths = lambdock_icon_search_paths_new();
  for (int i = 0; paths[i] != NULL; i++) {
    LOG_C_DEBUG("UI", "Adding icon search path: %s", paths[i]);
    gtk_icon_theme_add_search_path(icon_theme, paths[i]);
  }

  // --------------------------------------------------------------------------
  // Step 3: Resolve Icon Theme Name
  // Primary: Native GTK settings (`gtk-icon-theme-name`)
  // Fallback: GSettings / dconf (`org.gnome.desktop.interface icon-theme`)
  // --------------------------------------------------------------------------
  GtkSettings *gtk_settings = gtk_settings_get_for_display(display);
  char *current_gtk_theme = NULL;
  g_object_get(gtk_settings, "gtk-icon-theme-name", &current_gtk_theme, NULL);

  if (current_gtk_theme && *current_gtk_theme != '\0') {
    LOG_C_INFO("UI", "GTK already has an active icon theme set: '%s'",
               current_gtk_theme);
    g_free(current_gtk_theme);
  } else {
    g_free(current_gtk_theme);
    LOG_C_INFO("UI", "No active GTK icon theme name detected. Falling back to "
                     "GSettings / dconf...");

    char *gsettings_theme = get_gsettings_icon_theme();
    if (gsettings_theme) {
      LOG_C_INFO("UI",
                 "Found GSettings icon theme: '%s'. Applying to GtkSettings...",
                 gsettings_theme);
      g_object_set(gtk_settings, "gtk-icon-theme-name", gsettings_theme, NULL);
      g_free(gsettings_theme);
    } else {
      LOG_C_WARN(
          "UI",
          "Fallback GSettings lookup failed or returned an empty string!");
    }
  }
}
