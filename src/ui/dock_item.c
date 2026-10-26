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
#include "../accessibility.h"    // Screen reader labels and motion preferences
#include "../dock_window.h"      // Recording whether the dock is mid-animation
#include "../guile_bridge.h"     // Scheme/Guile configuration interface
#include "../logger.h"           // Logging macro utilities (e.g., LOG_C_INFO)
#include "../toplevel_tracker.h" // Foreign window tracking via Wayland protocols
#include "shelf.h"               // Glass shelf reflections (icons on the glass)
#include "shelf_follow.h"         // Items that follow the shelf's projection
#include "desktop_entry.h" // XDG desktop entries (app id matching)
#include "dock_dnd.h"            // Drag and drop (reorder, add, remove)
#include "icon_resolver.h" // Helper to load application icons from the system theme
#include "icon_zoom.h"     // Fisheye magnification by transform, not re-raster
#include "launchpad.h"     // Full screen application grid
#include "meter.h"         // Circular gauges for meter dynamic items
#include "popover_menu.h" // Custom right-click and multi-window popover menus
#include "stack.h"         // Folder stacks (Downloads-style dock icons)
#include "theme_engine.h" // CSS and theme engine utilities
#include "trash.h"         // Desktop trash (count, empty, open)
#include <glib/gi18n.h>

/* ============================================================================
 * FISHEYE ZOOM WRAPPER
 * ============================================================================
 */

/**
 * wrap_icon_for_zoom:
 * @image: (transfer none): the icon to put in a dock item.
 * @base_size: the icon size the dock lays out at rest.
 *
 * Wraps @image so the fisheye can magnify it by painting a transform instead of
 * asking the icon theme for the artwork at every pixel size the pointer passes
 * through (see icon_zoom.h).
 *
 * Returns: (transfer none) (nullable): the widget to append to the item's
 * content box.
 */
static GtkWidget *wrap_icon_for_zoom(GtkWidget *image, int base_size) {
  if (!image)
    return NULL;

  if (base_size <= 0)
    base_size = DEFAULT_ICON_SIZE;

  int max_size = (int)ceil((double)base_size * FISHEYE_MAX_SCALE);
  return lambdock_icon_zoom_new(image, base_size, max_size);
}

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
  case LAMBDOCK_ITEM_TRASH:
    return "trash";
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

int dock_item_icon_size(DockItem *item) {
  if (!item || !item->button || !GTK_IS_BUTTON(item->button))
    return 0;

  GtkWidget *child = gtk_button_get_child(GTK_BUTTON(item->button));
  if (!child)
    return 0;

  /* App and dynamic items hold their icon inside a zoom container; stacks, the
   * launchpad and meter items put it straight in the content box. */
  if (GTK_IS_BOX(child)) {
    for (GtkWidget *w = gtk_widget_get_first_child(child); w != NULL;
         w = gtk_widget_get_next_sibling(w)) {
      if (lambdock_icon_zoom_is_zoom(w))
        return lambdock_icon_zoom_target_size(w);
      if (GTK_IS_IMAGE(w) && gtk_image_get_pixel_size(GTK_IMAGE(w)) > 0)
        return gtk_image_get_pixel_size(GTK_IMAGE(w));
    }
    return 0;
  }

  if (lambdock_icon_zoom_is_zoom(child))
    return lambdock_icon_zoom_target_size(child);
  if (GTK_IS_IMAGE(child) && gtk_image_get_pixel_size(GTK_IMAGE(child)) > 0)
    return gtk_image_get_pixel_size(GTK_IMAGE(child));

  return 0;
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
 * ACCESSIBILITY (SCREEN READERS & KEYBOARD)
 * ============================================================================
 *
 * A dock is a row of icons, which a screen reader sees as a row of unnamed
 * buttons unless each one says what it is and what it currently does. The
 * labels are rebuilt whenever the item's state changes (a window opened, the
 * trash was filled, a reading arrived), and the arrow keys walk the row so the
 * whole dock can be used without a pointer.
 */

