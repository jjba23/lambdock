// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "config_values.h"

#include "../ui/launchpad.h"

#include <math.h>

/* Scheme can hand us NaN and infinities (#f multiplied by a number, or a
 * user's own arithmetic), so every sanitiser checks for a finite number first
 * instead of relying on the comparisons, which are all false for NaN. */
static gboolean value_is_finite(double value) { return isfinite(value); }

double lambdock_clamp_double(double value, double low, double high) {
  if (isnan(value))
    return low;
  if (value < low)
    return low;
  if (value > high)
    return high;
  return value;
}

int lambdock_clamp_int(double value, int low, int high) {
  if (isnan(value))
    return low;

  /* Compare before rounding: `round(1e300)` would overflow the int cast. */
  if (value <= (double)low)
    return low;
  if (value >= (double)high)
    return high;

  int rounded = (int)round(value);
  if (rounded < low)
    return low;
  if (rounded > high)
    return high;
  return rounded;
}

double lambdock_sanitize_double(double value, double fallback, double low,
                                double high) {
  if (!isfinite(value))
    return lambdock_clamp_double(fallback, low, high);
  return lambdock_clamp_double(value, low, high);
}

int lambdock_sanitize_int(double value, int fallback, int low, int high) {
  if (!isfinite(value))
    return lambdock_clamp_int((double)fallback, low, high);
  return lambdock_clamp_int(value, low, high);
}

int lambdock_icon_size_sanitize(double raw) {
  if (!value_is_finite(raw) || raw <= 0.0)
    return DEFAULT_ICON_SIZE;

  /* A one-pixel icon is as unusable as a 10k-pixel one; both ends are clamped
   * so the shelf and fisheye maths stay well inside int range. */
  return lambdock_sanitize_int(raw, DEFAULT_ICON_SIZE, 8,
                               LAMBDOCK_ICON_SIZE_MAX);
}

int lambdock_item_spacing_sanitize(double raw) {
  return lambdock_sanitize_int(raw, DOCK_ITEM_SPACING_PX, 0,
                               LAMBDOCK_ITEM_SPACING_MAX_PX);
}

int lambdock_capsule_padding_sanitize(double raw) {
  return lambdock_sanitize_int(raw, CAPSULE_MAIN_AXIS_PADDING_PX, 0,
                               CAPSULE_MAIN_AXIS_PADDING_MAX_PX);
}

int lambdock_launchpad_icon_size_sanitize(double raw) {
  return lambdock_sanitize_int(raw, LAUNCHPAD_ICON_SIZE, LAUNCHPAD_ICON_SIZE_MIN,
                               LAUNCHPAD_ICON_SIZE_MAX);
}

int lambdock_launchpad_columns_sanitize(double raw) {
  /* Zero (and anything below it) means "as many as fit". */
  if (!value_is_finite(raw) || raw <= 0.0)
    return LAUNCHPAD_COLUMNS_AUTO;
  return lambdock_sanitize_int(raw, LAUNCHPAD_COLUMNS_AUTO, LAUNCHPAD_COLUMNS_MIN,
                               LAUNCHPAD_COLUMNS_MAX);
}

LambdockShelfParams lambdock_shelf_params_sanitize(
    const LambdockShelfRawValues *raw, LambdockShelfParams defaults) {
  LambdockShelfRawValues values = {0};

  if (raw)
    values = *raw;
  else {
    values.tilt_deg = defaults.tilt_deg;
    values.perspective = defaults.perspective;
    values.margin_px = (double)defaults.margin_px;
    values.front_px = (double)defaults.front_px;
    values.reflection_ratio = defaults.reflection_ratio;
    values.reflection_intensity = defaults.reflection_intensity;
    values.reflection_blur_ratio = defaults.reflection_blur_ratio;
    values.flare_px = (double)defaults.flare_px;
  }

  LambdockShelfParams params;
  params.tilt_deg = lambdock_sanitize_double(
      values.tilt_deg, defaults.tilt_deg, -SHELF_TILT_MAX_DEG,
      SHELF_TILT_MAX_DEG);
  params.perspective = lambdock_sanitize_double(
      values.perspective, defaults.perspective, SHELF_PERSPECTIVE_MIN,
      SHELF_PERSPECTIVE_MAX);

  /* The margin may be negative: that is how the icons overhang the shelf. */
  params.margin_px = lambdock_sanitize_int(values.margin_px, defaults.margin_px,
                                           -SHELF_EXTRA_MAX_PX,
                                           SHELF_EXTRA_MAX_PX);
  params.front_px = lambdock_sanitize_int(values.front_px, defaults.front_px, 0,
                                          SHELF_EXTRA_MAX_PX);
  params.reflection_ratio =
      lambdock_sanitize_double(values.reflection_ratio,
                               defaults.reflection_ratio, 0.0,
                               SHELF_REFLECTION_RATIO_MAX);
  params.reflection_intensity =
      lambdock_sanitize_double(values.reflection_intensity,
                               defaults.reflection_intensity, 0.0, 1.0);
  params.reflection_blur_ratio =
      lambdock_sanitize_double(values.reflection_blur_ratio,
                               defaults.reflection_blur_ratio, 0.0,
                               SHELF_REFLECTION_BLUR_MAX);

  /* The keystone lean, in pixels per side. */
  params.flare_px = lambdock_sanitize_int(values.flare_px, defaults.flare_px,
                                          -SHELF_FLARE_MAX_PX,
                                          SHELF_FLARE_MAX_PX);

  return params;
}

GtkLayerShellEdge lambdock_edge_from_name(const char *name) {
  if (!name || *name == '\0')
    return GTK_LAYER_SHELL_EDGE_BOTTOM;

  if (g_ascii_strcasecmp(name, "top") == 0)
    return GTK_LAYER_SHELL_EDGE_TOP;
  if (g_ascii_strcasecmp(name, "left") == 0)
    return GTK_LAYER_SHELL_EDGE_LEFT;
  if (g_ascii_strcasecmp(name, "right") == 0)
    return GTK_LAYER_SHELL_EDGE_RIGHT;

  return GTK_LAYER_SHELL_EDGE_BOTTOM;
}

const char *lambdock_edge_name(GtkLayerShellEdge edge) {
  switch (edge) {
  case GTK_LAYER_SHELL_EDGE_TOP:
    return "top";
  case GTK_LAYER_SHELL_EDGE_LEFT:
    return "left";
  case GTK_LAYER_SHELL_EDGE_RIGHT:
    return "right";
  case GTK_LAYER_SHELL_EDGE_BOTTOM:
  default:
    return "bottom";
  }
}

gboolean lambdock_dock_mode_is_taskbar(const char *mode) {
  return mode && g_ascii_strcasecmp(mode, "taskbar") == 0;
}

gboolean lambdock_launchpad_close_on_right(const char *side) {
  return side && g_ascii_strcasecmp(side, "right") == 0;
}
