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
#include "shelf.h"               // Glass shelf reflections (icons on the glass)
#include "dock_dnd.h"            // Drag and drop (reorder, add, remove)
#include "icon_resolver.h" // Helper to load application icons from the system theme
#include "launchpad.h"     // Full screen application grid
#include "meter.h"         // Circular gauges for meter dynamic items
#include "popover_menu.h" // Custom right-click and multi-window popover menus
#include "stack.h"         // Folder stacks (Downloads-style dock icons)
#include "theme_engine.h" // CSS and theme engine utilities

/* ============================================================================
 * ITEM IDENTITY & WIDGET BACK POINTERS
 * ============================================================================
 */

const char *dock_item_kind_to_string(LambdockItemKind kind) {
  switch (kind) {
  case LAMBDOCK_ITEM_DYNAMIC:
    return "dynamic";
  case LAMBDOCK_ITEM_SEPARATOR:
    return "separator";
  case LAMBDOCK_ITEM_STACK:
    return "stack";
  case LAMBDOCK_ITEM_LAUNCHPAD:
    return "launchpad";
  case LAMBDOCK_ITEM_APP:
  default:
    return "app";
  }
}

char *dock_item_key(const DockItem *item) {
  if (!item)
    return NULL;
  return g_strdup_printf("%s:%s", dock_item_kind_to_string(item->kind),
                         item->name ? item->name : "");
}

/**
 * dock_item_attach_data:
 * Stores the owning DockItem on every widget of the item that deferred callbacks
 * may hold on to, and clears it again in free_dock_item(). This is what makes
 * drag-and-drop removals safe across hot reloads: the widget survives (it is
 * referenced by the drag machinery) but the data becomes NULL when the item is
 * destroyed, so the callback can bail out instead of touching freed memory.
 */
static void dock_item_attach_data(DockItem *item) {
  if (!item)
    return;
  if (item->button)
    g_object_set_data(G_OBJECT(item->button), "lambdock-item", item);
  if (item->container)
    g_object_set_data(G_OBJECT(item->container), "lambdock-item", item);
}

DockItem *dock_item_from_widget(GtkWidget *widget) {
  if (!widget || !G_IS_OBJECT(widget))
    return NULL;
  return g_object_get_data(G_OBJECT(widget), "lambdock-item");
}

/* ============================================================================
 * ALIGNMENT HELPERS
 * ============================================================================
 */

