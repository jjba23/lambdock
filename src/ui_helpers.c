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
#include "dock_window.h"
#include "toplevel_tracker.h"

/* =========================================================================
 * ICON SEARCH & RESOLUTION ENGINE
 * ========================================================================= */

/**
 * create_image_for_icon:
 * Resolves an icon using cascading fallbacks (File Path -> Theme Name ->
 * Pixmaps -> Fallback Icon).
 */
static GtkWidget *create_image_for_icon(GtkIconTheme *theme, const char *icon,
                                        int icon_size) {
  if (!icon || *icon == '\0') {
    GtkWidget *img = gtk_image_new_from_icon_name("application-x-executable");
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  // Fallback 1: Absolute file system path check
  if (g_file_test(icon, G_FILE_TEST_EXISTS)) {
    GtkWidget *img = gtk_image_new_from_file(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  // Fallback 2: Theme icon search
  if (gtk_icon_theme_has_icon(theme, icon)) {
    GtkWidget *img = gtk_image_new_from_icon_name(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    return img;
  }

  // Fallback 3: XDG system pixmaps folder search
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

  // Fallback 4: Symbolic variant search
  char *symbolic_name = g_strdup_printf("%s-symbolic", icon);
  if (gtk_icon_theme_has_icon(theme, symbolic_name)) {
    GtkWidget *img = gtk_image_new_from_icon_name(symbolic_name);
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_size);
    g_free(symbolic_name);
    return img;
  }
  g_free(symbolic_name);

  // Fallback 5: Generic default executable icon
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

/* =========================================================================
 * UNIFIED DOCK ITEM ACTION HANDLERS & POPOVER BUILDER
 * ========================================================================= */

typedef struct {
  DockItem *item;
  struct zwlr_foreign_toplevel_handle_v1 *handle;
} WindowActionData;

static void free_action_data(gpointer data, GClosure *closure) {
  (void)closure;
  g_free(data);
}

static void on_popover_focus_window(GtkButton *btn, gpointer user_data) {
  (void)btn;
  WindowActionData *data = (WindowActionData *)user_data;
  if (data && data->handle) {
    activate_toplevel_window(data->handle);
  }
  if (data && data->item && data->item->popover) {
    gtk_popover_popdown(GTK_POPOVER(data->item->popover));
  }
}

static void on_popover_close_window(GtkButton *btn, gpointer user_data) {
  (void)btn;
  WindowActionData *data = (WindowActionData *)user_data;
  if (data && data->handle) {
    close_toplevel_window(data->handle);
  }
  if (data && data->item && data->item->popover) {
    gtk_popover_popdown(GTK_POPOVER(data->item->popover));
  }
}

static void on_popover_close_all_windows(GtkButton *btn, gpointer user_data) {
  (void)btn;
  DockItem *item = (DockItem *)user_data;
  if (item) {
    GList *handles = NULL;
    for (GList *l = item->windows; l != NULL; l = l->next) {
      AppWindow *win = (AppWindow *)l->data;
      handles = g_list_append(handles, win->handle);
    }
    for (GList *l = handles; l != NULL; l = l->next) {
      close_toplevel_window((struct zwlr_foreign_toplevel_handle_v1 *)l->data);
    }
    g_list_free(handles);

    if (item->popover) {
      gtk_popover_popdown(GTK_POPOVER(item->popover));
    }
  }
}

static void on_popover_launch_clicked(DockItem *item) {
  if (item && item->exec && *item->exec != '\0') {
    GError *error = NULL;
    if (!g_spawn_command_line_async(item->exec, &error)) {
      g_warning("[Lambdock] Failed to launch '%s': %s", item->exec,
                error->message);
      g_clear_error(&error);
    } else {
      g_print("[Lambdock] Launched new instance: %s\n", item->exec);
    }
  }
  if (item && item->popover) {
    gtk_popover_popdown(GTK_POPOVER(item->popover));
  }
}

static void populate_popover_menu(DockItem *item) {
  if (!item || !item->popover)
    return;

  GtkWidget *old_child = gtk_popover_get_child(GTK_POPOVER(item->popover));
  if (old_child) {
    gtk_popover_set_child(GTK_POPOVER(item->popover), NULL);
  }

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start(vbox, 8);
  gtk_widget_set_margin_end(vbox, 8);
  gtk_widget_set_margin_top(vbox, 8);
  gtk_widget_set_margin_bottom(vbox, 8);

  const char *title = (item->name && *item->name != '\0')       ? item->name
                      : (item->app_id && *item->app_id != '\0') ? item->app_id
                                                                : "Application";
  GtkWidget *label = gtk_label_new(title);
  gtk_widget_add_css_class(label, "title");
  gtk_box_append(GTK_BOX(vbox), label);

  GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(vbox), sep1);

  if (item->windows != NULL) {
    int win_count = g_list_length(item->windows);

    for (GList *l = item->windows; l != NULL; l = l->next) {
      AppWindow *win = (AppWindow *)l->data;

      GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

      const char *win_title =
          (win->title && *win->title != '\0') ? win->title : "Window";
      GtkWidget *btn_focus = gtk_button_new_with_label(win_title);
      gtk_widget_add_css_class(btn_focus, "flat");
      gtk_widget_set_hexpand(btn_focus, TRUE);
      gtk_widget_set_halign(btn_focus, GTK_ALIGN_FILL);

      GtkWidget *child_label = gtk_button_get_child(GTK_BUTTON(btn_focus));
      if (GTK_IS_LABEL(child_label)) {
        gtk_label_set_ellipsize(GTK_LABEL(child_label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(child_label), 30);
      }

      WindowActionData *action_data = g_new0(WindowActionData, 1);
      action_data->item = item;
      action_data->handle = win->handle;

      g_signal_connect_data(btn_focus, "clicked",
                            G_CALLBACK(on_popover_focus_window), action_data,
                            free_action_data, 0);

      gtk_box_append(GTK_BOX(row), btn_focus);

      GtkWidget *btn_close =
          gtk_button_new_from_icon_name("window-close-symbolic");
      gtk_widget_add_css_class(btn_close, "flat");
      gtk_widget_set_tooltip_text(btn_close, "Close Window");

      WindowActionData *close_data = g_new0(WindowActionData, 1);
      close_data->item = item;
      close_data->handle = win->handle;

      g_signal_connect_data(btn_close, "clicked",
                            G_CALLBACK(on_popover_close_window), close_data,
                            free_action_data, 0);

      gtk_box_append(GTK_BOX(row), btn_close);
      gtk_box_append(GTK_BOX(vbox), row);
    }

    GtkWidget *sep2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(vbox), sep2);

    if (item->exec && *item->exec != '\0') {
      GtkWidget *btn_launch = gtk_button_new_with_label("New Instance");
      gtk_widget_add_css_class(btn_launch, "flat");
      g_signal_connect_swapped(btn_launch, "clicked",
                               G_CALLBACK(on_popover_launch_clicked), item);
      gtk_box_append(GTK_BOX(vbox), btn_launch);
    }

    if (win_count > 1) {
      GtkWidget *btn_close_all = gtk_button_new_with_label("Close All Windows");
      gtk_widget_add_css_class(btn_close_all, "flat");
      g_signal_connect(btn_close_all, "clicked",
                       G_CALLBACK(on_popover_close_all_windows), item);
      gtk_box_append(GTK_BOX(vbox), btn_close_all);
    }

  } else {
    if (item->exec && *item->exec != '\0') {
      GtkWidget *btn_launch = gtk_button_new_with_label("Launch Application");
      gtk_widget_add_css_class(btn_launch, "flat");
      g_signal_connect_swapped(btn_launch, "clicked",
                               G_CALLBACK(on_popover_launch_clicked), item);
      gtk_box_append(GTK_BOX(vbox), btn_launch);
    }
  }

  gtk_popover_set_child(GTK_POPOVER(item->popover), vbox);
}

static void on_dock_item_left_click(GtkButton *btn, gpointer user_data) {
  (void)btn;
  DockItem *item = (DockItem *)user_data;
  if (!item)
    return;

  if (item->windows != NULL) {
    if (g_list_length(item->windows) == 1) {
      AppWindow *win = (AppWindow *)item->windows->data;
      if (win && win->handle) {
        activate_toplevel_window(win->handle);
      }
    } else {
      populate_popover_menu(item);
      if (!gtk_widget_get_visible(item->popover)) {
        item->state->active_popovers++;
      }
      gtk_popover_popup(GTK_POPOVER(item->popover));
    }
  } else if (item->exec && *item->exec != '\0') {
    GError *error = NULL;
    if (!g_spawn_command_line_async(item->exec, &error)) {
      g_warning("[Lambdock] Failed to launch '%s': %s", item->exec,
                error->message);
      g_clear_error(&error);
    } else {
      g_print("[Lambdock] Launched: %s\n", item->exec);
    }
  }
}

static void on_dock_item_right_click(GtkGestureClick *gesture, gint n_press,
                                     gdouble x, gdouble y, gpointer user_data) {
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;
  DockItem *item = (DockItem *)user_data;

  if (!item || !item->popover || !item->state)
    return;

  populate_popover_menu(item);

  if (!gtk_widget_get_visible(item->popover)) {
    item->state->active_popovers++;
  }

  gtk_popover_popup(GTK_POPOVER(item->popover));
}

/* =========================================================================
 * UNIFIED DOCK ITEM FACTORY & MATCHING ENGINE
 * ========================================================================= */

DockItem *create_dock_item(LambdockState *state, const char *name,
                           const char *icon, const char *exec,
                           const char *app_id, gboolean is_favorite) {
  DockItem *item = g_new0(DockItem, 1);
  item->state = state;
  item->name = g_strdup(name);
  item->icon = g_strdup(icon);
  item->exec = g_strdup(exec);
  item->app_id = g_strdup(app_id ? app_id : icon);
  item->is_favorite = is_favorite;

  GdkDisplay *display = gdk_display_get_default();
  GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);

  GtkWidget *image = create_image_for_icon(icon_theme, item->icon, 32);

  item->button = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(item->button), image);
  gtk_widget_add_css_class(item->button, "flat");

  if (item->name && *item->name != '\0') {
    gtk_widget_set_tooltip_text(item->button, item->name);
  } else if (item->exec && *item->exec != '\0') {
    gtk_widget_set_tooltip_text(item->button, item->exec);
  }

  g_signal_connect(item->button, "clicked", G_CALLBACK(on_dock_item_left_click),
                   item);

  item->popover = gtk_popover_new();
  gtk_popover_set_has_arrow(GTK_POPOVER(item->popover), FALSE);
  gtk_widget_set_parent(item->popover, item->button);
  g_signal_connect(item->popover, "closed", G_CALLBACK(on_popover_closed),
                   state);

  GtkGesture *right_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click),
                                GDK_BUTTON_SECONDARY);
  g_signal_connect(right_click, "pressed", G_CALLBACK(on_dock_item_right_click),
                   item);
  gtk_widget_add_controller(item->button, GTK_EVENT_CONTROLLER(right_click));

  if (state && state->box) {
    gtk_box_append(GTK_BOX(state->box), item->button);
  }

  if (state) {
    state->dock_items = g_list_append(state->dock_items, item);
  }

  return item;
}

