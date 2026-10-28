// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "launchpad.h"
#include "../accessibility.h"
#include "../guile_bridge.h"
#include "../logger.h"
#include "desktop_entry.h"
#include "icon_resolver.h"
#include "popover_menu.h"
#include <glib/gi18n.h>
#include <string.h>

/* ============================================================================
 * LAUNCHPAD STATE
 * ============================================================================
 *
 * The launchpad is global (there is exactly one screen to cover), so it lives in
 * file statics rather than in a LambdockState. It is created lazily on the first
 * click, hidden rather than destroyed on close, and rescans the installed
 * applications every time it opens.
 */

static GtkWidget *g_launchpad_window = NULL;
static GtkWidget *g_launchpad_flow = NULL;
static GtkWidget *g_launchpad_search = NULL;
static char *g_launchpad_query = NULL;
static GPtrArray *g_launchpad_entries = NULL;

/* Appearance of the current launchpad, resolved from the dock settings of the
 * instance that opened it (see launchpad_apply_config()). */
static int g_launchpad_icon_size = LAUNCHPAD_ICON_SIZE;
static int g_launchpad_columns = LAUNCHPAD_COLUMNS_AUTO;
static gboolean g_launchpad_blur = FALSE;

/* Tiles per row the grid last laid out, so the arrow keys can move by a row. */
static guint g_launchpad_applied_columns = 8;

/* ============================================================================
 * TILES
 * ============================================================================
 */

static void on_launchpad_tile_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  const char *command = (const char *)user_data;

  if (command)
    lambdock_spawn_command(command);

  lambdock_launchpad_hide();
}

static void free_launched_command(gpointer data, GClosure *closure) {
  (void)closure;
  g_free(data);
}

static gboolean entry_matches_query(const LambdockDesktopEntry *entry,
                                    const char *query) {
  if (!query || *query == '\0')
    return TRUE;

  char *name = g_utf8_casefold(entry->name ? entry->name : "", -1);
  char *needle = g_utf8_casefold(query, -1);
  char *exec = g_utf8_casefold(entry->exec ? entry->exec : "", -1);

  gboolean match = (strstr(name, needle) != NULL) ||
                   (strstr(exec, needle) != NULL);

  g_free(name);
  g_free(needle);
  g_free(exec);
  return match;
}

/* A GtkFlowBox wraps every child in a GtkFlowBoxChild, and the "nothing
 * matched" label is wrapped like a tile. This returns the focusable tile button
 * inside WRAPPER, or NULL when the wrapper holds something else (the label). */
static GtkWidget *launchpad_tile_in(GtkWidget *wrapper) {
  if (!wrapper)
    return NULL;

  if (gtk_widget_has_css_class(wrapper, "launchpad-tile"))
    return wrapper;

  if (GTK_IS_FLOW_BOX_CHILD(wrapper)) {
    GtkWidget *inner = gtk_flow_box_child_get_child(GTK_FLOW_BOX_CHILD(wrapper));
    if (inner && gtk_widget_has_css_class(inner, "launchpad-tile"))
      return inner;
  }

  return NULL;
}

/* Built further down, next to the menu itself: a tile of an application that is
 * installed more than once carries the menu of its copies. */
static void launchpad_tile_add_copy_menu(GtkWidget *tile,
                                         LambdockDesktopEntry *entry);

static gboolean entry_has_copies(const LambdockDesktopEntry *entry) {
  return entry && entry->alternates && entry->alternates->len > 0;
}

