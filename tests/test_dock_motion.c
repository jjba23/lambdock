// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/core/dock_motion.h"

#include <glib.h>
#include <math.h>

/* These tests are the reason the motion arithmetic was pulled out of
 * dock_window.c: they exercise it with plain numbers, no display and no
 * widgets, which the GTK-bound code could not do. */

/* ============================================================================
 * SLIDE ANIMATION
 * ============================================================================ */

static void test_slide_progress(void) {
  g_assert_cmpfloat(lambdock_slide_progress(0, 0, 1000), ==, 0.0);
  g_assert_cmpfloat(lambdock_slide_progress(0, 250, 1000), ==, 0.25);
  g_assert_cmpfloat(lambdock_slide_progress(0, 1000, 1000), ==, 1.0);

  /* Frame times past the end, and clocks that jump backwards, are clamped
   * instead of driving the dock off screen. */
  g_assert_cmpfloat(lambdock_slide_progress(0, 5000, 1000), ==, 1.0);
  g_assert_cmpfloat(lambdock_slide_progress(500, 0, 1000), ==, 0.0);

  /* A zero (or negative) duration is a slide that is already over. */
  g_assert_cmpfloat(lambdock_slide_progress(0, 0, 0), ==, 1.0);
  g_assert_cmpfloat(lambdock_slide_progress(0, 0, -1000), ==, 1.0);
}

static void test_ease_out_cubic(void) {
  g_assert_cmpfloat(lambdock_ease_out_cubic(0.0), ==, 0.0);
  g_assert_cmpfloat(lambdock_ease_out_cubic(1.0), ==, 1.0);
  g_assert_cmpfloat(lambdock_ease_out_cubic(0.5), ==, 0.875);

  /* Decelerating: more than half the distance is covered in the first half of
   * the time, and the curve is monotonic across it. */
  g_assert_cmpfloat(lambdock_ease_out_cubic(0.25), >, 0.25);
  g_assert_cmpfloat(lambdock_ease_out_cubic(0.25), <,
                    lambdock_ease_out_cubic(0.5));
  g_assert_cmpfloat(lambdock_ease_out_cubic(0.5), <,
                    lambdock_ease_out_cubic(0.75));
  g_assert_cmpfloat(lambdock_ease_out_cubic(0.75), <, 1.0);
}

static void test_slide_position(void) {
  /* Reveal: from a hidden margin back to 0. */
  g_assert_cmpint(lambdock_slide_position(-800, 0, 0.0), ==, -800);
  g_assert_cmpint(lambdock_slide_position(-800, 0, 1.0), ==, 0);
  g_assert_cmpint(lambdock_slide_position(-800, 0, 0.5), ==, -100);

  /* Hide: from 0 to a negative margin, eased the same way. */
  g_assert_cmpint(lambdock_slide_position(0, -100, 0.5), ==, -87);

  /* A no-op slide stays put at every frame. */
  g_assert_cmpint(lambdock_slide_position(42, 42, 0.0), ==, 42);
  g_assert_cmpint(lambdock_slide_position(42, 42, 0.5), ==, 42);
  g_assert_cmpint(lambdock_slide_position(42, 42, 1.0), ==, 42);
}

/* ============================================================================
 * AUTO-HIDE OFF-SCREEN MARGIN
 * ============================================================================ */

static void test_hide_margin(void) {
  /* A laid-out dock is measured directly: leave the trigger strip on screen. */
  g_assert_cmpint(lambdock_hide_margin(800, 800, 20, 2), ==, -798);

  /* Before the first layout the measurement is 0, so the cross-axis request is
   * used instead. */
  g_assert_cmpint(lambdock_hide_margin(0, 800, 20, 2), ==, -798);

  /* A stale measurement from the other orientation is longer than the dock can
   * possibly be, and is discarded. */
  g_assert_cmpint(lambdock_hide_margin(1200, 800, 20, 2), ==, -798);

  /* Just past the request but within tolerance is still a real measurement. */
  g_assert_cmpint(lambdock_hide_margin(815, 800, 20, 2), ==, -813);

  /* A short, positive measurement (< the request) is believed. */
  g_assert_cmpint(lambdock_hide_margin(500, 800, 20, 2), ==, -498);

  /* A negative measurement is as useless as 0. */
  g_assert_cmpint(lambdock_hide_margin(-5, 800, 20, 2), ==, -798);
}

/* The strip is the *whole* of a tucked-away dock: it is the only thing the
 * pointer can reach, so the margin has to leave exactly that many pixels on
 * screen. Anything less and the dock cannot be brought back by a pointer at
 * all -- which is not a slow reveal, it is a dock that has gone for good. */
