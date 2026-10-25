// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY ; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock. If not, see <https://www.gnu.org/licenses/>.

#include "../src/ui/stack.h"
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <math.h>

/* ============================================================================
 * PATH HANDLING
 * ============================================================================ */

static void test_expand_path(void) {
  char *path = lambdock_stack_expand_path("~");
  g_assert_cmpstr(path, ==, g_get_home_dir());
  g_free(path);

  path = lambdock_stack_expand_path("~/Downloads");
  char *expected = g_build_filename(g_get_home_dir(), "Downloads", NULL);
  g_assert_cmpstr(path, ==, expected);
  g_free(expected);
  g_free(path);

  /* Absolute and relative paths are passed through untouched. */
  path = lambdock_stack_expand_path("/usr/share");
  g_assert_cmpstr(path, ==, "/usr/share");
  g_free(path);

  g_assert_null(lambdock_stack_expand_path(NULL));
  g_assert_null(lambdock_stack_expand_path(""));
}

/* ============================================================================
 * DIRECTORY LISTING
 * ============================================================================ */

static void test_list_directory(void) {
  char *dir = g_dir_make_tmp("lambdock-stack-XXXXXX", NULL);
  g_assert_nonnull(dir);

  char *sub = g_build_filename(dir, "zdir", NULL);
  g_assert_cmpint(g_mkdir_with_parents(sub, 0755), ==, 0);
  g_free(sub);

  char *file_b = g_build_filename(dir, "beta.txt", NULL);
  g_assert_true(g_file_set_contents(file_b, "b", -1, NULL));
  g_free(file_b);

  char *file_a = g_build_filename(dir, "Alpha.txt", NULL);
  g_assert_true(g_file_set_contents(file_a, "a", -1, NULL));
  g_free(file_a);

  char *hidden = g_build_filename(dir, ".hidden", NULL);
  g_assert_true(g_file_set_contents(hidden, "h", -1, NULL));
  g_free(hidden);

  GPtrArray *entries = lambdock_stack_list_directory(dir);
  g_assert_nonnull(entries);
  g_assert_cmpuint(entries->len, ==, 3);

  /* Folders first, then names in locale order, and dot files never show up. */
  LambdockStackEntry *entry = g_ptr_array_index(entries, 0);
  g_assert_true(entry->is_dir);
  g_assert_cmpstr(entry->name, ==, "zdir");
  g_assert_cmpstr(((LambdockStackEntry *)g_ptr_array_index(entries, 1))->name,
                  ==, "Alpha.txt");
  g_assert_cmpstr(((LambdockStackEntry *)g_ptr_array_index(entries, 2))->name,
                  ==, "beta.txt");

  /* The entry paths are absolute and usable. */
  g_assert_true(((LambdockStackEntry *)g_ptr_array_index(entries, 1))->path[0] ==
                '/');

  lambdock_stack_entries_free(entries);

  /* A missing directory is an empty list, not a crash (it does log a warning,
   * which the test framework would otherwise treat as fatal). */
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*not a readable directory*");
  entries = lambdock_stack_list_directory("/nonexistent/lambdock/stack");
  g_test_assert_expected_messages();
  g_assert_nonnull(entries);
  g_assert_cmpuint(entries->len, ==, 0);
  lambdock_stack_entries_free(entries);
  lambdock_stack_entries_free(NULL);

  char *paths[3];
  paths[0] = g_build_filename(dir, "beta.txt", NULL);
  paths[1] = g_build_filename(dir, "Alpha.txt", NULL);
  paths[2] = g_build_filename(dir, ".hidden", NULL);
  for (int i = 0; i < 3; i++) {
    g_assert_cmpint(g_remove(paths[i]), ==, 0);
    g_free(paths[i]);
  }
  char *sub2 = g_build_filename(dir, "zdir", NULL);
  g_assert_cmpint(g_rmdir(sub2), ==, 0);
  g_free(sub2);
  g_assert_cmpint(g_rmdir(dir), ==, 0);
  g_free(dir);
}

/* ============================================================================
 * VIEW MODES
 * ============================================================================ */

