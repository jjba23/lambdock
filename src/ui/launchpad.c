// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "launchpad.h"
#include "../accessibility.h"
#include "../dock_window.h"
#include "../guile_bridge.h"
#include "../logger.h"
#include "desktop_entry.h"
#include "icon_resolver.h"
#include "popover_menu.h"
#include <glib/gi18n.h>
#include <string.h>

/* ============================================================================
 * THE LAUNCHPAD
 * ============================================================================
 *
 * There is exactly one launchpad (there is exactly one screen to cover), so it
 * is one model value rather than a LambdockState: created lazily on first use,
 * hidden rather than destroyed on close, and rescanned every time it opens.
 *
 * The file follows one order:
 *
 *   types      the model, and the value types it is made of
 *   pure       filtering, column maths and the text of a tile: no widgets
 *   tiles      one tile, and the menu of its other installed copies
 *   grid       the flow box, its filling and its keyboard movement
 *   appearance settings of the dock that opened it
 *   callbacks  thin adapters that hand the model to the functions above
 *   window     creation, showing, hiding and teardown
 * ========================================================================= */

/* The grid lives inside these margins: the maths below subtracts them so the
 * last column never hangs off the screen. */
#define LAUNCHPAD_SIDE_MARGIN_PX 48
#define LAUNCHPAD_GRID_INSET_PX 16

/* A tile is at least this wide, and its icon is this much narrower than the
 * tile so a two-line label fits under it. */
#define LAUNCHPAD_TILE_MIN_WIDTH_PX 150
#define LAUNCHPAD_TILE_CHROME_PX 86

/* ============================================================================
 * TYPES
 * ============================================================================
 */

/* How the launchpad looks. Resolved from the settings of the dock that opened
 * it, re-read on every show so a change made in the preferences window shows up
 * without restarting lambdock. */
typedef struct {
  int icon_size;
  int columns; /* 0 = as many as fit */
  gboolean blur;
  gboolean close_on_right; /* the close button: left end unless asked otherwise */
} LaunchpadAppearance;

typedef struct {
  GtkWidget *window;
  GtkWidget *grid;   /* GtkFlowBox of tiles */
  GtkWidget *search; /* GtkSearchEntry */

  /* The header and the close button it carries: the button's end is what
   * `dock-launchpad-close-side' decides. */
  GtkWidget *header;
  GtkWidget *close;

  /* The copy menu of a tile, when one is open: it is the thing closest to the
   * user, so Escape closes it before the launchpad, and hiding the launchpad
   * has to let go of it. */
  GtkWidget *open_copy_menu;

  char *query;           /* owned, "" when nothing is typed */
  GPtrArray *entries;    /* owned, rescanned on every show */
  LaunchpadAppearance appearance;
  guint applied_columns; /* tiles per row the grid last laid out */
} LambdockLaunchpad;

/* One launchpad per process, like the one screen it covers. Everything below
 * takes the model as a parameter; only the public entry points and the signal
 * adapters reach for this. */
static LambdockLaunchpad g_launchpad;

static LambdockLaunchpad *launchpad_instance(void) { return &g_launchpad; }

/* ============================================================================
 * PURE HELPERS (no widgets, no I/O)
 * ============================================================================
 */

/**
 * lambdock_launchpad_entry_matches:
 * @entry: an installed application.
 * @query: what the user typed, or NULL/"" for "everything".
 *
 * Matches case-insensitively against the application's name and its command
 * line, so both "fire" and "--private" find the entry that names Firefox and
 * runs `firefox --private-window`.
 *
 * Returns: TRUE when @entry belongs in the grid.
 */
gboolean lambdock_launchpad_entry_matches(const LambdockDesktopEntry *entry,
                                          const char *query) {
  if (!entry)
    return FALSE;
  if (!query || *query == '\0')
    return TRUE;

  g_autofree char *name = g_utf8_casefold(entry->name ? entry->name : "", -1);
  g_autofree char *needle = g_utf8_casefold(query, -1);
  g_autofree char *exec = g_utf8_casefold(entry->exec ? entry->exec : "", -1);

  return strstr(name, needle) != NULL || strstr(exec, needle) != NULL;
}

/**
 * lambdock_launchpad_columns_for:
 * @usable_width: pixels available between the grid's margins.
 * @icon_size: configured tile icon size.
 * @configured_columns: requested tiles per row, 0 for "as many as fit".
 *
 * Returns: how many tiles fit in a row. A configured count is an upper bound:
 * asking for 14 tiles on a laptop must not push the last ones off the screen.
 */
int lambdock_launchpad_columns_for(int usable_width, int icon_size,
                                   int configured_columns) {
  int tile_width = MAX(LAUNCHPAD_TILE_MIN_WIDTH_PX,
                       icon_size + LAUNCHPAD_TILE_CHROME_PX);
  int fit = usable_width > 0 ? usable_width / tile_width : 0;

  if (configured_columns > 0) {
    if (fit <= 0)
      return configured_columns;
    return MIN(configured_columns, MAX(LAUNCHPAD_COLUMNS_MIN, fit));
  }

  if (fit <= 0)
    return LAUNCHPAD_FALLBACK_COLUMNS;

  return CLAMP(fit, LAUNCHPAD_AUTO_COLUMNS_MIN, LAUNCHPAD_AUTO_COLUMNS_MAX);
}

