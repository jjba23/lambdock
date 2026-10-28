// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/ui/meter.h"
#include <gtk/gtk.h>

/* ============================================================================
 * VALUES
 * ============================================================================ */

static void test_new_meter_is_empty(void) {
  GtkWidget *meter = lambdock_meter_new();

  g_assert_true(lambdock_meter_is_meter(meter));
  g_assert_cmpfloat(lambdock_meter_fraction(meter), ==, 0.0);

  g_object_ref_sink(meter);
  g_object_unref(meter);
}

static void test_set_value(void) {
  GtkWidget *meter = lambdock_meter_new();

  lambdock_meter_set_value(meter, 0.42, "42%");
  g_assert_cmpfloat(lambdock_meter_fraction(meter), ==, 0.42);

  /* The gauge is clamped: a poll function that reports nonsense (or a value past
   * its maximum) must not draw outside the ring. */
  lambdock_meter_set_value(meter, 1.7, "170%");
  g_assert_cmpfloat(lambdock_meter_fraction(meter), ==, 1.0);

  lambdock_meter_set_value(meter, -0.3, "-30%");
  g_assert_cmpfloat(lambdock_meter_fraction(meter), ==, 0.0);

  /* A NULL text keeps the previous reading. */
  lambdock_meter_set_value(meter, 0.5, NULL);
  g_assert_cmpfloat(lambdock_meter_fraction(meter), ==, 0.5);

  g_object_ref_sink(meter);
  g_object_unref(meter);
}

static void test_type_guards(void) {
  g_assert_false(lambdock_meter_is_meter(NULL));
  g_assert_cmpfloat(lambdock_meter_fraction(NULL), ==, 0.0);

  GtkWidget *label = gtk_label_new("not a meter");
  g_assert_false(lambdock_meter_is_meter(label));

  /* Setting values on something that is not a meter is a no-op, not a crash. */
  lambdock_meter_set_value(label, 0.5, "50%");
  lambdock_meter_set_icon_size(label, 64);

  g_object_ref_sink(label);
  g_object_unref(label);
}

/* ============================================================================
 * SIZING
 * ============================================================================ */

static void test_icon_size(void) {
  GtkWidget *meter = lambdock_meter_new();

  /* A meter is measured like an icon: a square of the requested size. */
  lambdock_meter_set_icon_size(meter, 64);

  int minimum = 0;
  int natural = 0;
  gtk_widget_measure(meter, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural,
                     NULL, NULL);
  g_assert_cmpint(minimum, ==, 64);
  g_assert_cmpint(natural, ==, 64);

  gtk_widget_measure(meter, GTK_ORIENTATION_VERTICAL, -1, &minimum, &natural,
                     NULL, NULL);
  g_assert_cmpint(minimum, ==, 64);
  g_assert_cmpint(natural, ==, 64);

  /* Non-positive sizes are ignored, so a broken dock-icon-size cannot collapse
   * the gauge. */
  lambdock_meter_set_icon_size(meter, 0);
  gtk_widget_measure(meter, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural,
                     NULL, NULL);
  g_assert_cmpint(minimum, ==, 64);

  g_object_ref_sink(meter);
  g_object_unref(meter);
}

static void test_default_size(void) {
  GtkWidget *meter = lambdock_meter_new();

  int minimum = 0;
  gtk_widget_measure(meter, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, NULL,
                     NULL, NULL);
  g_assert_cmpint(minimum, ==, DEFAULT_ICON_SIZE);

  g_object_ref_sink(meter);
  g_object_unref(meter);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  if (!gtk_init_check()) {
    g_print("Skipping meter tests: No display server available\n");
    return 77;
  }

  g_test_add_func("/ui/meter/new", test_new_meter_is_empty);
  g_test_add_func("/ui/meter/set_value", test_set_value);
  g_test_add_func("/ui/meter/type_guards", test_type_guards);
  g_test_add_func("/ui/meter/icon_size", test_icon_size);
  g_test_add_func("/ui/meter/default_size", test_default_size);

  return g_test_run();
}
