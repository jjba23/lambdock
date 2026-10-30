// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

/* Unit tests for the pure configuration-value rules. Nothing here needs a
 * display server or a Guile interpreter: that is the point of the module. */

#include "../src/core/config_values.h"
#include "../src/ui/launchpad.h"
#include "../src/ui/shelf.h"

#include <math.h>

static void test_clamp(void) {
  g_assert_cmpfloat(lambdock_clamp_double(5.0, 0.0, 10.0), ==, 5.0);
  g_assert_cmpfloat(lambdock_clamp_double(-5.0, 0.0, 10.0), ==, 0.0);
  g_assert_cmpfloat(lambdock_clamp_double(50.0, 0.0, 10.0), ==, 10.0);

  /* NaN and the infinities cannot satisfy any comparison, so they clamp to the
   * lower bound instead of leaking through. */
  g_assert_cmpfloat(lambdock_clamp_double(NAN, 1.0, 10.0), ==, 1.0);
  g_assert_cmpfloat(lambdock_clamp_double(INFINITY, 1.0, 10.0), ==, 10.0);
  g_assert_cmpfloat(lambdock_clamp_double(-INFINITY, 1.0, 10.0), ==, 1.0);

  g_assert_cmpint(lambdock_clamp_int(5.4, 0, 10), ==, 5);
  g_assert_cmpint(lambdock_clamp_int(5.5, 0, 10), ==, 6);
  g_assert_cmpint(lambdock_clamp_int(-3.0, 0, 10), ==, 0);
  g_assert_cmpint(lambdock_clamp_int(99.0, 0, 10), ==, 10);
  g_assert_cmpint(lambdock_sanitize_int(NAN, 7, 0, 10), ==, 7);

  /* A huge double must not overflow the int conversion. */
  g_assert_cmpint(lambdock_clamp_int(1e300, 0, 10), ==, 10);
  g_assert_cmpint(lambdock_clamp_int(-1e300, 0, 10), ==, 0);
}

static void test_sanitize(void) {
  g_assert_cmpfloat(lambdock_sanitize_double(NAN, 3.0, 0.0, 10.0), ==, 3.0);
  g_assert_cmpfloat(lambdock_sanitize_double(42.0, 3.0, 0.0, 10.0), ==, 10.0);
  g_assert_cmpint(lambdock_sanitize_int(INFINITY, 4, 0, 10), ==, 4);
  g_assert_cmpint(lambdock_sanitize_int(2.6, 4, 0, 10), ==, 3);
}

static void test_icon_size(void) {
  g_assert_cmpint(lambdock_icon_size_sanitize(64), ==, 64);
  g_assert_cmpint(lambdock_icon_size_sanitize(48.4), ==, 48);

  /* Zero, negatives and garbage fall back to the documented default. */
  g_assert_cmpint(lambdock_icon_size_sanitize(0), ==, DEFAULT_ICON_SIZE);
  g_assert_cmpint(lambdock_icon_size_sanitize(-12), ==, DEFAULT_ICON_SIZE);
  g_assert_cmpint(lambdock_icon_size_sanitize(NAN), ==, DEFAULT_ICON_SIZE);
  g_assert_cmpint(lambdock_icon_size_sanitize(INFINITY), ==, DEFAULT_ICON_SIZE);

  /* A one-pixel or a 10k-pixel icon is clamped, never passed through. */
  g_assert_cmpint(lambdock_icon_size_sanitize(1), ==, 8);
  g_assert_cmpint(lambdock_icon_size_sanitize(10000), ==,
                  LAMBDOCK_ICON_SIZE_MAX);
}

static void test_spacing_and_padding(void) {
  g_assert_cmpint(lambdock_item_spacing_sanitize(12), ==, 12);
  g_assert_cmpint(lambdock_item_spacing_sanitize(-5), ==, 0);
  g_assert_cmpint(lambdock_item_spacing_sanitize(1000), ==,
                  LAMBDOCK_ITEM_SPACING_MAX_PX);
  g_assert_cmpint(lambdock_item_spacing_sanitize(NAN), ==,
                  DOCK_ITEM_SPACING_PX);

  g_assert_cmpint(lambdock_capsule_padding_sanitize(24), ==, 24);
  g_assert_cmpint(lambdock_capsule_padding_sanitize(-1), ==, 0);
  g_assert_cmpint(lambdock_capsule_padding_sanitize(1000), ==,
                  CAPSULE_MAIN_AXIS_PADDING_MAX_PX);
  g_assert_cmpint(lambdock_capsule_padding_sanitize(INFINITY), ==,
                  CAPSULE_MAIN_AXIS_PADDING_PX);
}