static void test_view_mode_names(void) {
  g_assert_cmpint(lambdock_stack_view_from_string("grid"), ==,
                  LAMBDOCK_STACK_VIEW_GRID);
  g_assert_cmpint(lambdock_stack_view_from_string("list"), ==,
                  LAMBDOCK_STACK_VIEW_LIST);
  g_assert_cmpint(lambdock_stack_view_from_string("FAN"), ==,
                  LAMBDOCK_STACK_VIEW_FAN);

  /* Anything unknown is the default grid. */
  g_assert_cmpint(lambdock_stack_view_from_string(NULL), ==,
                  LAMBDOCK_STACK_VIEW_GRID);
  g_assert_cmpint(lambdock_stack_view_from_string(""), ==,
                  LAMBDOCK_STACK_VIEW_GRID);
  g_assert_cmpint(lambdock_stack_view_from_string("carousel"), ==,
                  LAMBDOCK_STACK_VIEW_GRID);

  /* Round trip. */
  for (int view = LAMBDOCK_STACK_VIEW_GRID; view <= LAMBDOCK_STACK_VIEW_FAN;
       view++) {
    const char *name = lambdock_stack_view_to_string((LambdockStackView)view);
    g_assert_cmpint(lambdock_stack_view_from_string(name), ==, view);
  }
}

/* ============================================================================
 * FAN HIT TESTING
 * ============================================================================ */

static void test_fan_contains(void) {
  /* A flat card: plain rectangle test. */
  g_assert_true(
      lambdock_stack_fan_contains(0, 0, 0, 0, 0, 100, 100));
  g_assert_true(
      lambdock_stack_fan_contains(49, -49, 0, 0, 0, 100, 100));
  g_assert_false(
      lambdock_stack_fan_contains(51, 0, 0, 0, 0, 100, 100));

  /* Centred away from the origin. */
  g_assert_true(
      lambdock_stack_fan_contains(110, 205, 100, 200, 0, 100, 100));

  /* Rotating the card by 90 degrees swaps which side the point falls on: a
   * point below the centre is now inside the (now tall) card, one to the right
   * is outside. */
  g_assert_true(
      lambdock_stack_fan_contains(100, 290, 100, 200, 90, 200, 40));
  g_assert_false(
      lambdock_stack_fan_contains(190, 200, 100, 200, 90, 200, 40));
}

/* ============================================================================
 * VIEW WIDGETS
 * ============================================================================ */

static void test_view_widgets(void) {
  GPtrArray *entries = g_ptr_array_new_with_free_func(
      (GDestroyNotify)lambdock_stack_entry_free);
  for (int i = 0; i < 5; i++) {
    LambdockStackEntry *entry = g_new0(LambdockStackEntry, 1);
    entry->name = g_strdup_printf("file-%d.txt", i);
    entry->path = g_strdup("/tmp/file.txt");
    entry->is_dir = FALSE;
    g_ptr_array_add(entries, entry);
  }

  /* Empty folder: a friendly label instead of an empty popover. */
  GtkWidget *empty = lambdock_stack_view_new(NULL, LAMBDOCK_STACK_VIEW_GRID, 48);
  g_assert_true(GTK_IS_LABEL(empty));
  g_object_ref_sink(empty);
  g_object_unref(empty);

  GtkWidget *grid = lambdock_stack_view_new(entries, LAMBDOCK_STACK_VIEW_GRID,
                                            48);
  g_assert_true(GTK_IS_SCROLLED_WINDOW(grid));
  g_object_ref_sink(grid);
  g_object_unref(grid);

  GtkWidget *list = lambdock_stack_view_new(entries, LAMBDOCK_STACK_VIEW_LIST,
                                            48);
  g_assert_true(GTK_IS_SCROLLED_WINDOW(list));
  g_object_ref_sink(list);
  g_object_unref(list);

  /* The fan paints itself: it must report a sensible size for its cards. */
  GtkWidget *fan = lambdock_stack_view_new(entries, LAMBDOCK_STACK_VIEW_FAN,
                                           48);
  g_assert_nonnull(fan);
  int minimum = 0;
  int natural = 0;
  gtk_widget_measure(fan, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural,
                     NULL, NULL);
  g_assert_cmpint(natural, >, 0);
  gtk_widget_measure(fan, GTK_ORIENTATION_VERTICAL, -1, &minimum, &natural,
                     NULL, NULL);
  g_assert_cmpint(natural, >, 0);
  g_object_ref_sink(fan);
  g_object_unref(fan);

  lambdock_stack_entries_free(entries);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/ui/stack/expand_path", test_expand_path);
  g_test_add_func("/ui/stack/list_directory", test_list_directory);
  g_test_add_func("/ui/stack/view_mode_names", test_view_mode_names);
  g_test_add_func("/ui/stack/fan_contains", test_fan_contains);

  if (!gtk_init_check()) {
    g_print("Skipping stack widget tests: No display server available\n");
    return g_test_run();
  }

  g_test_add_func("/ui/stack/view_widgets", test_view_widgets);

  return g_test_run();
}
