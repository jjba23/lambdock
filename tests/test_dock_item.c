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

#include "../src/lambdock.h"
#include "../src/ui/dock_item.h"
#include <gtk/gtk.h>

#define DUMMY_1 ((void *)0xDEADBEEF)
#define DUMMY_2 ((void *)0xCAFEBABE)
#define DUMMY_3 ((void *)0x12345678)

/* Attaches a minimal, self-contained dock item (button only) to STATE. The
 * caller owns the returned struct and its string members. */
static DockItem *make_plain_item(LambdockState *state, const char *app_id,
                                 const char *name, gboolean favorite) {
  DockItem *item = g_new0(DockItem, 1);
  item->state = state;
  item->is_favorite = favorite;
  item->app_id = g_strdup(app_id);
  item->name = g_strdup(name);
  item->icon = g_strdup(app_id);
  item->button = gtk_button_new();
  item->popover = gtk_popover_new();
  item->poll_fn = SCM_BOOL_F; /* free_dock_item expects a valid SCM here */
  state->dock_items = g_list_append(state->dock_items, item);
  return item;
}

static void free_plain_item(DockItem *item) {
  if (item->popover && gtk_widget_get_parent(item->popover))
    gtk_widget_unparent(item->popover);
  g_object_ref_sink(item->button);
  g_object_unref(item->button);
  if (item->popover) {
    g_object_ref_sink(item->popover);
    g_object_unref(item->popover);
  }
  g_free(item->app_id);
  g_free(item->name);
  g_free(item->icon);
  g_free(item);
}

/* ============================================================================
 * APP-ID / EXEC MATCHING
 * ============================================================================ */

static void test_find_dock_item_by_app_id(void) {
  LambdockState state = {0};

  DockItem item1 = {.app_id = g_strdup("org.gnome.Terminal"),
                    .icon = g_strdup("terminal"),
                    .name = g_strdup("Terminal"),
                    .exec = g_strdup("/usr/bin/gnome-terminal")};

  DockItem item2 = {.app_id = g_strdup("firefox"),
                    .icon = g_strdup("firefox-browser"),
                    .name = g_strdup("Firefox Web Browser"),
                    .exec = g_strdup("firefox-developer-edition %u")};

  state.dock_items = g_list_append(state.dock_items, &item1);
  state.dock_items = g_list_append(state.dock_items, &item2);

  DockItem *found = find_dock_item_by_app_id(&state, "org.gnome.Terminal");
  g_assert_nonnull(found);
  g_assert_cmpstr(found->name, ==, "Terminal");

  found = find_dock_item_by_app_id(&state, "FIREFOX-BROWSER");
  g_assert_nonnull(found);
  g_assert_cmpstr(found->app_id, ==, "firefox");

  found = find_dock_item_by_app_id(&state, "gnome-terminal");
  g_assert_nonnull(found);
  g_assert_cmpstr(found->app_id, ==, "org.gnome.Terminal");

  found = find_dock_item_by_app_id(&state, "nonexistent-app");
  g_assert_null(found);

  /* NULL / empty guards */
  g_assert_null(find_dock_item_by_app_id(&state, NULL));
  g_assert_null(find_dock_item_by_app_id(&state, ""));
  g_assert_null(find_dock_item_by_app_id(NULL, "firefox"));

  g_free(item1.app_id);
  g_free(item1.icon);
  g_free(item1.name);
  g_free(item1.exec);
  g_free(item2.app_id);
  g_free(item2.icon);
  g_free(item2.name);
  g_free(item2.exec);
  g_list_free(state.dock_items);
}

static DockItem *find_with_exec(const char *exec, const char *app_id) {
  LambdockState state = {0};
  DockItem item = {.app_id = g_strdup("unique-app-id"),
                   .name = g_strdup("Unique"),
                   .icon = g_strdup("unique-icon"),
                   .exec = g_strdup(exec)};
  state.dock_items = g_list_append(state.dock_items, &item);

  DockItem *found = find_dock_item_by_app_id(&state, app_id);

  g_free(item.app_id);
  g_free(item.name);
  g_free(item.icon);
  g_free(item.exec);
  g_list_free(state.dock_items);
  return found;
}