static void test_launchpad_values(void) {
  g_assert_cmpint(lambdock_launchpad_icon_size_sanitize(80), ==, 80);
  g_assert_cmpint(lambdock_launchpad_icon_size_sanitize(1), ==,
                  LAUNCHPAD_ICON_SIZE_MIN);
  g_assert_cmpint(lambdock_launchpad_icon_size_sanitize(4000), ==,
                  LAUNCHPAD_ICON_SIZE_MAX);
  g_assert_cmpint(lambdock_launchpad_icon_size_sanitize(NAN), ==,
                  LAUNCHPAD_ICON_SIZE);

  /* Anything non-positive means "as many columns as fit". */
  g_assert_cmpint(lambdock_launchpad_columns_sanitize(0), ==,
                  LAUNCHPAD_COLUMNS_AUTO);
  g_assert_cmpint(lambdock_launchpad_columns_sanitize(-4), ==,
                  LAUNCHPAD_COLUMNS_AUTO);
  g_assert_cmpint(lambdock_launchpad_columns_sanitize(NAN), ==,
                  LAUNCHPAD_COLUMNS_AUTO);
  g_assert_cmpint(lambdock_launchpad_columns_sanitize(6), ==, 6);
  g_assert_cmpint(lambdock_launchpad_columns_sanitize(100), ==,
                  LAUNCHPAD_COLUMNS_MAX);
}

static void test_shelf_params(void) {
  LambdockShelfParams defaults = lambdock_shelf_params_default();

  /* NULL means "all defaults", sanitised. */
  LambdockShelfParams params = lambdock_shelf_params_sanitize(NULL, defaults);
  g_assert_cmpfloat(params.tilt_deg, ==, defaults.tilt_deg);
  g_assert_cmpint(params.margin_px, ==, defaults.margin_px);
  g_assert_cmpint(params.flare_px, ==, defaults.flare_px);

  /* Hostile values are clamped into the ranges the geometry maths assumes. */
  LambdockShelfRawValues raw = {
      .tilt_deg = 10000.0,
      .perspective = -5.0,
      .margin_px = -10000.0,
      .front_px = 10000.0,
      .reflection_ratio = 99.0,
      .reflection_intensity = -3.0,
      .reflection_blur_ratio = 99.0,
      .flare_px = 10000.0,
  };
  params = lambdock_shelf_params_sanitize(&raw, defaults);
  g_assert_cmpfloat(params.tilt_deg, ==, SHELF_TILT_MAX_DEG);
  g_assert_cmpfloat(params.perspective, ==, SHELF_PERSPECTIVE_MIN);
  g_assert_cmpint(params.margin_px, ==, -SHELF_EXTRA_MAX_PX);
  g_assert_cmpint(params.front_px, ==, SHELF_EXTRA_MAX_PX);
  g_assert_cmpfloat(params.reflection_ratio, ==, SHELF_REFLECTION_RATIO_MAX);
  g_assert_cmpfloat(params.reflection_intensity, ==, 0.0);
  g_assert_cmpfloat(params.reflection_blur_ratio, ==,
                    SHELF_REFLECTION_BLUR_MAX);
  g_assert_cmpint(params.flare_px, ==, SHELF_FLARE_MAX_PX);

  /* NaN falls back to the caller's defaults rather than to zero. */
  LambdockShelfRawValues garbage = {
      .tilt_deg = NAN,
      .perspective = NAN,
      .margin_px = NAN,
      .front_px = NAN,
      .reflection_ratio = NAN,
      .reflection_intensity = NAN,
      .reflection_blur_ratio = NAN,
      .flare_px = NAN,
  };
  params = lambdock_shelf_params_sanitize(&garbage, defaults);
  g_assert_cmpfloat(params.tilt_deg, ==, defaults.tilt_deg);
  g_assert_cmpfloat(params.perspective, ==, defaults.perspective);
  g_assert_cmpint(params.margin_px, ==, defaults.margin_px);
  g_assert_cmpint(params.front_px, ==, defaults.front_px);
  g_assert_cmpfloat(params.reflection_ratio, ==, defaults.reflection_ratio);
  g_assert_cmpfloat(params.reflection_intensity, ==,
                    defaults.reflection_intensity);
  g_assert_cmpfloat(params.reflection_blur_ratio, ==,
                    defaults.reflection_blur_ratio);
  g_assert_cmpint(params.flare_px, ==, defaults.flare_px);
}

