// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "clock_face.h"

#include <math.h>

/* A hand position is a fraction of a turn, and a fraction that is 1.0 wraps to
 * 0.0: the minute hand at 60 minutes belongs at the top of the face, not a full
 * turn past it. */
static double wrap_turn(double turn) {
  double wrapped = fmod(turn, 1.0);

  if (wrapped < 0.0)
    wrapped += 1.0;

  return wrapped;
}

LambdockClockTurns lambdock_clock_turns_for_seconds(int seconds_of_day) {
  int seconds = seconds_of_day % LAMBDOCK_SECONDS_PER_DAY;
  if (seconds < 0)
    seconds += LAMBDOCK_SECONDS_PER_DAY;

  int hours = seconds / 3600;
  int minutes = (seconds % 3600) / 60;
  int rest = seconds % 60;

  LambdockClockTurns turns = {
      /* The hour hand sits between two hours for most of the day: a quarter of
       * the way past three has to look like it. */
      .hour = wrap_turn(((double)hours + (double)minutes / 60.0 +
                         (double)rest / 3600.0) /
                        12.0),
      .minute = wrap_turn(((double)minutes + (double)rest / 60.0) / 60.0),
      .second = wrap_turn((double)rest / 60.0),
  };

  return turns;
}

void lambdock_squircle_point(double turn, double half_width, double half_height,
                            double squareness, double *out_x, double *out_y) {
  double exponent = (squareness > 2.0) ? squareness : 2.0;
  double angle = wrap_turn(turn) * 2.0 * G_PI;
  double cosine = cos(angle);
  double sine = sin(angle);

  /* |cos|^(2/n) with its sign kept: n = 2 gives the unit circle, and a larger n
   * pushes the curve out towards the corners of the bounding box. The near-zero
   * snap keeps the four vertices exactly on the axes: cos(pi/2) is 6e-17, and
   * raising that to a fractional power leaves a visible sliver at large sizes. */
  double x = 0.0;
  double y = 0.0;
  if (fabs(cosine) > 1e-12)
    x = copysign(pow(fabs(cosine), 2.0 / exponent), cosine);
  if (fabs(sine) > 1e-12)
    y = copysign(pow(fabs(sine), 2.0 / exponent), sine);

  if (out_x)
    *out_x = x * half_width;
  if (out_y)
    *out_y = y * half_height;
}
