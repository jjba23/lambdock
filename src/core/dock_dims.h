// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAMBDOCK_CORE_DOCK_DIMS_H
#define LAMBDOCK_CORE_DOCK_DIMS_H

#include "shelf_params.h"

#include <glib.h>

/*
 * =========================================================================
 * DOCK DIMENSIONS (PURE)
 * =========================================================================
 * `update_dock_dimensions()` used to interleave three jobs: query the monitor,
 * decide how big everything should be, and then push those numbers into a dozen
 * widgets. The decision half is arithmetic on values, so it lives here as a
 * pure function that unit tests can call with no display and no widgets; the
 * widget half stays in dock_window.c and only applies a finished value struct.
 */

/* Fallback monitor size, used when no monitor is available at all. */
#define LAMBDOCK_FALLBACK_MONITOR_WIDTH 1920
#define LAMBDOCK_FALLBACK_MONITOR_HEIGHT 1080

/* The dock never grows past the monitor minus this margin, so a full-width
 * taskbar leaves a sliver of desktop visible on each side. */
#define LAMBDOCK_MONITOR_EDGE_MARGIN_PX 32

/* ... and never shrinks below this, so a tiny or bogus monitor size cannot
 * collapse the content area to nothing. */
#define LAMBDOCK_MIN_CONTENT_PX 100

/**
 * LambdockDockDims:
 * @icon_size: icon size actually used (never <= 0).
 * @capsule_cross_px: cross-axis thickness of the capsule.
 * @overlay_cross_px: cross-axis size requested for the overlay (fisheye
 *     headroom plus the glass and any icon overhang).
 * @hover_headroom_px: main-axis room, per side, that the row can grow into while
 *     the fish-eye magnifies it. Reserved once, so hovering never resizes the
 *     surface.
 * @max_main_h_px: maximum content width.
 * @max_main_v_px: maximum content height.
 * @padding_px: main-axis padding between the capsule's ends and the icons.
 * @outer_margin_px: main-axis margin that keeps the icons off the screen edge
 *     (never negative).
 *
 * Everything the dock's layout needs to know, as a value.
 */
typedef struct LambdockDockDims {
  int icon_size;
  int capsule_cross_px;
  int overlay_cross_px;
  int hover_headroom_px;
  int max_main_h_px;
  int max_main_v_px;
  int padding_px;
  int outer_margin_px;
} LambdockDockDims;

/**
 * lambdock_dock_dims_compute:
 * @icon_size: requested icon size (<= 0 means DEFAULT_ICON_SIZE).
 * @capsule_padding: configured main-axis padding (negative means the default).
 * @shelf: derived shelf geometry for this dock.
 * @monitor_width: monitor width in pixels (<= 0 means the fallback size).
 * @monitor_height: monitor height in pixels (<= 0 means the fallback size).
 * @hover_magnify: TRUE when the fish-eye magnifies the icons under the pointer,
 *     which is what decides whether the dock reserves room to spread into.
 *
 * Returns: the dimensions for this dock. Pure: same inputs, same output.
 */
LambdockDockDims lambdock_dock_dims_compute(int icon_size, int capsule_padding,
                                            LambdockShelfGeometry shelf,
                                            int monitor_width, int monitor_height,
                                            gboolean hover_magnify);

#endif // LAMBDOCK_CORE_DOCK_DIMS_H