static void test_exec_matching(void) {
  /* Whole-token matches at every kind of boundary. */
  g_assert_nonnull(find_with_exec("/usr/bin/gnome-terminal", "gnome-terminal"));
  g_assert_nonnull(find_with_exec("gnome-terminal --working-dir .",
                                  "gnome-terminal"));
  g_assert_nonnull(find_with_exec("vim -u foo,bar", "bar"));
  g_assert_nonnull(find_with_exec("[bar] --flag", "bar"));

  /* Case-insensitive matching. */
  g_assert_nonnull(find_with_exec("MyApp --run", "myapp"));

  /* A token embedded inside a longer alphanumeric word must NOT match. */
  g_assert_null(find_with_exec("myapp", "app"));
  g_assert_null(find_with_exec("firefox", "fire"));
  /* ...but a hyphen IS a token boundary, so this one does match. */
  g_assert_nonnull(find_with_exec("gnome-terminal", "terminal"));

  /* Below the minimum length, no heuristic matching at all. */
  g_assert_null(find_with_exec("vim", "vi"));
  g_assert_null(find_with_exec("firefox", "ox"));

  /* The reverse direction (exec contained in app_id) must not match. */
  g_assert_null(find_with_exec("abc", "abcdef"));

  /* No false positive across the token boundary of a longer word. */
  g_assert_null(find_with_exec("borgmatic", "borg"));
}

/* ============================================================================
 * WAYLAND WINDOW TRACKING
 * ============================================================================ */

static void test_dock_item_window_tracking(void) {
  LambdockState state = {0};
  DockItem *item = g_new0(DockItem, 1);
  item->state = &state;
  item->button = gtk_button_new();
  item->app_id = g_strdup("unique-app-id");
  item->poll_fn = SCM_BOOL_F;

  g_assert_false(gtk_widget_has_css_class(item->button, "running-app"));

  /* NULL guards */
  g_assert_null(dock_item_add_window(NULL, DUMMY_1));
  g_assert_null(dock_item_add_window(item, NULL));
  g_assert_cmpint(g_list_length(item->windows), ==, 0);

  AppWindow *win1 = dock_item_add_window(item, DUMMY_1);
  g_assert_nonnull(win1);
  g_assert_cmpint(g_list_length(item->windows), ==, 1);
  g_assert_true(gtk_widget_has_css_class(item->button, "running-app"));
  g_assert_true(win1->handle == DUMMY_1);
  g_assert_cmpstr(win1->title, ==, "unique-app-id");

  dock_item_add_window(item, DUMMY_2);
  g_assert_cmpint(g_list_length(item->windows), ==, 2);

  /* Idempotency: re-adding an already-tracked handle must not duplicate it
   * (sync_window_trackers and repeated app_id events do exactly this). */
  AppWindow *again = dock_item_add_window(item, DUMMY_1);
  g_assert_nonnull(again);
  g_assert_true(again == win1);
  g_assert_cmpint(g_list_length(item->windows), ==, 2);

  /* Unknown / NULL removals are no-ops. */
  dock_item_remove_window(item, DUMMY_3);
  dock_item_remove_window(item, NULL);
  dock_item_remove_window(NULL, DUMMY_1);
  g_assert_cmpint(g_list_length(item->windows), ==, 2);

  dock_item_remove_window(item, DUMMY_1);
  g_assert_cmpint(g_list_length(item->windows), ==, 1);
  g_assert_true(gtk_widget_has_css_class(item->button, "running-app"));

  dock_item_remove_window(item, DUMMY_2);
  g_assert_cmpint(g_list_length(item->windows), ==, 0);
  g_assert_false(gtk_widget_has_css_class(item->button, "running-app"));

  /* Removing again is still a no-op. */
  dock_item_remove_window(item, DUMMY_2);
  g_assert_cmpint(g_list_length(item->windows), ==, 0);

  g_object_ref_sink(item->button);
  g_object_unref(item->button);
  g_free(item->app_id);
  g_free(item);
}

