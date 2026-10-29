// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/core/clock_face.h"
#include <glib.h>

/* ============================================================================
 * WHERE THE HANDS POINT
 * ============================================================================
 */

static void test_midnight(void) {
  LambdockClockTurns turns = lambdock_clock_turns_for_seconds(0);

  g_assert_cmpfloat(turns.hour, ==, 0.0);
  g_assert_cmpfloat(turns.minute, ==, 0.0);
  g_assert_cmpfloat(turns.second, ==, 0.0);
}

static void test_quarter_past_three(void) {
  /* 03:00: the hour hand is exactly a quarter turn from the top. */
  LambdockClockTurns turns = lambdock_clock_turns_for_seconds(3 * 3600);

  g_assert_cmpfloat_with_epsilon(turns.hour, 0.25, 1e-9);
  g_assert_cmpfloat_with_epsilon(turns.minute, 0.0, 1e-9);
}

static void test_half_past_twelve_wraps_to_just_past_six(void) {
  /* 12:30 is half past the hour, and the hour hand is halfway between 12 and 1,
   * not a full turn plus half: 12:00 has to read as "up", not "up and once
   * around". */
  LambdockClockTurns noon = lambdock_clock_turns_for_seconds(12 * 3600);
  g_assert_cmpfloat_with_epsilon(noon.hour, 0.0, 1e-9);

  LambdockClockTurns turns = lambdock_clock_turns_for_seconds(12 * 3600 + 1800);
  g_assert_cmpfloat_with_epsilon(turns.hour, 0.5 / 12.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(turns.minute, 0.5, 1e-9);
}

static void test_hands_creep(void) {
  /* 10:10:30. The hour hand sits past the 10, the minute hand a little past the
   * 2 and the second hand halfway round: three independent readings that all
   * come from the same moment. */
  int seconds = 10 * 3600 + 10 * 60 + 30;
  LambdockClockTurns turns = lambdock_clock_turns_for_seconds(seconds);

  g_assert_cmpfloat_with_epsilon(
      turns.hour, (10.0 + 10.0 / 60.0 + 30.0 / 3600.0) / 12.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(turns.minute, (10.0 + 30.0 / 60.0) / 60.0,
                                 1e-9);
  g_assert_cmpfloat_with_epsilon(turns.second, 0.5, 1e-9);
}

static void test_last_second_of_the_day(void) {
  LambdockClockTurns turns =
      lambdock_clock_turns_for_seconds(LAMBDOCK_SECONDS_PER_DAY - 1);

  /* Every hand is just short of a full turn -- never 1.0, which the drawing code
   * would have to special-case. */
  g_assert_cmpfloat(turns.hour, <, 1.0);
  g_assert_cmpfloat(turns.hour, >, 0.999);
  g_assert_cmpfloat(turns.minute, <, 1.0);
  g_assert_cmpfloat(turns.minute, >, 0.999);
  g_assert_cmpfloat_with_epsilon(turns.second, 59.0 / 60.0, 1e-9);
}

static void test_out_of_range_seconds_wrap(void) {
  /* A poll that reads the clock across midnight can hand over a day's worth of
   * seconds; a negative reading (a timezone change, a caller's arithmetic) must
   * still produce a possible face. */
  LambdockClockTurns wrapped =
      lambdock_clock_turns_for_seconds(LAMBDOCK_SECONDS_PER_DAY);
  g_assert_cmpfloat(wrapped.hour, ==, 0.0);

  LambdockClockTurns yesterday =
      lambdock_clock_turns_for_seconds(-3600); /* 23:00 the day before */
  g_assert_cmpfloat_with_epsilon(yesterday.hour, 11.0 / 12.0, 1e-9);
}

/* ============================================================================
 * THE SQUIRCLE
 * ============================================================================
 */

static void test_squircle_passes_through_the_axes(void) {
  double x = 0.0;
  double y = 0.0;

  lambdock_squircle_point(0.0, 10.0, 8.0, 4.0, &x, &y);
  g_assert_cmpfloat_with_epsilon(x, 10.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(y, 0.0, 1e-9);

  lambdock_squircle_point(0.25, 10.0, 8.0, 4.0, &x, &y);
  g_assert_cmpfloat_with_epsilon(x, 0.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(y, 8.0, 1e-9);

  lambdock_squircle_point(0.5, 10.0, 8.0, 4.0, &x, &y);
  g_assert_cmpfloat_with_epsilon(x, -10.0, 1e-9);

  lambdock_squircle_point(0.75, 10.0, 8.0, 4.0, &x, &y);
  g_assert_cmpfloat_with_epsilon(y, -8.0, 1e-9);
}

static void test_squareness_two_is_a_circle(void) {
  double x = 0.0;
  double y = 0.0;

  lambdock_squircle_point(0.125, 10.0, 10.0, 2.0, &x, &y);

  double expected = 10.0 * G_SQRT2 / 2.0;
  g_assert_cmpfloat_with_epsilon(x, expected, 1e-9);
  g_assert_cmpfloat_with_epsilon(y, expected, 1e-9);
}

static void test_higher_squareness_rounds_further_out(void) {
  double circle_x = 0.0;
  double circle_y = 0.0;
  double square_x = 0.0;
  double square_y = 0.0;

  lambdock_squircle_point(0.125, 10.0, 10.0, 2.0, &circle_x, &circle_y);
  lambdock_squircle_point(0.125, 10.0, 10.0, 4.0, &square_x, &square_y);

  /* This is the whole point of the shape: the curve stays nearer the corner of
   * its box instead of bowing in like an ellipse. */
  g_assert_cmpfloat(square_x, >, circle_x);
  g_assert_cmpfloat(square_y, >, circle_y);
  g_assert_cmpfloat(square_x, <, 10.0);
}

static void test_squareness_below_two_is_clamped(void) {
  double clamped_x = 0.0;
  double circle_x = 0.0;
  double ignored_y = 0.0;

  /* A value below 2 would make the "squircle" star-shaped; it reads as a circle
   * instead of turning the bezel inside out. */
  lambdock_squircle_point(0.125, 10.0, 10.0, 0.5, &clamped_x, &ignored_y);
  lambdock_squircle_point(0.125, 10.0, 10.0, 2.0, &circle_x, &ignored_y);

  g_assert_cmpfloat_with_epsilon(clamped_x, circle_x, 1e-9);
}

static void test_squircle_wraps_and_ignores_null_outputs(void) {
  double x = 0.0;
  double y = 0.0;
  double wrapped_x = 0.0;
  double wrapped_y = 0.0;

  lambdock_squircle_point(0.0, 10.0, 8.0, 4.0, &x, &y);
  lambdock_squircle_point(1.0, 10.0, 8.0, 4.0, &wrapped_x, &wrapped_y);
  g_assert_cmpfloat_with_epsilon(wrapped_x, x, 1e-9);
  g_assert_cmpfloat_with_epsilon(wrapped_y, y, 1e-9);

  /* The caller may want only one coordinate. */
  lambdock_squircle_point(0.25, 10.0, 8.0, 4.0, NULL, &y);
  g_assert_cmpfloat_with_epsilon(y, 8.0, 1e-9);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/clock/hands/midnight", test_midnight);
  g_test_add_func("/clock/hands/quarter-past-three",
                  test_quarter_past_three);
  g_test_add_func("/clock/hands/half-past-twelve",
                  test_half_past_twelve_wraps_to_just_past_six);
  g_test_add_func("/clock/hands/creep", test_hands_creep);
  g_test_add_func("/clock/hands/last-second",
                  test_last_second_of_the_day);
  g_test_add_func("/clock/hands/wrap", test_out_of_range_seconds_wrap);
  g_test_add_func("/clock/squircle/axes", test_squircle_passes_through_the_axes);
  g_test_add_func("/clock/squircle/circle", test_squareness_two_is_a_circle);
  g_test_add_func("/clock/squircle/square",
                  test_higher_squareness_rounds_further_out);
  g_test_add_func("/clock/squircle/clamp",
                  test_squareness_below_two_is_clamped);
  g_test_add_func("/clock/squircle/wrap", test_squircle_wraps_and_ignores_null_outputs);

  return g_test_run();
}