static void test_edge_names(void) {
  g_assert_cmpint(lambdock_edge_from_name("top"), ==,
                  GTK_LAYER_SHELL_EDGE_TOP);
  g_assert_cmpint(lambdock_edge_from_name("LEFT"), ==,
                  GTK_LAYER_SHELL_EDGE_LEFT);
  g_assert_cmpint(lambdock_edge_from_name("right"), ==,
                  GTK_LAYER_SHELL_EDGE_RIGHT);
  g_assert_cmpint(lambdock_edge_from_name("bottom"), ==,
                  GTK_LAYER_SHELL_EDGE_BOTTOM);

  /* The default edge is the bottom one. */
  g_assert_cmpint(lambdock_edge_from_name(NULL), ==,
                  GTK_LAYER_SHELL_EDGE_BOTTOM);
  g_assert_cmpint(lambdock_edge_from_name(""), ==,
                  GTK_LAYER_SHELL_EDGE_BOTTOM);
  g_assert_cmpint(lambdock_edge_from_name("diagonal"), ==,
                  GTK_LAYER_SHELL_EDGE_BOTTOM);

  g_assert_cmpstr(lambdock_edge_name(GTK_LAYER_SHELL_EDGE_TOP), ==, "top");
  g_assert_cmpstr(lambdock_edge_name(GTK_LAYER_SHELL_EDGE_LEFT), ==, "left");
  g_assert_cmpstr(lambdock_edge_name(GTK_LAYER_SHELL_EDGE_RIGHT), ==, "right");
  g_assert_cmpstr(lambdock_edge_name(GTK_LAYER_SHELL_EDGE_BOTTOM), ==,
                  "bottom");

  /* Round trip through both conversions. */
  g_assert_cmpint(lambdock_edge_from_name(
                      lambdock_edge_name(GTK_LAYER_SHELL_EDGE_LEFT)),
                  ==, GTK_LAYER_SHELL_EDGE_LEFT);
}

static void test_dock_mode(void) {
  g_assert_true(lambdock_dock_mode_is_taskbar("taskbar"));
  g_assert_true(lambdock_dock_mode_is_taskbar("TASKBAR"));
  g_assert_false(lambdock_dock_mode_is_taskbar("dock"));
  g_assert_false(lambdock_dock_mode_is_taskbar(""));
  g_assert_false(lambdock_dock_mode_is_taskbar(NULL));
  g_assert_false(lambdock_dock_mode_is_taskbar("taskbar-ish"));
}

/* The launchpad's close button is on the left unless the setting says "right";
 * every other value (including a stale one) is the default side. */
static void test_launchpad_close_side(void) {
  g_assert_true(lambdock_launchpad_close_on_right("right"));
  g_assert_true(lambdock_launchpad_close_on_right("RIGHT"));
  g_assert_false(lambdock_launchpad_close_on_right("left"));
  g_assert_false(lambdock_launchpad_close_on_right(""));
  g_assert_false(lambdock_launchpad_close_on_right(NULL));
  g_assert_false(lambdock_launchpad_close_on_right("sideways"));
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/core/config_values/clamp", test_clamp);
  g_test_add_func("/core/config_values/sanitize", test_sanitize);
  g_test_add_func("/core/config_values/icon_size", test_icon_size);
  g_test_add_func("/core/config_values/spacing_and_padding",
                  test_spacing_and_padding);
  g_test_add_func("/core/config_values/launchpad", test_launchpad_values);
  g_test_add_func("/core/config_values/shelf_params", test_shelf_params);
  g_test_add_func("/core/config_values/edge_names", test_edge_names);
  g_test_add_func("/core/config_values/launchpad_close_side",
                  test_launchpad_close_side);
  g_test_add_func("/core/config_values/dock_mode", test_dock_mode);

  return g_test_run();
}
