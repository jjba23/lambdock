#include "ui_helpers.h"

static void free_exec_data(gpointer data, GClosure *closure) {
  (void)closure;
  g_free(data);
}

static void on_app_button_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  const char *exec_cmd = (const char *)user_data;

  if (exec_cmd && *exec_cmd != '\0') {
    GError *error = NULL;
    if (!g_spawn_command_line_async(exec_cmd, &error)) {
      g_warning("[Lambdock] Failed to launch '%s': %s", exec_cmd,
                error->message);
      g_clear_error(&error);
    } else {
      g_print("[Lambdock] Launched: %s\n", exec_cmd);
    }
  }
}

static GtkWidget *create_image_for_icon(GtkIconTheme *theme, const char *icon,
                                        int icon_size) {
  if (!icon || *icon == '\0') {
    GtkWidget *img = gtk_image_new_from_icon_name("application-x-executable");
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  if (g_file_test(icon, G_FILE_TEST_EXISTS)) {
    GtkWidget *img = gtk_image_new_from_file(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  if (gtk_icon_theme_has_icon(theme, icon)) {
    GtkWidget *img = gtk_image_new_from_icon_name(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

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

  char *symbolic_name = g_strdup_printf("%s-symbolic", icon);
  if (gtk_icon_theme_has_icon(theme, symbolic_name)) {
    GtkWidget *img = gtk_image_new_from_icon_name(symbolic_name);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    g_free(symbolic_name);
    return img;
  }
  g_free(symbolic_name);

  GtkWidget *img = gtk_image_new_from_icon_name("application-x-executable");
  gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
  return img;
}

void setup_icon_theme(void) {
  GdkDisplay *display = gdk_display_get_default();
  if (!display) {
    g_print("[ERROR] No display detected (GdkDisplay)\n");
    return;
  }

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
    if (g_file_test(user_icons, G_FILE_TEST_IS_DIR)) {
      gtk_icon_theme_add_search_path(icon_theme, user_icons);
    }
    g_free(user_icons);

    char *legacy_icons = g_build_filename(home, ".icons", NULL);
    if (g_file_test(legacy_icons, G_FILE_TEST_IS_DIR)) {
      gtk_icon_theme_add_search_path(icon_theme, legacy_icons);
    }
    g_free(legacy_icons);
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
        g_print("[Lambdock] Applied icon theme: %s\n", theme_name);
      }

      g_free(theme_name);
      g_object_unref(settings);
      g_settings_schema_unref(schema);
    }
  }
}

GtkWidget *create_app_button(const char *name, const char *icon,
                             const char *exec, int icon_size) {
  GtkWidget *button = gtk_button_new();

  GdkDisplay *display = gdk_display_get_default();
  GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);

  GtkWidget *image = create_image_for_icon(icon_theme, icon, icon_size);

  gtk_button_set_child(GTK_BUTTON(button), image);
  gtk_widget_add_css_class(button, "flat");

  if (name && *name != '\0') {
    gtk_widget_set_tooltip_text(button, name);
  } else if (exec && *exec != '\0') {
    gtk_widget_set_tooltip_text(button, exec);
  }

  g_signal_connect_data(button, "clicked", G_CALLBACK(on_app_button_clicked),
                        g_strdup(exec), free_exec_data, 0);

  return button;
}

GtkWidget *create_separator(void) {
  GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_margin_start(separator, 4);
  gtk_widget_set_margin_end(separator, 4);
  return separator;
}