GtkWidget *lambdock_launchpad_tile_new(LambdockDesktopEntry *entry,
                                       int icon_size) {
  GtkIconTheme *theme =
      gtk_icon_theme_get_for_display(gdk_display_get_default());

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_valign(content, GTK_ALIGN_CENTER);

  GtkWidget *image = create_image_for_icon(theme, entry->icon, icon_size);
  if (image)
    gtk_box_append(GTK_BOX(content), image);

  GtkWidget *label = gtk_label_new(entry->name);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 16);
  gtk_label_set_lines(GTK_LABEL(label), 2);
  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
  gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(content), label);

  GtkWidget *button = gtk_button_new();
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_add_css_class(button, "launchpad-tile");
  gtk_widget_set_size_request(button, MAX(120, icon_size + 56), -1);
  gtk_button_set_child(GTK_BUTTON(button), content);

  /* A tile of an installed-twice application says so, so the menu is
   * discoverable without a right click that would otherwise do nothing. */
  if (entry_has_copies(entry)) {
    char *tooltip = g_strdup_printf("%s\n%s", entry->exec,
                                    _("Right-click for the other installed "
                                      "copies."));
    gtk_widget_set_tooltip_text(button, tooltip);
    g_free(tooltip);
    lambdock_accessible_update(
        button, entry->name,
        _("Application with more than one installed copy. Press Enter to open "
          "it, or use the menu for the other copies."));
  } else {
    gtk_widget_set_tooltip_text(button, entry->exec);
    lambdock_accessible_update(button, entry->name,
                               _("Application. Press Enter to open."));
  }

  g_signal_connect_data(button, "clicked",
                        G_CALLBACK(on_launchpad_tile_clicked),
                        g_strdup(entry->exec), free_launched_command, 0);

  /* The copies travelling with the entry are reachable from here and nowhere
   * else. */
  if (entry_has_copies(entry))
    launchpad_tile_add_copy_menu(button, entry);

  return button;
}

/* ============================================================================
 * THE OTHER INSTALLED COPIES
 * ============================================================================
 * The grid lists an application once, but a program can be installed twice --
 * the distro package and a Flatpak export of it. The copies the scan did not
 * list ride along on the entry it kept (see desktop_entry.h), and the tile
 * offers them from its context menu: the copy that starts is then the one the
 * user picked instead of the one the scan happened to reach first.
 */

/* The one tile menu that can be open: Escape closes it before it closes the
 * launchpad, and hiding the launchpad has to let go of it. */
static GtkWidget *g_launchpad_menu = NULL;

static void on_launchpad_copy_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  const char *command = (const char *)user_data;

  if (command)
    lambdock_spawn_command(command);

  lambdock_launchpad_hide();
}

/* One row of the menu: where the copy comes from, and the command it runs. */
static GtkWidget *launchpad_copy_row_new(const char *source,
                                         const char *command) {
  GtkWidget *row = gtk_button_new();
  gtk_widget_add_css_class(row, "flat");
  gtk_widget_add_css_class(row, "copy-row");

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

  GtkWidget *label = gtk_label_new(source);
  gtk_widget_add_css_class(label, "copy-source");
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_box_append(GTK_BOX(box), label);

  GtkWidget *detail = gtk_label_new(command);
  gtk_widget_add_css_class(detail, "copy-command");
  gtk_label_set_xalign(GTK_LABEL(detail), 0);
  gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars(GTK_LABEL(detail), 44);
  gtk_box_append(GTK_BOX(box), detail);

  gtk_button_set_child(GTK_BUTTON(row), box);
  gtk_widget_set_tooltip_text(row, command);
  lambdock_accessible_update(row, source, command);

  g_signal_connect_data(row, "clicked", G_CALLBACK(on_launchpad_copy_clicked),
                        g_strdup(command), free_launched_command, 0);

  return row;
}

static GtkWidget *launchpad_install_menu_new(LambdockDesktopEntry *entry) {
  if (!entry)
    return NULL;

  GtkWidget *popover = gtk_popover_new();
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  gtk_widget_add_css_class(popover, "launchpad-copy-menu");

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

  GtkWidget *title = gtk_label_new(entry->name);
  gtk_widget_add_css_class(title, "menu-title");
  gtk_label_set_xalign(GTK_LABEL(title), 0);
  gtk_box_append(GTK_BOX(box), title);

  /* The tile starts the first copy, so the menu lists it first, then the
   * copies it was collapsed with, each named after where it comes from. */
  guint copies = 1 + (entry->alternates ? entry->alternates->len : 0);
  for (guint i = 0; i < copies; i++) {
    LambdockDesktopEntry *copy = (i == 0)
                                     ? entry
                                     : g_ptr_array_index(entry->alternates,
                                                         i - 1);
    char *source = lambdock_desktop_entry_source_label(copy);
    gtk_box_append(GTK_BOX(box), launchpad_copy_row_new(source, copy->exec));
    g_free(source);
  }

  gtk_popover_set_child(GTK_POPOVER(popover), box);
  lambdock_accessible_update(
      popover, entry->name,
      _("Choose which installed copy of this application to open."));

  return popover;
}

