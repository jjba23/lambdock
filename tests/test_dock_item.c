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

static void test_dock_item_window_tracking(void) {
  LambdockState state = {0};
  DockItem *item = g_new0(DockItem, 1);
  item->state = &state;
  item->button = gtk_button_new();

  struct zwlr_foreign_toplevel_handle_v1 *dummy_handle_1 = (void *)0xDEADBEEF;
  struct zwlr_foreign_toplevel_handle_v1 *dummy_handle_2 = (void *)0xCAFEBABE;

  g_assert_false(gtk_widget_has_css_class(item->button, "running-app"));

  AppWindow *win1 = dock_item_add_window(item, dummy_handle_1);
  g_assert_nonnull(win1);
  g_assert_cmpint(g_list_length(item->windows), ==, 1);
  g_assert_true(gtk_widget_has_css_class(item->button, "running-app"));

  dock_item_add_window(item, dummy_handle_2);
  g_assert_cmpint(g_list_length(item->windows), ==, 2);

  dock_item_remove_window(item, dummy_handle_1);
  g_assert_cmpint(g_list_length(item->windows), ==, 1);
  g_assert_true(gtk_widget_has_css_class(item->button, "running-app"));

  dock_item_remove_window(item, dummy_handle_2);
  g_assert_cmpint(g_list_length(item->windows), ==, 0);
  g_assert_false(gtk_widget_has_css_class(item->button, "running-app"));

  g_object_ref_sink(item->button);
  g_object_unref(item->button);
  g_free(item);
}

static void test_clear_favorite_dock_items(void) {
  LambdockState state = {0};

  // Item 1: Favorite launcher (no active windows)
  DockItem *fav_item = g_new0(DockItem, 1);
  fav_item->is_favorite = TRUE;
  fav_item->state = &state;
  fav_item->button = gtk_button_new(); // Provide valid widget for cleanup

  // Item 2: Dynamic running app (has active windows, not favorite)
  DockItem *running_item = g_new0(DockItem, 1);
  running_item->is_favorite = FALSE;
  running_item->state = &state;
  running_item->button = gtk_button_new(); // Provide valid widget for cleanup
  running_item->windows = g_list_append(NULL, g_new0(AppWindow, 1));

  state.dock_items = g_list_append(state.dock_items, fav_item);
  state.dock_items = g_list_append(state.dock_items, running_item);

  g_assert_cmpint(g_list_length(state.dock_items), ==, 2);

  // Clear favorites
  clear_favorite_dock_items(&state);

  // Only running dynamic item should remain
  g_assert_cmpint(g_list_length(state.dock_items), ==, 1);
  g_assert_true(state.dock_items->data == running_item);

  // Cleanup remaining running item safely
  g_free(running_item->windows->data);
  g_list_free(running_item->windows);
  g_object_ref_sink(running_item->button);
  g_object_unref(running_item->button);
  g_free(running_item);
  g_list_free(state.dock_items);
}

static void test_create_separator_dock_item(void) {
  LambdockState state = {0};
  state.icon_size = 48;
  state.orientation = GTK_ORIENTATION_HORIZONTAL;
  state.edge = GTK_LAYER_SHELL_EDGE_BOTTOM;

  DockItem *sep = create_separator_dock_item(&state, GTK_ORIENTATION_VERTICAL);

  g_assert_nonnull(sep);
  g_assert_nonnull(sep->button);
  g_assert_true(GTK_IS_SEPARATOR(sep->button));
  g_assert_true(sep->is_favorite);
  g_assert_false(sep->hover_animate);
  g_assert_cmpint(g_list_length(state.dock_items), ==, 1);

  g_object_ref_sink(sep->button);
  g_object_unref(sep->button);
  g_free(sep);
  g_list_free(state.dock_items);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/ui/dock_item/find_by_app_id",
                  test_find_dock_item_by_app_id);

  if (gtk_init_check()) {
    g_test_add_func("/ui/dock_item/window_tracking",
                    test_dock_item_window_tracking);
    g_test_add_func("/ui/dock_item/clear_favorite_dock_items",
                    test_clear_favorite_dock_items);
    g_test_add_func("/ui/dock_item/create_separator_dock_item",
                    test_create_separator_dock_item);
  } else {
    g_test_message("No display server detected. Running logic tests only.");
  }

  return g_test_run();
}