static void test_update_dock_item_status(void) {
  /* Guards must not crash. */
  update_dock_item_status(NULL);
  DockItem no_button = {0};
  update_dock_item_status(&no_button);

  LambdockState state = {0};
  DockItem *item = g_new0(DockItem, 1);
  item->state = &state;
  item->button = gtk_button_new();
  item->app_id = g_strdup("app");

  update_dock_item_status(item);
  g_assert_false(gtk_widget_has_css_class(item->button, "running-app"));

  dock_item_add_window(item, DUMMY_1);
  g_assert_true(gtk_widget_has_css_class(item->button, "running-app"));

  /* Calling it twice must not add the class twice / crash. */
  update_dock_item_status(item);
  g_assert_true(gtk_widget_has_css_class(item->button, "running-app"));

  g_object_ref_sink(item->button);
  g_object_unref(item->button);
  for (GList *l = item->windows; l != NULL; l = l->next) {
    AppWindow *w = (AppWindow *)l->data;
    g_free(w->title);
    g_free(w);
  }
  g_list_free(item->windows);
  g_free(item->app_id);
  g_free(item);
}

static void test_clear_tracked_windows(void) {
  LambdockState state = {0};
  state.show_when_clear = FALSE; /* avoid reveal_dock touching a NULL window */

  DockItem *fav = make_plain_item(&state, "fav", "Fav", TRUE);
  dock_item_add_window(fav, DUMMY_1);

  DockItem *tracked = make_plain_item(&state, "tracked", "Tracked", FALSE);
  dock_item_add_window(tracked, DUMMY_2);

  DockItem *tracked2 = make_plain_item(&state, "tracked2", "Tracked2", FALSE);
  dock_item_add_window(tracked2, DUMMY_3);

  state.window_trackers = g_list_append(state.window_trackers, DUMMY_1);
  state.window_trackers = g_list_append(state.window_trackers, DUMMY_2);
  state.window_trackers = g_list_append(state.window_trackers, DUMMY_3);

  clear_tracked_windows(&state);

  /* The two non-favorite items are gone, the favorite survives with no
   * windows, and the tracker list is emptied. */
  g_assert_cmpint(g_list_length(state.dock_items), ==, 1);
  g_assert_true(state.dock_items->data == fav);
  g_assert_null(fav->windows);
  g_assert_null(state.window_trackers);

  /* NULL guard */
  clear_tracked_windows(NULL);

  g_list_free(state.dock_items);
  free_plain_item(fav);
  /* tracked/tracked2 were already destroyed by clear_tracked_windows(). */
}

static void test_clear_favorite_dock_items(void) {
  LambdockState state = {0};

  DockItem *fav = make_plain_item(&state, "fav", "Fav", TRUE);
  DockItem *tracked = make_plain_item(&state, "tracked", "Tracked", FALSE);
  DockItem *fav2 = make_plain_item(&state, "fav2", "Fav2", TRUE);
  DockItem *tracked2 = make_plain_item(&state, "tracked2", "Tracked2", FALSE);

  g_assert_cmpint(g_list_length(state.dock_items), ==, 4);
  g_assert_true(state.dock_items->data == fav);
  g_assert_true(state.dock_items->next->next->data == fav2);

  clear_favorite_dock_items(&state);

  /* Only the non-favorite window-tracked items survive. */
  g_assert_cmpint(g_list_length(state.dock_items), ==, 2);
  g_assert_true(state.dock_items->data == tracked);
  g_assert_true(state.dock_items->next->data == tracked2);

  /* NULL / already-empty guards. */
  clear_favorite_dock_items(NULL);
  LambdockState empty = {0};
  clear_favorite_dock_items(&empty);

  g_list_free(state.dock_items);
  /* fav/fav2 were already destroyed by clear_favorite_dock_items(). */
  free_plain_item(tracked);
  free_plain_item(tracked2);
}

/* ============================================================================
 * ITEM CONSTRUCTION & LIFECYCLE
 * ============================================================================ */