static void test_hide_margin_leaves_the_configured_strip(void) {
  const int surface_px[] = {44, 72, 148, 220};

  for (guint i = 0; i < G_N_ELEMENTS(surface_px); i++) {
    int margin = lambdock_hide_margin(surface_px[i], surface_px[i], 20,
                                      AUTOHIDE_TRIGGER_STRIP_PX);

    g_assert_cmpint(surface_px[i] + margin, ==, AUTOHIDE_TRIGGER_STRIP_PX);
  }

  /* ...and the strip has to be hittable: a couple of pixels at a fractional
   * scale is a target a pointer can sit next to forever without ever landing
   * on, and the user reads that as "it never comes back". */
  g_assert_cmpint(AUTOHIDE_TRIGGER_STRIP_PX, >=, 4);
}

/* ============================================================================
 * SETTLING WINDOW
 * ============================================================================ */

static void test_settling(void) {
  /* A slide in flight is always settling. */
  g_assert_true(lambdock_settling(TRUE, 0, 1000000, 300));

  /* A dock that has never slid has no echo window. */
  g_assert_false(lambdock_settling(FALSE, 0, 1000000, 300));

  /* Just stopped: crossings are still echoes of the slide. */
  g_assert_true(lambdock_settling(FALSE, 1000000, 1000000, 300));
  g_assert_true(lambdock_settling(FALSE, 1000000, 1299000, 300));

  /* The window is exactly 300ms wide: once it is over, a leave is the user. */
  g_assert_false(lambdock_settling(FALSE, 1000000, 1300000, 300));
  g_assert_false(lambdock_settling(FALSE, 1000000, 1400000, 300));
}

/* ============================================================================
 * FISHEYE MAGNIFICATION
 * ============================================================================ */

