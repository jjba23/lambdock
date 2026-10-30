// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAMBDOCK_CORE_CONFIG_VALUES_H
#define LAMBDOCK_CORE_CONFIG_VALUES_H

#include "shelf_params.h"

#include <glib.h>
#include <gtk4-layer-shell.h>

/*
 * =========================================================================
 * PURE CONFIGURATION VALUES
 * =========================================================================
 * Everything in this module is a pure function: the same arguments always
 * produce the same result, nothing is allocated, and no Guile, widget or global
 * state is touched. The Guile bridge fetches raw values out of Scheme and hands
 * them here, so the "what is a sane value" rules live in one place that can be
 * unit tested without an interpreter or a display.
 *
 * Sanitisers take a double because that is what Scheme hands us (`#f`, NaN and
 * NaN-producing arithmetic all arrive as non-finite or out-of-range reals), and
 * they always return a usable value: a broken or hostile configuration can
 * never push garbage into the dock geometry.
 */

/* Upper bound for a dock icon. Nothing sane is bigger than a quarter of a 4K
 * screen's height, and the shelf maths multiplies this by ~2.6. */
#define LAMBDOCK_ICON_SIZE_MAX 512

/* Upper bound for the gap between dock items, matching what the settings UI
 * offers. */
#define LAMBDOCK_ITEM_SPACING_MAX_PX 64

/* The raw shelf numbers as they came out of Scheme, before validation. */
typedef struct LambdockShelfRawValues {
  double tilt_deg;
  double perspective;
  double margin_px;
  double front_px;
  double reflection_ratio;
  double reflection_intensity;
  double reflection_blur_ratio;
  double flare_px;
} LambdockShelfRawValues;

/**
 * lambdock_clamp_double / lambdock_clamp_int:
 * @value: the value to clamp.
 * @low: lower bound (inclusive).
 * @high: upper bound (inclusive).
 *
 * Returns: @value clamped into [@low, @high]. A NaN or infinite @value yields
 * @low, so callers always get a finite number back.
 */
double lambdock_clamp_double(double value, double low, double high);
int lambdock_clamp_int(double value, int low, int high);

/**
 * lambdock_sanitize_double:
 * @value: candidate value.
 * @fallback: used when @value is not a finite number.
 * @low: lower bound (inclusive).
 * @high: upper bound (inclusive).
 *
 * Returns: @value clamped into [@low, @high], or @fallback when @value is
 * NaN/infinite.
 */
double lambdock_sanitize_double(double value, double fallback, double low,
                                double high);

/**
 * lambdock_sanitize_int:
 * @value: candidate value (rounded to the nearest int).
 * @fallback: used when @value is not a finite number.
 * @low: lower bound (inclusive).
 * @high: upper bound (inclusive).
 *
 * Returns: the rounded, clamped value, or @fallback for non-finite input.
 */
int lambdock_sanitize_int(double value, int fallback, int low, int high);

/**
 * lambdock_icon_size_sanitize:
 * Returns: @raw rounded into [8, LAMBDOCK_ICON_SIZE_MAX], or
 * DEFAULT_ICON_SIZE when @raw is non-finite or <= 0.
 */
int lambdock_icon_size_sanitize(double raw);

/**
 * lambdock_item_spacing_sanitize:
 * Returns: @raw rounded into [0, LAMBDOCK_ITEM_SPACING_MAX_PX], or
 * DOCK_ITEM_SPACING_PX when @raw is non-finite.
 */
int lambdock_item_spacing_sanitize(double raw);

/**
 * lambdock_capsule_padding_sanitize:
 * Returns: @raw rounded into [0, CAPSULE_MAIN_AXIS_PADDING_MAX_PX], or
 * CAPSULE_MAIN_AXIS_PADDING_PX when @raw is non-finite.
 */
int lambdock_capsule_padding_sanitize(double raw);

/**
 * lambdock_launchpad_icon_size_sanitize / lambdock_launchpad_columns_sanitize:
 * Returns: the launchpad tile size in [LAUNCHPAD_ICON_SIZE_MIN,
 * LAUNCHPAD_ICON_SIZE_MAX] (default LAUNCHPAD_ICON_SIZE), and the number of
 * columns in [LAUNCHPAD_COLUMNS_MIN, LAUNCHPAD_COLUMNS_MAX], where any
 * non-positive request means LAUNCHPAD_COLUMNS_AUTO.
 */
int lambdock_launchpad_icon_size_sanitize(double raw);
int lambdock_launchpad_columns_sanitize(double raw);

/**
 * lambdock_shelf_params_sanitize:
 * @raw: (nullable): numbers straight out of Scheme.
 * @defaults: the values to use for non-finite input.
 *
 * Returns: a fully validated shelf parameter set. Every field is finite and
 * within the range the shelf geometry maths assumes, which is what makes that
 * maths total.
 */
LambdockShelfParams lambdock_shelf_params_sanitize(
    const LambdockShelfRawValues *raw, LambdockShelfParams defaults);

/**
 * lambdock_edge_from_name / lambdock_edge_name:
 * Convert between Layer Shell edges and the names used in settings
 * ("bottom", "top", "left", "right"). Unknown names fall back to the bottom
 * edge, which is the dock's documented default.
 */
GtkLayerShellEdge lambdock_edge_from_name(const char *name);
const char *lambdock_edge_name(GtkLayerShellEdge edge);

/**
 * lambdock_dock_mode_is_taskbar:
 * Returns: TRUE when @mode names the full-width taskbar layout ("taskbar",
 * case-insensitive). Anything else, including NULL, is the floating capsule.
 */
gboolean lambdock_dock_mode_is_taskbar(const char *mode);

/**
 * lambdock_launchpad_close_on_right:
 * Returns: TRUE when @side asks for the launchpad's close button on the right
 * ("right", case-insensitive). Anything else, including NULL, is the left side,
 * which is the launchpad's documented default.
 */
gboolean lambdock_launchpad_close_on_right(const char *side);

#endif // LAMBDOCK_CORE_CONFIG_VALUES_H