/** The tile's tooltip: the command, and why right-clicking is worth it. */
static char *tile_tooltip_text(const LambdockDesktopEntry *entry) {
  const char *command = entry->exec ? entry->exec : "";

  if (!entry->alternates || entry->alternates->len == 0)
    return g_strdup(command);

  return g_strdup_printf("%s\n%s", command,
                         _("Right-click for the other installed copies."));
}

/** The tile's accessibility description, which depends on the copies rule. */
static const char *tile_description(const LambdockDesktopEntry *entry) {
  if (entry->alternates && entry->alternates->len > 0)
    return _("Application with more than one installed copy. Press Enter to "
             "open it, or use the menu for the other copies.");

  return _("Application. Press Enter to open.");
}

/** Visits every entry that matches @query, in scan order. */
typedef void (*LaunchpadEntryFunc)(LambdockLaunchpad *launchpad,
                                   LambdockDesktopEntry *entry,
                                   gpointer user_data);

static void launchpad_for_each_matching(LambdockLaunchpad *launchpad,
                                        LaunchpadEntryFunc visit,
                                        gpointer user_data) {
  if (!launchpad->entries)
    return;

  for (guint i = 0; i < launchpad->entries->len; i++) {
    LambdockDesktopEntry *entry = g_ptr_array_index(launchpad->entries, i);
    if (lambdock_launchpad_entry_matches(entry, launchpad->query))
      visit(launchpad, entry, user_data);
  }
}

/* ============================================================================
 * TILES
 * ============================================================================
 */

static void on_tile_clicked(GtkButton *button, gpointer user_data) {
  (void)button;

  /* Launching is already logged by the spawn helper; hiding afterwards is the
   * same behaviour as the original launcher. */
  lambdock_spawn_command((const char *)user_data);
  lambdock_launchpad_hide();
}

static void free_clicked_command(gpointer data, GClosure *closure) {
  (void)closure;
  g_free(data);
}

/** The tile's contents: the icon over its name. */
static GtkWidget *make_tile_content(const LambdockDesktopEntry *entry,
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

  return content;
}

/* Built with the rest of the copy menu, further down. */
static void tile_add_copy_menu(GtkWidget *tile,
                               const LambdockDesktopEntry *entry);

GtkWidget *lambdock_launchpad_tile_new(const LambdockDesktopEntry *entry,
                                       int icon_size) {
  if (!entry)
    return NULL;

  GtkWidget *button = gtk_button_new();
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_add_css_class(button, "launchpad-tile");
  gtk_widget_set_size_request(button, MAX(120, icon_size + 56), -1);
  gtk_button_set_child(GTK_BUTTON(button),
                       make_tile_content(entry, icon_size));

  g_autofree char *tooltip = tile_tooltip_text(entry);
  gtk_widget_set_tooltip_text(button, tooltip);
  lambdock_accessible_update(button, entry->name, tile_description(entry));

  g_signal_connect_data(button, "clicked", G_CALLBACK(on_tile_clicked),
                        g_strdup(entry->exec), free_clicked_command, 0);

  /* The copies travel with the entry and are reachable from the tile and
   * nowhere else. */
  if (entry->alternates && entry->alternates->len > 0)
    tile_add_copy_menu(button, entry);

  return button;
}

/* ============================================================================
 * THE OTHER INSTALLED COPIES
 * ============================================================================
 * The grid lists an application once, but a program can be installed twice --
 * the distro package and a Flatpak export of it. The copies the scan did not
 * list ride along on the entry it kept (see desktop_entry.h), and the tile
 * offers them: the copy that starts is then the one the user picked, instead of
 * the one the scan happened to reach first.
 */

static void on_copy_clicked(GtkButton *button, gpointer user_data) {
  (void)button;

  lambdock_spawn_command((const char *)user_data);
  lambdock_launchpad_hide();
}