/* What a screen reader should say about an app item besides its name. */
static char *dock_item_running_text(const DockItem *item) {
  guint count = item->windows ? g_list_length(item->windows) : 0;

  if (count == 0)
    return g_strdup(_("Not running"));

  return g_strdup_printf(
      ngettext("Running, %u window", "Running, %u windows", count), count);
}

void dock_item_update_accessible(DockItem *item) {
  if (!item || !item->button || item->kind == LAMBDOCK_ITEM_SEPARATOR)
    return;

  const char *name = (item->name && *item->name != '\0') ? item->name : NULL;
  char *label = NULL;
  char *description = NULL;

  switch (item->kind) {
  case LAMBDOCK_ITEM_STACK:
    label = g_strdup_printf(_("%s, folder stack"), name ? name : _("Stack"));
    description = g_strdup(_("Opens the folder's contents."));
    break;
  case LAMBDOCK_ITEM_LAUNCHPAD:
    label = g_strdup(name ? name : _("Applications"));
    description = g_strdup(_("Opens the application grid. Escape closes it."));
    break;
  case LAMBDOCK_ITEM_TRASH:
    label = g_strdup(name ? name : _("Trash"));
    if (item->trash_count > 0) {
      char *count = g_strdup_printf(
          ngettext("%u item in the trash", "%u items in the trash",
                   item->trash_count),
          item->trash_count);
      description = g_strdup_printf(_("%s. Opens the trash."), count);
      g_free(count);
    } else {
      description = g_strdup(_("The trash is empty. Opens the trash."));
    }
    break;
  case LAMBDOCK_ITEM_DYNAMIC: {
    const char *reading = gtk_widget_get_tooltip_text(item->button);
    if (reading && *reading != '\0') {
      /* Meter items already put "name: reading" in the tooltip. */
      label = g_str_has_prefix(reading, name ? name : "")
                  ? g_strdup(reading)
                  : g_strdup_printf("%s: %s", name ? name : _("Reading"),
                                    reading);
    } else {
      label = g_strdup(name ? name : _("Reading"));
    }
    description = g_strdup(_("Live reading from the dock."));
    break;
  }
  case LAMBDOCK_ITEM_APP:
  default:
    label = g_strdup(name ? name : _("Application"));
    description = dock_item_running_text(item);
    break;
  }

  lambdock_accessible_update(item->button, label, description);
  g_free(label);
  g_free(description);
}

/* Separators are decoration: they must not take focus, and they have no
 * keyboard actions. */
static gboolean item_is_keyboard_reachable(const DockItem *item) {
  return item && item->button && item->kind != LAMBDOCK_ITEM_SEPARATOR;
}

int lambdock_dock_item_count(const LambdockState *state) {
  if (!state)
    return 0;

  int count = 0;
  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    if (item_is_keyboard_reachable((const DockItem *)l->data))
      count++;
  }
  return count;
}

DockItem *lambdock_dock_item_at_index(const LambdockState *state, int index) {
  if (!state || index < 0)
    return NULL;

  int i = 0;
  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    if (!item_is_keyboard_reachable(item))
      continue;
    if (i == index)
      return item;
    i++;
  }
  return NULL;
}

int lambdock_dock_item_index_of(const LambdockState *state,
                                const DockItem *item) {
  if (!state || !item)
    return -1;

  int i = 0;
  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *candidate = (DockItem *)l->data;
    if (!item_is_keyboard_reachable(candidate))
      continue;
    if (candidate == item)
      return i;
    i++;
  }
  return -1;
}

/* The item that currently holds the keyboard focus in this dock, looking through
 * the button's children so a focused label still resolves to its item. */