static void test_create_separator_dock_item(void) {
  LambdockState state = {0};
  state.icon_size = 48;
  state.orientation = GTK_ORIENTATION_HORIZONTAL;
  state.edge = GTK_LAYER_SHELL_EDGE_BOTTOM;

  DockItem *vert = create_separator_dock_item(&state, GTK_ORIENTATION_VERTICAL);
  g_assert_nonnull(vert);
  g_assert_nonnull(vert->button);
  g_assert_true(GTK_IS_SEPARATOR(vert->button));
  g_assert_true(vert->is_favorite);
  g_assert_false(vert->hover_animate);
  g_assert_cmpint(gtk_orientable_get_orientation(GTK_ORIENTABLE(vert->button)),
                  ==, GTK_ORIENTATION_VERTICAL);
  g_assert_cmpint(gtk_widget_get_margin_start(vert->button), ==,
                  SEPARATOR_MARGIN_PX);
  g_assert_cmpint(gtk_widget_get_margin_end(vert->button), ==,
                  SEPARATOR_MARGIN_PX);
  g_assert_cmpint(g_list_length(state.dock_items), ==, 1);

  DockItem *horiz =
      create_separator_dock_item(&state, GTK_ORIENTATION_HORIZONTAL);
  g_assert_true(GTK_IS_SEPARATOR(horiz->button));
  g_assert_cmpint(gtk_orientable_get_orientation(GTK_ORIENTABLE(horiz->button)),
                  ==, GTK_ORIENTATION_HORIZONTAL);
  g_assert_cmpint(gtk_widget_get_margin_top(horiz->button), ==,
                  SEPARATOR_MARGIN_PX);
  g_assert_cmpint(g_list_length(state.dock_items), ==, 2);

  /* Separators are not GTK buttons: alignment code must treat them safely. */
  apply_dock_item_alignment(vert);

  g_object_ref_sink(vert->button);
  g_object_unref(vert->button);
  g_object_ref_sink(horiz->button);
  g_object_unref(horiz->button);
  g_free(vert);
  g_free(horiz);
  g_list_free(state.dock_items);
}

static void test_create_dock_item(void) {
  LambdockState state = {0};
  state.icon_size = 48;
  state.orientation = GTK_ORIENTATION_HORIZONTAL;
  state.edge = GTK_LAYER_SHELL_EDGE_BOTTOM;
  state.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  DockItem *item = create_dock_item(&state, "Terminal", "utilities-terminal",
                                    "gnome-terminal", "org.gnome.Terminal",
                                    TRUE, TRUE);

  g_assert_nonnull(item);
  g_assert_cmpstr(item->name, ==, "Terminal");
  g_assert_cmpstr(item->icon, ==, "utilities-terminal");
  g_assert_cmpstr(item->exec, ==, "gnome-terminal");
  g_assert_cmpstr(item->app_id, ==, "org.gnome.Terminal");
  g_assert_true(item->is_favorite);
  g_assert_true(item->hover_animate);
  g_assert_true(item->poll_fn == SCM_BOOL_F);
  g_assert_cmpint(item->poll_timer_id, ==, 0);
  g_assert_null(item->windows);

  g_assert_true(GTK_IS_BUTTON(item->button));
  g_assert_true(gtk_widget_has_css_class(item->button, "flat"));

  /* Tooltip falls back to the display name. */
  g_assert_cmpstr(gtk_widget_get_tooltip_text(item->button), ==, "Terminal");

  /* Popover is parented to the button and the button to the dock box. */
  g_assert_nonnull(item->popover);
  g_assert_true(gtk_widget_get_parent(item->popover) == item->button);
  g_assert_true(gtk_widget_get_parent(item->button) == state.box);

  g_assert_cmpint(g_list_length(state.dock_items), ==, 1);
  g_assert_true(state.dock_items->data == item);

  free_dock_item(item);
  g_assert_cmpint(g_list_length(state.dock_items), ==, 1); /* list still refs */
  g_list_free(state.dock_items);

  g_object_ref_sink(state.box);
  g_object_unref(state.box);
}