/** One row of the menu: where the copy comes from, and the command it runs. */
static GtkWidget *make_copy_row(const char *source, const char *command) {
  GtkWidget *row = gtk_button_new();
  gtk_widget_add_css_class(row, "flat");
  gtk_widget_add_css_class(row, "copy-row");

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

  GtkWidget *source_label = gtk_label_new(source);
  gtk_widget_add_css_class(source_label, "copy-source");
  gtk_label_set_xalign(GTK_LABEL(source_label), 0);
  gtk_box_append(GTK_BOX(box), source_label);

  GtkWidget *command_label = gtk_label_new(command);
  gtk_widget_add_css_class(command_label, "copy-command");
  gtk_label_set_xalign(GTK_LABEL(command_label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(command_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars(GTK_LABEL(command_label), 44);
  gtk_box_append(GTK_BOX(box), command_label);

  gtk_button_set_child(GTK_BUTTON(row), box);
  gtk_widget_set_tooltip_text(row, command);
  lambdock_accessible_update(row, source, command);
  g_signal_connect_data(row, "clicked", G_CALLBACK(on_copy_clicked),
                        g_strdup(command), free_clicked_command, 0);

  return row;
}

/** One menu row per installed copy, the listed one first. */
static void append_copy_rows(GtkBox *box, const LambdockDesktopEntry *entry) {
  guint copies = 1 + (entry->alternates ? entry->alternates->len : 0);

  for (guint i = 0; i < copies; i++) {
    const LambdockDesktopEntry *copy =
        (i == 0) ? entry
                 : g_ptr_array_index(entry->alternates, i - 1);
    g_autofree char *source = lambdock_desktop_entry_source_label(copy);
    gtk_box_append(box, make_copy_row(source, copy->exec));
  }
}

static GtkWidget *make_copy_menu(const LambdockDesktopEntry *entry) {
  GtkWidget *popover = gtk_popover_new();
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  gtk_widget_add_css_class(popover, "launchpad-copy-menu");

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

  GtkWidget *title = gtk_label_new(entry->name);
  gtk_widget_add_css_class(title, "menu-title");
  gtk_label_set_xalign(GTK_LABEL(title), 0);
  gtk_box_append(GTK_BOX(box), title);

  append_copy_rows(GTK_BOX(box), entry);

  gtk_popover_set_child(GTK_POPOVER(popover), box);
  lambdock_accessible_update(
      popover, entry->name,
      _("Choose which installed copy of this application to open."));

  return popover;
}

/** Closes the open copy menu; TRUE when there was one. */
static gboolean launchpad_copy_menu_close(LambdockLaunchpad *launchpad) {
  if (!launchpad || !launchpad->open_copy_menu)
    return FALSE;

  GtkWidget *menu = launchpad->open_copy_menu;
  launchpad->open_copy_menu = NULL;

  if (gtk_widget_get_visible(menu))
    gtk_popover_popdown(GTK_POPOVER(menu));

  return TRUE;
}

static void launchpad_copy_menu_show(LambdockLaunchpad *launchpad,
                                     GtkWidget *menu) {
  if (!launchpad)
    return;

  launchpad_copy_menu_close(launchpad);
  launchpad->open_copy_menu = menu;
  gtk_popover_popup(GTK_POPOVER(menu));
}

static void on_copy_menu_closed(GtkPopover *popover, gpointer user_data) {
  LambdockLaunchpad *launchpad = user_data;

  if (launchpad && launchpad->open_copy_menu == GTK_WIDGET(popover))
    launchpad->open_copy_menu = NULL;
}

/* Adapter: a right click on a tile asks for its menu. */
static void on_tile_menu_request(GtkGestureClick *gesture, int n_press,
                                 double x, double y, gpointer user_data) {
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;

  LambdockLaunchpad *launchpad = launchpad_instance();
  launchpad_copy_menu_show(launchpad, GTK_WIDGET(user_data));
}

/* Adapter: the Menu key and Shift+F10 are the keyboard's right click. */
static gboolean on_tile_key_pressed(GtkEventControllerKey *controller,
                                    guint keyval, guint keycode,
                                    GdkModifierType state, gpointer user_data) {
  (void)controller;
  (void)keycode;

  if (keyval != GDK_KEY_Menu &&
      !(keyval == GDK_KEY_F10 && (state & GDK_SHIFT_MASK) != 0))
    return FALSE;

  launchpad_copy_menu_show(launchpad_instance(), GTK_WIDGET(user_data));
  return TRUE;
}

/** The copy menu a tile parents, if it has one. */
static GtkWidget *tile_copy_menu(GtkWidget *tile) {
  for (GtkWidget *child = gtk_widget_get_first_child(tile); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (GTK_IS_POPOVER(child))
      return child;
  }

  return NULL;
}

static void tile_add_copy_menu(GtkWidget *tile,
                               const LambdockDesktopEntry *entry) {
  GtkWidget *menu = make_copy_menu(entry);

  /* At most one copy menu is open in the process, so the bookkeeping always
   * belongs to the one launchpad -- including for a tile built on its own. */
  gtk_widget_set_parent(menu, tile);
  lambdock_popover_close_on_escape(menu);
  g_signal_connect(menu, "closed", G_CALLBACK(on_copy_menu_closed),
                   launchpad_instance());

  GtkGesture *right_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click),
                                GDK_BUTTON_SECONDARY);
  g_signal_connect(right_click, "pressed", G_CALLBACK(on_tile_menu_request),
                   menu);
  gtk_widget_add_controller(tile, GTK_EVENT_CONTROLLER(right_click));

  GtkEventController *keys = gtk_event_controller_key_new();
  /* Capture: the tile must see the Menu key even when the flow box would use
   * it first. */
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_tile_key_pressed), menu);
  gtk_widget_add_controller(tile, keys);
}

/* ============================================================================
 * GRID: FILLING
 * ============================================================================
 */

/** The tile inside @wrapper, or NULL when @wrapper holds something else (the
 * "nothing matched" label lives in the same flow box). */
static GtkWidget *tile_in(GtkWidget *wrapper) {
  if (!wrapper)
    return NULL;

  if (gtk_widget_has_css_class(wrapper, "launchpad-tile"))
    return wrapper;

  if (GTK_IS_FLOW_BOX_CHILD(wrapper)) {
    GtkWidget *inner =
        gtk_flow_box_child_get_child(GTK_FLOW_BOX_CHILD(wrapper));
    if (inner && gtk_widget_has_css_class(inner, "launchpad-tile"))
      return inner;
  }

  return NULL;
}

