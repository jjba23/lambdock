// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAMBDOCK_CORE_SHELF_PARAMS_H
#define LAMBDOCK_CORE_SHELF_PARAMS_H

#include <glib.h>

/*
 * =========================================================================
 * THE GLASS SHELF, AS VALUES
 * =========================================================================
 * The dock has no modes: it is one widget tree whose shape is described by
 * `LambdockShelfParams`, and whose derived measurements are a
 * `LambdockShelfGeometry`.
 *
 * Both live here rather than in `lambdock.h` / `ui/shelf.h` because the pure
 * layer needs them: `core/dock_dims.h` and `core/config_values.h` take and
 * return shelf geometry, and including the whole application header (or a
 * widget header) to name a struct is exactly the layering the tree is supposed
 * to have. The widget API stays in `ui/shelf.h`, which includes this file.
 *
 * Geometry (all values in pixels, `icon` = icon size):
 *
 *   band    = round(icon * reflection_ratio)
 *   outer   = max(front_px, band)          // glass on the screen-edge side
 *   capsule = icon + margin_px + outer
 *
 * With the defaults (tilt 0, reflection 0, front 0, margin = capsule padding)
 * that is exactly the classic `icon + 24px` capsule, the frame is a pass-through
 * and the reflections stay hidden.
 */

/* --- Limits used to keep user supplied parameters sane. --- */
#define SHELF_TILT_MAX_DEG 45.0
#define SHELF_PERSPECTIVE_MIN 50.0
#define SHELF_PERSPECTIVE_MAX 5000.0
#define SHELF_EXTRA_MAX_PX 200
#define SHELF_REFLECTION_RATIO_MAX 2.0
#define SHELF_REFLECTION_BLUR_MAX 0.25
/* How far the keystone (flare) may lean the dock's silhouette, per side. */
#define SHELF_FLARE_MAX_PX 80

/* --- Built-in defaults: the classic flat capsule. --- */
/* The inner glass defaults to the capsule padding the dock has always used
 * (`CAPSULE_PADDING_PX` in lambdock.h, 24px). */
#define SHELF_DEFAULT_TILT 0.0
#define SHELF_DEFAULT_PERSPECTIVE 400.0
#define SHELF_DEFAULT_MARGIN_PX 24
#define SHELF_DEFAULT_FRONT_PX 0
#define SHELF_DEFAULT_REFLECTION 0.0
#define SHELF_DEFAULT_REFLECTION_INTENSITY 0.55
#define SHELF_DEFAULT_REFLECTION_BLUR 0.05
#define SHELF_DEFAULT_FLARE_PX 0

/**
 * LambdockShelfParams:
 * Geometry of the dock's glass shelf.
 *
 * There are no dock modes: the dock always runs the same widgets and these
 * numbers describe its shape. The defaults reproduce the classic flat capsule.
 *
 * @tilt_deg: Shelf tilt away from the viewer, in degrees (0 = flat).
 * @perspective: Projection distance in pixels (smaller = stronger 3D).
 * @margin_px: Glass on the inner side of the icons (away from the screen edge).
 * Negative values let the icons overhang the shelf, like the OS X 3D dock.
 * @front_px: Glass on the outer side of the icons (the front face). The
 * reflection band lives here when it is taller than this.
 * @reflection_ratio: Reflection band height / icon size (0 = no reflections).
 * @reflection_intensity: Alpha of the reflection where it touches the icon.
 * @reflection_blur_ratio: Gaussian blur radius / icon size.
 * @flare_px: How much wider the dock is at the screen edge than at the inner
 * edge, in pixels per side. 0 keeps the slab rectangular; a positive value
 * gives it the OS X dock's keystone silhouette (wider where it meets the
 * screen), which is what makes the Mavericks slab look like a 3D shelf.
 */
typedef struct LambdockShelfParams {
  double tilt_deg;
  double perspective;
  int margin_px;
  int front_px;
  double reflection_ratio;
  double reflection_intensity;
  double reflection_blur_ratio;
  int flare_px;
} LambdockShelfParams;

/**
 * LambdockShelfGeometry:
 * The shelf measured out for one dock and icon size.
 *
 * @band_px: Height of the reflection band (0 when reflections are off).
 * @outer_px: Glass on the outer (screen edge) side of the icons.
 * @capsule_px: Capsule thickness across the dock.
 * @overhang_px: How far the icons stick out on the inner side (>= 0).
 */
typedef struct LambdockShelfGeometry {
  int band_px;
  int outer_px;
  int capsule_px;
  int overhang_px;
} LambdockShelfGeometry;

#endif // LAMBDOCK_CORE_SHELF_PARAMS_H
