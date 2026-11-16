// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/ui/clock.h"
#include "../src/core/clock_face.h"
#include <gtk/gtk.h>

/* The face itself is painted from `rust/src/core/clock_face.rs` and measured by
 * `cargo test`; what this binary checks is the C ABI in `src/ui/clock.h` -- the
 * constructor hands back a widget, the reading wraps into a day, a non-positive
 * size is ignored and every setter survives a widget that is not a clock. */

/* ============================================================================
 * VALUES
 * ============================================================================ */

static void test_new_clock_is_midnight(void) {
  GtkWidget *clock = lambdock_clock_new();

  g_assert_true(lambdock_clock_is_clock(clock));
  g_assert_cmpint(lambdock_clock_get_time(clock), ==, 0);

  g_object_ref_sink(clock);
  g_object_unref(clock);
}

static void test_time_wraps_into_a_day(void) {
  GtkWidget *clock = lambdock_clock_new();

  lambdock_clock_set_time(clock, 10 * 3600 + 10 * 60 + 30);
  g_assert_cmpint(lambdock_clock_get_time(clock), ==, 36630);

  /* A poll that reads the clock across midnight, and one whose arithmetic went
   * negative: the face is still told a possible time. */
  lambdock_clock_set_time(clock, LAMBDOCK_SECONDS_PER_DAY + 59);
  g_assert_cmpint(lambdock_clock_get_time(clock), ==, 59);
  lambdock_clock_set_time(clock, -1);
  g_assert_cmpint(lambdock_clock_get_time(clock), ==, LAMBDOCK_SECONDS_PER_DAY - 1);

  g_object_ref_sink(clock);
  g_object_unref(clock);
}

static void test_type_guards(void) {
  g_assert_false(lambdock_clock_is_clock(NULL));
  g_assert_cmpint(lambdock_clock_get_time(NULL), ==, 0);

  GtkWidget *label = gtk_label_new("not a clock");
  g_assert_false(lambdock_clock_is_clock(label));

  lambdock_clock_set_time(label, 3600);
  lambdock_clock_set_icon_size(label, 64);

  g_object_ref_sink(label);
  g_object_unref(label);
}

/* ============================================================================
 * SIZING
 * ============================================================================ */

static void test_icon_size(void) {
  GtkWidget *clock = lambdock_clock_new();

  /* A face is measured like an icon: a square of the requested size. */
  lambdock_clock_set_icon_size(clock, 64);

  int minimum = 0;
  int natural = 0;
  gtk_widget_measure(clock, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural,
                     NULL, NULL);
  g_assert_cmpint(minimum, ==, 64);
  g_assert_cmpint(natural, ==, 64);

  gtk_widget_measure(clock, GTK_ORIENTATION_VERTICAL, -1, &minimum, &natural,
                     NULL, NULL);
  g_assert_cmpint(minimum, ==, 64);

  /* Non-positive sizes are ignored, so a broken dock-icon-size cannot collapse
   * the face. */
  lambdock_clock_set_icon_size(clock, 0);
  gtk_widget_measure(clock, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, NULL,
                     NULL, NULL);
  g_assert_cmpint(minimum, ==, 64);

  g_object_ref_sink(clock);
  g_object_unref(clock);
}

static void test_default_size(void) {
  GtkWidget *clock = lambdock_clock_new();

  int minimum = 0;
  gtk_widget_measure(clock, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, NULL,
                     NULL, NULL);
  g_assert_cmpint(minimum, ==, DEFAULT_ICON_SIZE);

  g_object_ref_sink(clock);
  g_object_unref(clock);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  if (!gtk_init_check()) {
    g_print("Skipping clock tests: No display server available\n");
    return 77;
  }

  g_test_add_func("/ui/clock/new", test_new_clock_is_midnight);
  g_test_add_func("/ui/clock/time_wraps", test_time_wraps_into_a_day);
  g_test_add_func("/ui/clock/type_guards", test_type_guards);
  g_test_add_func("/ui/clock/icon_size", test_icon_size);
  g_test_add_func("/ui/clock/default_size", test_default_size);

  return g_test_run();
}