static void test_fisheye_scale(void) {
  const double radius = 140.0;
  const double max_scale = 1.45;

  /* Full magnification under the pointer... */
  g_assert_cmpfloat_with_epsilon(lambdock_fisheye_scale(0.0, radius, max_scale),
                                 max_scale, 1e-9);

  /* ... none at or past the edge of the influence ... */
  g_assert_cmpfloat_with_epsilon(
      lambdock_fisheye_scale(radius, radius, max_scale), 1.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(
      lambdock_fisheye_scale(radius * 4, radius, max_scale), 1.0, 1e-9);

  /* ... and half-way up the bell, every time. */
  g_assert_cmpfloat_with_epsilon(
      lambdock_fisheye_scale(radius / 2.0, radius, max_scale),
      1.0 + (max_scale - 1.0) / 2.0, 1e-9);

  /* Distance is a magnitude: which side of the icon the pointer is on does not
   * matter. */
  g_assert_cmpfloat_with_epsilon(lambdock_fisheye_scale(-70.0, radius,
                                                        max_scale),
                                 lambdock_fisheye_scale(70.0, radius,
                                                        max_scale),
                                 1e-9);

  /* A degenerate radius cannot magnify anything. */
  g_assert_cmpfloat(lambdock_fisheye_scale(10.0, 0.0, max_scale), ==, 1.0);
  g_assert_cmpfloat(lambdock_fisheye_scale(10.0, -140.0, max_scale), ==, 1.0);

  /* Monotonic: closer always means larger. */
  g_assert_cmpfloat(lambdock_fisheye_scale(20.0, radius, max_scale), >,
                    lambdock_fisheye_scale(60.0, radius, max_scale));
  g_assert_cmpfloat(lambdock_fisheye_scale(60.0, radius, max_scale), >,
                    lambdock_fisheye_scale(120.0, radius, max_scale));
}

static void test_scaled_size(void) {
  g_assert_cmpint(lambdock_scaled_size(48, 1.0), ==, 48);

  /* 48 * 1.45 = 69.6, which the layout has to round to whole pixels. */
  g_assert_cmpint(lambdock_scaled_size(48, 1.45), ==, 70);
  g_assert_cmpint(lambdock_scaled_size(48, 1.1), ==, 53); /* 52.8 */
  g_assert_cmpint(lambdock_scaled_size(0, 1.45), ==, 0);
}

static void test_points_differ(void) {
  /* A repeated position is not movement. */
  g_assert_false(lambdock_points_differ(10.0, 10.0, 10.0, 10.0, 1.0));

  /* Sub-pixel jitter neither. */
  g_assert_false(lambdock_points_differ(10.0, 10.0, 10.5, 10.5, 1.0));
  g_assert_false(lambdock_points_differ(10.0, 10.0, 9.5, 9.5, 1.0));

  /* A whole pixel along either axis is movement. */
  g_assert_true(lambdock_points_differ(10.0, 10.0, 11.0, 10.0, 1.0));
  g_assert_true(lambdock_points_differ(10.0, 10.0, 10.0, 11.0, 1.0));
  g_assert_true(lambdock_points_differ(10.0, 10.0, 8.0, 10.0, 1.0));
}

static void test_size_step(void) {
  /* A step of 1 (or nonsense) means "no quantising". */
  g_assert_cmpint(lambdock_size_step(53, 1), ==, 53);
  g_assert_cmpint(lambdock_size_step(53, 0), ==, 53);
  g_assert_cmpint(lambdock_size_step(53, -4), ==, 53);

  /* Exact multiples stay put... */
  g_assert_cmpint(lambdock_size_step(48, 2), ==, 48);
  g_assert_cmpint(lambdock_size_step(60, 4), ==, 60);

  /* ...and everything else rounds up to the next multiple. */
  g_assert_cmpint(lambdock_size_step(49, 2), ==, 50);
  g_assert_cmpint(lambdock_size_step(53, 2), ==, 54);
  g_assert_cmpint(lambdock_size_step(61, 4), ==, 64);
  g_assert_cmpint(lambdock_size_step(1, 2), ==, 2);
}

/* The layout cost of a hover sweep is the number of *distinct* sizes the row is
 * asked to re-measure for. Quantising halves it, while the painted scale keeps
 * changing every pixel (that is what `lambdock_fisheye_scale` is for). */
static void test_size_step_halves_layout_passes(void) {
  const int base = 48;
  const double radius = 140.0;

  int exact = 0;
  int stepped = 0;
  int previous_exact = -1;
  int previous_stepped = -1;
  int changed_scale = 0;
  double previous_scale = -1.0;

  for (int offset = 0; offset <= (int)radius; offset += 2) {
    double scale = lambdock_fisheye_scale((double)offset, radius, 1.45);
    int size = lambdock_scaled_size(base, scale);

    if (size != previous_exact) {
      exact++;
      previous_exact = size;
    }

    int quantised = lambdock_size_step(size, 2);
    if (quantised != previous_stepped) {
      stepped++;
      previous_stepped = quantised;
    }

    if (fabs(scale - previous_scale) > 0.0) {
      changed_scale++;
      previous_scale = scale;
    }
  }

  g_test_message("sweep: %d sizes exact, %d stepper, %d painted scales", exact,
                 stepped, changed_scale);

  g_assert_cmpint(stepped, <, exact);
  /* Every pointer position still gets its own magnification. */
  g_assert_cmpint(changed_scale, ==, (int)(radius / 2) + 1);
}

static void test_needs_full_input(void) {
  /* Parked off screen: the only part of the surface on screen is the trigger
   * strip, which lives in the headroom a dock-shaped input region cuts away. It
   * has to take input, or auto-hide could never be triggered again. */
  g_assert_true(lambdock_dock_needs_full_input(TRUE, 0));
  g_assert_true(lambdock_dock_needs_full_input(TRUE, -120));

  /* Sliding: same reasoning, and the strip moves every frame. */
  g_assert_true(lambdock_dock_needs_full_input(FALSE, -60));

  /* Fully out and at rest: the dock itself is all the surface that matters. */
  g_assert_false(lambdock_dock_needs_full_input(FALSE, 0));
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/core/dock_motion/slide_progress", test_slide_progress);
  g_test_add_func("/core/dock_motion/ease_out_cubic", test_ease_out_cubic);
  g_test_add_func("/core/dock_motion/slide_position", test_slide_position);
  g_test_add_func("/core/dock_motion/hide_margin", test_hide_margin);
  g_test_add_func("/core/dock_motion/hide_margin_leaves_the_configured_strip",
                  test_hide_margin_leaves_the_configured_strip);
  g_test_add_func("/core/dock_motion/settling", test_settling);
  g_test_add_func("/core/dock_motion/needs_full_input",
                  test_needs_full_input);
  g_test_add_func("/core/dock_motion/fisheye_scale", test_fisheye_scale);
  g_test_add_func("/core/dock_motion/scaled_size", test_scaled_size);
  g_test_add_func("/core/dock_motion/points_differ", test_points_differ);
  g_test_add_func("/core/dock_motion/size_step", test_size_step);
  g_test_add_func("/core/dock_motion/size_step_halves_layout_passes",
                  test_size_step_halves_layout_passes);

  return g_test_run();
}
