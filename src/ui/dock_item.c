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

#include "dock_item.h" // Struct definitions for DockItem, LambdockState, AppWindow
#include "../guile_bridge.h"     // Scheme/Guile configuration interface
#include "../logger.h"           // Logging macro utilities (e.g., LOG_C_INFO)
#include "../toplevel_tracker.h" // Foreign window tracking via Wayland protocols
#include "icon_resolver.h" // Helper to load application icons from the system theme
#include "popover_menu.h" // Custom right-click and multi-window popover menus
#include "theme_engine.h" // CSS and theme engine utilities

/* ============================================================================
 * ALIGNMENT HELPERS
 * ============================================================================
 */

/**
 * apply_dock_item_alignment:
 * Applies edge-anchored alignment rules (valign/halign) to DockItem widgets,
 * ensuring non-animated items and separators remain anchored to the dock edge.
 */
void apply_dock_item_alignment(DockItem *item) {
  if (!item || !item->button || !item->state)
    return;

  LambdockState *state = item->state;
  int base_sz = (state->icon_size > 0) ? state->icon_size : DEFAULT_ICON_SIZE;

  // Set fixed base dimension on separators to prevent stretching
  if (GTK_IS_SEPARATOR(item->button)) {
    if (state->orientation == GTK_ORIENTATION_HORIZONTAL) {
      gtk_widget_set_size_request(item->button, -1, base_sz);
    } else {
      gtk_widget_set_size_request(item->button, base_sz, -1);
    }
  }

  GtkWidget *child = NULL;
  if (GTK_IS_BUTTON(item->button)) {
    child = gtk_button_get_child(GTK_BUTTON(item->button));
  }

  if (state->edge == GTK_LAYER_SHELL_EDGE_BOTTOM) {
    gtk_widget_set_valign(item->button, GTK_ALIGN_END);
    gtk_widget_set_halign(item->button, GTK_ALIGN_CENTER);
    if (child) {
      gtk_widget_set_valign(child, GTK_ALIGN_END);
      gtk_widget_set_halign(child, GTK_ALIGN_CENTER);
      if (GTK_IS_BOX(child)) {
        for (GtkWidget *w = gtk_widget_get_first_child(child); w != NULL;
             w = gtk_widget_get_next_sibling(w)) {
          gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
        }
      }
    }
  } else if (state->edge == GTK_LAYER_SHELL_EDGE_TOP) {
    gtk_widget_set_valign(item->button, GTK_ALIGN_START);
    gtk_widget_set_halign(item->button, GTK_ALIGN_CENTER);
    if (child) {
      gtk_widget_set_valign(child, GTK_ALIGN_START);
      gtk_widget_set_halign(child, GTK_ALIGN_CENTER);
      if (GTK_IS_BOX(child)) {
        for (GtkWidget *w = gtk_widget_get_first_child(child); w != NULL;
             w = gtk_widget_get_next_sibling(w)) {
          gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
        }
      }
    }
  } else if (state->edge == GTK_LAYER_SHELL_EDGE_LEFT) {
    gtk_widget_set_halign(item->button, GTK_ALIGN_START);
    gtk_widget_set_valign(item->button, GTK_ALIGN_CENTER);
    if (child) {
      gtk_widget_set_halign(child, GTK_ALIGN_START);
      gtk_widget_set_valign(child, GTK_ALIGN_CENTER);
      if (GTK_IS_BOX(child)) {
        for (GtkWidget *w = gtk_widget_get_first_child(child); w != NULL;
             w = gtk_widget_get_next_sibling(w)) {
          gtk_widget_set_halign(w, GTK_ALIGN_CENTER);
        }
      }
    }
  } else if (state->edge == GTK_LAYER_SHELL_EDGE_RIGHT) {
    gtk_widget_set_halign(item->button, GTK_ALIGN_END);
    gtk_widget_set_valign(item->button, GTK_ALIGN_CENTER);
    if (child) {
      gtk_widget_set_halign(child, GTK_ALIGN_END);
      gtk_widget_set_valign(child, GTK_ALIGN_CENTER);
      if (GTK_IS_BOX(child)) {
        for (GtkWidget *w = gtk_widget_get_first_child(child); w != NULL;
             w = gtk_widget_get_next_sibling(w)) {
          gtk_widget_set_halign(w, GTK_ALIGN_CENTER);
        }
      }
    }
  }
}

