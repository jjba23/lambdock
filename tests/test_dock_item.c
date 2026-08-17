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

  // Test 1: Direct match by app_id
  DockItem *found = find_dock_item_by_app_id(&state, "org.gnome.Terminal");
  g_assert_nonnull(found);
  g_assert_cmpstr(found->name, ==, "Terminal");

  // Test 2: Case-insensitive match by icon name
  found = find_dock_item_by_app_id(&state, "FIREFOX-BROWSER");
  g_assert_nonnull(found);
  g_assert_cmpstr(found->app_id, ==, "firefox");

  // Test 3: Substring match via exec binary string
  found = find_dock_item_by_app_id(&state, "gnome-terminal");
  g_assert_nonnull(found);
  g_assert_cmpstr(found->app_id, ==, "org.gnome.Terminal");

  // Test 4: Non-existent app match
  found = find_dock_item_by_app_id(&state, "nonexistent-app");
  g_assert_null(found);

  // Cleanup
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

  // Initially should not have running-app CSS class
  g_assert_false(gtk_widget_has_css_class(item->button, "running-app"));

  // Add first window handle
  AppWindow *win1 = dock_item_add_window(item, dummy_handle_1);
  g_assert_nonnull(win1);
  g_assert_cmpint(g_list_length(item->windows), ==, 1);
  g_assert_true(gtk_widget_has_css_class(item->button, "running-app"));

  // Add second window handle
  dock_item_add_window(item, dummy_handle_2);
  g_assert_cmpint(g_list_length(item->windows), ==, 2);

  // Remove first window handle
  dock_item_remove_window(item, dummy_handle_1);
  g_assert_cmpint(g_list_length(item->windows), ==, 1);
  g_assert_true(gtk_widget_has_css_class(item->button, "running-app"));

  // Remove second window handle -> should remove running-app class
  dock_item_remove_window(item, dummy_handle_2);
  g_assert_cmpint(g_list_length(item->windows), ==, 0);
  g_assert_false(gtk_widget_has_css_class(item->button, "running-app"));

  g_object_ref_sink(item->button);
  g_object_unref(item->button);
  g_free(item);
}

int main(int argc, char **argv) {
  gtk_init();
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/ui/dock_item/find_by_app_id",
                  test_find_dock_item_by_app_id);
  g_test_add_func("/ui/dock_item/window_tracking",
                  test_dock_item_window_tracking);

  return g_test_run();
}
