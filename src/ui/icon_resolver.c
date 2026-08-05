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

#include "icon_resolver.h"
#include "../logger.h"

/* ============================================================================
 * ICON CREATION & RESOLUTION
 * ============================================================================
 */

/**
 * create_image_for_icon:
 * @theme: Pointer to the GTK icon theme instance for the active display.
 * @icon: String identifier, file path, or resource URI for the requested icon.
 * @icon_size: Pixel size (width/height) to render the target icon image (e.g., 32).
 *
 * Attempts to locate and build a GTK image widget for a given icon identifier
 * using a 6-step resolution cascade:
 * Step: Null/Empty string guard -> Fallback
 * Step: Check embedded GLib resource bundles (`resource://`)
 * Step: Check absolute file paths on disk
 * Step: Lookup in active GTK icon theme
 * Step: Search system XDG pixmap folders (`/usr/share/pixmaps/`)
 * Step: Search for a `-symbolic` icon variant in theme
 * Step: Fallback to generic `application-x-executable` icon
 *
 * Returns: A newly created `GtkWidget` pointer containing the GTK image.
 */
GtkWidget *create_image_for_icon(GtkIconTheme *theme, const char *icon,
                                 int icon_size) {
  // --------------------------------------------------------------------------
  // Defensive Guard Check for NULL or Empty Icon String
  // --------------------------------------------------------------------------
  if (!icon || *icon == '\0') {
    // If no icon name was provided, create a generic fallback icon
    GtkWidget *img = gtk_image_new_from_icon_name("application-x-executable");
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  // --------------------------------------------------------------------------
  // Step: GLib Resource Bundle Check
  // --------------------------------------------------------------------------
  // GLib allows embedding static binary assets (like SVG/PNG icons) inside
  // the executable itself using GResource.

  // Case A: Explicit `resource://` URI scheme prefix
  if (g_str_has_prefix(icon, "resource://")) {
    // `icon + 11` skips the 11 characters of "resource://" to pass the raw path
    GtkWidget *img = gtk_image_new_from_resource(icon + 11);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }
  // Case B: Internal Lambdock resource path (e.g. `/org/jointhefreeworld/lambdock/...`)
  else if (g_str_has_prefix(icon, "/org/jointhefreeworld/lambdock/")) {
    // Verify the resource actually exists inside the registered bundle before loading
    if (g_resources_get_info(icon, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL, NULL,
                             NULL)) {
      GtkWidget *img = gtk_image_new_from_resource(icon);
      gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
      return img;
    }
  }

  // --------------------------------------------------------------------------
  // Step: Absolute File Path Check
  // --------------------------------------------------------------------------
  // If the user specified a full path (e.g., `/usr/share/icons/custom.png`),
  // check if that file exists on the filesystem.
  if (g_file_test(icon, G_FILE_TEST_EXISTS)) {
    GtkWidget *img = gtk_image_new_from_file(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  // --------------------------------------------------------------------------
  // Step: Standard Icon Theme Lookup
  // --------------------------------------------------------------------------
  // Query the system GTK icon theme engine (e.g. Papirus, Adwaita) for the icon name.
  if (gtk_icon_theme_has_icon(theme, icon)) {
    GtkWidget *img = gtk_image_new_from_icon_name(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  // --------------------------------------------------------------------------
  // Step: System XDG Pixmap Directory Search
  // --------------------------------------------------------------------------
  // Older Linux apps place PNG icons directly in `/usr/share/pixmaps/` or
  // `/usr/local/share/pixmaps/`. We query all active system data directories.
  const char *const *sys_dirs = g_get_system_data_dirs();
  for (int i = 0; sys_dirs[i] != NULL; i++) {
    // Allocate formatted path string: e.g. "/usr/share" + "/pixmaps/" + "app" + ".png"
    char *pixmap_png = g_strdup_printf("%s/pixmaps/%s.png", sys_dirs[i], icon);

    if (g_file_test(pixmap_png, G_FILE_TEST_EXISTS)) {
      GtkWidget *img = gtk_image_new_from_file(pixmap_png);
      gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
      g_free(pixmap_png); // Clean up dynamic string allocation
      return img;
    }
    g_free(pixmap_png); // Free allocated string before moving to next directory
  }

  // --------------------------------------------------------------------------
  // Step: Symbolic Icon Variant Fallback
  // --------------------------------------------------------------------------
  // GNOME/GTK icons often have monochrome variants suffixed with `-symbolic`
  // (e.g., `utilities-terminal-symbolic`).
  char *symbolic_name = g_strdup_printf("%s-symbolic", icon);
  if (gtk_icon_theme_has_icon(theme, symbolic_name)) {
    GtkWidget *img = gtk_image_new_from_icon_name(symbolic_name);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    g_free(symbolic_name); // Free string buffer
    return img;
  }
  g_free(symbolic_name); // Free string buffer

  // --------------------------------------------------------------------------
  // Step: Fallback (Generic Binary Executable Icon)
  // --------------------------------------------------------------------------
  // If all search methods failed to locate the specified icon, return a standard
  // generic application icon so the dock button isn't blank.
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
 * syncs GTK's icon theme setting with the user's desktop environment setting
 * (retrieved via GNOME GSettings).
 */
void setup_icon_theme(void) {
  // Retrieve the default graphical display instance
  GdkDisplay *display = gdk_display_get_default();
  if (!display)
    return; // Exit early if running headless or display connection fails

  // Get GTK's icon theme engine instance bound to this graphical display
  GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);

  // --------------------------------------------------------------------------
  // Add System-wide Icon Search Paths (`/usr/share/icons`, etc.)
  // --------------------------------------------------------------------------
  const char *const *sys_dirs = g_get_system_data_dirs();
  for (int i = 0; sys_dirs[i] != NULL; i++) {
    // Safely construct multi-platform path: `<sys_dir>/icons`
    char *icons_dir = g_build_filename(sys_dirs[i], "icons", NULL);

    // If the directory exists, add it to GTK's icon theme search paths
    if (g_file_test(icons_dir, G_FILE_TEST_IS_DIR)) {
      gtk_icon_theme_add_search_path(icon_theme, icons_dir);
    }
    g_free(icons_dir); // Free allocated string buffer
  }

  // --------------------------------------------------------------------------
  // Add User-Specific Local Icon Search Path (`~/.local/share/icons`)
  // --------------------------------------------------------------------------
  const char *home = g_get_home_dir();
  if (home) {
    // Build path: `$HOME/.local/share/icons`
    char *user_icons = g_build_filename(home, ".local", "share", "icons", NULL);
    if (g_file_test(user_icons, G_FILE_TEST_IS_DIR))
      gtk_icon_theme_add_search_path(icon_theme, user_icons);
    g_free(user_icons); // Free path buffer
  }

  // --------------------------------------------------------------------------
  // Query GNOME Desktop GSettings for System Icon Theme Name
  // --------------------------------------------------------------------------
  // Read the active system icon theme setting (e.g. "Adwaita" or "Yaru") from
  // the `org.gnome.desktop.interface` configuration schema.

  GSettingsSchemaSource *schema_source = g_settings_schema_source_get_default();
  if (schema_source) {
    // Look up the schema for desktop interface settings
    GSettingsSchema *schema = g_settings_schema_source_lookup(
        schema_source, "org.gnome.desktop.interface", TRUE);

    if (schema) {
      // Create GSettings object to access schema keys
      GSettings *settings = g_settings_new("org.gnome.desktop.interface");

      // Read the string value from key `icon-theme`
      char *theme_name = g_settings_get_string(settings, "icon-theme");

      if (theme_name && *theme_name != '\0') {
        // Apply the detected theme name directly into GTK Display Settings
        GtkSettings *gtk_settings = gtk_settings_get_for_display(display);
        g_object_set(gtk_settings, "gtk-icon-theme-name", theme_name, NULL);

        LOG_C_INFO("UI", "Applied icon theme: %s", theme_name);
      }

      // Memory cleanup for GLib references and string buffer
      g_free(theme_name);
      g_object_unref(settings);
      g_settings_schema_unref(schema);
    }
  }
}