/* ============================================================================
 * EVENT CALLBACK HANDLERS (MOUSE CLICKS & INTERACTIONS)
 * ============================================================================
 */

/**
 * on_dock_item_left_click:
 * @btn: Pointer to the GtkButton widget that triggered the click.
 * @user_data: Generic pointer passed by GTK, pointing to our DockItem struct.
 *
 * Callback function triggered whenever the user left-clicks a dock icon.
 *
 * Behavior matrix:
 * 1. If 1 window is open  -> Focus/activate that window.
 * 2. If >1 window is open -> Open a popover menu listing all open windows.
 * 3. If 0 windows open    -> Execute the application launcher command
 * (`item->exec`).
 */
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

/**
 * on_dock_item_right_click:
 * @gesture: GTK gesture object that detected the click sequence.
 * @n_press: Number of press iterations (e.g., 1 for single click, 2 for
 * double).
 * @x: X coordinate of mouse click relative to the widget.
 * @y: Y coordinate of mouse click relative to the widget.
 * @user_data: Generic pointer passed by GTK, pointing to our DockItem struct.
 *
 * Callback function triggered when the user right-clicks a dock icon.
 * Always populates and opens the right-click context menu (popover).
 */
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

/* ============================================================================
 * DYNAMIC ITEM POLLING LOGIC & SCHEME EXCEPTION HANDLING
 * ============================================================================
 */

static SCM call_guile_proc_safe(void *data) {
  SCM fn = (SCM)data;
  return scm_call_0(fn);
}

static SCM handle_guile_proc_error(void *data, SCM key, SCM args) {
  (void)data;
  (void)key;
  (void)args;
  return SCM_BOOL_F;
}

static gboolean on_dynamic_item_poll(gpointer user_data) {
  DockItem *item = (DockItem *)user_data;
  if (!item || !scm_is_true(item->poll_fn) ||
      !scm_is_true(scm_procedure_p(item->poll_fn))) {
    return G_SOURCE_REMOVE;
  }

  SCM res =
      scm_internal_catch(SCM_BOOL_T, call_guile_proc_safe,
                         (void *)item->poll_fn, handle_guile_proc_error, NULL);

  if (scm_is_string(res)) {
    char *text = scm_to_utf8_string(res);
    if (text) {
      if (item->label && GTK_IS_LABEL(item->label)) {
        gtk_label_set_text(GTK_LABEL(item->label), text);
      }
      gtk_widget_set_tooltip_text(item->button, text);
      free(text);
    }
  }

  return G_SOURCE_CONTINUE;
}

/* ============================================================================
 * DOCK ITEM LIFECYCLE (CREATION & DESTRUCTION)
 * ============================================================================
 */

DockItem *create_dock_item(LambdockState *state, const char *name,
                           const char *icon, const char *exec,
                           const char *app_id, gboolean is_favorite,
                           gboolean hover_animate) {
  DockItem *item = g_new0(DockItem, 1);
  item->poll_fn = SCM_BOOL_F;

  item->state = state;
  item->name = g_strdup(name);
  item->icon = g_strdup(icon);
  item->exec = g_strdup(exec);
  item->app_id = g_strdup(app_id ? app_id : icon);
  item->is_favorite = is_favorite;
  item->hover_animate = hover_animate;

  GdkDisplay *display = gdk_display_get_default();
  GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);
  int icon_sz =
      (state && state->icon_size > 0) ? state->icon_size : DEFAULT_ICON_SIZE;
  GtkWidget *image = create_image_for_icon(icon_theme, item->icon, icon_sz);

  gtk_widget_set_overflow(image, GTK_OVERFLOW_VISIBLE);

  item->button = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(item->button), image);
  gtk_widget_add_css_class(item->button, "flat");
  gtk_widget_set_overflow(item->button, GTK_OVERFLOW_VISIBLE);

  if (state) {
    apply_dock_item_alignment(item);
  }

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