/* Closes the open copy menu, if there is one. */
static gboolean launchpad_copy_menu_close(void) {
  if (!g_launchpad_menu)
    return FALSE;

  GtkWidget *menu = g_launchpad_menu;
  g_launchpad_menu = NULL;

  if (gtk_widget_get_visible(menu))
    gtk_popover_popdown(GTK_POPOVER(menu));

  return TRUE;
}

static void launchpad_copy_menu_show(GtkWidget *menu) {
  launchpad_copy_menu_close();
  g_launchpad_menu = menu;
  gtk_popover_popup(GTK_POPOVER(menu));
}

static void on_launchpad_menu_closed(GtkPopover *popover, gpointer user_data) {
  (void)user_data;

  if (g_launchpad_menu == GTK_WIDGET(popover))
    g_launchpad_menu = NULL;
}

static void on_launchpad_tile_menu_request(GtkGestureClick *gesture,
                                           int n_press, double x, double y,
                                           gpointer user_data) {
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;

  launchpad_copy_menu_show(GTK_WIDGET(user_data));
}

/* The Menu key and Shift+F10 are the keyboard's right click, as on the dock. */
static gboolean on_launchpad_tile_key_pressed(GtkEventControllerKey *controller,
                                              guint keyval, guint keycode,
                                              GdkModifierType state,
                                              gpointer user_data) {
  (void)controller;
  (void)keycode;

  if (keyval != GDK_KEY_Menu &&
      !(keyval == GDK_KEY_F10 && (state & GDK_SHIFT_MASK) != 0))
    return FALSE;

  launchpad_copy_menu_show(GTK_WIDGET(user_data));
  return TRUE;
}

/* The copy menu a tile parents, if it has one. */
static GtkWidget *launchpad_tile_copy_menu(GtkWidget *tile) {
  for (GtkWidget *child = gtk_widget_get_first_child(tile); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (GTK_IS_POPOVER(child))
      return child;
  }

  return NULL;
}

/* Gives a tile the menu of its other installed copies. */
static void launchpad_tile_add_copy_menu(GtkWidget *tile,
                                         LambdockDesktopEntry *entry) {
  GtkWidget *menu = launchpad_install_menu_new(entry);
  if (!menu)
    return;

  gtk_widget_set_parent(menu, tile);
  lambdock_popover_close_on_escape(menu);
  g_signal_connect(menu, "closed", G_CALLBACK(on_launchpad_menu_closed), NULL);

  GtkGesture *right_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click),
                                GDK_BUTTON_SECONDARY);
  g_signal_connect(right_click, "pressed",
                   G_CALLBACK(on_launchpad_tile_menu_request), menu);
  gtk_widget_add_controller(tile, GTK_EVENT_CONTROLLER(right_click));

  GtkEventController *keys = gtk_event_controller_key_new();
  /* Capture, like the grid's arrow keys: the tile must see the Menu key even
   * when the flow box would otherwise use it. */
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed",
                   G_CALLBACK(on_launchpad_tile_key_pressed), menu);
  gtk_widget_add_controller(tile, keys);
}

/* ============================================================================
 * GRID & SEARCH
 * ============================================================================
 */