static DockItem *focused_dock_item(LambdockState *state) {
  if (!state || !state->window || !GTK_IS_WINDOW(state->window))
    return NULL;

  GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(state->window));
  for (GtkWidget *w = focus; w != NULL; w = gtk_widget_get_parent(w)) {
    DockItem *item = dock_item_from_widget(w);
    if (item)
      return item;
    if (w == state->window)
      break;
  }
  return NULL;
}

DockItem *lambdock_dock_item_focused(LambdockState *state) {
  return focused_dock_item(state);
}

gboolean lambdock_dock_item_focus_step(LambdockState *state, int delta) {
  if (!state || delta == 0)
    return FALSE;

  int count = lambdock_dock_item_count(state);
  if (count == 0)
    return FALSE;

  DockItem *current = focused_dock_item(state);
  int index = current ? lambdock_dock_item_index_of(state, current)
                      : (delta > 0 ? -1 : count);
  int next = CLAMP(index + delta, 0, count - 1);

  if (current && next == index)
    return FALSE;

  DockItem *target = lambdock_dock_item_at_index(state, next);
  if (!target || !target->button)
    return FALSE;

  gtk_widget_grab_focus(target->button);
  return TRUE;
}

gboolean lambdock_dock_item_focus_edge(LambdockState *state, gboolean last) {
  int count = lambdock_dock_item_count(state);
  if (count == 0)
    return FALSE;

  DockItem *target =
      lambdock_dock_item_at_index(state, last ? count - 1 : 0);
  if (!target || !target->button)
    return FALSE;

  gtk_widget_grab_focus(target->button);
  return TRUE;
}

/* Lays the dock's child widgets out in the order of state->dock_items. */
static void reorder_dock_widgets(LambdockState *state) {
  if (!state || !state->box || !GTK_IS_BOX(state->box))
    return;

  GtkWidget *previous = NULL;
  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    GtkWidget *child =
        item->container ? item->container : item->button;
    if (!child || gtk_widget_get_parent(child) != state->box)
      continue;

    gtk_box_reorder_child_after(GTK_BOX(state->box), child, previous);
    previous = child;
  }
}

gboolean lambdock_dock_item_move(LambdockState *state, DockItem *item,
                                 int delta) {
  if (!state || !item || delta == 0)
    return FALSE;

  int count = lambdock_dock_item_count(state);
  int index = lambdock_dock_item_index_of(state, item);
  int target = index + delta;
  if (index < 0 || target < 0 || target >= count)
    return FALSE;

  DockItem *other = lambdock_dock_item_at_index(state, target);
  if (!other)
    return FALSE;

  GList *link = g_list_find(state->dock_items, item);
  GList *other_link = g_list_find(state->dock_items, other);
  if (!link || !other_link)
    return FALSE;

  state->dock_items = g_list_delete_link(state->dock_items, link);
  other_link = g_list_find(state->dock_items, other);
  if (!other_link) {
    /* The neighbour vanished from under us: put the item back rather than
     * dropping it from the dock. */
    state->dock_items = g_list_append(state->dock_items, item);
    return FALSE;
  }

  state->dock_items = g_list_insert_before(
      state->dock_items, delta > 0 ? other_link->next : other_link, item);

  reorder_dock_widgets(state);
  dock_dnd_persist_order(state);
  return TRUE;
}

void lambdock_dock_item_show_menu(DockItem *item) {
  if (!item || !item->popover || !item->state)
    return;

  if (gtk_widget_get_visible(item->popover)) {
    gtk_popover_popdown(GTK_POPOVER(item->popover));
    return;
  }

  populate_popover_menu(item);

  if (!gtk_widget_get_visible(item->popover))
    item->state->active_popovers++;

  gtk_popover_popup(GTK_POPOVER(item->popover));
}