static void test_create_dock_item_app_id_fallback(void) {
  LambdockState state = {0};
  state.icon_size = 32;

  /* With a NULL app_id, the icon name becomes the identifier. */
  DockItem *item = create_dock_item(&state, "Foo", "utilities-terminal", "foo",
                                    NULL, FALSE, FALSE);
  g_assert_nonnull(item);
  g_assert_cmpstr(item->app_id, ==, "utilities-terminal");
  g_assert_false(item->is_favorite);

  free_dock_item(item);
  g_list_free(state.dock_items);
}

static void test_create_dynamic_dock_item_no_poller(void) {
  LambdockState state = {0};
  state.icon_size = 48;
  state.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  DockItem *item = create_dynamic_dock_item(&state, "Clock", "appointment-new",
                                            "gnome-calendar", SCM_BOOL_F, 1000,
                                            FALSE);
  g_assert_nonnull(item);
  g_assert_cmpstr(item->name, ==, "Clock");
  g_assert_cmpstr(item->app_id, ==, "Clock"); /* dynamic items use the name */
  g_assert_true(item->is_favorite);
  g_assert_false(item->hover_animate);

  /* Without a valid poll procedure no timer is registered. */
  g_assert_true(item->poll_fn == SCM_BOOL_F);
  g_assert_cmpint(item->poll_timer_id, ==, 0);

  g_assert_nonnull(item->label);
  g_assert_true(GTK_IS_LABEL(item->label));
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(item->label)), ==, "Clock");
  g_assert_true(gtk_widget_has_css_class(item->label, "dynamic-item-label"));

  g_assert_true(gtk_widget_has_css_class(item->button, "dynamic-item"));
  g_assert_true(gtk_widget_has_css_class(item->button, "flat"));
  g_assert_true(gtk_widget_get_parent(item->button) == state.box);

  /* The button holds a box containing the label. */
  GtkWidget *child = gtk_button_get_child(GTK_BUTTON(item->button));
  g_assert_true(GTK_IS_BOX(child));
  g_assert_true(gtk_widget_get_parent(item->label) == child);

  free_dock_item(item);
  g_list_free(state.dock_items);
  g_object_ref_sink(state.box);
  g_object_unref(state.box);
}

static void test_free_dock_item_releases_popover_slot(void) {
  LambdockState state = {0};
  state.active_popovers = 1;

  DockItem *item = make_plain_item(&state, "app", "App", FALSE);
  gtk_widget_set_parent(item->popover, item->button);
  /* A popover can only be shown from inside a realized toplevel. */
  GtkWidget *win = gtk_window_new();
  gtk_window_set_child(GTK_WINDOW(win), item->button);
  gtk_window_present(GTK_WINDOW(win));
  gtk_widget_set_visible(item->popover, TRUE);
  g_assert_true(gtk_widget_get_visible(item->popover));

  /* Tearing the item down while its popover is open must release the slot,
   * otherwise the dock can never auto-hide again. */
  free_dock_item(item);
  g_assert_cmpint(state.active_popovers, ==, 0);
  g_list_free(state.dock_items);
  gtk_window_destroy(GTK_WINDOW(win));

  /* NULL is tolerated. */
  free_dock_item(NULL);

  /* A closed popover must not drive the counter below zero. */
  LambdockState state2 = {0};
  DockItem *item2 = make_plain_item(&state2, "app", "App", FALSE);
  gtk_widget_set_visible(item2->popover, FALSE);
  free_dock_item(item2);
  g_assert_cmpint(state2.active_popovers, ==, 0);
  g_list_free(state2.dock_items);
}

/* ============================================================================
 * ALIGNMENT
 * ============================================================================ */

static void test_apply_dock_item_alignment_guards(void) {
  apply_dock_item_alignment(NULL);

  DockItem no_button = {0};
  apply_dock_item_alignment(&no_button);

  LambdockState state = {0};
  DockItem no_state = {.button = gtk_button_new()};
  apply_dock_item_alignment(&no_state);
  g_object_ref_sink(no_state.button);
  g_object_unref(no_state.button);

  DockItem has_state = {.state = &state};
  apply_dock_item_alignment(&has_state); /* button == NULL */
}