static void launchpad_clear_flow(void) {
  if (!g_launchpad_flow)
    return;

  /* The tiles own their menus, so the pointer to the open one has to go before
   * the tiles do. */
  launchpad_copy_menu_close();

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(g_launchpad_flow)) != NULL) {
    /* A widget has to let go of its popover before GTK finalizes it, or it
     * complains about being finalized with children left (the same reason
     * free_dock_item() unparents one), and this grid is rebuilt on every
     * keystroke. */
    GtkWidget *tile = launchpad_tile_in(child);
    GtkWidget *menu = tile ? launchpad_tile_copy_menu(tile) : NULL;
    if (menu)
      gtk_widget_unparent(menu);

    gtk_flow_box_remove(GTK_FLOW_BOX(g_launchpad_flow), child);
  }
}

static void launchpad_refill(void) {
  if (!g_launchpad_flow || !g_launchpad_entries)
    return;

  launchpad_clear_flow();

  guint shown = 0;
  for (guint i = 0; i < g_launchpad_entries->len; i++) {
    LambdockDesktopEntry *entry = g_ptr_array_index(g_launchpad_entries, i);
    if (!entry_matches_query(entry, g_launchpad_query))
      continue;

    gtk_flow_box_append(GTK_FLOW_BOX(g_launchpad_flow),
                        lambdock_launchpad_tile_new(entry, g_launchpad_icon_size));
    shown++;
  }

  if (shown == 0) {
    GtkWidget *empty = gtk_label_new(_("No applications match your search"));
    gtk_widget_add_css_class(empty, "dim-label");
    gtk_flow_box_append(GTK_FLOW_BOX(g_launchpad_flow), empty);

    /* A screen reader user typing into the field cannot see the grid go
     * empty; say it once instead of on every keystroke. */
    lambdock_accessible_announce(g_launchpad_flow,
                                 _("No applications match your search"), FALSE);
  }
}

/* ============================================================================
 * GRID KEYBOARD NAVIGATION
 * ============================================================================
 * The grid is a GtkFlowBox of plain buttons: Tab walks them, but a launcher is
 * expected to be usable with the arrow keys alone. Movement is by tile index,
 * so it follows the columns the grid actually laid out rather than a fixed
 * count that would jump rows on a resized window.
 */

/* The TILE-th application tile, skipping the "nothing matched" label that the
 * empty grid holds. */
static GtkWidget *launchpad_tile_at(GtkWidget *flow, guint tile) {
  if (!flow)
    return NULL;

  guint seen = 0;
  for (GtkWidget *child = gtk_widget_get_first_child(flow);
       child != NULL; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *tile_button = launchpad_tile_in(child);
    if (!tile_button)
      continue;
    if (seen == tile)
      return tile_button;
    seen++;
  }
  return NULL;
}

static guint launchpad_tile_count(GtkWidget *flow) {
  if (!flow)
    return 0;

  guint count = 0;
  for (GtkWidget *child = gtk_widget_get_first_child(flow); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (launchpad_tile_in(child))
      count++;
  }
  return count;
}

/* Which tile holds the focus, or -1 when the focus is elsewhere (typically the
 * search field). */
static int launchpad_focused_tile(GtkWidget *flow) {
  if (!flow)
    return -1;

  GtkRoot *root = gtk_widget_get_root(flow);
  if (!root || !GTK_IS_WINDOW(root))
    return -1;

  GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(root));
  if (!focus)
    return -1;

  int index = -1;
  guint tile = 0;
  for (GtkWidget *child = gtk_widget_get_first_child(flow); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *tile_button = launchpad_tile_in(child);
    if (!tile_button)
      continue;

    for (GtkWidget *w = focus; w != NULL; w = gtk_widget_get_parent(w)) {
      if (w == tile_button) {
        index = (int)tile;
        break;
      }
      if (w == flow)
        break;
    }
    if (index >= 0)
      return index;
    tile++;
  }
  return -1;
}

static gboolean launchpad_focus_tile(GtkWidget *flow, int tile) {
  GtkWidget *target = launchpad_tile_at(flow, tile < 0 ? 0 : (guint)tile);
  if (!target)
    return FALSE;

  gtk_widget_grab_focus(target);
  return TRUE;
}

