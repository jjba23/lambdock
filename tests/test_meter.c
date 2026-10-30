// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/ui/meter.h"
#include <gtk/gtk.h>
#include <math.h>

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

/* The gauge's proportions are a value, so they can be checked without painting:
 * the tile stays inside the box, the ring inside the tile, and everything scales
 * with the icon size. */
static void test_geometry(void) {
  LambdockMeterGeometry g = lambdock_meter_geometry_compute(48, 48);

  /* The tile is the box minus 4% of padding on each side, with 22% corners. */
  g_assert_cmpfloat_with_epsilon(g.tile.origin.x, 48 * 0.04, 1e-4);
  g_assert_cmpfloat_with_epsilon(g.tile.origin.y, 48 * 0.04, 1e-4);
  g_assert_cmpfloat_with_epsilon(g.tile.size.width, 48 - 2 * 48 * 0.04, 1e-4);
  g_assert_cmpfloat_with_epsilon(g.tile_radius, 48 * 0.22, 1e-4);

  /* Centred, with a ring that fits inside the tile. */
  g_assert_cmpfloat_with_epsilon(g.center_x, 24.0, 1e-4);
  g_assert_cmpfloat_with_epsilon(g.center_y, 24.0, 1e-4);
  g_assert_cmpfloat(g.line_width, >=, 2.0f);
  g_assert_cmpfloat(g.radius, >, 0.0f);
  g_assert_cmpfloat(g.radius + g.line_width / 2.0, <, 48 * 0.5);

  /* The reading gets a square inside the ring. */
  g_assert_cmpfloat_with_epsilon(g.text_box, 48 * 0.66, 1e-4);

  /* Bigger icons are the same drawing, scaled: the ring grows with the box. */
  LambdockMeterGeometry big = lambdock_meter_geometry_compute(96, 96);
  g_assert_cmpfloat(big.radius, >, g.radius);
  g_assert_cmpfloat(big.tile.size.width, >, g.tile.size.width);
  g_assert_cmpfloat_with_epsilon(big.radius / g.radius, 2.0, 0.05);

  /* A gauge in a rectangular box uses the short side, so it never overflows. */
  LambdockMeterGeometry wide = lambdock_meter_geometry_compute(120, 40);
  g_assert_cmpfloat_with_epsilon(wide.center_x, 60.0, 1e-4);
  g_assert_cmpfloat_with_epsilon(wide.center_y, 20.0, 1e-4);
  g_assert_cmpfloat(wide.radius + wide.line_width / 2.0, <, 20.0);

  /* No area, no geometry: the snapshot function returns before drawing. */
  LambdockMeterGeometry none = lambdock_meter_geometry_compute(0, 48);
  g_assert_cmpfloat(none.radius, ==, 0.0f);
  g_assert_cmpfloat(none.tile.size.width, ==, 0.0f);
}

/* Colour carries the meaning: green while there is room, amber when it gets
 * tight, red when it is nearly full. */
static void test_tone_thresholds(void) {
  GdkRGBA green = lambdock_meter_tone_for_fraction(0.10);
  GdkRGBA green_edge = lambdock_meter_tone_for_fraction(0.59);
  GdkRGBA amber = lambdock_meter_tone_for_fraction(0.60);
  GdkRGBA amber_edge = lambdock_meter_tone_for_fraction(0.84);
  GdkRGBA red = lambdock_meter_tone_for_fraction(0.85);
  GdkRGBA full = lambdock_meter_tone_for_fraction(1.0);

  g_assert_cmpfloat_with_epsilon(green.green, green_edge.green, 1e-6);
  g_assert_true(green_edge.green != amber.green);
  g_assert_cmpfloat_with_epsilon(amber.green, amber_edge.green, 1e-6);
  g_assert_true(amber_edge.green != red.green);
  g_assert_cmpfloat_with_epsilon(red.green, full.green, 1e-6);

  /* Green, then amber (less green, more red), then red dominates. */
  g_assert_cmpfloat(green.green, >, amber.green);
  g_assert_cmpfloat(amber.red, >, green.red);
  g_assert_cmpfloat(red.red, >, red.green);

  /* Out-of-range and NaN readings still get a colour. */
  GdkRGBA below = lambdock_meter_tone_for_fraction(-1.0);
  g_assert_cmpfloat_with_epsilon(below.green, green.green, 1e-6);
  GdkRGBA above = lambdock_meter_tone_for_fraction(2.0);
  g_assert_cmpfloat_with_epsilon(above.red, red.red, 1e-6);
  GdkRGBA nan_tone = lambdock_meter_tone_for_fraction(NAN);
  g_assert_cmpfloat(nan_tone.alpha, ==, 1.0f);
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
  g_test_add_func("/ui/meter/geometry", test_geometry);
  g_test_add_func("/ui/meter/tone_thresholds", test_tone_thresholds);

  return g_test_run();
}