static void launchpad_clear_grid(LambdockLaunchpad *launchpad) {
  if (!launchpad->grid)
    return;

  /* The tiles own their menus, so the pointer to the open one has to go before
   * the tiles do. */
  launchpad_copy_menu_close(launchpad);

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(launchpad->grid)) != NULL) {
    /* A widget has to let go of its popover before GTK finalizes it, or GTK
     * complains about finalizing a widget with children (the same reason
     * free_dock_item() unparents one), and this grid is rebuilt on every
     * keystroke. */
    GtkWidget *tile = tile_in(child);
    GtkWidget *menu = tile ? tile_copy_menu(tile) : NULL;
    if (menu)
      gtk_widget_unparent(menu);

    gtk_flow_box_remove(GTK_FLOW_BOX(launchpad->grid), child);
  }
}

static void append_tile(LambdockLaunchpad *launchpad,
                        LambdockDesktopEntry *entry, gpointer user_data) {
  guint *shown = user_data;

  gtk_flow_box_append(
      GTK_FLOW_BOX(launchpad->grid),
      lambdock_launchpad_tile_new(entry, launchpad->appearance.icon_size));
  (*shown)++;
}

/** The "nothing matched" hint, which a screen reader hears once. */
static void launchpad_show_empty_hint(LambdockLaunchpad *launchpad) {
  GtkWidget *empty = gtk_label_new(_("No applications match your search"));
  gtk_widget_add_css_class(empty, "dim-label");
  gtk_flow_box_append(GTK_FLOW_BOX(launchpad->grid), empty);

  lambdock_accessible_announce(launchpad->grid,
                               _("No applications match your search"), FALSE);
}

/** Rebuilds the grid for the current query. */
static void launchpad_refill(LambdockLaunchpad *launchpad) {
  if (!launchpad->grid || !launchpad->entries)
    return;

  launchpad_clear_grid(launchpad);

  guint shown = 0;
  launchpad_for_each_matching(launchpad, append_tile, &shown);

  if (shown == 0)
    launchpad_show_empty_hint(launchpad);
}

/* ============================================================================
 * GRID: KEYBOARD MOVEMENT
 * ============================================================================
 * The grid is a flow box of plain buttons: Tab walks them, but a launcher is
 * expected to work with the arrow keys alone. Movement is by tile index, so it
 * follows the columns the grid actually laid out rather than a fixed count that
 * would jump rows on a resized window.
 */

static guint tile_count(GtkWidget *grid) {
  guint count = 0;

  for (GtkWidget *child = gtk_widget_get_first_child(grid); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (tile_in(child))
      count++;
  }

  return count;
}

/** The @tile-th application tile, skipping the "nothing matched" label. */
static GtkWidget *tile_at(GtkWidget *grid, guint tile) {
  guint seen = 0;

  for (GtkWidget *child = gtk_widget_get_first_child(grid); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *tile_button = tile_in(child);
    if (!tile_button)
      continue;
    if (seen == tile)
      return tile_button;
    seen++;
  }

  return NULL;
}

/** Which tile holds the focus, or -1 when the focus is elsewhere (typically the
 * search field). */
static int focused_tile(GtkWidget *grid) {
  GtkRoot *root = gtk_widget_get_root(grid);
  if (!root || !GTK_IS_WINDOW(root))
    return -1;

  GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(root));
  if (!focus)
    return -1;

  int index = -1;
  guint tile = 0;

  for (GtkWidget *child = gtk_widget_get_first_child(grid); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *tile_button = tile_in(child);
    if (!tile_button)
      continue;

    for (GtkWidget *w = focus; w != NULL; w = gtk_widget_get_parent(w)) {
      if (w == tile_button) {
        index = (int)tile;
        break;
      }
      if (w == grid)
        break;
    }

    if (index >= 0)
      return index;
    tile++;
  }

  return -1;
}

static gboolean focus_tile(GtkWidget *grid, int tile) {
  GtkWidget *target = tile_at(grid, tile < 0 ? 0 : (guint)tile);
  if (!target)
    return FALSE;

  gtk_widget_grab_focus(target);
  return TRUE;
}

/** Moves the focus by @delta tiles, clamped to the grid's ends. */
static gboolean move_focus(GtkWidget *grid, int delta) {
  guint count = tile_count(grid);
  if (count == 0)
    return FALSE;

  int current = focused_tile(grid);
  if (current < 0)
    return focus_tile(grid, 0);

  int next = CLAMP(current + delta, 0, (int)count - 1);
  if (next == current)
    return TRUE;

  return focus_tile(grid, next);
}

/* Adapter: the grid's arrow keys. */
static gboolean on_grid_key_pressed(GtkEventControllerKey *controller,
                                    guint keyval, guint keycode,
                                    GdkModifierType state, gpointer user_data) {
  (void)controller;
  (void)keycode;
  (void)state;

  LambdockLaunchpad *launchpad = user_data;
  if (!launchpad->grid)
    return FALSE;

  int columns = MAX(1, (int)launchpad->applied_columns);

  switch (keyval) {
  case GDK_KEY_Left:
    return move_focus(launchpad->grid, -1);
  case GDK_KEY_Right:
    return move_focus(launchpad->grid, 1);
  case GDK_KEY_Up:
    return move_focus(launchpad->grid, -columns);
  case GDK_KEY_Down:
    return move_focus(launchpad->grid, columns);
  case GDK_KEY_Home:
    return focus_tile(launchpad->grid, 0);
  case GDK_KEY_End:
    return focus_tile(launchpad->grid, (int)tile_count(launchpad->grid) - 1);
  default:
    return FALSE;
  }
}