gboolean lambdock_dock_items_close_popovers(LambdockState *state) {
  if (!state)
    return FALSE;

  gboolean closed = FALSE;
  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    if (item && item->popover && gtk_widget_get_visible(item->popover)) {
      gtk_popover_popdown(GTK_POPOVER(item->popover));
      closed = TRUE;
    }
  }

  if (state->popover && gtk_widget_get_visible(state->popover)) {
    gtk_popover_popdown(GTK_POPOVER(state->popover));
    closed = TRUE;
  }

  if (lambdock_launchpad_is_open()) {
    lambdock_launchpad_hide();
    closed = TRUE;
  }

  return closed;
}

gboolean lambdock_dock_items_handle_key(LambdockState *state, guint keyval,
                                        GdkModifierType modifiers) {
  if (!state)
    return FALSE;

  gboolean ctrl = (modifiers & GDK_CONTROL_MASK) != 0;
  DockItem *focused = focused_dock_item(state);

  if (keyval == GDK_KEY_Escape)
    return lambdock_dock_items_close_popovers(state);

  /* The Menu key and Shift+F10 are the keyboard's right click. */
  if (keyval == GDK_KEY_Menu ||
      (keyval == GDK_KEY_F10 && (modifiers & GDK_SHIFT_MASK) != 0)) {
    if (!focused)
      return FALSE;
    lambdock_dock_item_show_menu(focused);
    return TRUE;
  }

  int step = 0;
  if (state->orientation == GTK_ORIENTATION_HORIZONTAL) {
    if (keyval == GDK_KEY_Left)
      step = -1;
    else if (keyval == GDK_KEY_Right)
      step = 1;
  } else {
    if (keyval == GDK_KEY_Up)
      step = -1;
    else if (keyval == GDK_KEY_Down)
      step = 1;
  }

  if (step != 0) {
    /* Ctrl+arrow rearranges the dock, so the pointer drag is not the only way
     * to move an icon. */
    if (ctrl) {
      if (!focused)
        return FALSE;
      return lambdock_dock_item_move(state, focused, step);
    }
    return lambdock_dock_item_focus_step(state, step);
  }

  if (keyval == GDK_KEY_Home)
    return lambdock_dock_item_focus_edge(state, FALSE);
  if (keyval == GDK_KEY_End)
    return lambdock_dock_item_focus_edge(state, TRUE);

  return FALSE;
}

static gboolean on_dock_key_pressed(GtkEventControllerKey *controller,
                                    guint keyval, guint keycode,
                                    GdkModifierType modifiers,
                                    gpointer user_data) {
  (void)controller;
  (void)keycode;
  return lambdock_dock_items_handle_key((LambdockState *)user_data, keyval,
                                        modifiers);
}

void lambdock_dock_attach_key_navigation(GtkWidget *root, LambdockState *state) {
  if (!root || !state)
    return;

  if (g_object_get_data(G_OBJECT(root), "lambdock-key-navigation"))
    return;

  GtkEventController *keys = gtk_event_controller_key_new();
  /* Capture phase: the arrow keys must reach the dock before a child (a
   * focused icon, the scroll view) consumes them as its own navigation. */
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_dock_key_pressed), state);
  gtk_widget_add_controller(root, keys);
  g_object_set_data(G_OBJECT(root), "lambdock-key-navigation",
                    GINT_TO_POINTER(1));
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

  if (item->kind == LAMBDOCK_ITEM_TRASH) {
    if (!lambdock_trash_open())
      LOG_C_WARN("UI", "Could not open the trash");
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
    dock_item_update_accessible(item);

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
      dock_item_update_accessible(item);
      free(text);
    }
  }

  return G_SOURCE_CONTINUE;
}

/* ============================================================================
 * DOCK ITEM LIFECYCLE (CREATION & DESTRUCTION)
 * ============================================================================
 */

/**
 * resolve_desktop_entry:
 * Finds the desktop entry an item's launcher refers to, so the item can be
 * matched to windows by the application's own identity instead of by the name
 * the user happened to give it.
 *
 * The exec line is the most reliable hint (it is what the entry itself runs),
 * then the icon (entries name their icon after the app), then the app id.
 *
 * Returns: (transfer none) (nullable): the entry, owned by the cache.
 */
