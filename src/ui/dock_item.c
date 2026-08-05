#include "dock_item.h"
#include "guile_bridge.h"
#include "icon_resolver.h"
#include "logger.h"
#include "popover_menu.h"
#include "theme_engine.h"
#include "toplevel_tracker.h"

static void on_dock_item_left_click(GtkButton *btn, gpointer user_data) {
  (void)btn;
  DockItem *item = (DockItem *)user_data;
  if (!item)
    return;

  if (item->windows != NULL) {
    if (g_list_length(item->windows) == 1) {
      AppWindow *win = (AppWindow *)item->windows->data;
      if (win && win->handle)
        activate_toplevel_window(win->handle);
    } else {
      populate_popover_menu(item);
      if (!gtk_widget_get_visible(item->popover))
        item->state->active_popovers++;
      gtk_popover_popup(GTK_POPOVER(item->popover));
    }
  } else if (item->exec && *item->exec != '\0') {
    GError *error = NULL;
    if (!g_spawn_command_line_async(item->exec, &error)) {
      g_warning("[Lambdock] Failed to launch '%s': %s", item->exec,
                error->message);
      g_clear_error(&error);
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
  if (!gtk_widget_get_visible(item->popover))
    item->state->active_popovers++;
  gtk_popover_popup(GTK_POPOVER(item->popover));
}

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

  if (state && state->box)
    gtk_box_append(GTK_BOX(state->box), item->button);
  if (state)
    state->dock_items = g_list_append(state->dock_items, item);

  return item;
}

void free_dock_item(DockItem *item) {
  if (!item)
    return;

  if (item->popover) {
    gtk_widget_unparent(item->popover);
    item->popover = NULL;
  }
  if (item->button) {
    GtkWidget *parent = gtk_widget_get_parent(item->button);
    if (parent && GTK_IS_BOX(parent))
      gtk_box_remove(GTK_BOX(parent), item->button);
    item->button = NULL;
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

void clear_favorite_dock_items(LambdockState *state) {
  if (!state || !state->dock_items)
    return;

  for (GList *l = state->window_trackers; l != NULL; l = l->next) {
    void *tracker_ptr = l->data;
    if (tracker_ptr) {
      void **dock_item_ptr =
          (void **)((char *)tracker_ptr + sizeof(void *) * 2);
      void **app_win_ptr = (void **)((char *)tracker_ptr + sizeof(void *) * 3);
      *dock_item_ptr = NULL;
      *app_win_ptr = NULL;
    }
  }

  GList *curr = state->dock_items;
  while (curr != NULL) {
    GList *next = curr->next;
    DockItem *item = (DockItem *)curr->data;
    if (item && item->is_favorite) {
      state->dock_items = g_list_delete_link(state->dock_items, curr);
      free_dock_item(item);
    }
    curr = next;
  }
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
    if (!gtk_widget_has_css_class(item->button, "running-app"))
      gtk_widget_add_css_class(item->button, "running-app");
  } else {
    if (gtk_widget_has_css_class(item->button, "running-app"))
      gtk_widget_remove_css_class(item->button, "running-app");
  }
}

DockItem *find_dock_item_by_app_id(LambdockState *state, const char *app_id) {
  if (!state || !app_id || *app_id == '\0')
    return NULL;

  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    if (item->app_id && g_ascii_strcasecmp(item->app_id, app_id) == 0)
      return item;
    if (item->icon && g_ascii_strcasecmp(item->icon, app_id) == 0)
      return item;
    if (item->name && g_ascii_strcasecmp(item->name, app_id) == 0)
      return item;

    if (item->exec && *item->exec != '\0') {
      char *lower_exec = g_ascii_strdown(item->exec, -1);
      char *lower_id = g_ascii_strdown(app_id, -1);
      gboolean match = (strstr(lower_exec, lower_id) != NULL) ||
                       (strstr(lower_id, lower_exec) != NULL);
      g_free(lower_exec);
      g_free(lower_id);
      if (match)
        return item;
    }
  }
  return NULL;
}

DockItem *create_separator_dock_item(LambdockState *state,
                                     GtkOrientation orientation) {
  DockItem *item = g_new0(DockItem, 1);
  item->state = state;
  item->is_favorite = TRUE;
  item->button = gtk_separator_new(orientation);

  if (orientation == GTK_ORIENTATION_VERTICAL) {
    gtk_widget_set_margin_start(item->button, 4);
    gtk_widget_set_margin_end(item->button, 4);
  } else {
    gtk_widget_set_margin_top(item->button, 4);
    gtk_widget_set_margin_bottom(item->button, 4);
  }

  if (state && state->box)
    gtk_box_append(GTK_BOX(state->box), item->button);
  if (state)
    state->dock_items = g_list_append(state->dock_items, item);

  return item;
}

void reload_dock_instance(LambdockState *state) {
  if (!state)
    return;

  LOG_C_INFO("UI", "Hot-reloading UI layout and theme state...");
  g_free(state->theme);
  state->theme = get_theme_config();
  load_custom_css(state);

  state->auto_hide = get_autohide_config();
  clear_favorite_dock_items(state);
  populate_dock_items(state);
  sync_window_trackers(state);
}
