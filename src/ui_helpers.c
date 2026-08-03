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

#include "ui_helpers.h"

/* =========================================================================
 * CALLBACK HELPERS & PROCESS LAUNCHING
 * ========================================================================= */

/**
 * free_exec_data:
 * @data: Pointer to the heap-allocated string passed to g_signal_connect_data.
 * @closure: Closure context pointer (unused).
 *
 * GDestroyNotify callback triggered automatically when the button or signal is
 * destroyed. Frees heap memory reserved by g_strdup().
 */
static void free_exec_data(gpointer data, GClosure *closure) {
  (void)closure;
  g_free(data);
}

/**
 * on_app_button_clicked:
 * @btn: Pointer to the clicked GTK button widget (unused directly).
 * @user_data: Pointer to the string execution command passed from button setup.
 *
 * Callback triggered when a launcher button is clicked. Spawns the associated
 * desktop command line asynchronously without blocking the UI main loop.
 */
static void on_app_button_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  const char *exec_cmd = (const char *)user_data;

  if (exec_cmd && *exec_cmd != '\0') {
    GError *error = NULL;

    // Asynchronously launch command line process in background
    if (!g_spawn_command_line_async(exec_cmd, &error)) {
      g_warning("[Lambdock] Failed to launch '%s': %s", exec_cmd,
                error->message);
      g_clear_error(&error); // Clean up allocated GError memory structure
    } else {
      g_print("[Lambdock] Launched: %s\n", exec_cmd);
    }
  }
}

/* =========================================================================
 * ICON SEARCH & RESOLUTION ENGINE
 * ========================================================================= */

/**
 * create_image_for_icon:
 * @theme: Active GtkIconTheme context for icon lookup.
 * @icon: Icon name, theme identifier, or raw file path.
 * @icon_size: Desired image render size in pixels.
 *
 * Attempts to resolve an icon using a cascading fallback mechanism to ensure an
 * image widget is always returned, even if preferred themes are missing.
 *
 * Returns: A newly constructed GtkImage widget pointer.
 */