/* Adapter: Down enters the grid from the search field; Return launches the
 * first match without making the user tab into it. */
static gboolean on_search_key_pressed(GtkEventControllerKey *controller,
                                      guint keyval, guint keycode,
                                      GdkModifierType state,
                                      gpointer user_data) {
  (void)controller;
  (void)keycode;
  (void)state;

  LambdockLaunchpad *launchpad = user_data;
  if (!launchpad->grid)
    return FALSE;

  if (keyval == GDK_KEY_Down)
    return focus_tile(launchpad->grid, 0);

  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
    GtkWidget *first = tile_at(launchpad->grid, 0);
    if (!first)
      return FALSE;
    gtk_widget_activate(first);
    return TRUE;
  }

  return FALSE;
}

/* Adapter: typing re-filters the grid. */
static void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
  LambdockLaunchpad *launchpad = user_data;

  g_free(launchpad->query);
  launchpad->query = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));

  launchpad_refill(launchpad);
}

/* ============================================================================
 * GRID: THE WIDGET
 * ============================================================================
 */

/** Applies @columns to the flow box and remembers them for the arrow keys. */
static void grid_apply_columns(LambdockLaunchpad *launchpad, int columns) {
  launchpad->applied_columns = (guint)columns;

  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(launchpad->grid),
                                         (guint)columns);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(launchpad->grid),
                                         (guint)columns);
}

/** Columns for a window of @width pixels. */
static int columns_for_width(const LambdockLaunchpad *launchpad, int width) {
  int usable = MAX(0, width - 2 * LAUNCHPAD_SIDE_MARGIN_PX -
                          LAUNCHPAD_GRID_INSET_PX);

  return lambdock_launchpad_columns_for(usable, launchpad->appearance.icon_size,
                                        launchpad->appearance.columns);
}

/**
 * launchpad_fit_columns:
 * Chooses how many tiles fit across the launchpad and applies it.
 *
 * GTK4 has no "this widget was resized" signal (GTK3's size-allocate is gone),
 * so this runs when the surface maps and when the launchpad is shown. The
 * window is a full-screen layer-shell surface, so the monitor width is what the
 * grid will get; before the first allocation (@width is 0) that is the only
 * sensible answer, and on the normal-window fallback the real width is used
 * once it exists.
 */
static void launchpad_fit_columns(LambdockLaunchpad *launchpad,
                                  GtkWidget *window) {
  if (!launchpad->grid || !window)
    return;

  int width = gtk_widget_get_width(window);

  if (width <= 0) {
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
    GdkDisplay *display = gtk_widget_get_display(window);
    GdkMonitor *monitor =
        surface ? gdk_display_get_monitor_at_surface(display, surface) : NULL;
    GdkRectangle geometry = {0};
    if (monitor) {
      gdk_monitor_get_geometry(monitor, &geometry);
      width = geometry.width;
    }
  }

  if (width <= 0)
    return;

  grid_apply_columns(launchpad, columns_for_width(launchpad, width));
}

GtkWidget *lambdock_launchpad_grid_new(GPtrArray *entries) {
  (void)entries; /* The grid is filled by the launchpad, from its own model. */

  LambdockLaunchpad *launchpad = launchpad_instance();

  GtkWidget *grid = gtk_flow_box_new();
  launchpad->grid = grid;

  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(grid), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(grid), TRUE);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(grid),
                                         LAUNCHPAD_FALLBACK_COLUMNS);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(grid),
                                         LAUNCHPAD_FALLBACK_COLUMNS);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(grid), 16);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(grid), 16);
  gtk_widget_add_css_class(grid, "launchpad-grid");
  gtk_widget_set_hexpand(grid, TRUE);
  gtk_widget_set_vexpand(grid, TRUE);

  lambdock_accessible_update(
      grid, _("Applications"),
      _("Application grid. Use the arrow keys to choose an application and "
        "Enter to open it."));

  /* Capture: the flow box has arrow bindings of its own, and the grid's
   * movement has to follow the tiles rather than an invisible cursor. */
  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_grid_key_pressed),
                   launchpad);
  gtk_widget_add_controller(grid, keys);

  return grid;
}

/* ============================================================================
 * APPEARANCE
 * ============================================================================
 */

/** Reads the appearance out of the settings of the dock that opened us. */
static LaunchpadAppearance appearance_for(const LambdockState *state) {
  const char *config_path = state ? state->config_path : NULL;

  LaunchpadAppearance appearance = {
      .icon_size = get_launchpad_icon_size_config_for(config_path),
      .columns = get_launchpad_columns_config_for(config_path),
      .blur = get_launchpad_blur_config_for(config_path),
      .close_on_right = get_launchpad_close_on_right_config_for(config_path),
  };

  return appearance;
}