DockItem *create_dynamic_dock_item(LambdockState *state, const char *name,
                                   const char *icon, const char *exec,
                                   SCM poll_fn, guint interval_ms,
                                   gboolean hover_animate) {
  DockItem *item = g_new0(DockItem, 1);

  item->state = state;
  item->name = g_strdup(name);
  item->icon = g_strdup(icon);
  item->exec = g_strdup(exec);
  item->app_id = g_strdup(name);
  item->is_favorite = TRUE;
  item->poll_fn = SCM_BOOL_F;
  item->hover_animate = hover_animate;

  if (scm_is_true(poll_fn) && scm_is_true(scm_procedure_p(poll_fn))) {
    item->poll_fn = poll_fn;
    scm_gc_protect_object(poll_fn);
  }

  item->button = gtk_button_new();

  GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  if (icon && *icon != '\0') {
    GdkDisplay *display = gdk_display_get_default();
    GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);
    int icon_sz =
        (state && state->icon_size > 0) ? state->icon_size : DEFAULT_ICON_SIZE;
    GtkWidget *image = create_image_for_icon(icon_theme, item->icon, icon_sz);
    gtk_widget_set_overflow(image, GTK_OVERFLOW_VISIBLE);
    gtk_box_append(GTK_BOX(content_box), image);
  }

  item->label = gtk_label_new(name);
  gtk_label_set_yalign(GTK_LABEL(item->label), 0.5f);
  gtk_widget_set_valign(item->label, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(item->label, "dynamic-item-label");
  gtk_box_append(GTK_BOX(content_box), item->label);

  gtk_button_set_child(GTK_BUTTON(item->button), content_box);
  gtk_widget_add_css_class(item->button, "flat");
  gtk_widget_add_css_class(item->button, "dynamic-item");
  gtk_widget_set_overflow(item->button, GTK_OVERFLOW_VISIBLE);

  if (state) {
    apply_dock_item_alignment(item);
  }

  if (item->name && *item->name != '\0') {
    gtk_widget_set_tooltip_text(item->button, item->name);
  }

  g_signal_connect(item->button, "clicked", G_CALLBACK(on_dock_item_left_click),
                   item);

  item->popover = gtk_popover_new();
  gtk_popover_set_has_arrow(GTK_POPOVER(item->popover), FALSE);
  gtk_widget_set_parent(item->popover, item->button);

  if (state) {
    g_signal_connect(item->popover, "closed", G_CALLBACK(on_popover_closed),
                     state);
  }

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

  on_dynamic_item_poll(item);

  if (interval_ms > 0 && scm_is_true(poll_fn)) {
    item->poll_timer_id =
        g_timeout_add(interval_ms, on_dynamic_item_poll, item);
  }

  return item;
}

