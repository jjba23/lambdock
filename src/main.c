// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock.If not, see <https://www.gnu.org/licenses/>.

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <libguile.h>

typedef struct {
  GtkWidget *window;
  gboolean auto_hide;
  guint hide_timer_id;
  gboolean is_hidden;
} LambdockState;

/* =============================================================================
 * Utilities & Theme Setup
 * =============================================================================
 */

static gboolean get_autohide_config(void) {
  // Lookup setting 'dock-auto-hide?' from (lambdock core)
  SCM get_autohide_fn =
      scm_c_public_ref("lambdock core", "get-dock-auto-hide?");
  if (scm_is_true(get_autohide_fn)) {
    SCM res = scm_call_0(get_autohide_fn);
    return scm_to_bool(res);
  }
  return FALSE;
}

static void free_exec_data(gpointer data, GClosure *closure) {
  (void)closure;
  g_free(data);
}

/**
 * Registers user and system XDG icon directories (critical for Guix/Nix),
 * and syncs GtkSettings with user's GSettings icon theme.
 */
static void setup_icon_theme(void) {
  GdkDisplay *display = gdk_display_get_default();
  if (!display) {
    g_print("[ERROR] No display detected (GdkDisplay)\n");
    return;
  }

  GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);

  // Add all XDG Data Dirs (critical for Guix store & profile paths)
  const char *const *sys_dirs = g_get_system_data_dirs();
  for (int i = 0; sys_dirs[i] != NULL; i++) {
    char *icons_dir = g_build_filename(sys_dirs[i], "icons", NULL);
    if (g_file_test(icons_dir, G_FILE_TEST_IS_DIR)) {
      gtk_icon_theme_add_search_path(icon_theme, icons_dir);
    }
    g_free(icons_dir);
  }

  // Add user home icon search paths (~/.local/share/icons, ~/.icons)
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

  // Read configured theme name from GSettings org.gnome.desktop.interface
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
    } else {
      g_print("[Lambdock] Note: GSettings schema org.gnome.desktop.interface "
              "not found.\n");
    }
  }
}

static void reveal_dock(LambdockState *state) {
  if (state->hide_timer_id > 0) {
    g_source_remove(state->hide_timer_id);
    state->hide_timer_id = 0;
  }
  if (state->is_hidden) {
    // Reset bottom margin to reveal window
    gtk_layer_set_margin(GTK_WINDOW(state->window), GTK_LAYER_SHELL_EDGE_BOTTOM,
                         0);
    state->is_hidden = FALSE;
  }
}

static void hide_dock(LambdockState *state) {
  if (!state->auto_hide || state->is_hidden)
    return;

  int window_height = gtk_widget_get_height(state->window);
  // Slide dock down, leaving 2px trigger area visible at bottom
  int hide_margin = -(window_height - 2);

  gtk_layer_set_margin(GTK_WINDOW(state->window), GTK_LAYER_SHELL_EDGE_BOTTOM,
                       hide_margin);
  state->is_hidden = TRUE;
}

static gboolean on_hide_timeout(gpointer user_data) {
  LambdockState *state = (LambdockState *)user_data;
  hide_dock(state);
  state->hide_timer_id = 0;
  return G_SOURCE_REMOVE;
}

static void on_motion_enter(GtkEventControllerMotion *controller, double x,
                            double y, gpointer user_data) {
  (void)controller;
  (void)x;
  (void)y;
  LambdockState *state = (LambdockState *)user_data;
  reveal_dock(state);
}

static void on_motion_leave(GtkEventControllerMotion *controller,
                            gpointer user_data) {
  (void)controller;
  LambdockState *state = (LambdockState *)user_data;
  if (state->auto_hide) {
    if (state->hide_timer_id != 0) {
      g_source_remove(state->hide_timer_id);
    }
    // Delay hiding by 400ms to prevent jittering
    state->hide_timer_id = g_timeout_add(400, on_hide_timeout, state);
  }
}

/* =============================================================================
 * App Launcher Callback
 * =============================================================================
 */

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

/* =============================================================================
 * GTK Widget Factory Helpers
 * =============================================================================
 */

/**
 * Intelligently resolves an icon by checking direct paths, theme presence,
 * pixmap folders, symbolic fallbacks, and standard system defaults.
 */