void free_dock_item(DockItem *item) {
  if (!item)
    return;

  if (item->popover) {
    gtk_widget_unparent(item->popover);
    item->popover = NULL;
  }

  for (GList *l = item->windows; l != NULL; l = l->next) {
    AppWindow *win = (AppWindow *)l->data;
    g_free(win->title);
    g_free(win);
  }
  g_list_free(item->windows);

  g_free(item->name);
  g_free(item->icon);
  g_free(item->exec);
  g_free(item->app_id);
  g_free(item);
}

AppWindow *
dock_item_add_window(DockItem *item,
                     struct zwlr_foreign_toplevel_handle_v1 *handle) {
  if (!item || !handle)
    return NULL;

  AppWindow *win = g_new0(AppWindow, 1);
  win->handle = handle;
  win->title = g_strdup(item->name ? item->name : item->app_id);

  item->windows = g_list_append(item->windows, win);
  update_dock_item_status(item);

  return win;
}

void dock_item_remove_window(DockItem *item,
                             struct zwlr_foreign_toplevel_handle_v1 *handle) {
  if (!item || !handle)
    return;

  for (GList *l = item->windows; l != NULL; l = l->next) {
    AppWindow *win = (AppWindow *)l->data;
    if (win->handle == handle) {
      item->windows = g_list_remove(item->windows, win);
      g_free(win->title);
      g_free(win);
      break;
    }
  }

  update_dock_item_status(item);
}