static gboolean launchpad_move_focus(GtkWidget *flow, int delta) {
  guint count = launchpad_tile_count(flow);
  if (count == 0)
    return FALSE;

  int current = launchpad_focused_tile(flow);
  if (current < 0)
    return launchpad_focus_tile(flow, 0);

  int next = current + delta;
  if (next < 0)
    next = 0;
  if (next >= (int)count)
    next = (int)count - 1;

  if (next == current)
    return TRUE;

  return launchpad_focus_tile(flow, next);
}

static gboolean on_launchpad_grid_key_pressed(GtkEventControllerKey *controller,
                                              guint keyval, guint keycode,
                                              GdkModifierType state,
                                              gpointer user_data) {
  (void)controller;
  (void)keycode;
  (void)state;

  GtkWidget *flow = GTK_WIDGET(user_data);
  if (!flow)
    flow = g_launchpad_flow;

  int columns = (int)g_launchpad_applied_columns;
  if (columns < 1)
    columns = 1;

  switch (keyval) {
  case GDK_KEY_Left:
    return launchpad_move_focus(flow, -1);
  case GDK_KEY_Right:
    return launchpad_move_focus(flow, 1);
  case GDK_KEY_Up:
    return launchpad_move_focus(flow, -columns);
  case GDK_KEY_Down:
    return launchpad_move_focus(flow, columns);
  case GDK_KEY_Home:
    return launchpad_focus_tile(flow, 0);
  case GDK_KEY_End:
    return launchpad_focus_tile(flow, (int)launchpad_tile_count(flow) - 1);
  default:
    return FALSE;
  }
}

/* Down and Return in the search field: Down enters the grid, Return launches
 * the first match without making the user tab into it. */
static gboolean on_launchpad_search_key_pressed(GtkEventControllerKey *controller,
                                                guint keyval, guint keycode,
                                                GdkModifierType state,
                                                gpointer user_data) {
  (void)controller;
  (void)keycode;
  (void)state;
  (void)user_data;

  if (keyval == GDK_KEY_Down)
    return launchpad_focus_tile(g_launchpad_flow, 0);

  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
    GtkWidget *first = launchpad_tile_at(g_launchpad_flow, 0);
    if (!first)
      return FALSE;
    gtk_widget_activate(first);
    return TRUE;
  }

  return FALSE;
}

static void on_launchpad_search_changed(GtkSearchEntry *entry,
                                        gpointer user_data) {
  (void)user_data;

  g_free(g_launchpad_query);
  g_launchpad_query = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));

  launchpad_refill();
}

static int launchpad_columns(GtkWidget *window) {
  /* The grid lives inside 48px side margins and its own padding; counting them
   * is what keeps the last column from hanging off the screen. */
  int width = gtk_widget_get_width(window);
  int usable = MAX(0, width - 2 * 48 - 16);

  /* Tiles grow with the configured icon size, so a wide grid does not turn
   * into a row of overlapping labels on a smaller screen. */
  int tile = MAX(150, g_launchpad_icon_size + 86);
  int fit = usable > 0 ? usable / tile : 0;

  /* A configured count is an upper bound: asking for 14 tiles on a laptop
   * should not push the last ones off the screen. 0 means "as many as fit". */
  if (g_launchpad_columns > 0)
    return fit > 0 ? MIN(g_launchpad_columns, MAX(LAUNCHPAD_COLUMNS_MIN, fit))
                   : g_launchpad_columns;

  if (fit <= 0)
    return 8;

  int columns = fit;
  if (columns < 4)
    columns = 4;
  if (columns > 10)
    columns = 10;
  return columns;
}

static void on_launchpad_size_allocate(GtkWidget *widget, int width, int height,
                                       gpointer user_data) {
  (void)width;
  (void)height;
  (void)user_data;

  if (!g_launchpad_flow)
    return;

  int columns = launchpad_columns(widget);
  g_launchpad_applied_columns = (guint)columns;
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(g_launchpad_flow),
                                         (guint)columns);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(g_launchpad_flow),
                                         (guint)columns);
}

