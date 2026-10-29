// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

/* Unit tests for the tracker rules: booleans and strings in, booleans out.
 * Deliberately no Wayland, GTK or dock sources are linked here. */

#include "../src/core/tracker_rules.h"

#include <glib.h>

static void test_ignores_app_id(void) {
  /* The compositor sends an empty id before it knows the app. */
  g_assert_true(lambdock_tracker_ignores_app_id(NULL));
  g_assert_true(lambdock_tracker_ignores_app_id(""));

  /* The dock must not track its own windows, however they are spelled. */
  g_assert_true(lambdock_tracker_ignores_app_id("lambdock"));
  g_assert_true(lambdock_tracker_ignores_app_id("LambDock"));
  g_assert_true(
      lambdock_tracker_ignores_app_id("org.jointhefreeworld.lambdock"));
  g_assert_true(
      lambdock_tracker_ignores_app_id("org.jointhefreeworld.Lambdock"));

  /* Everyone else is tracked. */
  g_assert_false(lambdock_tracker_ignores_app_id("org.gnome.Nautilus"));
  g_assert_false(lambdock_tracker_ignores_app_id("firefox"));
  /* Not the same id, only a similar one. */
  g_assert_false(lambdock_tracker_ignores_app_id("lambdock-helper"));
}

static void test_item_should_be_dropped(void) {
  /* A window-tracked item with no windows left is gone. */
  g_assert_true(lambdock_tracker_item_should_be_dropped(FALSE, FALSE));

  /* A pinned item stays, with or without windows. */
  g_assert_false(lambdock_tracker_item_should_be_dropped(FALSE, TRUE));
  g_assert_false(lambdock_tracker_item_should_be_dropped(TRUE, TRUE));

  /* An item that still tracks a window stays. */
  g_assert_false(lambdock_tracker_item_should_be_dropped(TRUE, FALSE));
}

static void test_should_hide(void) {
  /* Both settings on, and something is running: the dock slides away. */
  g_assert_true(lambdock_tracker_should_hide(TRUE, TRUE, TRUE));

  /* Any of the three off means the dock stays. */
  g_assert_false(lambdock_tracker_should_hide(FALSE, TRUE, TRUE));
  g_assert_false(lambdock_tracker_should_hide(TRUE, FALSE, TRUE));
  g_assert_false(lambdock_tracker_should_hide(TRUE, TRUE, FALSE));
}

static void test_should_reveal(void) {
  /* Only docks that hide when idle come back when the last window closes. */
  g_assert_true(lambdock_tracker_should_reveal(TRUE, FALSE));
  g_assert_false(lambdock_tracker_should_reveal(TRUE, TRUE));

  /* A dock that stays visible is never revealed again. */
  g_assert_false(lambdock_tracker_should_reveal(FALSE, FALSE));
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/core/tracker_rules/ignores_app_id", test_ignores_app_id);
  g_test_add_func("/core/tracker_rules/item_should_be_dropped",
                  test_item_should_be_dropped);
  g_test_add_func("/core/tracker_rules/should_hide", test_should_hide);
  g_test_add_func("/core/tracker_rules/should_reveal", test_should_reveal);

  return g_test_run();
}
