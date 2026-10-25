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

#include "launchpad.h"
#include "../logger.h"
#include "desktop_entry.h"
#include "icon_resolver.h"
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

static GtkWidget *launchpad_tile_new(LambdockDesktopEntry *entry) {
  GtkIconTheme *theme =
      gtk_icon_theme_get_for_display(gdk_display_get_default());

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_valign(content, GTK_ALIGN_CENTER);

  GtkWidget *image = create_image_for_icon(theme, entry->icon,
                                           LAUNCHPAD_ICON_SIZE);
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
  gtk_button_set_child(GTK_BUTTON(button), content);
  gtk_widget_set_tooltip_text(button, entry->exec);

  g_signal_connect_data(button, "clicked",
                        G_CALLBACK(on_launchpad_tile_clicked),
                        g_strdup(entry->exec), free_launched_command, 0);

  return button;
}

/* ============================================================================
 * GRID & SEARCH
 * ============================================================================
 */

static void launchpad_clear_flow(void) {
  if (!g_launchpad_flow)
    return;

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(g_launchpad_flow)) != NULL)
    gtk_flow_box_remove(GTK_FLOW_BOX(g_launchpad_flow), child);
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
                        launchpad_tile_new(entry));
    shown++;
  }

  if (shown == 0) {
    GtkWidget *empty = gtk_label_new(_("No applications match your search"));
    gtk_widget_add_css_class(empty, "dim-label");
    gtk_flow_box_append(GTK_FLOW_BOX(g_launchpad_flow), empty);
  }
}

static void on_launchpad_search_changed(GtkSearchEntry *entry,
                                        gpointer user_data) {
  (void)user_data;

  g_free(g_launchpad_query);
  g_launchpad_query = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));

  launchpad_refill();
}

static int launchpad_columns(GtkWidget *window) {
  int width = gtk_widget_get_width(window);
  if (width <= 0)
    return 8;

  int columns = width / 150;
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
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 12);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 12);
  gtk_widget_add_css_class(flow, "launchpad-grid");

  gtk_widget_set_hexpand(flow, TRUE);
  gtk_widget_set_vexpand(flow, TRUE);

  (void)entries; /* The flow box is filled by launchpad_refill(). */
  return flow;
}

/* ============================================================================
 * WINDOW
 * ============================================================================
 */

void lambdock_launchpad_hide(void) {
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
    lambdock_launchpad_hide();
    return TRUE;
  }
  return FALSE;
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
      GTK_IS_ENTRY(picked) || GTK_IS_TEXT(picked))
    return;

  /* Widgets inside a tile or the search field bubble up to their parents. */
  for (GtkWidget *parent = gtk_widget_get_parent(picked); parent != NULL;
       parent = gtk_widget_get_parent(parent)) {
    if (gtk_widget_has_css_class(parent, "launchpad-tile") ||
        gtk_widget_has_css_class(parent, "launchpad-search") ||
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

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_start(vbox, 48);
  gtk_widget_set_margin_end(vbox, 48);
  gtk_widget_set_margin_top(vbox, 36);
  gtk_widget_set_margin_bottom(vbox, 36);
  gtk_widget_set_valign(vbox, GTK_ALIGN_FILL);

  GtkWidget *search = gtk_search_entry_new();
  gtk_widget_add_css_class(search, "launchpad-search");
  gtk_widget_set_halign(search, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(search, 360, -1);
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search),
                                       _("Search applications"));
  g_signal_connect(search, "search-changed",
                   G_CALLBACK(on_launchpad_search_changed), NULL);
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
  gtk_window_set_child(GTK_WINDOW(window), vbox);

  GtkEventController *keys = gtk_event_controller_key_new();
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
  if (!g_launchpad_window) {
    g_launchpad_window = launchpad_window_new(state);
    if (!g_launchpad_window) {
      LOG_C_WARN("UI", "Cannot open the launchpad without an application");
      return;
    }
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
  if (g_launchpad_window) {
    gtk_window_destroy(GTK_WINDOW(g_launchpad_window));
    g_launchpad_window = NULL;
  }

  g_launchpad_flow = NULL;
  g_launchpad_search = NULL;

  if (g_launchpad_entries) {
    lambdock_desktop_entries_free(g_launchpad_entries);
    g_launchpad_entries = NULL;
  }

  g_free(g_launchpad_query);
  g_launchpad_query = NULL;
}
