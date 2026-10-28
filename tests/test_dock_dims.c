// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

/* Unit tests for the pure dock-dimension arithmetic: no widgets, no display. */

#include "../src/core/dock_dims.h"

#include <glib.h>

static LambdockShelfGeometry flat_shelf(void) {
  /* The classic capsule: no glass, no reflection band, no overhang. */
  LambdockShelfGeometry shelf = {
      .band_px = 0,
      .outer_px = CAPSULE_PADDING_PX,
      .capsule_px = DEFAULT_ICON_SIZE + CAPSULE_PADDING_PX,
      .overhang_px = 0,
  };

  return shelf;
}

static void test_defaults(void) {
  LambdockDockDims dims = lambdock_dock_dims_compute(
      0, -1, flat_shelf(), 1920, 1080);

  /* A non-positive icon size means the documented default, and so does a
   * negative padding. */
  g_assert_cmpint(dims.icon_size, ==, DEFAULT_ICON_SIZE);
  g_assert_cmpint(dims.padding_px, ==, CAPSULE_MAIN_AXIS_PADDING_PX);
  g_assert_cmpint(dims.capsule_cross_px, ==, flat_shelf().capsule_px);

  /* The content area keeps its margin from both monitor edges. */
  g_assert_cmpint(dims.max_main_h_px, ==,
                  1920 - LAMBDOCK_MONITOR_EDGE_MARGIN_PX);
  g_assert_cmpint(dims.max_main_v_px, ==,
                  1080 - LAMBDOCK_MONITOR_EDGE_MARGIN_PX);

  /* The front glass pushes the icons off the edge; there is no reflection band
   * to subtract from it, so the whole glass thickness is the margin. */
  g_assert_cmpint(dims.outer_margin_px, ==, CAPSULE_PADDING_PX);
}

static void test_overlay_headroom(void) {
  LambdockShelfGeometry shelf = flat_shelf();
  LambdockDockDims dims = lambdock_dock_dims_compute(64, 12, shelf, 2560, 1440);

  int expected = (int)(64 * CROSS_DIM_SCALE_FACTOR) + CROSS_DIM_PADDING_PX +
                 shelf.outer_px + shelf.overhang_px;
  g_assert_cmpint(dims.overlay_cross_px, ==, expected);
  g_assert_cmpint(dims.padding_px, ==, 12);
}

static void test_monitor_fallbacks(void) {
  /* A headless run (or a monitor that just disappeared) falls back to a sane
   * screen instead of producing negative content sizes. */
  LambdockDockDims dims = lambdock_dock_dims_compute(48, 0, flat_shelf(), 0, 0);
  g_assert_cmpint(dims.max_main_h_px, ==,
                  LAMBDOCK_FALLBACK_MONITOR_WIDTH - LAMBDOCK_MONITOR_EDGE_MARGIN_PX);
  g_assert_cmpint(dims.max_main_v_px, ==,
                  LAMBDOCK_FALLBACK_MONITOR_HEIGHT - LAMBDOCK_MONITOR_EDGE_MARGIN_PX);

  /* A tiny monitor cannot shrink the content area below the usable minimum. */
  dims = lambdock_dock_dims_compute(48, 0, flat_shelf(), 50, 40);
  g_assert_cmpint(dims.max_main_h_px, ==, LAMBDOCK_MIN_CONTENT_PX);
  g_assert_cmpint(dims.max_main_v_px, ==, LAMBDOCK_MIN_CONTENT_PX);
}

static void test_outer_margin_from_reflection(void) {
  /* A reflection band taller than the front glass leaves a positive margin. */
  LambdockShelfGeometry shelf = {
      .band_px = 40,
      .outer_px = 90,
      .capsule_px = 120,
      .overhang_px = 10,
  };

  LambdockDockDims dims = lambdock_dock_dims_compute(48, 8, shelf, 1920, 1080);
  g_assert_cmpint(dims.outer_margin_px, ==, 50);

  /* The other way round (front glass thicker than the band) gives zero, never a
   * negative margin that would push the icons into the capsule. */
  shelf.band_px = 60;
  shelf.outer_px = 20;
  dims = lambdock_dock_dims_compute(48, 8, shelf, 1920, 1080);
  g_assert_cmpint(dims.outer_margin_px, ==, 0);
}

static void test_pure(void) {
  LambdockShelfGeometry shelf = flat_shelf();

  LambdockDockDims first = lambdock_dock_dims_compute(48, 12, shelf, 1920, 1080);
  LambdockDockDims second =
      lambdock_dock_dims_compute(48, 12, shelf, 1920, 1080);

  g_assert_cmpint(first.icon_size, ==, second.icon_size);
  g_assert_cmpint(first.capsule_cross_px, ==, second.capsule_cross_px);
  g_assert_cmpint(first.overlay_cross_px, ==, second.overlay_cross_px);
  g_assert_cmpint(first.max_main_h_px, ==, second.max_main_h_px);
  g_assert_cmpint(first.max_main_v_px, ==, second.max_main_v_px);
  g_assert_cmpint(first.padding_px, ==, second.padding_px);
  g_assert_cmpint(first.outer_margin_px, ==, second.outer_margin_px);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/core/dock_dims/defaults", test_defaults);
  g_test_add_func("/core/dock_dims/overlay_headroom", test_overlay_headroom);
  g_test_add_func("/core/dock_dims/monitor_fallbacks", test_monitor_fallbacks);
  g_test_add_func("/core/dock_dims/outer_margin", test_outer_margin_from_reflection);
  g_test_add_func("/core/dock_dims/pure", test_pure);

  return g_test_run();
}