/** Pushes the appearance into the widgets that exist. */
static void launchpad_apply_appearance(LambdockLaunchpad *launchpad,
                                       LaunchpadAppearance appearance) {
  launchpad->appearance = appearance;

  if (!launchpad->window)
    return;

  if (appearance.blur)
    gtk_widget_add_css_class(launchpad->window, "launchpad-frosted");
  else
    gtk_widget_remove_css_class(launchpad->window, "launchpad-frosted");

  /* The close button follows the setting on every show, so moving it in the
   * preferences window takes effect the next time the launchpad opens. */
  lambdock_launchpad_header_place_close(launchpad->header, launchpad->close,
                                        appearance.close_on_right);

  if (launchpad->grid)
    launchpad_fit_columns(launchpad, launchpad->window);
}

/* ============================================================================
 * CLOSING
 * ============================================================================
 */

/* Every part of the launchpad that a click must not treat as "the backdrop".
 * The grid is in the list because a click on the empty area of the grid is not
 * a click outside the launchpad. */
/* GTK's own predicates take a non-const widget, so this one does too. */
static gboolean widget_is_launchpad_content(GtkWidget *widget) {
  return gtk_widget_has_css_class(widget, "launchpad-tile") ||
         gtk_widget_has_css_class(widget, "launchpad-grid") ||
         gtk_widget_has_css_class(widget, "launchpad-search") ||
         gtk_widget_has_css_class(widget, "launchpad-close") ||
         GTK_IS_ENTRY(widget) || GTK_IS_TEXT(widget);
}

/** TRUE when @picked sits inside something the click must not close. */
static gboolean click_hits_content(GtkWidget *picked) {
  for (GtkWidget *widget = picked; widget != NULL;
       widget = gtk_widget_get_parent(widget)) {
    if (widget_is_launchpad_content(widget))
      return TRUE;
  }

  return FALSE;
}

/* Adapter: a click on the backdrop closes the launchpad, like clicking outside
 * Launchpad on macOS. */
static void on_backdrop_click(GtkGestureClick *gesture, int n_press, double x,
                              double y, gpointer user_data) {
  (void)gesture;
  (void)n_press;
  (void)user_data;

  GtkWidget *window = launchpad_instance()->window;
  if (!window)
    return;

  GtkWidget *picked = gtk_widget_pick(window, x, y, GTK_PICK_DEFAULT);
  if (picked && !click_hits_content(picked))
    lambdock_launchpad_hide();
}

/* Adapter: Escape closes the copy menu first, then the launchpad. */
static gboolean on_window_key_pressed(GtkEventControllerKey *controller,
                                      guint keyval, guint keycode,
                                      GdkModifierType state,
                                      gpointer user_data) {
  (void)controller;
  (void)keycode;
  (void)state;
  (void)user_data;

  if (keyval != GDK_KEY_Escape)
    return FALSE;

  if (launchpad_copy_menu_close(launchpad_instance()))
    return TRUE;

  lambdock_launchpad_hide();
  return TRUE;
}

/* Adapter: the close button. */
static void on_close_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  (void)user_data;
  lambdock_launchpad_hide();
}

/* ============================================================================
 * THE HEADER
 * ============================================================================
 * The launchpad has no visible title: it covers the screen with a grid of every
 * installed application, which is its own label, and the search field says what
 * the field is for. What is left of the header is the close button, on the end
 * the dock's `dock-launchpad-close-side' asks for (left by default, like a
 * window's own controls).
 */

GtkWidget *lambdock_launchpad_header_new(GtkWidget **out_close) {
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(header, "launchpad-top");

  GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
  gtk_widget_add_css_class(close, "flat");
  gtk_widget_add_css_class(close, "launchpad-close");
  gtk_widget_set_tooltip_text(close, _("Close the launchpad (Esc)"));
  lambdock_accessible_update(close, _("Close launchpad"),
                             _("Closes the application grid. Escape does the "
                               "same."));
  gtk_widget_set_size_request(close, 38, 38);
  gtk_widget_set_valign(close, GTK_ALIGN_CENTER);
  g_signal_connect(close, "clicked", G_CALLBACK(on_close_clicked), NULL);

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);

  /* Left by default: the button first, then the spacer that fills the row. */
  gtk_box_append(GTK_BOX(header), close);
  gtk_box_append(GTK_BOX(header), spacer);

  if (out_close)
    *out_close = close;

  return header;
}

void lambdock_launchpad_header_place_close(GtkWidget *header, GtkWidget *close,
                                           gboolean on_right) {
  if (!header || !close || !GTK_IS_BOX(header))
    return;

  if (on_right) {
    /* Last child: everything else comes before it. */
    GtkWidget *sibling = NULL;
    for (GtkWidget *child = gtk_widget_get_first_child(header); child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
      if (child != close)
        sibling = child;
    }

    if (sibling)
      gtk_box_reorder_child_after(GTK_BOX(header), close, sibling);
    return;
  }

  /* A NULL sibling puts the button first, which is the default side. */
  gtk_box_reorder_child_after(GTK_BOX(header), close, NULL);
}

/* ============================================================================
 * WINDOW
 * ============================================================================
 */

static void configure_layer_shell(GtkWidget *window, LambdockState *state) {
  GtkWindow *gtk_window = GTK_WINDOW(window);

  if (!lambdock_layer_shell_available())
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

  if (state && is_monitor_valid(state->monitor))
    gtk_layer_set_monitor(gtk_window, state->monitor);
}

