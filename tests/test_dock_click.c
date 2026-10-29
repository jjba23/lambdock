// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

/* Unit tests for the dock click policy: counts, flags and a command string in,
 * one enum out. Deliberately no GTK and no Wayland. */

#include "../src/core/dock_click.h"

#include <glib.h>

static void test_nothing_running(void) {
  /* A pinned launcher with nothing open starts the app. */
  g_assert_cmpint(lambdock_click_action_for(0, FALSE, FALSE, "firefox"), ==,
                  LAMBDOCK_CLICK_LAUNCH);

  /* Without a command there is nothing sensible to do. */
  g_assert_cmpint(lambdock_click_action_for(0, FALSE, FALSE, NULL), ==,
                  LAMBDOCK_CLICK_NOTHING);
  g_assert_cmpint(lambdock_click_action_for(0, FALSE, FALSE, ""), ==,
                  LAMBDOCK_CLICK_NOTHING);
}

static void test_one_window_toggles(void) {
  /* Focused: the click tucks the window away. */
  g_assert_cmpint(lambdock_click_action_for(1, FALSE, TRUE, "x"), ==,
                  LAMBDOCK_CLICK_MINIMIZE);

  /* Minimized: the click brings it back, even if the compositor still reports
   * it as active (the two flags disagree only while events are in flight). */
  g_assert_cmpint(lambdock_click_action_for(1, TRUE, FALSE, "x"), ==,
                  LAMBDOCK_CLICK_ACTIVATE);
  g_assert_cmpint(lambdock_click_action_for(1, TRUE, TRUE, "x"), ==,
                  LAMBDOCK_CLICK_ACTIVATE);

  /* In the background, neither focused nor minimized: focus it. */
  g_assert_cmpint(lambdock_click_action_for(1, FALSE, FALSE, "x"), ==,
                  LAMBDOCK_CLICK_ACTIVATE);

  /* A running app is never launched again, whatever its command is. */
  g_assert_cmpint(lambdock_click_action_for(1, FALSE, TRUE, NULL), ==,
                  LAMBDOCK_CLICK_MINIMIZE);
  g_assert_cmpint(lambdock_click_action_for(1, TRUE, FALSE, NULL), ==,
                  LAMBDOCK_CLICK_ACTIVATE);
}

static void test_several_windows_show_the_menu(void) {
  /* The state flags describe the single window only; with more than one the
   * user picks from the popover instead. */
  g_assert_cmpint(lambdock_click_action_for(2, FALSE, TRUE, "x"), ==,
                  LAMBDOCK_CLICK_MENU);
  g_assert_cmpint(lambdock_click_action_for(9, TRUE, FALSE, NULL), ==,
                  LAMBDOCK_CLICK_MENU);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/core/dock_click/nothing_running", test_nothing_running);
  g_test_add_func("/core/dock_click/one_window_toggles",
                  test_one_window_toggles);
  g_test_add_func("/core/dock_click/several_windows_show_menu",
                  test_several_windows_show_the_menu);

  return g_test_run();
}
