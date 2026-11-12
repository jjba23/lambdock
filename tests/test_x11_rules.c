// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

/* Unit tests for the X11/EWMH rules: property values in, decisions out.
 * Deliberately no Xlib, GTK or dock sources are linked here -- reading an X
 * property is the backend's job (src/x11_tracker.c), and what to do with it is
 * decided here. */

#include "../src/core/x11_rules.h"

#include <glib.h>

static LambdockX11Rect monitor(int x, int y, int width, int height) {
  return (LambdockX11Rect){.x = x, .y = y, .width = width, .height = height};
}

static void test_type_is_desktop_chrome(void) {
  /* The desktop owns these surfaces: listing them would put an icon for the
   * panel, the desktop or a menu inside the dock. */
  g_assert_true(
      lambdock_x11_type_is_desktop_chrome("_NET_WM_WINDOW_TYPE_DESKTOP"));
  g_assert_true(lambdock_x11_type_is_desktop_chrome("_NET_WM_WINDOW_TYPE_DOCK"));
  g_assert_true(
      lambdock_x11_type_is_desktop_chrome("_NET_WM_WINDOW_TYPE_TOOLBAR"));
  g_assert_true(lambdock_x11_type_is_desktop_chrome("_NET_WM_WINDOW_TYPE_MENU"));
  g_assert_true(
      lambdock_x11_type_is_desktop_chrome("_NET_WM_WINDOW_TYPE_UTILITY"));
  g_assert_true(
      lambdock_x11_type_is_desktop_chrome("_NET_WM_WINDOW_TYPE_SPLASH"));
  g_assert_true(
      lambdock_x11_type_is_desktop_chrome("_NET_WM_WINDOW_TYPE_NOTIFICATION"));
  g_assert_true(
      lambdock_x11_type_is_desktop_chrome("_NET_WM_WINDOW_TYPE_TOOLTIP"));
  g_assert_true(
      lambdock_x11_type_is_desktop_chrome("_NET_WM_WINDOW_TYPE_COMBO"));
  g_assert_true(
      lambdock_x11_type_is_desktop_chrome("_NET_WM_WINDOW_TYPE_DND"));

  /* Applications are not: this is what the dock is for. */
  g_assert_false(lambdock_x11_type_is_desktop_chrome("_NET_WM_WINDOW_TYPE_NORMAL"));
  g_assert_false(lambdock_x11_type_is_desktop_chrome("_NET_WM_WINDOW_TYPE_DIALOG"));

  /* A window that declares no type at all is an ordinary window. */
  g_assert_false(lambdock_x11_type_is_desktop_chrome(NULL));
  g_assert_false(lambdock_x11_type_is_desktop_chrome(""));
}

static void test_shows_window(void) {
  /* An ordinary application window is listed. */
  g_assert_true(
      lambdock_x11_shows_window(FALSE, FALSE, FALSE, TRUE));

  /* Desktop chrome is never listed. */
  g_assert_false(
      lambdock_x11_shows_window(TRUE, FALSE, FALSE, TRUE));

  /* A window that asked the taskbar to skip it is honoured. */
  g_assert_false(
      lambdock_x11_shows_window(FALSE, TRUE, FALSE, TRUE));

  /* The dock's own windows -- the dock, its preferences, the launchpad -- would
   * otherwise show up as a running application called lambdock. */
  g_assert_false(
      lambdock_x11_shows_window(FALSE, FALSE, TRUE, TRUE));

  /* Without a WM_CLASS there is no app id to attach the window to. */
  g_assert_false(
      lambdock_x11_shows_window(FALSE, FALSE, FALSE, FALSE));
}

static void test_window_state(void) {
  LambdockX11WindowState state = lambdock_x11_window_state(FALSE, 42, 42);
  /* Focused and not hidden: the window being looked at. */
  g_assert_false(state.minimized);
  g_assert_true(state.activated);

  /* _NET_WM_STATE_HIDDEN is how EWMH spells "minimized". */
  state = lambdock_x11_window_state(TRUE, 42, 42);
  g_assert_true(state.minimized);

  /* A window that is not the active one. */
  state = lambdock_x11_window_state(FALSE, 42, 7);
  g_assert_false(state.minimized);
  g_assert_false(state.activated);

  /* Nothing focused at all (no active window on the desktop). */
  state = lambdock_x11_window_state(FALSE, 42, 0);
  g_assert_false(state.activated);
}

static void test_placement_bottom_taskbar(void) {
  /* A full-width bar on the bottom edge of a 1920x1080 screen: it covers the
   * whole width, sits on the edge, and reserves its capsule's thickness. */
  LambdockX11Placement p = lambdock_x11_placement_compute(
      GTK_LAYER_SHELL_EDGE_BOTTOM, monitor(0, 0, 1920, 1080),
      monitor(0, 0, 1920, 1080), 1920, 96, 72);

  g_assert_cmpint(p.x, ==, 0);
  g_assert_cmpint(p.y, ==, 1080 - 96);
  g_assert_cmpint(p.width, ==, 1920);
  g_assert_cmpint(p.height, ==, 96);

  g_assert_cmpint(p.strut_bottom, ==, 72);
  g_assert_cmpint(p.strut_bottom_start_x, ==, 0);
  g_assert_cmpint(p.strut_bottom_end_x, ==, 1919);

  /* Only the edge the dock is on gets a strut. */
  g_assert_cmpint(p.strut_top, ==, 0);
  g_assert_cmpint(p.strut_left, ==, 0);
  g_assert_cmpint(p.strut_right, ==, 0);
}