static void test_apply_dock_item_alignment_edges(void) {
  LambdockState state = {0};
  state.icon_size = 48;
  state.orientation = GTK_ORIENTATION_HORIZONTAL;

  static const struct {
    GtkLayerShellEdge edge;
    GtkOrientation orientation;
    GtkAlign button_valign;
    GtkAlign button_halign;
  } cases[] = {
      {GTK_LAYER_SHELL_EDGE_BOTTOM, GTK_ORIENTATION_HORIZONTAL,
       GTK_ALIGN_END, GTK_ALIGN_CENTER},
      {GTK_LAYER_SHELL_EDGE_TOP, GTK_ORIENTATION_HORIZONTAL, GTK_ALIGN_START,
       GTK_ALIGN_CENTER},
      {GTK_LAYER_SHELL_EDGE_LEFT, GTK_ORIENTATION_VERTICAL, GTK_ALIGN_CENTER,
       GTK_ALIGN_START},
      {GTK_LAYER_SHELL_EDGE_RIGHT, GTK_ORIENTATION_VERTICAL, GTK_ALIGN_CENTER,
       GTK_ALIGN_END},
  };

  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    DockItem item = {0};
    item.state = &state;
    item.button = gtk_button_new();
    state.edge = cases[i].edge;
    state.orientation = cases[i].orientation;

    apply_dock_item_alignment(&item);

    int w = 0, h = 0;
    gtk_widget_get_size_request(item.button, &w, &h);
    if (state.orientation == GTK_ORIENTATION_HORIZONTAL) {
      g_assert_cmpint(w, ==, -1);
      g_assert_cmpint(h, ==, 48);
    } else {
      g_assert_cmpint(w, ==, 48);
      g_assert_cmpint(h, ==, -1);
    }

    g_assert_cmpint(gtk_widget_get_valign(item.button), ==,
                    cases[i].button_valign);
    g_assert_cmpint(gtk_widget_get_halign(item.button), ==,
                    cases[i].button_halign);

    g_object_ref_sink(item.button);
    g_object_unref(item.button);
  }

  /* An unset icon size falls back to the default (48). */
  DockItem def = {0};
  def.state = &state;
  def.button = gtk_button_new();
  state.icon_size = 0;
  state.edge = GTK_LAYER_SHELL_EDGE_BOTTOM;
  state.orientation = GTK_ORIENTATION_HORIZONTAL;
  apply_dock_item_alignment(&def);
  int w2 = 0, h2 = 0;
  gtk_widget_get_size_request(def.button, &w2, &h2);
  g_assert_cmpint(h2, ==, DEFAULT_ICON_SIZE);
  g_object_ref_sink(def.button);
  g_object_unref(def.button);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/ui/dock_item/find_by_app_id",
                  test_find_dock_item_by_app_id);
  g_test_add_func("/ui/dock_item/exec_matching", test_exec_matching);

  if (gtk_init_check()) {
    g_test_add_func("/ui/dock_item/window_tracking",
                    test_dock_item_window_tracking);
    g_test_add_func("/ui/dock_item/update_status",
                    test_update_dock_item_status);
    g_test_add_func("/ui/dock_item/clear_tracked_windows",
                    test_clear_tracked_windows);
    g_test_add_func("/ui/dock_item/clear_favorite_items",
                    test_clear_favorite_dock_items);
    g_test_add_func("/ui/dock_item/create_separator",
                    test_create_separator_dock_item);
    g_test_add_func("/ui/dock_item/create_item", test_create_dock_item);
    g_test_add_func("/ui/dock_item/create_item_app_id_fallback",
                    test_create_dock_item_app_id_fallback);
    g_test_add_func("/ui/dock_item/create_dynamic_item",
                    test_create_dynamic_dock_item_no_poller);
    g_test_add_func("/ui/dock_item/free_releases_popover_slot",
                    test_free_dock_item_releases_popover_slot);
    g_test_add_func("/ui/dock_item/alignment_guards",
                    test_apply_dock_item_alignment_guards);
    g_test_add_func("/ui/dock_item/alignment_edges",
                    test_apply_dock_item_alignment_edges);
  } else {
    g_test_message("No display server detected. Running logic tests only.");
  }

  return g_test_run();
}