/** The header line: the close button, on the side the dock asked for. */
static GtkWidget *make_window_header(LambdockLaunchpad *launchpad) {
  GtkWidget *header = lambdock_launchpad_header_new(&launchpad->close);
  launchpad->header = header;

  lambdock_launchpad_header_place_close(header, launchpad->close,
                                        launchpad->appearance.close_on_right);

  return header;
}

/** The search field, with its own key controller. */
static GtkWidget *make_search_entry(LambdockLaunchpad *launchpad) {
  GtkWidget *search = gtk_search_entry_new();
  gtk_widget_add_css_class(search, "launchpad-search");
  gtk_widget_set_halign(search, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(search, 460, -1);
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search),
                                        _("Search applications"));
  lambdock_accessible_update(
      search, _("Search applications"),
      _("Filters the grid as you type. Down enters the grid."));
  g_signal_connect(search, "search-changed", G_CALLBACK(on_search_changed),
                   launchpad);

  /* Capture: the entry consumes Return for its own "activate" signal, and Down
   * has to leave the field instead of moving the text cursor. */
  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_search_key_pressed),
                   launchpad);
  gtk_widget_add_controller(search, keys);

  /* Remember it on the model: this pointer is what keyboards focus, what
   * launchpad_reset_query() clears and what lambdock_launchpad_search_entry()
   * hands out. Leaving it NULL is why typing into a freshly opened launchpad
   * used to do nothing until the field was clicked. */
  launchpad->search = search;

  return search;
}

/** The scrolling area that holds the grid. */
static GtkWidget *make_grid_viewport(void) {
  GtkWidget *scrolled = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_width(
      GTK_SCROLLED_WINDOW(scrolled), TRUE);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(scrolled), TRUE);
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled),
                                lambdock_launchpad_grid_new(NULL));

  return scrolled;
}

/** Header, search field and grid, stacked with the launchpad's margins. */
static GtkWidget *make_window_body(LambdockLaunchpad *launchpad) {
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
  gtk_widget_set_margin_start(vbox, LAUNCHPAD_SIDE_MARGIN_PX);
  gtk_widget_set_margin_end(vbox, LAUNCHPAD_SIDE_MARGIN_PX);
  gtk_widget_set_margin_top(vbox, 28);
  gtk_widget_set_margin_bottom(vbox, 36);
  gtk_widget_set_valign(vbox, GTK_ALIGN_FILL);
  gtk_widget_set_vexpand(vbox, TRUE);

  gtk_box_append(GTK_BOX(vbox), make_window_header(launchpad));
  gtk_box_append(GTK_BOX(vbox), make_search_entry(launchpad));
  gtk_box_append(GTK_BOX(vbox), make_grid_viewport());

  return vbox;
}

/* Escape (capture phase, because the search entry consumes it itself) and the
 * backdrop click that closes the launchpad. */
/**
 * on_window_mapped:
 * Hands the keyboard to the search field the moment the surface appears, and
 * sizes the grid for the surface it just got.
 *
 * The launchpad is a layer-shell overlay whose keyboard mode is EXCLUSIVE, so
 * the compositor gives it the keyboard as soon as it maps. `set_focus` is the
 * right call here rather than `grab_focus`: it records the widget the window
 * should focus and lets GTK apply it once the children are mapped too (a
 * GtkSearchEntry focuses its inner text widget, not itself).
 */
static void on_window_mapped(GtkWidget *window, gpointer user_data) {
  LambdockLaunchpad *launchpad = user_data;

  launchpad_fit_columns(launchpad, window);

  if (launchpad->search)
    gtk_window_set_focus(GTK_WINDOW(window), launchpad->search);
}

static void install_window_controllers(LambdockLaunchpad *launchpad,
                                       GtkWidget *window) {
  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_window_key_pressed),
                   launchpad);
  gtk_widget_add_controller(window, keys);

  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
  g_signal_connect(click, "pressed", G_CALLBACK(on_backdrop_click), launchpad);
  gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(click));

  /* Typing must work as soon as the launchpad appears, without a click. */
  g_signal_connect(window, "map", G_CALLBACK(on_window_mapped), launchpad);
}

/** The application to parent the launchpad to, or NULL when there is none. */
static GtkApplication *application_for(const LambdockState *state) {
  if (!state || !state->window || !GTK_IS_WINDOW(state->window))
    return NULL;

  return gtk_window_get_application(GTK_WINDOW(state->window));
}

static GtkWidget *make_window_new(LambdockLaunchpad *launchpad,
                                  LambdockState *state) {
  GtkApplication *app = application_for(state);
  if (!app)
    return NULL;

  GtkWidget *window = gtk_application_window_new(app);
  gtk_widget_add_css_class(window, "launchpad");
  gtk_window_set_title(GTK_WINDOW(window), _("Applications"));
  gtk_window_set_default_size(GTK_WINDOW(window), 1280, 800);
  gtk_window_set_hide_on_close(GTK_WINDOW(window), TRUE);

  /* The surface fills the whole overlay so the frosted background covers the
   * screen edge to edge; the content keeps its own margins inside it. */
  GtkWidget *surface = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(surface, "launchpad-surface");
  gtk_box_append(GTK_BOX(surface), make_window_body(launchpad));
  gtk_window_set_child(GTK_WINDOW(window), surface);

  install_window_controllers(launchpad, window);
  configure_layer_shell(window, state);

  return window;
}