void update_dock_item_status(DockItem *item) {
  if (!item || !item->button)
    return;

  if (item->windows != NULL) {
    if (!gtk_widget_has_css_class(item->button, "running-app")) {
      gtk_widget_add_css_class(item->button, "running-app");
    }
  } else {
    if (gtk_widget_has_css_class(item->button, "running-app")) {
      gtk_widget_remove_css_class(item->button, "running-app");
    }
  }
}

static gboolean dock_item_matches_app_id(DockItem *item, const char *app_id) {
  if (!item || !app_id || *app_id == '\0')
    return FALSE;

  if (item->app_id && g_ascii_strcasecmp(item->app_id, app_id) == 0)
    return TRUE;

  if (item->icon && g_ascii_strcasecmp(item->icon, app_id) == 0)
    return TRUE;

  if (item->name && g_ascii_strcasecmp(item->name, app_id) == 0)
    return TRUE;

  if (item->exec && *item->exec != '\0') {
    char *lower_exec = g_ascii_strdown(item->exec, -1);
    char *lower_id = g_ascii_strdown(app_id, -1);

    gboolean match = (strstr(lower_exec, lower_id) != NULL) ||
                     (strstr(lower_id, lower_exec) != NULL);

    g_free(lower_exec);
    g_free(lower_id);

    if (match)
      return TRUE;
  }

  return FALSE;
}

DockItem *find_dock_item_by_app_id(LambdockState *state, const char *app_id) {
  if (!state || !app_id || *app_id == '\0')
    return NULL;

  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    if (dock_item_matches_app_id(item, app_id)) {
      return item;
    }
  }
  return NULL;
}

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

void load_custom_css(void) {
  GtkCssProvider *provider = gtk_css_provider_new();
  char *css_path = NULL;

#ifdef DOCK_CSS_PATH
  if (g_file_test(DOCK_CSS_PATH, G_FILE_TEST_IS_DIR)) {
    // css_path = g_build_filename(DOCK_CSS_PATH, "snow-leopard-dock.css",
    // NULL);
    css_path = g_build_filename(DOCK_CSS_PATH, "dock.css", NULL);
  } else if (g_file_test(DOCK_CSS_PATH, G_FILE_TEST_EXISTS)) {
    css_path = g_strdup(DOCK_CSS_PATH);
  }
#endif

  if (css_path) {
    g_print("[Lambdock] Loading CSS from: %s\n", css_path);
    gtk_css_provider_load_from_path(provider, css_path);
    g_free(css_path);
  } else {
    g_warning("[Lambdock] Could not locate dock.css");
  }

  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  g_object_unref(provider);
}