static GtkWidget *create_image_for_icon(GtkIconTheme *theme, const char *icon,
                                        int icon_size) {
  if (!icon || *icon == '\0') {
    GtkWidget *img = gtk_image_new_from_icon_name("application-x-executable");
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  // Direct File Path
  if (g_file_test(icon, G_FILE_TEST_EXISTS)) {
    GtkWidget *img = gtk_image_new_from_file(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  // Active Theme Icon
  if (gtk_icon_theme_has_icon(theme, icon)) {
    GtkWidget *img = gtk_image_new_from_icon_name(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  // System Pixmaps (~/pixmaps or /share/pixmaps)
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

  // Symbolic Variant Fallback
  char *symbolic_name = g_strdup_printf("%s-symbolic", icon);
  if (gtk_icon_theme_has_icon(theme, symbolic_name)) {
    GtkWidget *img = gtk_image_new_from_icon_name(symbolic_name);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    g_free(symbolic_name);
    return img;
  }
  g_free(symbolic_name);

  // Default Fallback Icon
  GtkWidget *img = gtk_image_new_from_icon_name("application-x-executable");
  gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
  return img;
}

/**
 * Creates an app launcher button handling both named GTK theme icons
 * and direct file paths.
 */
static GtkWidget *create_app_button(const char *name, const char *icon,
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

/**
 * Creates a vertical separator for the dock layout.
 */
static GtkWidget *create_separator(void) {
  GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_margin_start(separator, 4);
  gtk_widget_set_margin_end(separator, 4);
  return separator;
}

/* =============================================================================
 * Guile Integration
 * =============================================================================
 */

static void setup_guile_load_path(void) {
  scm_c_eval_string("(set! %load-path (cons \"scheme\" %load-path))");
#ifdef LAMBDOCK_SCHEME_DIR
  scm_c_eval_string("(set! %load-path (cons \"" LAMBDOCK_SCHEME_DIR
                    "\" %load-path))");
#endif
}

static void populate_dock_items(GtkWidget *box) {
  SCM init_fn = scm_c_public_ref("lambdock core", "init-dock!");
  SCM get_type_fn = scm_c_public_ref("lambdock core", "dock-item-type");
  SCM get_name_fn = scm_c_public_ref("lambdock core", "dock-item-name");
  SCM get_icon_fn = scm_c_public_ref("lambdock core", "dock-item-icon");
  SCM get_exec_fn = scm_c_public_ref("lambdock core", "dock-item-exec");
  SCM get_icon_sz_fn = scm_c_public_ref("lambdock core", "get-icon-size");

  // MUST call init-dock! FIRST so settings.scm runs configure-dock!
  SCM items_list = scm_call_0(init_fn);

  // Read icon size AFTER configure-dock! has executed
  int icon_size = scm_to_int(scm_call_0(get_icon_sz_fn));

  while (scm_is_pair(items_list)) {
    SCM item = scm_car(items_list);

    SCM type_scm = scm_call_1(get_type_fn, item);
    SCM type_str_scm = scm_symbol_to_string(type_scm);
    char *type_str = scm_to_utf8_string(type_str_scm);

    if (g_strcmp0(type_str, "app") == 0) {
      char *name = scm_to_utf8_string(scm_call_1(get_name_fn, item));
      char *icon = scm_to_utf8_string(scm_call_1(get_icon_fn, item));
      char *exec = scm_to_utf8_string(scm_call_1(get_exec_fn, item));

      GtkWidget *btn = create_app_button(name, icon, exec, icon_size);
      gtk_box_append(GTK_BOX(box), btn);

      g_print("[Lambdock] Created app button: %s (%s)\n", name, exec);

      free(name);
      free(icon);
      free(exec);
    } else if (g_strcmp0(type_str, "separator") == 0) {
      GtkWidget *sep = create_separator();
      gtk_box_append(GTK_BOX(box), sep);
      g_print("[Lambdock] Created separator\n");
    }

    free(type_str);
    items_list = scm_cdr(items_list);
  }
}

/* =============================================================================
 * Window & GTK Initialization
 * =============================================================================
 */

static void setup_layer_shell_window(GtkWindow *window, gboolean auto_hide) {
  gtk_layer_init_for_window(window);
  gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_TOP);
  gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
  if (auto_hide) {
    gtk_layer_set_exclusive_zone(window, 0);
  } else {
    gtk_layer_auto_exclusive_zone_enable(window);
  }
}

static void activate(GtkApplication *app, gpointer user_data) {
  (void)user_data;
  setup_guile_load_path();
  scm_c_use_module("lambdock core");

  setup_icon_theme();

  LambdockState *state = g_new0(LambdockState, 1);

  GtkWidget *window = gtk_application_window_new(app);
  state->window = window;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_window_set_child(GTK_WINDOW(window), box);

  // 1. Run init-dock! first via populate_dock_items to load user settings
  populate_dock_items(box);

  // 2. Query auto_hide AFTER settings have been initialized in Guile
  state->auto_hide = get_autohide_config();
  g_print("[Lambdock] state->auto_hide: %d\n", state->auto_hide);

  // 3. Configure layer-shell with the actual loaded config
  setup_layer_shell_window(GTK_WINDOW(window), state->auto_hide);

  // Attach motion controller for hover detection
  GtkEventController *motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "enter", G_CALLBACK(on_motion_enter), state);
  g_signal_connect(motion, "leave", G_CALLBACK(on_motion_leave), state);
  gtk_widget_add_controller(window, motion);

  // Clean up state on window destroy
  g_signal_connect_swapped(window, "destroy", G_CALLBACK(g_free), state);

  gtk_window_present(GTK_WINDOW(window));

  // Initial state setup for auto-hide
  if (state->auto_hide) {
    state->hide_timer_id = g_timeout_add(1000, on_hide_timeout, state);
  }
}

static void inner_main(void *data, int argc, char **argv) {
  (void)data;
  GtkApplication *app =
      gtk_application_new("org.scheme.lambdock", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
}

int main(int argc, char **argv) {
#ifdef DEFAULT_GSK_RENDERER
  g_setenv("GSK_RENDERER", DEFAULT_GSK_RENDERER, FALSE);
#endif
  scm_boot_guile(argc, argv, inner_main, NULL);
  return 0;
}
