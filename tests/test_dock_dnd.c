// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/lambdock.h"
#include "../src/ui/dock_dnd.h"
#include "../src/ui/dock_item.h"
#include <gtk/gtk.h>

/* ============================================================================
 * DROP POSITION -> INSERTION INDEX
 * ============================================================================ */

static void test_drop_index_for_position(void) {
  const double centers[] = {10.0, 30.0, 50.0};
  const int n = (int)(sizeof(centers) / sizeof(centers[0]));

  /* Left of the first centre inserts at the very front... */
  g_assert_cmpint(dock_drop_index_for_position(centers, n, 0.0), ==, 0);
  /* ...falling on a centre belongs to the icon on the right... */
  g_assert_cmpint(dock_drop_index_for_position(centers, n, 10.0), ==, 1);
  /* ...and past the last centre appends. */
  g_assert_cmpint(dock_drop_index_for_position(centers, n, 50.0), ==, 3);
  g_assert_cmpint(dock_drop_index_for_position(centers, n, 999.0), ==, 3);

  g_assert_cmpint(dock_drop_index_for_position(centers, n, 29.9), ==, 1);
  g_assert_cmpint(dock_drop_index_for_position(centers, n, 30.1), ==, 2);

  /* Degenerate inputs: an empty dock and a missing array. */
  g_assert_cmpint(dock_drop_index_for_position(centers, 0, 5.0), ==, 0);
  g_assert_cmpint(dock_drop_index_for_position(NULL, 3, 5.0), ==, 0);
}

/* ============================================================================
 * REORDERING A GList
 * ============================================================================ */

static void test_list_move(void) {
  GList *list = NULL;
  for (int i = 0; i < 4; i++)
    list = g_list_append(list, GINT_TO_POINTER(i));
  g_assert_cmpint(g_list_length(list), ==, 4);

  /* Move the first element to the end. */
  list = dock_list_move(list, GINT_TO_POINTER(0), 3);
  g_assert_cmpint(GPOINTER_TO_INT(g_list_nth_data(list, 0)), ==, 1);
  g_assert_cmpint(GPOINTER_TO_INT(g_list_nth_data(list, 3)), ==, 0);

  /* Move the last element to the front. */
  list = dock_list_move(list, GINT_TO_POINTER(0), 0);
  g_assert_cmpint(GPOINTER_TO_INT(g_list_nth_data(list, 0)), ==, 0);
  g_assert_cmpint(GPOINTER_TO_INT(g_list_nth_data(list, 3)), ==, 3);

  /* Out-of-range destinations clamp instead of corrupting the list. */
  list = dock_list_move(list, GINT_TO_POINTER(1), 99);
  g_assert_cmpint(g_list_length(list), ==, 4);
  g_assert_cmpint(GPOINTER_TO_INT(g_list_nth_data(list, 3)), ==, 1);
  list = dock_list_move(list, GINT_TO_POINTER(1), -5);
  g_assert_cmpint(g_list_length(list), ==, 4);
  g_assert_cmpint(GPOINTER_TO_INT(g_list_nth_data(list, 0)), ==, 1);

  /* An element that is not in the list changes nothing. */
  GList *unchanged = dock_list_move(list, GINT_TO_POINTER(42), 0);
  g_assert(unchanged == list);

  g_list_free(list);
}

/* ============================================================================
 * ITEM IDENTITY
 * ============================================================================ */

static void test_item_identity(void) {
  g_assert_cmpstr(dock_item_kind_to_string(LAMBDOCK_ITEM_APP), ==, "app");
  g_assert_cmpstr(dock_item_kind_to_string(LAMBDOCK_ITEM_DYNAMIC), ==,
                  "dynamic");
  g_assert_cmpstr(dock_item_kind_to_string(LAMBDOCK_ITEM_SEPARATOR), ==,
                  "separator");
  g_assert_cmpstr(dock_item_kind_to_string(LAMBDOCK_ITEM_STACK), ==, "stack");
  g_assert_cmpstr(dock_item_kind_to_string(LAMBDOCK_ITEM_LAUNCHPAD), ==,
                  "launchpad");

  DockItem item = {0};
  item.name = (char *)"Firefox";
  item.kind = LAMBDOCK_ITEM_APP;

  char *key = dock_item_key(&item);
  g_assert_cmpstr(key, ==, "app:Firefox");
  g_free(key);

  item.kind = LAMBDOCK_ITEM_STACK;
  key = dock_item_key(&item);
  g_assert_cmpstr(key, ==, "stack:Firefox");
  g_free(key);

  g_assert_null(dock_item_key(NULL));
}

/* ============================================================================
 * WIDGET LOOKUPS (need a display)
 * ============================================================================ */

static void test_item_from_widget(void) {
  DockItem item = {0};
  GtkWidget *button = gtk_button_new();

  g_assert_null(dock_item_from_widget(NULL));
  g_assert_null(dock_item_from_widget(button));

  item.button = button;
  g_object_set_data(G_OBJECT(button), "lambdock-item", &item);
  g_assert_true(dock_item_from_widget(button) == &item);

  /* Destroying an item clears the back pointer, which is what makes deferred
   * drag callbacks safe across a hot reload. */
  g_object_set_data(G_OBJECT(button), "lambdock-item", NULL);
  g_assert_null(dock_item_from_widget(button));

  g_object_ref_sink(button);
  g_object_unref(button);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/ui/dock_dnd/drop_index", test_drop_index_for_position);
  g_test_add_func("/ui/dock_dnd/list_move", test_list_move);
  g_test_add_func("/ui/dock_dnd/item_identity", test_item_identity);

  if (!gtk_init_check()) {
    g_print("Skipping drag and drop widget tests: No display server\n");
    return g_test_run();
  }

  g_test_add_func("/ui/dock_dnd/item_from_widget", test_item_from_widget);

  return g_test_run();
}
