// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "dock_motion.h"

#include <math.h>

/*
 * =========================================================================
 * DOCK MOTION (PURE)
 * =========================================================================
 * Every function here is a value -> value mapping: no GTK, no GLib main loop,
 * no globals. `G_PI` rather than `M_PI` keeps the file compiling under a strict
 * `-std=c11` where the platform's math.h does not expose the BSD constant.
 */

double lambdock_ease_out_cubic(double t) {
  double remaining = 1.0 - t;
  return 1.0 - (remaining * remaining * remaining);
}

double lambdock_slide_progress(gint64 start_us, gint64 now_us,
                              gint64 duration_us) {
  if (duration_us <= 0)
    return 1.0;

  double progress = (double)(now_us - start_us) / (double)duration_us;

  if (progress < 0.0)
    return 0.0;

  return (progress > 1.0) ? 1.0 : progress;
}

int lambdock_slide_position(int from_px, int to_px, double progress) {
  double eased = lambdock_ease_out_cubic(progress);
  return from_px + (int)((double)(to_px - from_px) * eased);
}

int lambdock_hide_margin(int measured_main_px, int cross_axis_px,
                         int tolerance_px, int trigger_strip_px) {
  int main_px = measured_main_px;

  if (main_px <= 0 || main_px > cross_axis_px + tolerance_px)
    main_px = cross_axis_px;

  return -(main_px - trigger_strip_px);
}

gboolean lambdock_settling(gboolean sliding, gint64 settled_at_us,
                           gint64 now_us, guint settle_ms) {
  if (sliding)
    return TRUE;

  if (settled_at_us == 0)
    return FALSE;

  return (now_us - settled_at_us) < (gint64)settle_ms * 1000;
}

gboolean lambdock_dock_needs_full_input(gboolean is_hidden,
                                        int current_margin) {
  return is_hidden || current_margin != 0;
}

double lambdock_fisheye_scale(double distance_px, double radius_px,
                              double max_scale) {
  if (radius_px <= 0.0)
    return 1.0;

  double distance = fabs(distance_px);
  if (distance >= radius_px)
    return 1.0;

  /* Cosine bell: full magnification at the centre, easing to none at the edge
   * of the influence, so neighbouring icons grow smoothly together. */
  double factor = 0.5 * (1.0 + cos(G_PI * distance / radius_px));
  return 1.0 + (max_scale - 1.0) * factor;
}

int lambdock_scaled_size(int base_px, double scale) {
  return (int)lround((double)base_px * scale);
}

gboolean lambdock_points_differ(double ax, double ay, double bx, double by,
                                double epsilon_px) {
  return fabs(ax - bx) >= epsilon_px || fabs(ay - by) >= epsilon_px;
}

int lambdock_size_step(int size_px, int step_px) {
  if (step_px <= 1)
    return size_px;

  return ((size_px + step_px - 1) / step_px) * step_px;
}