static void test_placement_bottom_floating(void) {
  /* A floating capsule centred on the right-hand monitor of a two-monitor
   * desktop: same reserved strip, but narrower and centred on its output. */
  LambdockX11Placement p = lambdock_x11_placement_compute(
      GTK_LAYER_SHELL_EDGE_BOTTOM, monitor(0, 0, 3840, 1080),
      monitor(1920, 0, 1920, 1080), 600, 96, 72);

  g_assert_cmpint(p.x, ==, 1920 + (1920 - 600) / 2);
  g_assert_cmpint(p.y, ==, 1080 - 96);
  g_assert_cmpint(p.width, ==, 600);
  g_assert_cmpint(p.height, ==, 96);

  /* The strut ranges are absolute, which is what lets a window manager shrink
   * the work area of that monitor and not the other one. */
  g_assert_cmpint(p.strut_bottom, ==, 72);
  g_assert_cmpint(p.strut_bottom_start_x, ==, 2580);
  g_assert_cmpint(p.strut_bottom_end_x, ==, 3179);
}

static void test_placement_top(void) {
  LambdockX11Placement p = lambdock_x11_placement_compute(
      GTK_LAYER_SHELL_EDGE_TOP, monitor(0, 0, 1920, 1080),
      monitor(0, 0, 1920, 1080), 800, 90, 60);

  g_assert_cmpint(p.y, ==, 0);
  g_assert_cmpint(p.strut_top, ==, 60);
  g_assert_cmpint(p.strut_top_start_x, ==, (1920 - 800) / 2);
  g_assert_cmpint(p.strut_top_end_x, ==, (1920 - 800) / 2 + 799);
  g_assert_cmpint(p.strut_bottom, ==, 0);
}

static void test_placement_left(void) {
  LambdockX11Placement p = lambdock_x11_placement_compute(
      GTK_LAYER_SHELL_EDGE_LEFT, monitor(0, 0, 1920, 1080),
      monitor(0, 0, 1920, 1080), 400, 80, 60);

  g_assert_cmpint(p.x, ==, 0);
  g_assert_cmpint(p.y, ==, (1080 - 400) / 2);
  g_assert_cmpint(p.width, ==, 80);
  g_assert_cmpint(p.height, ==, 400);

  g_assert_cmpint(p.strut_left, ==, 60);
  g_assert_cmpint(p.strut_left_start_y, ==, (1080 - 400) / 2);
  g_assert_cmpint(p.strut_left_end_y, ==, (1080 - 400) / 2 + 399);
}

static void test_placement_right_on_second_monitor(void) {
  LambdockX11Placement p = lambdock_x11_placement_compute(
      GTK_LAYER_SHELL_EDGE_RIGHT, monitor(0, 0, 3840, 1080),
      monitor(1920, 0, 1920, 1080), 400, 80, 60);

  g_assert_cmpint(p.x, ==, 3840 - 80);
  g_assert_cmpint(p.strut_right, ==, 60);
  g_assert_cmpint(p.strut_right_start_y, ==, (1080 - 400) / 2);
  g_assert_cmpint(p.strut_right_end_y, ==, (1080 - 400) / 2 + 399);
  g_assert_cmpint(p.strut_left, ==, 0);
}

static void test_placement_edges(void) {
  /* A row wider than its monitor is clamped to the monitor: the dock scrolls
   * its content, and a window wider than the screen would hang off it. */
  LambdockX11Placement p = lambdock_x11_placement_compute(
      GTK_LAYER_SHELL_EDGE_BOTTOM, monitor(0, 0, 1920, 1080),
      monitor(0, 0, 1920, 1080), 4000, 96, 72);
  g_assert_cmpint(p.width, ==, 1920);
  g_assert_cmpint(p.x, ==, 0);
  g_assert_cmpint(p.strut_bottom_end_x, ==, 1919);

  /* No content measured yet (the first frame of a dock): the bar spans its
   * monitor rather than collapsing to nothing. */
  p = lambdock_x11_placement_compute(GTK_LAYER_SHELL_EDGE_BOTTOM,
                                     monitor(0, 0, 1920, 1080),
                                     monitor(0, 0, 1920, 1080), 0, 96, 72);
  g_assert_cmpint(p.width, ==, 1920);

  /* A cross axis taller than the monitor is clamped too, so the window stays
   * on screen instead of being positioned above the top edge. */
  p = lambdock_x11_placement_compute(GTK_LAYER_SHELL_EDGE_BOTTOM,
                                     monitor(0, 0, 1920, 1080),
                                     monitor(0, 0, 1920, 1080), 800, 2000, 72);
  g_assert_cmpint(p.height, ==, 1080);
  g_assert_cmpint(p.y, ==, 0);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/core/x11_rules/type_is_desktop_chrome",
                  test_type_is_desktop_chrome);
  g_test_add_func("/core/x11_rules/shows_window", test_shows_window);
  g_test_add_func("/core/x11_rules/window_state", test_window_state);
  g_test_add_func("/core/x11_rules/placement_bottom_taskbar",
                  test_placement_bottom_taskbar);
  g_test_add_func("/core/x11_rules/placement_bottom_floating",
                  test_placement_bottom_floating);
  g_test_add_func("/core/x11_rules/placement_top", test_placement_top);
  g_test_add_func("/core/x11_rules/placement_left", test_placement_left);
  g_test_add_func("/core/x11_rules/placement_right_on_second_monitor",
                  test_placement_right_on_second_monitor);
  g_test_add_func("/core/x11_rules/placement_edges", test_placement_edges);

  return g_test_run();
}