/**
 * apply_dock_item_alignment:
 * Applies edge-anchored alignment rules (valign/halign) to DockItem widgets,
 * ensuring non-animated items and separators remain anchored to the dock edge.
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

  // Ensure ALL buttons (icons, text-only, separators) request full base_sz
  // cross-axis headroom
  if (state->orientation == GTK_ORIENTATION_HORIZONTAL) {
    gtk_widget_set_size_request(item->button, -1, base_sz);
  } else {
    gtk_widget_set_size_request(item->button, base_sz, -1);
  }

  GtkAlign halign = GTK_ALIGN_CENTER;
  GtkAlign valign = GTK_ALIGN_CENTER;

  if (state->edge == GTK_LAYER_SHELL_EDGE_BOTTOM) {
    valign = GTK_ALIGN_END;
  } else if (state->edge == GTK_LAYER_SHELL_EDGE_TOP) {
    valign = GTK_ALIGN_START;
  } else if (state->edge == GTK_LAYER_SHELL_EDGE_LEFT) {
    halign = GTK_ALIGN_START;
  } else if (state->edge == GTK_LAYER_SHELL_EDGE_RIGHT) {
    halign = GTK_ALIGN_END;
  }

  gtk_widget_set_valign(item->button, valign);
  gtk_widget_set_halign(item->button, halign);

  /* The per-item container holds the button plus (when reflections are on)
   * its glass shelf reflection, so it hugs the same dock edge as the button. */
  if (item->container) {
    gtk_widget_set_valign(item->container, valign);
    gtk_widget_set_halign(item->container, halign);
  }

  GtkWidget *child = NULL;
  if (GTK_IS_BUTTON(item->button)) {
    child = gtk_button_get_child(GTK_BUTTON(item->button));
  }

  if (child) {
    gtk_widget_set_valign(child, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(child, GTK_ALIGN_CENTER);
    if (GTK_IS_BOX(child)) {
      for (GtkWidget *w = gtk_widget_get_first_child(child); w != NULL;
           w = gtk_widget_get_next_sibling(w)) {
        gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
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
 * If 1 window is open  -> Focus/activate that window.
 * If >1 window is open -> Open a popover menu listing all open windows.
 * If 0 windows open    -> Execute the application launcher command
 * (`item->exec`).
 */
static void on_dock_item_left_click(GtkButton *btn, gpointer user_data) {
  (void)btn;

  DockItem *item = (DockItem *)user_data;
  if (!item)
    return;

  /* Stacks open a popover with their folder contents; the launchpad opens the
   * full screen application grid. Neither has windows or a command to run. */
  if (item->kind == LAMBDOCK_ITEM_STACK) {
    lambdock_stack_show(item);
    return;
  }

  if (item->kind == LAMBDOCK_ITEM_LAUNCHPAD) {
    lambdock_launchpad_show(item->state);
    return;
  }

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

  /* Meter items read a number: it fills the ring and is written in the middle
   * with the configured unit ("42%", "54°C"). */
  if (item->meter && scm_is_number(res)) {
    double value = scm_to_double(res);
    double maximum = (item->meter_max > 0.0) ? item->meter_max : 100.0;
    char *text = g_strdup_printf("%.0f%s", value,
                                 item->meter_unit ? item->meter_unit : "");

    lambdock_meter_set_value(item->meter, value / maximum, text);

    char *tooltip = g_strdup_printf("%s: %s", item->name ? item->name : "",
                                    text);
    gtk_widget_set_tooltip_text(item->button, tooltip);

    g_free(tooltip);
    g_free(text);
    return G_SOURCE_CONTINUE;
  }

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
  item->kind = LAMBDOCK_ITEM_APP;

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

  /* App items host their icon and an (initially hidden) window-title label in
   * a horizontal box. The label is shown only when `show-window-titles?` is on
   * and the app has a running window (Windows XP-style taskbar buttons). */
  GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_valign(content_box, GTK_ALIGN_CENTER);

  if (image) {
    gtk_widget_set_overflow(image, GTK_OVERFLOW_VISIBLE);
    gtk_box_append(GTK_BOX(content_box), image);
  }

  item->label = gtk_label_new(NULL);
  gtk_widget_add_css_class(item->label, "window-title-label");
  gtk_label_set_ellipsize(GTK_LABEL(item->label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(item->label),
                                WINDOW_TITLE_MAX_CHARS);
  gtk_label_set_single_line_mode(GTK_LABEL(item->label), TRUE);
  gtk_widget_set_valign(item->label, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(item->label, FALSE);
  gtk_box_append(GTK_BOX(content_box), item->label);
  item->window_title_label = TRUE;

  gtk_button_set_child(GTK_BUTTON(item->button), content_box);

  gtk_widget_add_css_class(item->button, "flat");
  gtk_widget_set_overflow(item->button, GTK_OVERFLOW_VISIBLE);

  /* The icon can cast a mirrored, blurred reflection over the glass shelf.
   * It stays hidden (and takes no room) while the reflection parameter is 0,
   * so the flat dock is unaffected. */
  if (image) {
    item->reflection = shelf_reflection_new(image);
    gtk_widget_set_visible(item->reflection, FALSE);
  }
  item->container = shelf_wrap_item(state, item->button, item->reflection);

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

  dock_dnd_setup_item(item);

  if (state && state->box) {
    GtkWidget *child = item->container ? item->container : item->button;
    gtk_box_append(GTK_BOX(state->box), child);
  }

  if (state)
    state->dock_items = g_list_append(state->dock_items, item);

  dock_item_attach_data(item);

  return item;
}

DockItem *create_dynamic_dock_item(LambdockState *state, const char *name,
                                   const char *icon, const char *exec,
                                   SCM poll_fn, guint interval_ms,
                                   gboolean hover_animate) {
  DockItem *item = g_new0(DockItem, 1);

  item->kind = LAMBDOCK_ITEM_DYNAMIC;
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
  gtk_widget_set_valign(content_box,
                        GTK_ALIGN_CENTER); // Add explicit vertical centering

  GtkWidget *image = NULL;
  if (icon && *icon != '\0') {
    GdkDisplay *display = gdk_display_get_default();
    GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);
    int icon_sz =
        (state && state->icon_size > 0) ? state->icon_size : DEFAULT_ICON_SIZE;
    image = create_image_for_icon(icon_theme, item->icon, icon_sz);
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

  if (image) {
    item->reflection = shelf_reflection_new(image);
    gtk_widget_set_visible(item->reflection, FALSE);
  }
  item->container = shelf_wrap_item(state, item->button, item->reflection);

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

  dock_dnd_setup_item(item);

  if (state && state->box) {
    GtkWidget *child = item->container ? item->container : item->button;
    gtk_box_append(GTK_BOX(state->box), child);
  }

  if (state)
    state->dock_items = g_list_append(state->dock_items, item);

  dock_item_attach_data(item);

  on_dynamic_item_poll(item);

  if (interval_ms > 0 && scm_is_true(poll_fn)) {
    item->poll_timer_id =
        g_timeout_add(interval_ms, on_dynamic_item_poll, item);
  }

  return item;
}

/**
 * dock_item_enable_meter:
 * Turns a dynamic item into a gauge: one widget that is both the "icon" and the
 * reading, instead of an icon next to a changing string.
 *
 * The text label (and any icon) the item was built with is dropped, because the
 * gauge carries the number itself; the poll callback then feeds it a number
 * instead of a string.
 */
void dock_item_enable_meter(DockItem *item, const char *unit,
                            double max_value) {
  if (!item || !item->button)
    return;

  GtkWidget *content = gtk_button_get_child(GTK_BUTTON(item->button));
  if (!content || !GTK_IS_BOX(content))
    return;

  /* Drop the text form: the label would otherwise keep a stale reading next to
   * the gauge (and pointer to a widget that is about to be destroyed). */
  item->label = NULL;
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(content)) != NULL)
    gtk_box_remove(GTK_BOX(content), child);

  /* A gauge has no icon to mirror on the shelf. */
  if (item->reflection) {
    GtkWidget *parent = gtk_widget_get_parent(item->reflection);
    if (parent && GTK_IS_BOX(parent))
      gtk_box_remove(GTK_BOX(parent), item->reflection);
    else if (parent)
      gtk_widget_unparent(item->reflection);
    item->reflection = NULL;
  }

  int icon_size =
      (item->state && item->state->icon_size > 0) ? item->state->icon_size
                                                 : DEFAULT_ICON_SIZE;

  GtkWidget *meter = lambdock_meter_new();
  lambdock_meter_set_icon_size(meter, icon_size);
  gtk_box_append(GTK_BOX(content), meter);

  item->meter = meter;
  g_free(item->meter_unit);
  item->meter_unit = g_strdup(unit ? unit : "");
  item->meter_max = (max_value > 0.0) ? max_value : 100.0;

  gtk_widget_add_css_class(item->button, "dynamic-meter");

  /* Paint the first reading now instead of after the first interval. */
  on_dynamic_item_poll(item);
}

void free_dock_item(DockItem *item) {
  if (!item)
    return;

  // Cancel dynamic polling timer before deallocating memory
  if (item->poll_timer_id > 0) {
    g_source_remove(item->poll_timer_id);
    item->poll_timer_id = 0;
  }

  /* Detach the back pointers first: deferred drag callbacks may still hold the
   * widgets, and must see "no item" instead of a freed one. */
  if (item->button)
    g_object_set_data(G_OBJECT(item->button), "lambdock-item", NULL);
  if (item->container)
    g_object_set_data(G_OBJECT(item->container), "lambdock-item", NULL);

  // Unprotect Scheme procedure thunk
  if (item->poll_fn != SCM_BOOL_F &&
      scm_is_true(scm_procedure_p(item->poll_fn))) {
    scm_gc_unprotect_object(item->poll_fn);
    item->poll_fn = SCM_BOOL_F;
  }

  // Disconnect button & popover handlers and unparent
  if (item->popover && GTK_IS_WIDGET(item->popover)) {
    g_signal_handlers_disconnect_by_data(item->popover, item->state);
    g_signal_handlers_disconnect_by_data(item->popover, item);
    /* If this item is torn down while its popover is still open, the "closed"
     * handler that normally decrements the counter is now disconnected, so
     * release the popover slot here. Otherwise active_popovers stays > 0 and
     * the dock can never auto-hide again. */
    if (item->state && gtk_widget_get_visible(item->popover) &&
        item->state->active_popovers > 0) {
      item->state->active_popovers--;
    }
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

  /* Detach the per-item wrapper from the dock box, keeping a reference while
   * it is unparented. The reflection (if any) is owned by the wrapper and is
   * destroyed along with it. */
  item->reflection = NULL;
  if (item->container && GTK_IS_WIDGET(item->container)) {
    g_object_ref_sink(item->container);
    GtkWidget *container_parent = gtk_widget_get_parent(item->container);
    if (container_parent && GTK_IS_BOX(container_parent))
      gtk_box_remove(GTK_BOX(container_parent), item->container);
    g_object_unref(item->container);
    item->container = NULL;
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
  g_free(item->stack_path);
  g_free(item->stack_view);
  g_free(item->meter_unit);

  g_free(item);
}

void clear_tracked_windows(LambdockState *state) {
  if (!state)
    return;

  /* Drop every AppWindow and all tracked handles, then remove the non-favorite
   * items that existed only because a window was tracked. Used when the Wayland
   * foreign-toplevel manager goes away and its handles become invalid, so no
   * dock keeps dangling pointers. */
  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    if (!item)
      continue;

    for (GList *w = item->windows; w != NULL; w = w->next) {
      AppWindow *win = (AppWindow *)w->data;
      if (win) {
        g_free(win->title);
        g_free(win);
      }
    }
    g_list_free(item->windows);
    item->windows = NULL;
    update_dock_item_status(item);
  }

  GList *curr = state->dock_items;
  while (curr != NULL) {
    GList *next = curr->next;
    DockItem *item = (DockItem *)curr->data;
    if (item && !item->is_favorite) {
      state->dock_items = g_list_delete_link(state->dock_items, curr);
      free_dock_item(item);
    }
    curr = next;
  }

  g_list_free(state->window_trackers);
  state->window_trackers = NULL;

  if (state->show_when_clear)
    reveal_dock(state);
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

  /* Idempotency guard: a handle must map to exactly one AppWindow. Callers
   * such as sync_window_trackers() (invoked on every reload) and repeated
   * toplevel app_id events can re-add the same handle; without this check they
   * would duplicate entries, leak AppWindow structs and inflate window counts. */
  for (GList *l = item->windows; l != NULL; l = l->next) {
    AppWindow *existing = (AppWindow *)l->data;
    if (existing && existing->handle == handle)
      return existing;
  }

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

  /* The glass shelf draws its own running indicator near the outer edge. */
  if (item->reflection)
    shelf_reflection_set_running(item->reflection, item->windows != NULL);

  /* Keep the window-title label in sync for app items (never for dynamic items,
   * whose label carries polled text, or separators). */
  if (!item->window_title_label || !item->label)
    return;

  gboolean show = item->state && item->state->show_window_titles &&
                  item->windows != NULL;
  if (show) {
    AppWindow *win = (AppWindow *)item->windows->data;
    const char *title = (win && win->title && *win->title != '\0')
                            ? win->title
                            : (item->name ? item->name : item->app_id);
    gtk_label_set_text(GTK_LABEL(item->label), title ? title : "");
    gtk_widget_set_visible(item->label, TRUE);
  } else {
    gtk_widget_set_visible(item->label, FALSE);
  }
}

/**
 * exec_matches_app_id:
 * Allocation-free, case-insensitive token match of APP_ID inside EXEC, used as
 * a last-resort heuristic when the exact app_id/icon/name comparisons fail.
 * Requires whole-token (word boundary) matches and a minimum length so short
 * ids cannot match unrelated executables, and it no longer tests the reverse
 * direction (exec contained in app_id), which produced false positives.
 */
static gboolean exec_matches_app_id(const char *exec, const char *app_id) {
  if (!exec || *exec == '\0' || !app_id)
    return FALSE;

  size_t idlen = strlen(app_id);
  if (idlen < 3)
    return FALSE;

  for (const char *p = exec; *p != '\0'; p++) {
    if (p != exec && g_ascii_isalnum((guchar)p[-1]))
      continue; /* not at a token boundary */

    if (g_ascii_strncasecmp(p, app_id, idlen) != 0)
      continue;

    char right = p[idlen];
    if (right == '\0' || !g_ascii_isalnum((guchar)right))
      return TRUE;
  }
  return FALSE;
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

    if (exec_matches_app_id(item->exec, app_id))
      return item;
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
  item->kind = LAMBDOCK_ITEM_SEPARATOR;
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

  /* Separators carry no icon, so they only get the wrapper box (keeping the
   * item list uniform); no reflection is created for them. */
  item->container = shelf_wrap_item(state, item->button, NULL);

  if (state && state->box) {
    GtkWidget *child = item->container ? item->container : item->button;
    gtk_box_append(GTK_BOX(state->box), child);
  }
  if (state)
    state->dock_items = g_list_append(state->dock_items, item);

  dock_item_attach_data(item);

  return item;
}

/* ============================================================================
 * STACKS & THE LAUNCHPAD
 * ============================================================================
 */

/* Creates the widget shell shared by the icon-only item kinds (stacks and the
 * launchpad): button with the icon, context popover, right click menu, glass
 * reflection, drag source and dock container. */
static DockItem *create_icon_dock_item(LambdockState *state,
                                       LambdockItemKind kind, const char *name,
                                       const char *icon, gboolean hover_animate) {
  DockItem *item = g_new0(DockItem, 1);
  item->poll_fn = SCM_BOOL_F;
  item->kind = kind;
  item->state = state;
  item->name = g_strdup(name);
  item->icon = g_strdup(icon);
  item->exec = g_strdup("");
  item->app_id = g_strdup(name);
  item->is_favorite = TRUE;
  item->hover_animate = hover_animate;

  GtkIconTheme *icon_theme =
      gtk_icon_theme_get_for_display(gdk_display_get_default());
  int icon_sz =
      (state && state->icon_size > 0) ? state->icon_size : DEFAULT_ICON_SIZE;

  GtkWidget *image = create_image_for_icon(icon_theme, item->icon, icon_sz);
  item->button = gtk_button_new();

  GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign(content_box, GTK_ALIGN_CENTER);
  if (image) {
    gtk_widget_set_overflow(image, GTK_OVERFLOW_VISIBLE);
    gtk_box_append(GTK_BOX(content_box), image);
  }
  gtk_button_set_child(GTK_BUTTON(item->button), content_box);

  gtk_widget_add_css_class(item->button, "flat");
  gtk_widget_add_css_class(item->button,
                           (kind == LAMBDOCK_ITEM_STACK) ? "stack-item"
                                                         : "launchpad-item");
  gtk_widget_set_overflow(item->button, GTK_OVERFLOW_VISIBLE);

  if (image) {
    item->reflection = shelf_reflection_new(image);
    gtk_widget_set_visible(item->reflection, FALSE);
  }
  item->container = shelf_wrap_item(state, item->button, item->reflection);

  if (state)
    apply_dock_item_alignment(item);

  if (item->name && *item->name != '\0')
    gtk_widget_set_tooltip_text(item->button, item->name);

  g_signal_connect(item->button, "clicked", G_CALLBACK(on_dock_item_left_click),
                   item);

  item->popover = gtk_popover_new();
  gtk_popover_set_has_arrow(GTK_POPOVER(item->popover), FALSE);
  gtk_widget_set_parent(item->popover, item->button);
  if (state)
    g_signal_connect(item->popover, "closed", G_CALLBACK(on_popover_closed),
                     state);

  GtkGesture *right_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click),
                                GDK_BUTTON_SECONDARY);
  g_signal_connect(right_click, "pressed", G_CALLBACK(on_dock_item_right_click),
                   item);
  gtk_widget_add_controller(item->button, GTK_EVENT_CONTROLLER(right_click));

  dock_dnd_setup_item(item);

  if (state && state->box) {
    GtkWidget *child = item->container ? item->container : item->button;
    gtk_box_append(GTK_BOX(state->box), child);
  }
  if (state)
    state->dock_items = g_list_append(state->dock_items, item);

  dock_item_attach_data(item);
  return item;
}

DockItem *create_stack_dock_item(LambdockState *state, const char *name,
                                 const char *icon, const char *path,
                                 const char *view, gboolean hover_animate) {
  DockItem *item = create_icon_dock_item(
      state, LAMBDOCK_ITEM_STACK, name,
      (icon && *icon != '\0') ? icon : "folder", hover_animate);

  item->stack_path = g_strdup(path ? path : "");
  item->stack_view =
      g_strdup((view && *view != '\0') ? view : "grid");

  LOG_C_INFO("UI", "Created stack item: %s (%s, %s view)",
             item->name ? item->name : "stack", item->stack_path,
             item->stack_view);
  return item;
}

DockItem *create_launchpad_dock_item(LambdockState *state, const char *name,
                                     const char *icon,
                                     gboolean hover_animate) {
  DockItem *item = create_icon_dock_item(
      state, LAMBDOCK_ITEM_LAUNCHPAD, name,
      (icon && *icon != '\0') ? icon : "view-app-grid-symbolic",
      hover_animate);

  LOG_C_INFO("UI", "Created launchpad item: %s", item->name ? item->name : "");
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

  // Reload active theme config and custom CSS for this specific dock instance
  free(state->theme); /* get_theme_config_for may return a scm_to_utf8_string result */
  state->theme = get_theme_config_for(state->config_path);

  // Mode flags must refresh before CSS reload so the taskbar modifier
  // stylesheet is (un)loaded to match.
  gboolean prev_taskbar = state->taskbar;
  LambdockShelfParams prev_shelf = state->shelf;
  state->taskbar = get_taskbar_config_for(state->config_path);
  state->show_window_titles =
      get_show_window_titles_config_for(state->config_path);
  get_shelf_config_for(state->config_path, &state->shelf);

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
  state->track_windows = get_track_windows_config_for(state->config_path);
  state->show_when_clear = get_show_when_clear_config_for(state->config_path);
  state->hover_animate = get_hover_animate_config_for(state->config_path);
  state->icon_size = get_icon_size_config_for(state->config_path);
  state->item_spacing = get_item_spacing_config_for(state->config_path);
  if (state->box) {
    gtk_box_set_spacing(GTK_BOX(state->box), state->item_spacing);
  }

  // Clear pinned items and repopulate dock launcher layout first
  clear_favorite_dock_items(state);
  populate_dock_items_for(state);

  // Re-sync Wayland foreign window handles back to newly populated launchers
  sync_window_trackers(state);

  // Update widget alignments and status (running indicator + window titles)
  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    if (!item || !item->button)
      continue;

    apply_dock_item_alignment(item);
    update_dock_item_status(item);
  }

  apply_dock_mode_classes(state);
  if (state->taskbar != prev_taskbar ||
      memcmp(&state->shelf, &prev_shelf, sizeof(LambdockShelfParams)) != 0) {
    refresh_dock_mode(state);
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
                                 dock_capsule_cross_dim(state));
    reveal_dock(state);
  }
}