/** Loads a fresh application list, replacing the previous one. */
static void launchpad_rescan(LambdockLaunchpad *launchpad) {
  g_clear_pointer(&launchpad->entries, lambdock_desktop_entries_free);
  launchpad->entries = lambdock_desktop_entries_scan();
}

/** Clears the search field and the query it feeds. */
static void launchpad_reset_query(LambdockLaunchpad *launchpad) {
  g_free(launchpad->query);
  launchpad->query = g_strdup("");

  if (launchpad->search && GTK_IS_EDITABLE(launchpad->search))
    gtk_editable_set_text(GTK_EDITABLE(launchpad->search), "");
}

/** Drops the fade-in class on the next idle, which is what runs the transition. */
static gboolean clear_fade_in_idle(gpointer user_data) {
  LambdockLaunchpad *launchpad = user_data;

  if (launchpad->window)
    gtk_widget_remove_css_class(launchpad->window, "launchpad-fading-in");

  return G_SOURCE_REMOVE;
}

void lambdock_launchpad_show(LambdockState *state) {
  LambdockLaunchpad *launchpad = launchpad_instance();
  LaunchpadAppearance appearance = appearance_for(state);

  if (!launchpad->window) {
    launchpad->window = make_window_new(launchpad, state);
    if (!launchpad->window) {
      LOG_C_WARN("UI", "Cannot open the launchpad without an application");
      return;
    }
  }

  /* The window exists now, so the appearance reaches the widgets it owns. */
  launchpad_apply_appearance(launchpad, appearance);

  /* Rescan on every show: installing an application should be enough for it to
   * appear, without restarting the dock. */
  launchpad_rescan(launchpad);
  launchpad_reset_query(launchpad);
  launchpad_refill(launchpad);

  LOG_C_INFO("UI", "Opening launchpad with %u applications",
             launchpad->entries ? launchpad->entries->len : 0);

  gtk_window_present(GTK_WINDOW(launchpad->window));

  /* Fade in, unless motion is reduced: the class is dropped on the next idle so
   * the first frame is drawn transparent and the CSS transition runs. */
  if (lambdock_animations_enabled(state)) {
    gtk_widget_add_css_class(launchpad->window, "launchpad-fading-in");
    g_idle_add(clear_fade_in_idle, launchpad);
  }

  /* Name the focus widget before presenting, so the window has one to give the
   * keyboard to, and grab it now for the case where the window is already
   * mapped. on_window_mapped() repeats this for the first open, where the
   * surface appears asynchronously. */
  if (launchpad->search) {
    gtk_window_set_focus(GTK_WINDOW(launchpad->window), launchpad->search);
    gtk_widget_grab_focus(launchpad->search);
  }
}

void lambdock_launchpad_hide(void) {
  LambdockLaunchpad *launchpad = launchpad_instance();

  /* The copy menu belongs to a tile of this window. */
  launchpad_copy_menu_close(launchpad);

  if (launchpad->window) {
    /* Hiding during the fade would otherwise leave the surface transparent for
     * the next open, which starts a fresh fade anyway. */
    gtk_widget_remove_css_class(launchpad->window, "launchpad-fading-in");
    gtk_widget_set_visible(launchpad->window, FALSE);
  }
}

gboolean lambdock_launchpad_is_open(void) {
  LambdockLaunchpad *launchpad = launchpad_instance();

  return launchpad->window && gtk_widget_get_visible(launchpad->window);
}

GtkWidget *lambdock_launchpad_search_entry(void) {
  return launchpad_instance()->search;
}

void lambdock_launchpad_set_query(const char *query) {
  LambdockLaunchpad *launchpad = launchpad_instance();

  if (!launchpad->search || !GTK_IS_EDITABLE(launchpad->search))
    return;

  /* Setting the text emits "search-changed", which is the same path typing
   * takes: the grid filters exactly as it would for a person. */
  gtk_editable_set_text(GTK_EDITABLE(launchpad->search), query ? query : "");
}

void lambdock_launchpad_shutdown(void) {
  LambdockLaunchpad *launchpad = launchpad_instance();

  /* The tiles go with the window, so they have to let go of their copy menus
   * first. */
  launchpad_clear_grid(launchpad);

  if (launchpad->window) {
    gtk_window_destroy(GTK_WINDOW(launchpad->window));
    launchpad->window = NULL;
  }

  launchpad->grid = NULL;
  launchpad->search = NULL;
  launchpad->header = NULL;
  launchpad->close = NULL;

  LaunchpadAppearance defaults = {
      .icon_size = LAUNCHPAD_ICON_SIZE,
      .columns = LAUNCHPAD_COLUMNS_AUTO,
      .blur = FALSE,
      .close_on_right = FALSE,
  };
  launchpad->appearance = defaults;
  launchpad->applied_columns = LAUNCHPAD_FALLBACK_COLUMNS;

  g_clear_pointer(&launchpad->entries, lambdock_desktop_entries_free);
  g_clear_pointer(&launchpad->query, g_free);
}