static const LambdockDesktopEntry *resolve_desktop_entry(const char *exec,
                                                        const char *icon,
                                                        const char *app_id) {
  const char *candidates[3] = {exec, icon, app_id};

  for (int i = 0; i < 3; i++) {
    if (!candidates[i] || *candidates[i] == '\0')
      continue;

    const LambdockDesktopEntry *entry =
        lambdock_desktop_entry_lookup(candidates[i]);
    if (entry && entry->stem)
      return entry;
  }

  return NULL;
}

/**
 * icon_is_resolvable:
 * A quick check of whether an icon name will actually draw: a resource, a file
 * on disk, an icon in the theme, or its symbolic sibling. It mirrors the first
 * stages of create_image_for_icon(), and is only used to decide whether a
 * desktop entry's icon is a better bet than a configured one.
 */
static gboolean icon_is_resolvable(GtkIconTheme *theme, const char *icon) {
  if (!icon || *icon == '\0')
    return FALSE;

  if (g_str_has_prefix(icon, "resource://"))
    return TRUE;

  if (g_file_test(icon, G_FILE_TEST_EXISTS))
    return TRUE;

  if (theme && gtk_icon_theme_has_icon(theme, icon))
    return TRUE;

  char *symbolic = g_strdup_printf("%s-symbolic", icon);
  gboolean resolvable = theme && gtk_icon_theme_has_icon(theme, symbolic);
  g_free(symbolic);
  return resolvable;
}

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

  /* The desktop entry behind this launcher gives the item its canonical
   * identity, and a better icon when the configured one is not in the theme
   * (a preset may name "pavucontrol" while the icon lives under
   * "org.pulseaudio.pavucontrol"). */
  const LambdockDesktopEntry *entry = resolve_desktop_entry(exec, icon, app_id);
  if (entry && entry->stem)
    item->desktop_id = g_strdup(entry->stem);

  if (entry && entry->icon && *entry->icon != '\0' &&
      !icon_is_resolvable(icon_theme, item->icon) &&
      icon_is_resolvable(icon_theme, entry->icon)) {
    LOG_C_INFO("UI", "Icon '%s' is not in the icon theme; using '%s' from %s",
               item->icon, entry->icon, entry->id);
    g_free(item->icon);
    item->icon = g_strdup(entry->icon);
  }

  int icon_sz =
      (state && state->icon_size > 0) ? state->icon_size : DEFAULT_ICON_SIZE;
  GtkWidget *image = create_image_for_icon(icon_theme, item->icon, icon_sz);

  gtk_widget_set_overflow(image, GTK_OVERFLOW_VISIBLE);

  item->button = gtk_button_new();
  gtk_widget_add_css_class(item->button, "dock-item");

  /* App items host their icon and an (initially hidden) window-title label in
   * a horizontal box. The label is shown only when `show-window-titles?` is on
   * and the app has a running window (Windows XP-style taskbar buttons). */
  GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_valign(content_box, GTK_ALIGN_CENTER);

  if (image) {
    GtkWidget *zoom = wrap_icon_for_zoom(image, icon_sz);
    gtk_box_append(GTK_BOX(content_box), zoom);
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
  lambdock_popover_close_on_escape(item->popover);

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
  dock_item_update_accessible(item);

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
  gtk_widget_add_css_class(item->button, "dock-item");

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
    gtk_box_append(GTK_BOX(content_box), wrap_icon_for_zoom(image, icon_sz));
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
  lambdock_popover_close_on_escape(item->popover);

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

  dock_item_update_accessible(item);
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

  // Cancel the trash refresh timer of a trash item
  if (item->trash_timer_id > 0) {
    g_source_remove(item->trash_timer_id);
    item->trash_timer_id = 0;
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
  g_free(item->desktop_id);

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

  /* The running state is part of what a screen reader announces, so refresh the
   * label before anything else can return early. */
  dock_item_update_accessible(item);

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

    if (item->desktop_id && g_ascii_strcasecmp(item->desktop_id, app_id) == 0)
      return item;
  }

  /* Nothing matched the raw id. The desktop entry behind it knows the names the
   * application is actually installed under, so use those for a second pass:
   * a pinned "Files" item (icon "nautilus") tracks a window whose app id is
   * "org.gnome.Nautilus", and a pinned "Terminal" tracks "org.gnome.Terminal". */
  const LambdockDesktopEntry *entry = lambdock_desktop_entry_lookup(app_id);
  if (entry) {
    for (GList *l = state->dock_items; l != NULL; l = l->next) {
      DockItem *item = (DockItem *)l->data;

      /* Both sides resolved to the same desktop file: this is the precise
       * match, and the one that keeps a pinned launcher and a running window
       * from showing up as two icons (a pinned "IntelliJ" whose exec is `idea`,
       * and a window reporting app id `jetbrains-idea`, both land on
       * jetbrains-idea-....desktop). */
      if (item->desktop_id && entry->stem &&
          g_ascii_strcasecmp(item->desktop_id, entry->stem) == 0)
        return item;

      if (item->app_id && (g_ascii_strcasecmp(item->app_id, entry->stem) == 0 ||
                           g_ascii_strcasecmp(item->app_id, entry->id) == 0))
        return item;

      if (item->icon && entry->icon &&
          g_ascii_strcasecmp(item->icon, entry->icon) == 0)
        return item;

      if (item->exec && entry->exec &&
          exec_matches_app_id(entry->exec, item->exec))
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
  item->kind = LAMBDOCK_ITEM_SEPARATOR;
  item->state = state;
  item->is_favorite = TRUE;
  item->hover_animate = FALSE;

  item->button = gtk_separator_new(orientation);
  gtk_widget_add_css_class(item->button, "dock-separator");

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

  /* A separator stands *on* the shelf, so it follows the slab's projection: on
   * a keystoned or tilted dock it leans with the glass instead of standing
   * upright in front of it. On a flat capsule the follower is a pass-through. */
  GtkWidget *body = item->button;
  if (state && state->bg_shelf) {
    body = shelf_follow_new(item->button, state->bg_shelf,
                            state->bg_capsule);
    item->separator_follow = body;
  }

  /* Separators carry no icon, so they only get the wrapper box (keeping the
   * item list uniform); no reflection is created for them. */
  item->container = shelf_wrap_item(state, body, NULL);

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
  item->icon_image = image;
  item->button = gtk_button_new();
  gtk_widget_add_css_class(item->button, "dock-item");

  GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign(content_box, GTK_ALIGN_CENTER);
  if (image) {
    gtk_widget_set_overflow(image, GTK_OVERFLOW_VISIBLE);
    gtk_box_append(GTK_BOX(content_box), wrap_icon_for_zoom(image, icon_sz));
  }
  gtk_button_set_child(GTK_BUTTON(item->button), content_box);

  gtk_widget_add_css_class(item->button, "flat");
  switch (kind) {
  case LAMBDOCK_ITEM_STACK:
    gtk_widget_add_css_class(item->button, "stack-item");
    break;
  case LAMBDOCK_ITEM_LAUNCHPAD:
    gtk_widget_add_css_class(item->button, "launchpad-item");
    break;
  case LAMBDOCK_ITEM_TRASH:
    gtk_widget_add_css_class(item->button, "trash-item");
    break;
  default:
    break;
  }
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
  lambdock_popover_close_on_escape(item->popover);
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
  dock_item_update_accessible(item);
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

/* ============================================================================
 * THE TRASH
 * ============================================================================
 */

/* `user-trash` and `user-trash-full` are the Icon Naming Specification's names
 * for the empty and full trash, so every icon theme carries them. A theme set up
 * for symbolic icons gets the symbolic full icon instead. */
static const char *trash_full_icon_for(const char *icon) {
  return (icon && g_str_has_suffix(icon, "-symbolic"))
             ? "user-trash-full-symbolic"
             : "user-trash-full";
}

static const char *trash_icon_for(const DockItem *item, gboolean full) {
  if (full)
    return trash_full_icon_for(item->icon);
  return (item->icon && *item->icon != '\0') ? item->icon : "user-trash";
}

/* Whether the item's artwork can be swapped between the two themed trash icons.
 * A configured file, resource or unknown icon is not touched: replacing it would
 * lose the artwork the user chose (and an icon name is not a path). */
static gboolean trash_artwork_is_swappable(const DockItem *item) {
  if (!item->icon_image || !GTK_IS_IMAGE(item->icon_image))
    return FALSE;
  if (gtk_image_get_storage_type(GTK_IMAGE(item->icon_image)) !=
      GTK_IMAGE_ICON_NAME)
    return FALSE;
  if (!item->icon || *item->icon == '\0')
    return TRUE;

  GtkIconTheme *theme =
      gtk_icon_theme_get_for_display(gdk_display_get_default());
  return theme && gtk_icon_theme_has_icon(theme, item->icon);
}

void dock_item_refresh_trash(DockItem *item) {
  if (!item || item->kind != LAMBDOCK_ITEM_TRASH)
    return;

  guint count = lambdock_trash_count();
  gboolean full = (count > 0);

  if (count == item->trash_count && full == item->trash_full)
    return;

  item->trash_count = count;
  item->trash_full = full;

  if (trash_artwork_is_swappable(item)) {
    const char *icon = trash_icon_for(item, full);
    const char *current = gtk_image_get_icon_name(GTK_IMAGE(item->icon_image));
    if (g_strcmp0(current, icon) != 0)
      gtk_image_set_from_icon_name(GTK_IMAGE(item->icon_image), icon);
  }

  const char *name =
      (item->name && *item->name != '\0') ? item->name : _("Trash");
  char *tooltip = NULL;
  if (full) {
    char *items_text =
        g_strdup_printf(ngettext("%u item", "%u items", count), count);
    tooltip = g_strdup_printf("%s — %s", name, items_text);
    g_free(items_text);
  } else {
    tooltip = g_strdup(name);
  }

  if (item->button)
    gtk_widget_set_tooltip_text(item->button, tooltip);
  g_free(tooltip);

  dock_item_update_accessible(item);
}

static gboolean on_trash_refresh(gpointer user_data) {
  DockItem *item = (DockItem *)user_data;
  if (!item)
    return G_SOURCE_REMOVE;

  dock_item_refresh_trash(item);
  return G_SOURCE_CONTINUE;
}

DockItem *create_trash_dock_item(LambdockState *state, const char *name,
                                 const char *icon, gboolean hover_animate) {
  DockItem *item = create_icon_dock_item(
      state, LAMBDOCK_ITEM_TRASH, name,
      (icon && *icon != '\0') ? icon : "user-trash", hover_animate);

  /* The trash is changed by every other program on the desktop, so the item
   * counts it again on a timer instead of waiting for a signal nobody sends. */
  item->trash_timer_id =
      g_timeout_add(TRASH_REFRESH_INTERVAL_MS, on_trash_refresh, item);
  dock_item_refresh_trash(item);

  LOG_C_INFO("UI", "Created trash item: %s", item->name ? item->name : "");
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
  state->reduce_motion = get_reduce_motion_config_for(state->config_path);
  state->icon_size = get_icon_size_config_for(state->config_path);
  state->item_spacing = get_item_spacing_config_for(state->config_path);
  state->capsule_padding =
      get_capsule_padding_config_for(state->config_path);
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

  /* A toggle from the preferences window takes effect on the running dock. */
  dock_apply_motion_preference(state);

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
