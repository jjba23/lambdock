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

GtkWidget *create_image_for_icon(GtkIconTheme *theme, const char *icon,
                                 int icon_size) {
  if (!icon || *icon == '\0') {
    GtkWidget *img = gtk_image_new_from_icon_name("application-x-executable");
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  // 1. GResource check
  if (g_str_has_prefix(icon, "resource://")) {
    GtkWidget *img = gtk_image_new_from_resource(icon + 11);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  } else if (g_str_has_prefix(icon, "/org/jointhefreeworld/lambdock/")) {
    if (g_resources_get_info(icon, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL, NULL,
                             NULL)) {
      GtkWidget *img = gtk_image_new_from_resource(icon);
      gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
      return img;
    }
  }

  // 2. Absolute path check
  if (g_file_test(icon, G_FILE_TEST_EXISTS)) {
    GtkWidget *img = gtk_image_new_from_file(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  // 3. Theme lookup
  if (gtk_icon_theme_has_icon(theme, icon)) {
    GtkWidget *img = gtk_image_new_from_icon_name(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  // 4. Pixmap folder check
  const char *const *sys_dirs = g_get_system_data_dirs();
  for (int i = 0; sys_dirs[i] != NULL; i++) {
    char *pixmap_png = g_strdup_printf("%s/pixmaps/%s.png", sys_dirs[i], icon);
    if (g_file_test(pixmap_png, G_FILE_TEST_EXISTS)) {
      GtkWidget *img = gtk_image_new_from_file(pixmap_png);
      gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
      g_free(pixmap_png);
      return img;
    }
    g_free(pixmap_png);
  }

  // 5. Symbolic variant fallback
  char *symbolic_name = g_strdup_printf("%s-symbolic", icon);
  if (gtk_icon_theme_has_icon(theme, symbolic_name)) {
    GtkWidget *img = gtk_image_new_from_icon_name(symbolic_name);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    g_free(symbolic_name);
    return img;
  }
  g_free(symbolic_name);

  // 6. Generic executable fallback
  GtkWidget *img = gtk_image_new_from_icon_name("application-x-executable");
  gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
  return img;
}

void setup_icon_theme(void) {
  GdkDisplay *display = gdk_display_get_default();
  if (!display)
    return;

  GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);
  const char *const *sys_dirs = g_get_system_data_dirs();
  for (int i = 0; sys_dirs[i] != NULL; i++) {
    char *icons_dir = g_build_filename(sys_dirs[i], "icons", NULL);
    if (g_file_test(icons_dir, G_FILE_TEST_IS_DIR)) {
      gtk_icon_theme_add_search_path(icon_theme, icons_dir);
    }
    g_free(icons_dir);
  }

  const char *home = g_get_home_dir();
  if (home) {
    char *user_icons = g_build_filename(home, ".local", "share", "icons", NULL);
    if (g_file_test(user_icons, G_FILE_TEST_IS_DIR))
      gtk_icon_theme_add_search_path(icon_theme, user_icons);
    g_free(user_icons);
  }

  GSettingsSchemaSource *schema_source = g_settings_schema_source_get_default();
  if (schema_source) {
    GSettingsSchema *schema = g_settings_schema_source_lookup(
        schema_source, "org.gnome.desktop.interface", TRUE);
    if (schema) {
      GSettings *settings = g_settings_new("org.gnome.desktop.interface");
      char *theme_name = g_settings_get_string(settings, "icon-theme");
      if (theme_name && *theme_name != '\0') {
        GtkSettings *gtk_settings = gtk_settings_get_for_display(display);
        g_object_set(gtk_settings, "gtk-icon-theme-name", theme_name, NULL);
        LOG_C_INFO("UI", "Applied icon theme: %s", theme_name);
      }
      g_free(theme_name);
      g_object_unref(settings);
      g_settings_schema_unref(schema);
    }
  }
}