static GtkWidget *create_image_for_icon(GtkIconTheme *theme, const char *icon,
                                        int icon_size) {
  // Fallback 0: Default executable icon if input name string is empty or NULL
  if (!icon || *icon == '\0') {
    GtkWidget *img = gtk_image_new_from_icon_name("application-x-executable");
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  // Fallback 1: Direct absolute file path check (e.g.,
  // "/usr/share/icons/custom.png")
  if (g_file_test(icon, G_FILE_TEST_EXISTS)) {
    GtkWidget *img = gtk_image_new_from_file(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  // Fallback 2: Active GTK Icon Theme lookup by icon name
  if (gtk_icon_theme_has_icon(theme, icon)) {
    GtkWidget *img = gtk_image_new_from_icon_name(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  // Fallback 3: System pixmaps directory search (/usr/share/pixmaps/app.png)
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

  // Fallback 4: Appended symbolic icon variant (e.g., "firefox-symbolic")
  char *symbolic_name = g_strdup_printf("%s-symbolic", icon);
  if (gtk_icon_theme_has_icon(theme, symbolic_name)) {
    GtkWidget *img = gtk_image_new_from_icon_name(symbolic_name);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    g_free(symbolic_name);
    return img;
  }
  g_free(symbolic_name);

  // Fallback 5: Generic fallback executable icon if all searches fail
  GtkWidget *img = gtk_image_new_from_icon_name("application-x-executable");
  gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
  return img;
}

/**
 * setup_icon_theme:
 *
 * Configures search paths for icon themes across user home directories and XDG
 * system locations. Reads desktop preferences from GSettings to apply active
 * system icon theme names.
 */
void setup_icon_theme(void) {
  GdkDisplay *display = gdk_display_get_default();
  if (!display) {
    g_print("[ERROR] No display detected (GdkDisplay)\n");
    return;
  }

  GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);

  // Register XDG system data directories (/usr/share, /usr/local/share, etc.)
  const char *const *sys_dirs = g_get_system_data_dirs();
  for (int i = 0; sys_dirs[i] != NULL; i++) {
    char *icons_dir = g_build_filename(sys_dirs[i], "icons", NULL);
    if (g_file_test(icons_dir, G_FILE_TEST_IS_DIR)) {
      gtk_icon_theme_add_search_path(icon_theme, icons_dir);
    }
    g_free(icons_dir);
  }

  // Register user-local home icon search locations (~/.local/share/icons,
  // ~/.icons)
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

  // Read desktop icon theme preferences from GNOME GSettings database
  GSettingsSchemaSource *schema_source = g_settings_schema_source_get_default();
  if (schema_source) {
    GSettingsSchema *schema = g_settings_schema_source_lookup(
        schema_source, "org.gnome.desktop.interface", TRUE);

    if (schema) {
      GSettings *settings = g_settings_new("org.gnome.desktop.interface");
      char *theme_name = g_settings_get_string(settings, "icon-theme");

      // Apply retrieved icon theme name to GTK display settings context
      if (theme_name && *theme_name != '\0') {
        GtkSettings *gtk_settings = gtk_settings_get_for_display(display);
        g_object_set(gtk_settings, "gtk-icon-theme-name", theme_name, NULL);
        g_print("[Lambdock] Applied icon theme: %s\n", theme_name);
      }

      // Memory Cleanup
      g_free(theme_name);
      g_object_unref(settings);
      g_settings_schema_unref(schema);
    }
  }
}

/* =========================================================================
 * UI CONSTRUCTORS & STYLING LOGIC
 * ========================================================================= */

/**
 * create_app_button:
 * @name: App name used for hover tooltip.
 * @icon: Icon identifier or path.
 * @exec: Command line execution string.
 * @icon_size: Pixel dimension for icon image widget.
 *
 * Constructs a GTK button configured with custom icons, tooltips, CSS styling
 * classes, and memory-safe signal hooks for app execution.
 *
 * Returns: Newly constructed GtkButton widget pointer.
 */
GtkWidget *create_app_button(const char *name, const char *icon,
                             const char *exec, int icon_size) {
  GtkWidget *button = gtk_button_new();

  GdkDisplay *display = gdk_display_get_default();
  GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);

  GtkWidget *image = create_image_for_icon(icon_theme, icon, icon_size);

  // Set constructed image as single child of GTK Button
  gtk_button_set_child(GTK_BUTTON(button), image);
  gtk_widget_add_css_class(button, "flat");

  // Configure hover tooltip text
  if (name && *name != '\0') {
    gtk_widget_set_tooltip_text(button, name);
  } else if (exec && *exec != '\0') {
    gtk_widget_set_tooltip_text(button, exec);
  }

  // Connect "clicked" signal with memory notify callback (free_exec_data) to
  // prevent leaks
  g_signal_connect_data(button, "clicked", G_CALLBACK(on_app_button_clicked),
                        g_strdup(exec), free_exec_data, 0);

  return button;
}

/**
 * create_separator:
 *
 * Constructs a vertical line separator widget used to visually divide dock
 * sections.
 *
 * Returns: A newly created GtkSeparator widget pointer.
 */
GtkWidget *create_separator(GtkOrientation orientation) {
  GtkWidget *separator = gtk_separator_new(orientation);

  if (orientation == GTK_ORIENTATION_VERTICAL) {
    gtk_widget_set_margin_start(separator, 4);
    gtk_widget_set_margin_end(separator, 4);
  } else {
    gtk_widget_set_margin_top(separator, 4);
    gtk_widget_set_margin_bottom(separator, 4);
  }

  return separator;
}

/**
 * load_custom_css:
 *
 * Creates a GtkCssProvider, loads dock stylesheet resources from disk, and
 * registers the provider globally across the default GdkDisplay.
 */
void load_custom_css(void) {
  GtkCssProvider *provider = gtk_css_provider_new();

#ifdef DOCK_CSS_PATH
  if (g_file_test(DOCK_CSS_PATH, G_FILE_TEST_EXISTS)) {
    g_print("Loading CSS from DOCK_CSS_PATH: %s", DOCK_CSS_PATH);
    gtk_css_provider_load_from_path(provider, DOCK_CSS_PATH);
  } else if (g_file_test("resources/css/dock.css", G_FILE_TEST_EXISTS)) {
    g_print("Loading CSS from ./resources/css/dock.css");
    gtk_css_provider_load_from_path(provider, "resources/css/dock.css");
  } else {
    g_warning("[Lambdock] Could not find dock.css at %s", DOCK_CSS_PATH);
  }
#endif

  // Apply CSS rules globally with application priority across the default
  // display
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  g_object_unref(provider); // Decrement reference count; provider is owned by
                            // display context
}
