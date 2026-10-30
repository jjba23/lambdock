// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "dock_dims.h"
#include "../lambdock.h"

#include <math.h>

/* A monitor size of zero (headless run, monitor just unplugged) would produce a
 * negative content area, so it falls back to a sane screen instead. */
static int monitor_dim_or_fallback(int value, int fallback) {
  return value > 0 ? value : fallback;
}

/* The dock keeps a margin from the monitor's edge, but never shrinks below a
 * usable content size. */
static int max_content_dim(int monitor_dim, int margin) {
  int dim = monitor_dim - margin;

  return dim < LAMBDOCK_MIN_CONTENT_PX ? LAMBDOCK_MIN_CONTENT_PX : dim;
}

/**
 * hover_headroom_px:
 * How much room, per side, the row can grow into while the fish-eye magnifies it.
 *
 * The fish-eye grows every icon within FISHEYE_RADIUS_PX of the pointer by up to
 * FISHEYE_MAX_SCALE, and no more than (2 * radius / icon + 1) icons fit in that
 * reach, so the row can never get wider than this. Reserving it once is what
 * keeps the surface from being resized on every pointer frame -- a resize per
 * frame is what made the dock flicker and tear at its edges while hovering.
 */
static int hover_headroom_px(int icon, gboolean hover_magnify) {
  if (!hover_magnify)
    return 0;

  double reach = 2.0 * FISHEYE_RADIUS_PX / (double)icon + 1.0;
  double growth = (FISHEYE_MAX_SCALE - 1.0) * (double)icon * reach;

  /* Half of the total growth per side: the row stays centred as it spreads. */
  return (int)ceil(growth / 2.0);
}

LambdockDockDims lambdock_dock_dims_compute(int icon_size, int capsule_padding,
                                            LambdockShelfGeometry shelf,
                                            int monitor_width, int monitor_height,
                                            gboolean hover_magnify) {
  int icon = icon_size > 0 ? icon_size : DEFAULT_ICON_SIZE;

  int width =
      monitor_dim_or_fallback(monitor_width, LAMBDOCK_FALLBACK_MONITOR_WIDTH);
  int height =
      monitor_dim_or_fallback(monitor_height, LAMBDOCK_FALLBACK_MONITOR_HEIGHT);

  LambdockDockDims dims = {
      .icon_size = icon,
      .capsule_cross_px = shelf.capsule_px,
      /* Fisheye magnification headroom, plus the glass, plus any overhang. */
      .overlay_cross_px = (int)(icon * CROSS_DIM_SCALE_FACTOR) +
                          CROSS_DIM_PADDING_PX + shelf.outer_px +
                          shelf.overhang_px,
      .hover_headroom_px = hover_headroom_px(icon, hover_magnify),
      .max_main_h_px = max_content_dim(width, LAMBDOCK_MONITOR_EDGE_MARGIN_PX),
      .max_main_v_px = max_content_dim(height, LAMBDOCK_MONITOR_EDGE_MARGIN_PX),
      .padding_px = capsule_padding >= 0 ? capsule_padding
                                         : CAPSULE_MAIN_AXIS_PADDING_PX,
      .outer_margin_px = shelf.outer_px - shelf.band_px > 0
                             ? shelf.outer_px - shelf.band_px
                             : 0,
  };

  return dims;
}