void free_dock_item(DockItem *item) {
  if (!item)
    return;

  // 1. Cancel dynamic polling timer before deallocating memory
  if (item->poll_timer_id > 0) {
    g_source_remove(item->poll_timer_id);
    item->poll_timer_id = 0;
  }

  // 2. Unprotect Scheme procedure thunk
  if (item->poll_fn != SCM_BOOL_F &&
      scm_is_true(scm_procedure_p(item->poll_fn))) {
    scm_gc_unprotect_object(item->poll_fn);
    item->poll_fn = SCM_BOOL_F;
  }

  // 3. Disconnect button & popover handlers and unparent
  if (item->popover && GTK_IS_WIDGET(item->popover)) {
    g_signal_handlers_disconnect_by_data(item->popover, item->state);
    g_signal_handlers_disconnect_by_data(item->popover, item);
    if (gtk_widget_get_parent(item->popover)) {
      gtk_widget_unparent(item->popover);
    }
    item->popover = NULL;
  }

  if (item->button && GTK_IS_WIDGET(item->button)) {
    g_signal_handlers_disconnect_by_data(item->button, item);
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
  item->windows = NULL;

  g_free(item->name);
  g_free(item->icon);
  g_free(item->exec);
  g_free(item->app_id);

  g_free(item);
}

void clear_favorite_dock_items(LambdockState *state) {
  if (!state || !state->dock_items)
    return;

  clear_window_tracker_dock_references(state);

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

/* ============================================================================
 * WAYLAND WINDOW TRACKING INTEGRATION
 * ============================================================================
 */

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

/* ============================================================================
 * LAYOUT SEPARATORS & RELOAD HELPERS
 * ============================================================================
 */

DockItem *create_separator_dock_item(LambdockState *state,
                                     GtkOrientation orientation) {
  DockItem *item = g_new0(DockItem, 1);
  item->poll_fn = SCM_BOOL_F;
  item->state = state;
  item->is_favorite = TRUE;
  item->hover_animate = FALSE;

  item->button = gtk_separator_new(orientation);

  if (orientation == GTK_ORIENTATION_VERTICAL) {
    gtk_widget_set_margin_start(item->button, SEPARATOR_MARGIN_PX);
    gtk_widget_set_margin_end(item->button, SEPARATOR_MARGIN_PX);
  } else {
    gtk_widget_set_margin_top(item->button, SEPARATOR_MARGIN_PX);
    gtk_widget_set_margin_bottom(item->button, SEPARATOR_MARGIN_PX);
  }

  if (state) {
    apply_dock_item_alignment(item);
  }

  if (state && state->box)
    gtk_box_append(GTK_BOX(state->box), item->button);
  if (state)
    state->dock_items = g_list_append(state->dock_items, item);

  return item;
}

/**
 * reload_dock_instance:
 * @state: Global Lambdock application state.
 *
 * Hot-reloads dock UI appearance, Scheme/Guile configuration rules for
 * state->config_path, custom CSS themes, autohide behavior, screen edge
 * positioning, and rebuilds dock launcher layout without restarting
 * application.
 */
void reload_dock_instance(LambdockState *state) {
  if (!state)
    return;

  LOG_C_INFO("UI", "Hot-reloading UI layout for config: %s",
             state->config_path ? state->config_path : "default");

  // Re-evaluates Scheme settings file for this specific dock instance
  if (state->config_path) {
    load_dock_config_scm(state->config_path);
  }

  // Reload active theme config and custom CSS for this specific dock instance
  g_free(state->theme);
  state->theme = get_theme_config_for(state->config_path);
  load_custom_css(state);

  // Cancel any pending auto-hide timer
  if (state->hide_timer_id != 0) {
    g_source_remove(state->hide_timer_id);
    state->hide_timer_id = 0;
  }

  // Hot-reload Position & Layer Shell Edge Anchoring
  GtkLayerShellEdge new_edge = get_position_config_for(state->config_path);
  if (new_edge != state->edge) {
    if (state->tick_cb_id != 0) {
      gtk_widget_remove_tick_callback(state->window, state->tick_cb_id);
      state->tick_cb_id = 0;
    }

    gtk_layer_set_margin(GTK_WINDOW(state->window), state->edge, 0);
    gtk_layer_set_anchor(GTK_WINDOW(state->window), state->edge, FALSE);

    state->edge = new_edge;
    gtk_layer_set_anchor(GTK_WINDOW(state->window), state->edge, TRUE);

    state->orientation = (state->edge == GTK_LAYER_SHELL_EDGE_LEFT ||
                          state->edge == GTK_LAYER_SHELL_EDGE_RIGHT)
                             ? GTK_ORIENTATION_VERTICAL
                             : GTK_ORIENTATION_HORIZONTAL;
    gtk_orientable_set_orientation(GTK_ORIENTABLE(state->box),
                                   state->orientation);
    if (state->bg_capsule) {
      gtk_orientable_set_orientation(GTK_ORIENTABLE(state->bg_capsule),
                                     state->orientation);
    }

    state->current_margin = 0;
    state->target_margin = 0;
    state->start_margin = 0;
    state->is_hidden = FALSE;
  }

  // Hot-reload Auto-Hide, Hover Animation, Base Icon Size & Layer Shell
  // Exclusive Zone
  state->auto_hide = get_autohide_config_for(state->config_path);
  state->show_when_clear = get_show_when_clear_config_for(state->config_path);
  state->hover_animate = get_hover_animate_config_for(state->config_path);
  state->icon_size = get_icon_size_config_for(state->config_path);

  // Clear pinned items and repopulate dock launcher layout first
  clear_favorite_dock_items(state);
  populate_dock_items_for(state);

  // Re-sync Wayland foreign window handles back to newly populated launchers
  sync_window_trackers(state);

  // Update widget alignments for all dock items
  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    if (!item || !item->button)
      continue;

    apply_dock_item_alignment(item);
  }

  update_dock_dimensions(state);

  if (state->auto_hide) {
    gtk_layer_set_exclusive_zone(GTK_WINDOW(state->window), 0);

    if (state->show_when_clear && state->window_trackers == NULL) {
      reveal_dock(state);
    } else {
      hide_dock(state);
    }
  } else {
    gtk_layer_set_exclusive_zone(GTK_WINDOW(state->window),
                                 state->icon_size + CAPSULE_PADDING_PX);
    reveal_dock(state);
  }
}
