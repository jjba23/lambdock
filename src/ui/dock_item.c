// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "dock_item.h"
#include "dock_item_windows.h"
#include "../app_window.h"
#include "../core/config_values.h" // Struct definitions for DockItem, LambdockState, AppWindow
#include "../core/dock_click.h"    // What a left click on an item should do
#include "../core/item_text.h"     // Pure wording for labels, tooltips and gauges
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
#include "clock.h"         // Analogue faces for clock dynamic items
#include "meter.h"         // Circular gauges for meter dynamic items
#include "popover_menu.h" // Custom right-click and multi-window popover menus
#include "stack.h"         // Folder stacks (Downloads-style dock icons)
#include "theme_engine.h" // CSS and theme engine utilities
#include "trash.h"         // Desktop trash (count, empty, open)
#include "widget_utils.h" // Idempotent CSS-class helpers

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

void dock_item_activate_stack(DockItem *item);
void dock_item_activate_launchpad(DockItem *item);
void dock_item_activate_trash(DockItem *item);

/* The per-kind accessible text; see ITEM_KIND_OPS. Each one only reads the item
 * and hands plain values to core/item_text.c, which owns the wording. */
static LambdockItemText describe_app_item(const DockItem *item);
static LambdockItemText describe_dynamic_item(const DockItem *item);
static LambdockItemText describe_stack_item(const DockItem *item);
static LambdockItemText describe_launchpad_item(const DockItem *item);
static LambdockItemText describe_trash_item(const DockItem *item);

/**
 * LambdockItemKindOps:
 * Everything the rest of the file needs to know about one kind of dock item.
 *
 * The dock used to answer "what is this kind?" in four separate switches (key
 * name, CSS class, click behaviour, accessibility text). Adding a kind meant
 * finding all four. Now a kind is one row of `ITEM_KIND_OPS`, and the switches
 * are gone: the table is the single place where a kind is defined.
 */
typedef struct {
  const char *key_name; /* stable identity prefix, e.g. "app" */
  const char *css_class; /* extra CSS class, NULL for none */
  void (*activate)(DockItem *item); /* NULL means "app behaviour" */
  LambdockItemText (*describe)(const DockItem *item);
} LambdockItemKindOps;

static const LambdockItemKindOps ITEM_KIND_OPS[] = {
    [LAMBDOCK_ITEM_APP] = {"app", NULL, NULL, describe_app_item},
    [LAMBDOCK_ITEM_DYNAMIC] = {"dynamic", "dynamic-item", NULL,
                               describe_dynamic_item},
    [LAMBDOCK_ITEM_SEPARATOR] = {"separator", NULL, NULL, NULL},
    [LAMBDOCK_ITEM_STACK] = {"stack", "stack-item", dock_item_activate_stack,
                             describe_stack_item},
    [LAMBDOCK_ITEM_LAUNCHPAD] = {"launchpad", "launchpad-item",
                                 dock_item_activate_launchpad,
                                 describe_launchpad_item},
    [LAMBDOCK_ITEM_TRASH] = {"trash", "trash-item", dock_item_activate_trash,
                             describe_trash_item},
};

/** The descriptor of @kind; unknown kinds behave like a plain application. */
static const LambdockItemKindOps *kind_ops(LambdockItemKind kind) {
  gsize index = (gsize)kind;

  if (index >= G_N_ELEMENTS(ITEM_KIND_OPS) || !ITEM_KIND_OPS[index].key_name)
    return &ITEM_KIND_OPS[LAMBDOCK_ITEM_APP];

  return &ITEM_KIND_OPS[index];
}

const char *dock_item_kind_to_string(LambdockItemKind kind) {
  return kind_ops(kind)->key_name;
}

char *dock_item_key(const DockItem *item) {
  if (!item)
    return NULL;
  return g_strdup_printf("%s:%s", dock_item_kind_to_string(item->kind),
                         item->name ? item->name : "");
}

const char *lambdock_item_title(const DockItem *item, const char *fallback) {
  if (item) {
    if (item->name && *item->name != '\0')
      return item->name;
    if (item->app_id && *item->app_id != '\0')
      return item->app_id;
  }

  return fallback;
}


/* The key deferred callbacks use to get from a widget back to its item. */
#define DOCK_ITEM_DATA_KEY "lambdock-item"

/**
 * dock_item_set_backpointers:
 * Stores @owner (or, with NULL, clears) on every widget of the item that
 * deferred callbacks may hold on to.
 */
static void dock_item_set_backpointers(DockItem *item, DockItem *owner) {
  if (!item)
    return;

  if (item->button)
    g_object_set_data(G_OBJECT(item->button), DOCK_ITEM_DATA_KEY, owner);
  if (item->container)
    g_object_set_data(G_OBJECT(item->container), DOCK_ITEM_DATA_KEY, owner);
}

/**
 * dock_item_attach_data:
 * Stores the owning DockItem on the item's widgets, so a drag callback can find
 * it. dock_item_detach_data() clears it again in free_dock_item(): the widget
 * survives a hot reload (the drag machinery references it) but the data becomes
 * NULL, so the callback bails out instead of touching freed memory.
 */
static void dock_item_attach_data(DockItem *item) {
  dock_item_set_backpointers(item, item);
}

/** Clears the back pointers, so deferred callbacks see "no item". */
static void dock_item_detach_data(DockItem *item) {
  dock_item_set_backpointers(item, NULL);
}