GtkWidget *lambdock_launchpad_grid_new(GPtrArray *entries) {
  GtkWidget *flow = gtk_flow_box_new();

  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 8);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 8);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 16);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 16);
  gtk_widget_add_css_class(flow, "launchpad-grid");

  gtk_widget_set_hexpand(flow, TRUE);
  gtk_widget_set_vexpand(flow, TRUE);
  lambdock_accessible_update(
      flow, _("Applications"),
      _("Application grid. Use the arrow keys to choose an application and "
        "Enter to open it."));

  GtkEventController *keys = gtk_event_controller_key_new();
  /* Capture: the flow box has its own arrow bindings, and the grid's movement
   * has to follow the tiles rather than an invisible cursor. */
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed",
                   G_CALLBACK(on_launchpad_grid_key_pressed), flow);
  gtk_widget_add_controller(flow, keys);

  (void)entries; /* The flow box is filled by launchpad_refill(). */
  return flow;
}

/* ============================================================================
 * APPEARANCE
 * ============================================================================
 * The launchpad is global, but its settings belong to the dock instance that
 * opened it. They are resolved again on every show, which is also what makes a
 * change made in the preferences window visible without restarting lambdock.
 */

static void launchpad_apply_columns(void) {
  if (!g_launchpad_flow || !g_launchpad_window)
    return;

  int columns = launchpad_columns(g_launchpad_window);
  g_launchpad_applied_columns = (guint)columns;
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(g_launchpad_flow),
                                         (guint)columns);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(g_launchpad_flow),
                                         (guint)columns);
}

static void launchpad_apply_config(LambdockState *state) {
  const char *config_path = state ? state->config_path : NULL;

  g_launchpad_icon_size = get_launchpad_icon_size_config_for(config_path);
  g_launchpad_columns = get_launchpad_columns_config_for(config_path);
  g_launchpad_blur = get_launchpad_blur_config_for(config_path);

  if (!g_launchpad_window)
    return;

  if (g_launchpad_blur)
    gtk_widget_add_css_class(g_launchpad_window, "launchpad-frosted");
  else
    gtk_widget_remove_css_class(g_launchpad_window, "launchpad-frosted");

  launchpad_apply_columns();
}

/* ============================================================================
 * WINDOW
 * ============================================================================
 */

void lambdock_launchpad_hide(void) {
  /* The copies menu belongs to a tile of this window. */
  launchpad_copy_menu_close();

  if (g_launchpad_window)
    gtk_widget_set_visible(g_launchpad_window, FALSE);
}

gboolean lambdock_launchpad_is_open(void) {
  return g_launchpad_window && gtk_widget_get_visible(g_launchpad_window);
}

static gboolean on_launchpad_key_pressed(GtkEventControllerKey *controller,
                                         guint keyval, guint keycode,
                                         GdkModifierType state,
                                         gpointer user_data) {
  (void)controller;
  (void)keycode;
  (void)state;
  (void)user_data;

  if (keyval == GDK_KEY_Escape) {
    /* An open menu is the thing closest to the user, so it goes first. */
    if (launchpad_copy_menu_close())
      return TRUE;

    lambdock_launchpad_hide();
    return TRUE;
  }
  return FALSE;
}

static void on_launchpad_close_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  (void)user_data;
  lambdock_launchpad_hide();
}

static void on_launchpad_click(GtkGestureClick *gesture, int n_press, double x,
                               double y, gpointer user_data) {
  (void)gesture;
  (void)n_press;
  (void)user_data;

  GtkWidget *window = g_launchpad_window;
  if (!window)
    return;

  /* A click on the backdrop (not on a tile or the search field) closes the
   * launchpad, like clicking outside Launchpad on macOS. */
  GtkWidget *picked = gtk_widget_pick(window, x, y, GTK_PICK_DEFAULT);
  if (!picked)
    return;

  if (gtk_widget_has_css_class(picked, "launchpad-tile") ||
      gtk_widget_has_css_class(picked, "launchpad-grid") ||
      gtk_widget_has_css_class(picked, "launchpad-search") ||
      gtk_widget_has_css_class(picked, "launchpad-close") ||
      GTK_IS_ENTRY(picked) || GTK_IS_TEXT(picked))
    return;

  /* Widgets inside a tile or the search field bubble up to their parents. */
  for (GtkWidget *parent = gtk_widget_get_parent(picked); parent != NULL;
       parent = gtk_widget_get_parent(parent)) {
    if (gtk_widget_has_css_class(parent, "launchpad-tile") ||
        gtk_widget_has_css_class(parent, "launchpad-search") ||
        gtk_widget_has_css_class(parent, "launchpad-close") ||
        GTK_IS_ENTRY(parent))
      return;
  }

  lambdock_launchpad_hide();
}

