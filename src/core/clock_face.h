// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAMBDOCK_CORE_CLOCK_FACE_H
#define LAMBDOCK_CORE_CLOCK_FACE_H

/*
 * =========================================================================
 * CLOCK FACE GEOMETRY (PURE)
 * =========================================================================
 * Where the hands point, and where a squircle runs, as plain numbers. The
 * widget in src/ui/clock.c paints what these return, which keeps the part that
 * can be wrong in a way nobody notices -- an hour hand on the wrong minute, a
 * "squircle" that is really an ellipse -- testable without a display.
 */

#include <glib.h>

/* Seconds in a day: the unit the hands are derived from. */
#define LAMBDOCK_SECONDS_PER_DAY 86400

/*
 * LambdockClockTurns:
 * @hour: hour hand, in turns clockwise from 12 o'clock (0.25 = 3 o'clock).
 * @minute: minute hand, same unit.
 * @second: second hand, same unit.
 *
 * Hand positions for one reading. Every value is in [0, 1).
 */
typedef struct LambdockClockTurns {
  double hour;
  double minute;
  double second;
} LambdockClockTurns;

/**
 * lambdock_clock_turns_for_seconds:
 * @seconds_of_day: seconds since local midnight (wrapped into a day when out of
 * range, so a caller cannot produce an impossible face).
 *
 * Returns: where the three hands point at that moment. The hour hand creeps
 * forward with the minutes and the minute hand with the seconds, which is what
 * makes the face read as a clock instead of three independent dials.
 */
LambdockClockTurns lambdock_clock_turns_for_seconds(int seconds_of_day);

/**
 * lambdock_squircle_point:
 * @turn: position around the shape, in turns clockwise from +x (0.25 = +y).
 * @half_width: half-size along x, in pixels.
 * @half_height: half-size along y, in pixels.
 * @squareness: superellipse exponent (2 = ellipse, 4 = squircle, higher =
 * squarer). Values below 2 are clamped to 2.
 * @out_x: (out): x coordinate, measured from the centre.
 * @out_y: (out): y coordinate, measured from the centre.
 *
 * Samples the superellipse |x/a|^n + |y/b|^n = 1. The parametrisation walks the
 * curve monotonically, so a loop over 0..1 turns builds a closed path with no
 * crossings -- which is what the bezel, the face and the glass are drawn from.
 */
void lambdock_squircle_point(double turn, double half_width, double half_height,
                            double squareness, double *out_x, double *out_y);

#endif // LAMBDOCK_CORE_CLOCK_FACE_H