DockItem *dock_item_from_widget(GtkWidget *widget) {
  if (!widget || !G_IS_OBJECT(widget))
    return NULL;
  return g_object_get_data(G_OBJECT(widget), DOCK_ITEM_DATA_KEY);
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

/* What a screen reader should say about each kind of item. The wording itself
 * lives in core/item_text.c; these only extract the inputs from the item. */
static LambdockItemText describe_app_item(const DockItem *item) {
  return lambdock_item_text_app(item->name, g_list_length(item->windows));
}

static LambdockItemText describe_dynamic_item(const DockItem *item) {
  /* The item's tooltip already carries the latest reading (meter readings are
   * written as "name: reading"), so that is the value to describe. */
  return lambdock_item_text_dynamic(item->name,
                                    gtk_widget_get_tooltip_text(item->button));
}

static LambdockItemText describe_stack_item(const DockItem *item) {
  return lambdock_item_text_stack(item->name);
}

static LambdockItemText describe_launchpad_item(const DockItem *item) {
  return lambdock_item_text_launchpad(item->name);
}

static LambdockItemText describe_trash_item(const DockItem *item) {
  return lambdock_item_text_trash(item->name, item->trash_count);
}

void dock_item_update_accessible(DockItem *item) {
  if (!item || !item->button || item->kind == LAMBDOCK_ITEM_SEPARATOR)
    return;

  const LambdockItemKindOps *ops = kind_ops(item->kind);
  if (!ops->describe)
    return;

  LambdockItemText text = ops->describe(item);
  lambdock_accessible_update(item->button, text.label, text.description);
  lambdock_item_text_clear(&text);
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

/** The shared "show the popover" path: used by clicks, keys and menus. */
static void dock_item_popup_menu(DockItem *item) {
  if (!item || !item->popover || !item->state)
    return;

  populate_popover_menu(item);

  if (!gtk_widget_get_visible(item->popover))
    item->state->active_popovers++;

  gtk_popover_popup(GTK_POPOVER(item->popover));

  /* Put the keyboard on the menu's first row, so the arrow keys work from the
   * moment it opens (a stack panel focuses its own first entry instead). */
  lambdock_popover_menu_focus_first(item->popover);
}

void lambdock_dock_item_show_menu(DockItem *item) {
  if (!item || !item->popover || !item->state)
    return;

  if (gtk_widget_get_visible(item->popover)) {
    gtk_popover_popdown(GTK_POPOVER(item->popover));
    return;
  }

  dock_item_popup_menu(item);
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

/** How far a key moves along the dock (-1, 0 = "not a step key", +1). */
static int dock_key_step(GtkOrientation orientation, guint keyval) {
  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    if (keyval == GDK_KEY_Left)
      return -1;
    if (keyval == GDK_KEY_Right)
      return 1;
    return 0;
  }

  if (keyval == GDK_KEY_Up)
    return -1;
  if (keyval == GDK_KEY_Down)
    return 1;
  return 0;
}

/** The Menu key and Shift+F10 are the keyboard's right click. */
static gboolean dock_key_opens_menu(guint keyval, GdkModifierType modifiers) {
  return keyval == GDK_KEY_Menu ||
         (keyval == GDK_KEY_F10 && (modifiers & GDK_SHIFT_MASK) != 0);
}

gboolean lambdock_dock_items_handle_key(LambdockState *state, guint keyval,
                                        GdkModifierType modifiers) {
  if (!state)
    return FALSE;

  if (keyval == GDK_KEY_Escape)
    return lambdock_dock_items_close_popovers(state);

  DockItem *focused = focused_dock_item(state);

  if (dock_key_opens_menu(keyval, modifiers)) {
    if (!focused)
      return FALSE;
    lambdock_dock_item_show_menu(focused);
    return TRUE;
  }

  int step = dock_key_step(state->orientation, keyval);
  if (step != 0) {
    /* Ctrl+arrow rearranges the dock, so the pointer drag is not the only way
     * to move an icon. */
    if ((modifiers & GDK_CONTROL_MASK) != 0) {
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
 * Alignment is a decision (which edge is the dock on?) followed by three widget
 * calls (size request, alignment, centring the content). The decision is a
 * value, so it is written once and read by every applier.
 */

/**
 * dock_item_icon_size:
 * The icon size in use, with the default for partly built states.
 */
static int dock_item_icon_size(const LambdockState *state) {
  return (state && state->icon_size > 0) ? state->icon_size : DEFAULT_ICON_SIZE;
}

/** How an item's widgets align themselves on a dock attached to an edge. */
typedef struct DockItemAlignment {
  GtkAlign halign;
  GtkAlign valign;
} DockItemAlignment;

static DockItemAlignment dock_item_alignment_for_edge(GtkLayerShellEdge edge) {
  switch (edge) {
  case GTK_LAYER_SHELL_EDGE_BOTTOM:
    return (DockItemAlignment){GTK_ALIGN_CENTER, GTK_ALIGN_END};
  case GTK_LAYER_SHELL_EDGE_TOP:
    return (DockItemAlignment){GTK_ALIGN_CENTER, GTK_ALIGN_START};
  case GTK_LAYER_SHELL_EDGE_LEFT:
    return (DockItemAlignment){GTK_ALIGN_START, GTK_ALIGN_CENTER};
  case GTK_LAYER_SHELL_EDGE_RIGHT:
    return (DockItemAlignment){GTK_ALIGN_END, GTK_ALIGN_CENTER};
  default:
    return (DockItemAlignment){GTK_ALIGN_CENTER, GTK_ALIGN_CENTER};
  }
}

/** Every item's content asks for one icon-sized cell across the dock.
 *
 * The theme's button padding sits outside that cell, so an icon, a label
 * ("Thu Oct 29") and a gauge all end up exactly as tall as each other. Sizing
 * the button instead leaves the theme's padding out of the sum, and the shorter
 * items then hang at the bottom of the row. */
static void apply_dock_item_size_request(DockItem *item) {
  int icon_size = dock_item_icon_size(item->state);
  gboolean horizontal = item->state->orientation == GTK_ORIENTATION_HORIZONTAL;
  GtkWidget *target = item->cell ? item->cell : item->button;

  if (horizontal)
    gtk_widget_set_size_request(target, -1, icon_size);
  else
    gtk_widget_set_size_request(target, icon_size, -1);
}

static void apply_alignment(GtkWidget *widget, DockItemAlignment alignment) {
  gtk_widget_set_halign(widget, alignment.halign);
  gtk_widget_set_valign(widget, alignment.valign);
}

/** The content of the button (icon, label) rides centred inside it. */
static void centre_button_content(GtkWidget *button) {
  if (!GTK_IS_BUTTON(button))
    return;

  GtkWidget *child = gtk_button_get_child(GTK_BUTTON(button));
  if (!child)
    return;

  gtk_widget_set_halign(child, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(child, GTK_ALIGN_CENTER);

  if (!GTK_IS_BOX(child))
    return;

  for (GtkWidget *w = gtk_widget_get_first_child(child); w != NULL;
       w = gtk_widget_get_next_sibling(w)) {
    gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
  }
}

/**
 * apply_dock_item_alignment:
 * Applies edge-anchored alignment rules (valign/halign) to DockItem widgets,
 * ensuring non-animated items and separators remain anchored to the dock edge.
 */
void apply_dock_item_alignment(DockItem *item) {
  if (!item || !item->button || !item->state)
    return;

  apply_dock_item_size_request(item);

  DockItemAlignment alignment = dock_item_alignment_for_edge(item->state->edge);
  apply_alignment(item->button, alignment);

  /* A separator's line fills the cell it sits in -- the cell is what follows the
   * dock edge, and the line is all that is inside it -- so it spans the icons'
   * height instead of collapsing to the one-pixel separator default. */
  if (item->kind == LAMBDOCK_ITEM_SEPARATOR) {
    gboolean horizontal =
        item->state->orientation == GTK_ORIENTATION_HORIZONTAL;
    gtk_widget_set_valign(item->button,
                          horizontal ? GTK_ALIGN_FILL : GTK_ALIGN_CENTER);
    gtk_widget_set_halign(item->button,
                          horizontal ? GTK_ALIGN_CENTER : GTK_ALIGN_FILL);
  }

  /* The per-item container holds the button plus (when reflections are on)
   * its glass shelf reflection, so it hugs the same dock edge as the button. */
  if (item->container)
    apply_alignment(item->container, alignment);

  centre_button_content(item->button);
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
void dock_item_activate_stack(DockItem *item) { lambdock_stack_show(item); }

void dock_item_activate_launchpad(DockItem *item) {
  lambdock_launchpad_show(item->state);
}

void dock_item_activate_trash(DockItem *item) {
  (void)item;
  if (!lambdock_trash_open())
    LOG_C_WARN("UI", "Could not open the trash");
}

/** Runs @command, reporting a failure without taking the dock down. */
static void launch_command(const char *command) {
  GError *error = NULL;

  if (!g_spawn_command_line_async(command, &error)) {
    LOG_C_WARN("UI", "Failed to launch '%s': %s", command,
               (error && error->message) ? error->message : "unknown");
    g_clear_error(&error);
  }
}

/**
 * activate_app_item:
 * What a left click on an application item does.
 *
 * The decision is core/dock_click.c's -- "launch, minimize, restore or list" --
 * and this only applies it, so the policy stays testable without a compositor.
 * Clicking the icon of the focused app minimizes it (minimize to dock);
 * clicking an icon whose window is minimized or behind another brings it
 * forward.
 */
static void activate_app_item(DockItem *item) {
  AppWindow *only_window = (item->windows && item->windows->next == NULL)
                               ? (AppWindow *)item->windows->data
                               : NULL;

  LambdockClickAction action =
      lambdock_click_action_for(g_list_length(item->windows),
                                only_window && only_window->minimized,
                                only_window && only_window->activated,
                                item->exec);

  switch (action) {
  case LAMBDOCK_CLICK_MINIMIZE:
    if (only_window)
      minimize_toplevel_window(only_window->handle);
    return;
  case LAMBDOCK_CLICK_ACTIVATE:
    if (only_window)
      activate_toplevel_window(only_window->handle);
    return;
  case LAMBDOCK_CLICK_MENU:
    /* Several windows: let the user pick one from the menu. */
    dock_item_popup_menu(item);
    return;
  case LAMBDOCK_CLICK_LAUNCH:
    launch_command(item->exec);
    return;
  case LAMBDOCK_CLICK_NOTHING:
    return;
  }
}

static void on_dock_item_left_click(GtkButton *btn, gpointer user_data) {
  (void)btn;

  DockItem *item = (DockItem *)user_data;
  if (!item)
    return;

  void (*activate)(DockItem *) = kind_ops(item->kind)->activate;
  if (activate) {
    activate(item);
    return;
  }

  activate_app_item(item);
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

  dock_item_popup_menu(item);
}

/* ============================================================================
 * DYNAMIC ITEM POLLING LOGIC & SCHEME EXCEPTION HANDLING
 * ============================================================================
 */

/**
 * dock_item_set_poller:
 * Remembers @poll_fn as the item's reading source.
 *
 * @poll_fn is the bridge's opaque handle. Protecting the procedure from the GC
 * is the bridge's job too: this module never sees an SCM value.
 */
static void dock_item_set_poller(DockItem *item, void *poll_fn) {
  if (!item)
    return;

  item->poll_fn = lambdock_poll_fn_protect(poll_fn);
}

/** Whether the item still has a callable Scheme poller. */
static gboolean dynamic_item_has_poller(const DockItem *item) {
  return item && lambdock_poll_fn_is_callable(item->poll_fn);
}

/**
 * apply_meter_reading:
 * Draws a numeric reading on the item's gauge.
 *
 * Returns: TRUE when the item is a meter and @value was a number.
 */
static gboolean apply_meter_reading(DockItem *item,
                                    const LambdockReading *value) {
  if (!item->meter || !value || value->kind != LAMBDOCK_READING_NUMBER)
    return FALSE;

  double reading = value->number;
  g_autofree char *text =
      lambdock_meter_reading_text(reading, item->meter_unit);

  lambdock_meter_set_value(
      item->meter,
      lambdock_meter_reading_fraction(reading, item->meter_max), text);

  g_autofree char *tooltip =
      g_strdup_printf("%s: %s", item->name ? item->name : "", text);
  gtk_widget_set_tooltip_text(item->button, tooltip);
  dock_item_update_accessible(item);

  return TRUE;
}

/** Writes a text reading into the item's label and tooltip. */
static void apply_text_reading(DockItem *item, const char *text) {
  if (!item || !text)
    return;

  if (item->label && GTK_IS_LABEL(item->label))
    gtk_label_set_text(GTK_LABEL(item->label), text);

  gtk_widget_set_tooltip_text(item->button, text);
  dock_item_update_accessible(item);
}

/** Points the item's clock face at the current local time. */
static void apply_clock_time(DockItem *item) {
  GDateTime *now = g_date_time_new_now_local();
  if (!now)
    return;

  int seconds = g_date_time_get_hour(now) * 3600 +
                g_date_time_get_minute(now) * 60 + g_date_time_get_second(now);
  g_date_time_unref(now);

  lambdock_clock_set_time(item->clock, seconds);
}

/** Polls the item once and applies whatever the poller returned. */
static void refresh_dynamic_item(DockItem *item) {
  /* An item whose Scheme side gave it no procedure has nothing to poll. Calling
   * it anyway would apply #f -- "Wrong type to apply" raised on the GTK thread,
   * where Guile has no handler, which aborts the process. */
  if (!dynamic_item_has_poller(item))
    return;

  LambdockReading value = {0};
  gboolean have_value = lambdock_poll_fn_call(item->poll_fn, &value);

  /* A clock's hands come from the system clock, not from the Scheme value: the
   * poller only formats the reading the tooltip and the screen reader announce. */
  if (item->clock)
    apply_clock_time(item);

  if (have_value && !apply_meter_reading(item, &value))
    apply_text_reading(item, value.text);

  g_free(value.text);
}

/** TRUE when this item's dock is not on screen (hidden, unmapped or going away). */
static gboolean dock_is_off_screen(const DockItem *item) {
  if (!item || !item->state)
    return FALSE;

  const LambdockState *state = item->state;
  if (state->is_hidden || state->is_destroying)
    return TRUE;

  if (!state->window || !gtk_widget_get_mapped(state->window))
    return TRUE;

  return FALSE;
}

static gboolean on_dynamic_item_poll(gpointer user_data) {
  DockItem *item = (DockItem *)user_data;

  if (!dynamic_item_has_poller(item)) {
    /* The source is gone once we return: forget its id so teardown does not try
     * to remove it (and cannot remove an unrelated source that reused it). */
    if (item)
      item->poll_timer_id = 0;
    return G_SOURCE_REMOVE;
  }

  /* A hidden dock with a clock, a CPU gauge and a trash count would wake the
   * process several times a second to update widgets nobody can see. Skip the
   * work; dock_items_refresh_polls() catches every item up the moment the dock
   * comes back, so the values are current when they are visible again. */
  if (dock_is_off_screen(item))
    return G_SOURCE_CONTINUE;

  refresh_dynamic_item(item);
  return G_SOURCE_CONTINUE;
}

void dock_items_refresh_polls(LambdockState *state) {
  if (!state)
    return;

  for (GList *node = state->dock_items; node != NULL; node = node->next) {
    DockItem *item = node->data;
    if (!item)
      continue;

    if (dynamic_item_has_poller(item))
      refresh_dynamic_item(item);

    if (item->trash_timer_id != 0)
      dock_item_refresh_trash(item);
  }
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

/* ----------------------------------------------------------------------------
 * THE ITEM SHELL
 * ----------------------------------------------------------------------------
 * Every kind of item is the same GTK scaffold around different content:
 *
 *   button -> content box -> [icon, label?] -> glass reflection
 *          -> shelf container -> the dock row
 *
 * That scaffold used to be copy-pasted into three constructors (~50 lines
 * each), so touching the popover, the gestures or the drop targets meant
 * touching all three. A constructor now builds only its content and calls
 * dock_item_mount(); the scaffold exists once.
 */

/** What a constructor tells dock_item_new() about the item it is building. */
typedef struct DockItemShell {
  LambdockState *state;
  LambdockItemKind kind;
  const char *name; /* borrowed: copied into the item */
  const char *icon;
  const char *exec;
  const char *app_id;
  gboolean is_favorite;
  gboolean hover_animate;
} DockItemShell;

/** Allocates an item with its scalar fields filled in; no widgets yet. */
static DockItem *dock_item_new(const DockItemShell *shell) {
  DockItem *item = g_new0(DockItem, 1);

  item->poll_fn = NULL;
  item->kind = shell->kind;
  item->state = shell->state;
  item->name = g_strdup(shell->name);
  item->icon = g_strdup(shell->icon);
  item->exec = g_strdup(shell->exec);
  item->app_id = g_strdup(shell->app_id);
  item->is_favorite = shell->is_favorite;
  item->hover_animate = shell->hover_animate;

  return item;
}

/**
 * dock_item_begin_content:
 * Creates the item's button and an empty content box inside it, carrying the
 * classes every item shares plus the one its kind adds.
 *
 * Returns: (transfer none): the box the constructor appends its content to.
 */
static GtkWidget *dock_item_begin_content(DockItem *item, int spacing) {
  item->button = gtk_button_new();
  gtk_widget_add_css_class(item->button, "dock-item");
  gtk_widget_add_css_class(item->button, "flat");

  const char *kind_class = kind_ops(item->kind)->css_class;
  if (kind_class)
    gtk_widget_add_css_class(item->button, kind_class);

  gtk_widget_set_overflow(item->button, GTK_OVERFLOW_VISIBLE);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
  gtk_widget_set_valign(content, GTK_ALIGN_CENTER);
  gtk_button_set_child(GTK_BUTTON(item->button), content);
  item->cell = content;

  return content;
}

/** Loads an item's artwork from the icon theme. */
static GtkWidget *dock_item_load_icon(const LambdockState *state,
                                      const char *icon) {
  GtkIconTheme *theme =
      gtk_icon_theme_get_for_display(gdk_display_get_default());

  return create_image_for_icon(theme, icon, dock_item_icon_size(state));
}

/** Appends @image to the content box, wrapped so the fisheye can scale it. */
static void dock_item_append_icon(GtkWidget *content, GtkWidget *image,
                                  int icon_size) {
  if (!image)
    return;

  gtk_widget_set_overflow(image, GTK_OVERFLOW_VISIBLE);
  gtk_box_append(GTK_BOX(content), wrap_icon_for_zoom(image, icon_size));
}

/**
 * dock_item_attach_reflection:
 * Gives the item its glass-shelf reflection slot. It stays hidden (and takes no
 * room) until the shelf parameters ask for reflections.
 *
 * Every item gets the slot, including the ones with nothing to mirror -- a
 * gauge, a separator, an item whose icon did not load. The band the slot
 * reserves is part of the cell's height, so dropping it would leave those items
 * a band lower than the icons next to them on a shelf theme.
 */
static void dock_item_attach_reflection(DockItem *item, GtkWidget *image) {
  item->reflection = shelf_reflection_new(image);
  gtk_widget_set_visible(item->reflection, FALSE);
}

/** The item's own context popover, parented to its button. */
static void dock_item_install_popover(DockItem *item) {
  item->popover = gtk_popover_new();
  gtk_popover_set_has_arrow(GTK_POPOVER(item->popover), FALSE);
  gtk_widget_set_parent(item->popover, item->button);
  lambdock_popover_close_on_escape(item->popover);

  /* on_popover_closed() releases the dock's popover slot, so it is only
   * connected for items that belong to a dock. */
  if (item->state) {
    g_signal_connect(item->popover, "closed", G_CALLBACK(on_popover_closed),
                     item->state);
  }
}

/** Left click activates the item, right click opens its context menu. */
static void dock_item_install_gestures(DockItem *item) {
  g_signal_connect(item->button, "clicked", G_CALLBACK(on_dock_item_left_click),
                   item);

  GtkGesture *right_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click),
                                GDK_BUTTON_SECONDARY);
  g_signal_connect(right_click, "pressed", G_CALLBACK(on_dock_item_right_click),
                   item);
  gtk_widget_add_controller(item->button, GTK_EVENT_CONTROLLER(right_click));
}

/**
 * dock_item_mount:
 * Finishes an item once its content exists: the glass-shelf container, its
 * alignment, the tooltip, the context menu, drag and drop, and its place in the
 * dock row.
 *
 * Returns: (transfer none): @item, so constructors read
 * `return dock_item_mount(item);`.
 */
static DockItem *dock_item_mount(DockItem *item) {
  LambdockState *state = item->state;

  item->container = shelf_wrap_item(state, item->button, item->reflection);

  if (state)
    apply_dock_item_alignment(item);

  if (item->name && *item->name != '\0')
    gtk_widget_set_tooltip_text(item->button, item->name);

  dock_item_install_gestures(item);
  dock_item_install_popover(item);
  dock_dnd_setup_item(item);

  if (state && state->box) {
    GtkWidget *child = item->container ? item->container : item->button;
    gtk_box_append(GTK_BOX(state->box), child);
  }

  if (state) {
    state->dock_items = g_list_append(state->dock_items, item);
    /* A new item moves every centre after it. */
    state->centres_dirty = TRUE;
  }

  dock_item_attach_data(item);
  dock_item_update_accessible(item);

  return item;
}

/**
 * apply_desktop_entry_identity:
 * Gives an application item the desktop entry behind it: its canonical id (what
 * keeps a pinned launcher and the window it opens one icon) and, when the
 * configured icon is not in the theme, the entry's icon.
 */
static void apply_desktop_entry_identity(DockItem *item) {
  const LambdockDesktopEntry *entry =
      resolve_desktop_entry(item->exec, item->icon, item->app_id);

  if (!entry)
    return;

  if (entry->stem)
    item->desktop_id = g_strdup(entry->stem);

  /* A preset may name "pavucontrol" while the icon lives under
   * "org.pulseaudio.pavucontrol". */
  if (!entry->icon || *entry->icon == '\0')
    return;

  GtkIconTheme *theme =
      gtk_icon_theme_get_for_display(gdk_display_get_default());
  if (icon_is_resolvable(theme, item->icon) ||
      !icon_is_resolvable(theme, entry->icon)) {
    return;
  }

  LOG_C_INFO("UI", "Icon '%s' is not in the icon theme; using '%s' from %s",
             item->icon, entry->icon, entry->id);
  g_free(item->icon);
  item->icon = g_strdup(entry->icon);
}

/** The app-item label that taskbar mode shows a running window's title in. */
static void dock_item_add_window_title_label(DockItem *item,
                                             GtkWidget *content) {
  item->label = gtk_label_new(NULL);
  gtk_widget_add_css_class(item->label, "window-title-label");
  gtk_label_set_ellipsize(GTK_LABEL(item->label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(item->label),
                                WINDOW_TITLE_MAX_CHARS);
  gtk_label_set_single_line_mode(GTK_LABEL(item->label), TRUE);
  gtk_widget_set_valign(item->label, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(item->label, FALSE);
  gtk_box_append(GTK_BOX(content), item->label);
  item->window_title_label = TRUE;
}

/** The label a dynamic item writes its polled reading into. */
static void dock_item_add_dynamic_label(DockItem *item, GtkWidget *content,
                                        const char *name) {
  item->label = gtk_label_new(name);
  gtk_label_set_yalign(GTK_LABEL(item->label), 0.5f);
  gtk_widget_set_valign(item->label, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(item->label, "dynamic-item-label");
  gtk_box_append(GTK_BOX(content), item->label);
}

DockItem *create_dock_item(LambdockState *state, const char *name,
                           const char *icon, const char *exec,
                           const char *app_id, gboolean is_favorite,
                           gboolean hover_animate) {
  DockItemShell shell = {
      .state = state,
      .kind = LAMBDOCK_ITEM_APP,
      .name = name,
      .icon = icon,
      .exec = exec,
      .app_id = app_id ? app_id : icon,
      .is_favorite = is_favorite,
      .hover_animate = hover_animate,
  };
  DockItem *item = dock_item_new(&shell);

  apply_desktop_entry_identity(item);

  /* App items host their icon and an (initially hidden) window-title label in
   * a horizontal box. The label is shown only when `show-window-titles?` is on
   * and the app has a running window (Windows XP-style taskbar buttons). */
  GtkWidget *content = dock_item_begin_content(item, 6);
  GtkWidget *image = dock_item_load_icon(state, item->icon);
  dock_item_append_icon(content, image, dock_item_icon_size(state));
  dock_item_add_window_title_label(item, content);
  dock_item_attach_reflection(item, image);

  return dock_item_mount(item);
}

DockItem *create_dynamic_dock_item(LambdockState *state, const char *name,
                                   const char *icon, const char *exec,
                                   void *poll_fn, guint interval_ms,
                                   gboolean hover_animate) {
  DockItemShell shell = {
      .state = state,
      .kind = LAMBDOCK_ITEM_DYNAMIC,
      .name = name,
      .icon = icon,
      .exec = exec,
      .app_id = name, /* dynamic items are known by their name */
      .is_favorite = TRUE,
      .hover_animate = hover_animate,
  };
  DockItem *item = dock_item_new(&shell);

  dock_item_set_poller(item, poll_fn);

  GtkWidget *content = dock_item_begin_content(item, 6);
  GtkWidget *image =
      (icon && *icon != '\0') ? dock_item_load_icon(state, item->icon) : NULL;
  dock_item_append_icon(content, image, dock_item_icon_size(state));
  dock_item_add_dynamic_label(item, content, name);
  dock_item_attach_reflection(item, image);

  dock_item_mount(item);

  /* Paint the first reading now instead of after the first interval. */
  if (dynamic_item_has_poller(item))
    refresh_dynamic_item(item);

  if (interval_ms > 0 && dynamic_item_has_poller(item)) {
    item->poll_timer_id =
        g_timeout_add(interval_ms, on_dynamic_item_poll, item);
  }

  dock_item_update_accessible(item);
  return item;
}

/**
 * replace_dynamic_content:
 * Replaces a dynamic item's text form (its icon and label) with one widget that
 * carries the reading itself: a gauge, a clock face.
 *
 * The label goes with it -- it would otherwise keep a stale reading next to the
 * widget -- and the reflection slot stays, cleared of artwork: the band that slot
 * reserves is what holds the row at one height.
 *
 * Returns: TRUE when the content was replaced.
 */
static gboolean replace_dynamic_content(DockItem *item, GtkWidget *widget) {
  if (!item || !item->button || !widget)
    return FALSE;

  GtkWidget *content = gtk_button_get_child(GTK_BUTTON(item->button));
  if (!content || !GTK_IS_BOX(content))
    return FALSE;

  item->label = NULL;
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(content)) != NULL)
    gtk_box_remove(GTK_BOX(content), child);

  shelf_reflection_clear_source(item->reflection);
  gtk_box_append(GTK_BOX(content), widget);

  return TRUE;
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

  GtkWidget *meter = lambdock_meter_new();
  lambdock_meter_set_icon_size(meter, dock_item_icon_size(item->state));

  if (!replace_dynamic_content(item, meter)) {
    g_object_ref_sink(meter);
    g_object_unref(meter);
    return;
  }

  item->meter = meter;
  g_free(item->meter_unit);
  item->meter_unit = g_strdup(unit ? unit : "");
  item->meter_max = (max_value > 0.0) ? max_value : 100.0;

  gtk_widget_add_css_class(item->button, "dynamic-meter");

  /* Paint the first reading now instead of after the first interval. This is the
   * explicit read, not the timer callback, so it is not skipped when the dock is
   * still unmapped (startup, tests). */
  refresh_dynamic_item(item);
}

/**
 * dock_item_enable_clock:
 * Turns a dynamic item into an analogue clock face.
 *
 * The text form goes the same way as a meter's: a face in an icon slot reads as
 * part of the row, where a string of digits pushed everything after it out of
 * line. The item keeps its Scheme poller -- its reading is the face's tooltip and
 * accessible name ("Thu Oct 29 13:07") -- and the hands are set from the system
 * clock on every poll.
 */
void dock_item_enable_clock(DockItem *item) {
  if (!item || !item->button || item->clock)
    return;

  GtkWidget *clock = lambdock_clock_new();
  lambdock_clock_set_icon_size(clock, dock_item_icon_size(item->state));

  if (!replace_dynamic_content(item, clock)) {
    g_object_ref_sink(clock);
    g_object_unref(clock);
    return;
  }

  item->clock = clock;
  gtk_widget_add_css_class(item->button, "dynamic-clock");

  /* Point the hands now instead of after the first interval, so a dock that is
   * unmapped at startup (tests, or a dock on another workspace) still shows the
   * right time when it appears. */
  refresh_dynamic_item(item);
}

/** Stops the item's polling timers. */
static void free_item_timers(DockItem *item) {
  /* g_clear_handle_id() only removes a source that is still alive, so a timer
   * that already fired cannot make us remove an unrelated (recycled) id. */
  g_clear_handle_id(&item->poll_timer_id, g_source_remove);
  g_clear_handle_id(&item->trash_timer_id, g_source_remove);
}

/** Releases the Scheme poller's GC protection. */
static void free_item_poller(DockItem *item) {
  if (!item->poll_fn)
    return;

  lambdock_poll_fn_release(item->poll_fn);
  item->poll_fn = NULL;
}

/**
 * free_item_popover:
 * Unparents the item's context menu.
 *
 * A popover torn down while it is still open never emits "closed", so the dock's
 * popover slot is released here; otherwise active_popovers stays above zero and
 * the dock can never auto-hide again.
 */
static void free_item_popover(DockItem *item) {
  if (!item->popover || !GTK_IS_WIDGET(item->popover))
    return;

  g_signal_handlers_disconnect_by_data(item->popover, item->state);
  g_signal_handlers_disconnect_by_data(item->popover, item);

  if (item->state && gtk_widget_get_visible(item->popover) &&
      item->state->active_popovers > 0) {
    item->state->active_popovers--;
  }

  if (gtk_widget_get_parent(item->popover))
    gtk_widget_unparent(item->popover);

  item->popover = NULL;
}

/** Detaches the button from the dock, then the container that held it. */
static void free_item_widgets(DockItem *item) {
  if (item->button && GTK_IS_WIDGET(item->button)) {
    g_signal_handlers_disconnect_by_data(item->button, item);

    GtkWidget *parent = gtk_widget_get_parent(item->button);
    if (parent && GTK_IS_BOX(parent))
      gtk_box_remove(GTK_BOX(parent), item->button);

    item->button = NULL;
  }

  /* The reflection (if any) is owned by the container and dies with it, so the
   * pointer is dropped before the container goes. */
  item->reflection = NULL;

  if (!item->container || !GTK_IS_WIDGET(item->container))
    return;

  /* Keep the container alive across the unparent, which drops the box's
   * reference to it. */
  g_object_ref_sink(item->container);

  GtkWidget *parent = gtk_widget_get_parent(item->container);
  if (parent && GTK_IS_BOX(parent))
    gtk_box_remove(GTK_BOX(parent), item->container);

  g_object_unref(item->container);
  item->container = NULL;
}

/** Frees the item's strings. */
static void free_item_strings(DockItem *item) {
  g_free(item->name);
  g_free(item->icon);
  g_free(item->exec);
  g_free(item->app_id);
  g_free(item->stack_path);
  g_free(item->stack_view);
  g_free(item->meter_unit);
  g_free(item->desktop_id);
}

void free_dock_item(DockItem *item) {
  if (!item)
    return;

  /* The row is about to lose an item: every centre after it moves. */
  if (item->state)
    item->state->centres_dirty = TRUE;

  free_item_timers(item);

  /* Back pointers go before anything is unparented: deferred drag callbacks may
   * still hold the widgets, and must see "no item" instead of a freed one. */
  dock_item_detach_data(item);

  free_item_poller(item);
  free_item_popover(item);
  free_item_widgets(item);

  app_windows_free(item->windows);
  item->windows = NULL;

  free_item_strings(item);
  g_free(item);
}

/**
 * clear_favorite_dock_items:
 * Frees the pinned launchers while keeping the running-window tracking alive.
 */
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
 * LAYOUT SEPARATORS
 * ============================================================================
 */

DockItem *create_separator_dock_item(LambdockState *state,
                                     GtkOrientation orientation) {
  DockItem *item = g_new0(DockItem, 1);
  item->poll_fn = NULL;
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

  /* The line is shorter than an icon on purpose. It sits in the same icon-sized
   * cell as every other item's content, so it is centred on the row instead of
   * hanging at its bottom edge; vexpand is what makes it fill the cell (minus
   * its own margins) now that the cell, not the line, carries the size. */
  GtkWidget *cell = gtk_box_new(
      (orientation == GTK_ORIENTATION_VERTICAL) ? GTK_ORIENTATION_VERTICAL
                                                : GTK_ORIENTATION_HORIZONTAL,
      0);
  gtk_widget_set_halign(cell, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(cell, GTK_ALIGN_CENTER);
  /* Along the cell's own axis the line has to claim the spare room; across it
   * the cell is exactly as wide as the line plus its margins. */
  gtk_widget_set_vexpand(item->button,
                         orientation == GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_hexpand(item->button,
                         orientation == GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(cell), item->button);
  item->cell = cell;

  /* A separator stands *on* the shelf, so it follows the slab's projection: on
   * a keystoned or tilted dock it leans with the glass instead of standing
   * upright in front of it. On a flat capsule the follower is a pass-through. */
  GtkWidget *body = cell;
  if (state && state->bg_shelf) {
    body = shelf_follow_new(cell, state->bg_shelf, state->bg_capsule);
    item->separator_follow = body;
  }

  /* Nothing to mirror, but the slot still reserves the shelf's band, which is
   * what keeps the line level with the icons. */
  dock_item_attach_reflection(item, NULL);
  item->container = shelf_wrap_item(state, body, item->reflection);

  if (state) {
    /* After the cell exists: the size request goes to the cell, not the line. */
    apply_dock_item_alignment(item);
  }

  if (state && state->box) {
    GtkWidget *child = item->container ? item->container : item->cell;
    gtk_box_append(GTK_BOX(state->box), child);
  }
  if (state) {
    state->dock_items = g_list_append(state->dock_items, item);
    /* A new item moves every centre after it. */
    state->centres_dirty = TRUE;
  }

  dock_item_attach_data(item);

  return item;
}

/* ============================================================================
 * STACKS & THE LAUNCHPAD
 * ============================================================================
 */

/* Creates the widget shell shared by the icon-only item kinds (stacks, the
 * launchpad and the trash): an icon, no label, and the kind's own CSS class. */
static DockItem *create_icon_dock_item(LambdockState *state,
                                       LambdockItemKind kind, const char *name,
                                       const char *icon,
                                       gboolean hover_animate) {
  DockItemShell shell = {
      .state = state,
      .kind = kind,
      .name = name,
      .icon = icon,
      .exec = "",
      .app_id = name,
      .is_favorite = TRUE,
      .hover_animate = hover_animate,
  };
  DockItem *item = dock_item_new(&shell);

  GtkWidget *content = dock_item_begin_content(item, 0);
  GtkWidget *image = dock_item_load_icon(state, item->icon);

  /* Kept so an item whose artwork depends on state (the trash) can swap it. */
  item->icon_image = image;

  dock_item_append_icon(content, image, dock_item_icon_size(state));
  dock_item_attach_reflection(item, image);

  return dock_item_mount(item);
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

  /* The wording (and its plural form) is in core/item_text.c, the same place the
   * screen-reader description comes from. */
  g_autofree char *tooltip = lambdock_item_trash_tooltip(item->name, count);

  if (item->button)
    gtk_widget_set_tooltip_text(item->button, tooltip);

  dock_item_update_accessible(item);
}

static gboolean on_trash_refresh(gpointer user_data) {
  DockItem *item = (DockItem *)user_data;
  if (!item)
    return G_SOURCE_REMOVE;

  /* Same rule as on_dynamic_item_poll(): do not scan the trash directories for a
   * dock nobody can see. */
  if (dock_is_off_screen(item))
    return G_SOURCE_CONTINUE;

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

/* ============================================================================
 * HOT RELOAD
 * ============================================================================
 * reload_dock_instance() is a sequence of named stages rather than one long
 * body: what a reload *is* is "re-read the settings, move the surface, rebuild
 * the row, re-apply the derived state, put the dock where it belongs". Each
 * stage below is one of those sentences.
 */

/** Mode-relevant settings, captured before a reload so it can be compared
 * afterwards. */
typedef struct DockModeSnapshot {
  gboolean taskbar;
  LambdockShelfParams shelf;
} DockModeSnapshot;

/** Re-reads the settings CSS depends on, and reloads the stylesheets. */
static DockModeSnapshot reload_appearance(LambdockState *state) {
  DockModeSnapshot before = {
      .taskbar = state->taskbar,
      .shelf = state->shelf,
  };

  g_free(state->theme);
  state->theme = get_theme_config_for(state->config_path);

  /* Mode flags refresh before the CSS so the taskbar modifier stylesheet is
   * (un)loaded to match. */
  state->taskbar = get_taskbar_config_for(state->config_path);
  state->show_window_titles =
      get_show_window_titles_config_for(state->config_path);
  get_shelf_config_for(state->config_path, &state->shelf);

  load_custom_css(state);

  return before;
}

/** Re-reads the geometry and behaviour settings. */
static void reload_settings(LambdockState *state) {
  /* A pending auto-hide timeout would fire against the layout being replaced. */
  if (state->hide_timer_id != 0) {
    g_source_remove(state->hide_timer_id);
    state->hide_timer_id = 0;
  }

  state->auto_hide = get_autohide_config_for(state->config_path);
  state->track_windows = get_track_windows_config_for(state->config_path);
  state->show_when_clear = get_show_when_clear_config_for(state->config_path);
  state->hover_animate = get_hover_animate_config_for(state->config_path);
  state->reduce_motion = get_reduce_motion_config_for(state->config_path);
  state->icon_size = get_icon_size_config_for(state->config_path);
  state->item_spacing = get_item_spacing_config_for(state->config_path);
  state->capsule_padding = get_capsule_padding_config_for(state->config_path);

  if (state->box)
    gtk_box_set_spacing(GTK_BOX(state->box), state->item_spacing);
}

/** Moves the surface to a new screen edge, re-orienting the dock with it. */
static void apply_edge_change(LambdockState *state, GtkLayerShellEdge edge) {
  if (edge == state->edge)
    return;

  if (state->tick_cb_id != 0) {
    gtk_widget_remove_tick_callback(state->window, state->tick_cb_id);
    state->tick_cb_id = 0;
  }

  /* The edge lives in layer-shell anchors; without a layer surface (X11, GNOME)
   * there is nothing to re-anchor, and every call would log a critical. */
  if (gtk_layer_is_layer_window(GTK_WINDOW(state->window))) {
    gtk_layer_set_margin(GTK_WINDOW(state->window), state->edge, 0);
    gtk_layer_set_anchor(GTK_WINDOW(state->window), state->edge, FALSE);

    state->edge = edge;
    gtk_layer_set_anchor(GTK_WINDOW(state->window), state->edge, TRUE);
  } else {
    state->edge = edge;
  }

  state->orientation = (edge == GTK_LAYER_SHELL_EDGE_LEFT ||
                        edge == GTK_LAYER_SHELL_EDGE_RIGHT)
                           ? GTK_ORIENTATION_VERTICAL
                           : GTK_ORIENTATION_HORIZONTAL;
  gtk_orientable_set_orientation(GTK_ORIENTABLE(state->box),
                                 state->orientation);
  if (state->bg_capsule) {
    gtk_orientable_set_orientation(GTK_ORIENTABLE(state->bg_capsule),
                                   state->orientation);
  }

  /* A dock that just changed edges is on screen, at rest. */
  state->current_margin = 0;
  state->target_margin = 0;
  state->start_margin = 0;
  state->is_hidden = FALSE;
}

/** Rebuilds the row from the configuration and re-syncs the tracked windows. */
static void reload_items(LambdockState *state) {
  clear_favorite_dock_items(state);
  populate_dock_items_for(state);

  /* Re-sync Wayland foreign window handles back to the new launchers. */
  sync_window_trackers(state);

  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    if (!item || !item->button)
      continue;

    apply_dock_item_alignment(item);
    update_dock_item_status(item);
  }
}

/** Reserves space for the dock (or not) and puts it where the setting wants it. */
static void apply_auto_hide_state(LambdockState *state) {
  dock_apply_exclusive_zone(state);

  gboolean stay_visible =
      !state->auto_hide ||
      (state->show_when_clear && state->window_trackers == NULL);

  if (stay_visible)
    reveal_dock(state);
  else
    hide_dock(state);
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

  DockModeSnapshot before = reload_appearance(state);
  reload_settings(state);
  apply_edge_change(state, get_position_config_for(state->config_path));
  reload_items(state);

  /* Mode classes and dimensions depend on both the new flags and the new row. */
  apply_dock_mode_classes(state);
  if (before.taskbar != state->taskbar ||
      memcmp(&before.shelf, &state->shelf, sizeof(LambdockShelfParams)) != 0) {
    refresh_dock_mode(state);
  }

  update_dock_dimensions(state);

  /* A toggle from the preferences window takes effect on the running dock. */
  dock_apply_motion_preference(state);

  apply_auto_hide_state(state);
}