static void launchpad_configure_layer_shell(GtkWidget *window,
                                            LambdockState *state) {
  GtkWindow *gtk_window = GTK_WINDOW(window);

  if (!gtk_layer_is_supported())
    return;

  gtk_layer_init_for_window(gtk_window);
  gtk_layer_set_namespace(gtk_window, "lambdock-launchpad");
  gtk_layer_set_layer(gtk_window, GTK_LAYER_SHELL_LAYER_OVERLAY);
  gtk_layer_set_exclusive_zone(gtk_window, -1);
  gtk_layer_set_keyboard_mode(gtk_window,
                              GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);

  gtk_layer_set_anchor(gtk_window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
  gtk_layer_set_anchor(gtk_window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
  gtk_layer_set_anchor(gtk_window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
  gtk_layer_set_anchor(gtk_window, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);

  if (state && state->monitor)
    gtk_layer_set_monitor(gtk_window, state->monitor);
}

static GtkWidget *launchpad_window_new(LambdockState *state) {
  GtkApplication *app = NULL;
  if (state && state->window && GTK_IS_WINDOW(state->window))
    app = gtk_window_get_application(GTK_WINDOW(state->window));

  if (!app)
    return NULL;

  GtkWidget *window = gtk_application_window_new(app);
  gtk_widget_add_css_class(window, "launchpad");
  gtk_window_set_title(GTK_WINDOW(window), _("Applications"));
  gtk_window_set_default_size(GTK_WINDOW(window), 1280, 800);
  gtk_window_set_hide_on_close(GTK_WINDOW(window), TRUE);

  /* The surface fills the whole overlay, so the frosted background covers the
   * screen edge to edge. The content keeps its own margins inside it. */
  GtkWidget *surface = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(surface, "launchpad-surface");

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
  gtk_widget_set_margin_start(vbox, 48);
  gtk_widget_set_margin_end(vbox, 48);
  gtk_widget_set_margin_top(vbox, 28);
  gtk_widget_set_margin_bottom(vbox, 36);
  gtk_widget_set_valign(vbox, GTK_ALIGN_FILL);
  gtk_widget_set_vexpand(vbox, TRUE);

  GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(top, "launchpad-top");

  GtkWidget *title = gtk_label_new(_("Applications"));
  gtk_widget_add_css_class(title, "launchpad-title");
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(top), title);

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(top), spacer);

  GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
  gtk_widget_add_css_class(close, "flat");
  gtk_widget_add_css_class(close, "launchpad-close");
  gtk_widget_set_tooltip_text(close, _("Close the launchpad (Esc)"));
  lambdock_accessible_update(close, _("Close launchpad"),
                             _("Closes the application grid. Escape does the "
                               "same."));
  gtk_widget_set_size_request(close, 38, 38);
  g_signal_connect(close, "clicked", G_CALLBACK(on_launchpad_close_clicked),
                   NULL);
  gtk_box_append(GTK_BOX(top), close);

  gtk_box_append(GTK_BOX(vbox), top);

  GtkWidget *search = gtk_search_entry_new();
  gtk_widget_add_css_class(search, "launchpad-search");
  gtk_widget_set_halign(search, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(search, 460, -1);
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search),
                                       _("Search applications"));
  lambdock_accessible_update(
      search, _("Search applications"),
      _("Filters the grid as you type. Down enters the grid."));
  g_signal_connect(search, "search-changed",
                   G_CALLBACK(on_launchpad_search_changed), NULL);

  GtkEventController *search_keys = gtk_event_controller_key_new();
  /* Capture: the entry consumes Return for its own "activate" signal, and Down
   * has to leave the field instead of moving the text cursor. */
  gtk_event_controller_set_propagation_phase(search_keys, GTK_PHASE_CAPTURE);
  g_signal_connect(search_keys, "key-pressed",
                   G_CALLBACK(on_launchpad_search_key_pressed), NULL);
  gtk_widget_add_controller(search, search_keys);
  gtk_box_append(GTK_BOX(vbox), search);
  g_launchpad_search = search;

  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_width(
      GTK_SCROLLED_WINDOW(scrolled), TRUE);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(scrolled), TRUE);
  gtk_widget_set_vexpand(scrolled, TRUE);

  GtkWidget *flow = lambdock_launchpad_grid_new(NULL);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), flow);
  g_launchpad_flow = flow;

  gtk_box_append(GTK_BOX(vbox), scrolled);
  gtk_box_append(GTK_BOX(surface), vbox);
  gtk_window_set_child(GTK_WINDOW(window), surface);

  GtkEventController *keys = gtk_event_controller_key_new();
  /* Capture phase: the search entry consumes Escape itself (to clear its text),
   * so a bubbling handler on the window would never see it. */
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_launchpad_key_pressed),
                   NULL);
  gtk_widget_add_controller(window, keys);

  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
  g_signal_connect(click, "pressed", G_CALLBACK(on_launchpad_click), NULL);
  gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(click));

  g_signal_connect(window, "size-allocate",
                   G_CALLBACK(on_launchpad_size_allocate), NULL);

  launchpad_configure_layer_shell(window, state);

  return window;
}

void lambdock_launchpad_show(LambdockState *state) {
  launchpad_apply_config(state);

  if (!g_launchpad_window) {
    g_launchpad_window = launchpad_window_new(state);
    if (!g_launchpad_window) {
      LOG_C_WARN("UI", "Cannot open the launchpad without an application");
      return;
    }

    /* The window exists now: apply the frosted class and the grid width that
     * were resolved before it could be built. */
    launchpad_apply_config(state);
  }

  /* Rescan on every show: installing an application should be enough for it to
   * appear, without restarting the dock. */
  if (g_launchpad_entries)
    lambdock_desktop_entries_free(g_launchpad_entries);
  g_launchpad_entries = lambdock_desktop_entries_scan();

  g_free(g_launchpad_query);
  g_launchpad_query = g_strdup("");
  if (g_launchpad_search && GTK_IS_EDITABLE(g_launchpad_search))
    gtk_editable_set_text(GTK_EDITABLE(g_launchpad_search), "");

  launchpad_refill();

  LOG_C_INFO("UI", "Opening launchpad with %u applications",
             g_launchpad_entries ? g_launchpad_entries->len : 0);

  gtk_window_present(GTK_WINDOW(g_launchpad_window));

  if (g_launchpad_search)
    gtk_widget_grab_focus(g_launchpad_search);
}

void lambdock_launchpad_shutdown(void) {
  /* The tiles go with the window, so they have to let go of their copy menus
   * first. */
  launchpad_clear_flow();

  if (g_launchpad_window) {
    gtk_window_destroy(GTK_WINDOW(g_launchpad_window));
    g_launchpad_window = NULL;
  }

  g_launchpad_flow = NULL;
  g_launchpad_search = NULL;

  g_launchpad_icon_size = LAUNCHPAD_ICON_SIZE;
  g_launchpad_columns = LAUNCHPAD_COLUMNS_AUTO;
  g_launchpad_blur = FALSE;
  g_launchpad_applied_columns = 8;

  if (g_launchpad_entries) {
    lambdock_desktop_entries_free(g_launchpad_entries);
    g_launchpad_entries = NULL;
  }

  g_free(g_launchpad_query);
  g_launchpad_query = NULL;
}
