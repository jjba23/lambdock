// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! Pure configuration values: `src/core/config_values.h` in Rust.
//!
//! The Guile bridge fetches raw values out of Scheme and hands them here, so the
//! "what is a sane value" rules live in one place that can be tested without an
//! interpreter or a display. Sanitisers take a `double` because that is what
//! Scheme hands over: `#f`, NaN and NaN-producing arithmetic all arrive as
//! non-finite or out-of-range reals, and a sanitiser always returns a usable
//! value, so a broken or hostile configuration cannot push garbage into the
//! dock's geometry.

use std::ffi::CStr;
use std::os::raw::{c_char, c_int};

// ---------------------------------------------------------------------------
// The limits, repeated from the headers.
//
// A `#define` cannot cross the FFI, so these numbers live on both sides. The C
// test asserts every one of them against the macro it belongs to, which is what
// keeps the two copies honest.
// ---------------------------------------------------------------------------

/// `DEFAULT_ICON_SIZE` in `lambdock.h`.
pub const DEFAULT_ICON_SIZE: c_int = 48;
/// `LAMBDOCK_ICON_SIZE_MAX` in `config_values.h`.
pub const ICON_SIZE_MAX: c_int = 512;
/// `DOCK_ITEM_SPACING_PX` in `lambdock.h`.
pub const ITEM_SPACING_PX: c_int = 4;
/// `LAMBDOCK_ITEM_SPACING_MAX_PX` in `config_values.h`.
pub const ITEM_SPACING_MAX_PX: c_int = 64;
/// `CAPSULE_MAIN_AXIS_PADDING_PX` in `lambdock.h`.
pub const CAPSULE_PADDING_PX: c_int = 12;
/// `CAPSULE_MAIN_AXIS_PADDING_MAX_PX` in `lambdock.h`.
pub const CAPSULE_PADDING_MAX_PX: c_int = 96;
/// `LAUNCHPAD_ICON_SIZE` and its bounds, in `ui/launchpad.h`.
pub const LAUNCHPAD_ICON_SIZE: c_int = 80;
pub const LAUNCHPAD_ICON_SIZE_MIN: c_int = 32;
pub const LAUNCHPAD_ICON_SIZE_MAX: c_int = 160;
/// `LAUNCHPAD_COLUMNS_*` in `ui/launchpad.h`; zero means "as many as fit".
pub const LAUNCHPAD_COLUMNS_AUTO: c_int = 0;
pub const LAUNCHPAD_COLUMNS_MIN: c_int = 3;
pub const LAUNCHPAD_COLUMNS_MAX: c_int = 14;
/// `SHELF_*` limits in `core/shelf_params.h`.
pub const SHELF_TILT_MAX_DEG: f64 = 45.0;
pub const SHELF_PERSPECTIVE_MIN: f64 = 50.0;
pub const SHELF_PERSPECTIVE_MAX: f64 = 5_000.0;
pub const SHELF_EXTRA_MAX_PX: c_int = 200;
pub const SHELF_REFLECTION_RATIO_MAX: f64 = 2.0;
pub const SHELF_REFLECTION_BLUR_MAX: f64 = 0.25;
pub const SHELF_FLARE_MAX_PX: c_int = 80;

/// `SHELF_DEFAULT_*` in `core/shelf_params.h`: the classic flat capsule.
pub const SHELF_DEFAULT_TILT: f64 = 0.0;
pub const SHELF_DEFAULT_PERSPECTIVE: f64 = 400.0;
pub const SHELF_DEFAULT_MARGIN_PX: c_int = 24;
pub const SHELF_DEFAULT_FRONT_PX: c_int = 0;
pub const SHELF_DEFAULT_REFLECTION: f64 = 0.0;
pub const SHELF_DEFAULT_REFLECTION_INTENSITY: f64 = 0.55;
pub const SHELF_DEFAULT_REFLECTION_BLUR: f64 = 0.05;
pub const SHELF_DEFAULT_FLARE_PX: c_int = 0;

/// `GtkLayerShellEdge`: LEFT, RIGHT, TOP, BOTTOM.
pub const EDGE_LEFT: c_int = 0;
pub const EDGE_RIGHT: c_int = 1;
pub const EDGE_TOP: c_int = 2;
pub const EDGE_BOTTOM: c_int = 3;

/// The raw shelf numbers as they came out of Scheme, before validation.
///
/// The field order is the contract: `LambdockShelfRawValues` in the header.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ShelfRawValues {
    pub tilt_deg: f64,
    pub perspective: f64,
    pub margin_px: f64,
    pub front_px: f64,
    pub reflection_ratio: f64,
    pub reflection_intensity: f64,
    pub reflection_blur_ratio: f64,
    pub flare_px: f64,
}

/// Geometry of the dock's glass shelf.
///
/// The field order is the contract: `LambdockShelfParams` in
/// `core/shelf_params.h`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ShelfParams {
    pub tilt_deg: f64,
    pub perspective: f64,
    pub margin_px: c_int,
    pub front_px: c_int,
    pub reflection_ratio: f64,
    pub reflection_intensity: f64,
    pub reflection_blur_ratio: f64,
    pub flare_px: c_int,
}

/// Whether a C string equals a Rust one, ASCII case-insensitively, as
/// `g_ascii_strcasecmp` did. A null pointer matches nothing.
unsafe fn eq_ascii(text: *const c_char, expected: &str) -> bool {
    if text.is_null() {
        return false;
    }

    CStr::from_ptr(text)
        .to_bytes()
        .eq_ignore_ascii_case(expected.as_bytes())
}

/// @value clamped into `[low, high]`; NaN or infinite yields @low.
pub fn lambdock_clamp_double(value: f64, low: f64, high: f64) -> f64 {
    if value.is_nan() {
        return low;
    }

    if value < low {
        return low;
    }

    if value > high {
        return high;
    }

    value
}

/// @value rounded into `[low, high]`; NaN yields @low.
pub fn lambdock_clamp_int(value: f64, low: c_int, high: c_int) -> c_int {
    if value.is_nan() {
        return low;
    }

    // Compare before rounding: round(1e300) would overflow the int cast.
    if value <= f64::from(low) {
        return low;
    }

    if value >= f64::from(high) {
        return high;
    }

    let rounded = value.round() as c_int;

    if rounded < low {
        low
    } else if rounded > high {
        high
    } else {
        rounded
    }
}

/// @value clamped, or @fallback when it is not a finite number.
pub fn lambdock_sanitize_double(
    value: f64,
    fallback: f64,
    low: f64,
    high: f64,
) -> f64 {
    if !value.is_finite() {
        return lambdock_clamp_double(fallback, low, high);
    }

    lambdock_clamp_double(value, low, high)
}

/// @value rounded and clamped, or @fallback when it is not a finite number.
pub fn lambdock_sanitize_int(
    value: f64,
    fallback: c_int,
    low: c_int,
    high: c_int,
) -> c_int {
    if !value.is_finite() {
        return lambdock_clamp_int(f64::from(fallback), low, high);
    }

    lambdock_clamp_int(value, low, high)
}

/// The icon size a user asked for, or the default for a broken request.
pub fn lambdock_icon_size_sanitize(raw: f64) -> c_int {
    if !raw.is_finite() || raw <= 0.0 {
        return DEFAULT_ICON_SIZE;
    }

    // A one-pixel icon is as unusable as a 10k-pixel one; both ends are clamped
    // so the shelf and fisheye maths stay well inside int range.
    lambdock_sanitize_int(raw, DEFAULT_ICON_SIZE, 8, ICON_SIZE_MAX)
}

/// The gap between dock items, or the default for a broken request.
pub fn lambdock_item_spacing_sanitize(raw: f64) -> c_int {
    lambdock_sanitize_int(raw, ITEM_SPACING_PX, 0, ITEM_SPACING_MAX_PX)
}

/// The capsule's end padding, or the default for a broken request.
pub fn lambdock_capsule_padding_sanitize(raw: f64) -> c_int {
    lambdock_sanitize_int(raw, CAPSULE_PADDING_PX, 0, CAPSULE_PADDING_MAX_PX)
}

/// The launchpad tile size, or the default for a broken request.
pub fn lambdock_launchpad_icon_size_sanitize(raw: f64) -> c_int {
    lambdock_sanitize_int(
        raw,
        LAUNCHPAD_ICON_SIZE,
        LAUNCHPAD_ICON_SIZE_MIN,
        LAUNCHPAD_ICON_SIZE_MAX,
    )
}

/// The launchpad's column count, where any non-positive request means auto.
pub fn lambdock_launchpad_columns_sanitize(raw: f64) -> c_int {
    if !raw.is_finite() || raw <= 0.0 {
        return LAUNCHPAD_COLUMNS_AUTO;
    }

    lambdock_sanitize_int(
        raw,
        LAUNCHPAD_COLUMNS_AUTO,
        LAUNCHPAD_COLUMNS_MIN,
        LAUNCHPAD_COLUMNS_MAX,
    )
}

/// The built-in shelf parameters: the classic flat capsule.
pub fn lambdock_shelf_params_default() -> ShelfParams {
    ShelfParams {
        tilt_deg: SHELF_DEFAULT_TILT,
        perspective: SHELF_DEFAULT_PERSPECTIVE,
        margin_px: SHELF_DEFAULT_MARGIN_PX,
        front_px: SHELF_DEFAULT_FRONT_PX,
        reflection_ratio: SHELF_DEFAULT_REFLECTION,
        reflection_intensity: SHELF_DEFAULT_REFLECTION_INTENSITY,
        reflection_blur_ratio: SHELF_DEFAULT_REFLECTION_BLUR,
        flare_px: SHELF_DEFAULT_FLARE_PX,
    }
}

/// A fully validated shelf parameter set.
///
/// # Safety
///
/// `raw` must be null or point to a readable `LambdockShelfRawValues`.
pub unsafe fn lambdock_shelf_params_sanitize(
    raw: *const ShelfRawValues,
    defaults: ShelfParams,
) -> ShelfParams {
    let values = if raw.is_null() {
        ShelfRawValues {
            tilt_deg: defaults.tilt_deg,
            perspective: defaults.perspective,
            margin_px: f64::from(defaults.margin_px),
            front_px: f64::from(defaults.front_px),
            reflection_ratio: defaults.reflection_ratio,
            reflection_intensity: defaults.reflection_intensity,
            reflection_blur_ratio: defaults.reflection_blur_ratio,
            flare_px: f64::from(defaults.flare_px),
        }
    } else {
        *raw
    };

    ShelfParams {
        tilt_deg: lambdock_sanitize_double(
            values.tilt_deg,
            defaults.tilt_deg,
            -SHELF_TILT_MAX_DEG,
            SHELF_TILT_MAX_DEG,
        ),
        perspective: lambdock_sanitize_double(
            values.perspective,
            defaults.perspective,
            SHELF_PERSPECTIVE_MIN,
            SHELF_PERSPECTIVE_MAX,
        ),
        // The margin may be negative: that is how the icons overhang the shelf.
        margin_px: lambdock_sanitize_int(
            values.margin_px,
            defaults.margin_px,
            -SHELF_EXTRA_MAX_PX,
            SHELF_EXTRA_MAX_PX,
        ),
        front_px: lambdock_sanitize_int(values.front_px, defaults.front_px, 0, SHELF_EXTRA_MAX_PX),
        reflection_ratio: lambdock_sanitize_double(
            values.reflection_ratio,
            defaults.reflection_ratio,
            0.0,
            SHELF_REFLECTION_RATIO_MAX,
        ),
        reflection_intensity: lambdock_sanitize_double(
            values.reflection_intensity,
            defaults.reflection_intensity,
            0.0,
            1.0,
        ),
        reflection_blur_ratio: lambdock_sanitize_double(
            values.reflection_blur_ratio,
            defaults.reflection_blur_ratio,
            0.0,
            SHELF_REFLECTION_BLUR_MAX,
        ),
        // The keystone lean, in pixels per side.
        flare_px: lambdock_sanitize_int(
            values.flare_px,
            defaults.flare_px,
            -SHELF_FLARE_MAX_PX,
            SHELF_FLARE_MAX_PX,
        ),
    }
}

/// The Layer Shell edge a settings name asks for; unknown names are the bottom.
///
/// # Safety
///
/// `name` must be null or a NUL-terminated string.
pub unsafe fn lambdock_edge_from_name(name: *const c_char) -> c_int {
    if name.is_null() || *name == 0 {
        return EDGE_BOTTOM;
    }

    if eq_ascii(name, "top") {
        return EDGE_TOP;
    }

    if eq_ascii(name, "left") {
        return EDGE_LEFT;
    }

    if eq_ascii(name, "right") {
        return EDGE_RIGHT;
    }

    EDGE_BOTTOM
}

/// The settings name of an edge; anything unknown is "bottom".
pub fn lambdock_edge_name(edge: c_int) -> *const c_char {
    match edge {
        EDGE_TOP => c"top".as_ptr(),
        EDGE_LEFT => c"left".as_ptr(),
        EDGE_RIGHT => c"right".as_ptr(),
        _ => c"bottom".as_ptr(),
    }
}

/// Whether @mode names the full-width taskbar layout.
///
/// # Safety
///
/// `mode` must be null or a NUL-terminated string.
pub unsafe fn lambdock_dock_mode_is_taskbar(mode: *const c_char) -> c_int {
    eq_ascii(mode, "taskbar") as c_int
}

/// Whether @side asks for the launchpad's close button on the right.
///
/// # Safety
///
/// `side` must be null or a NUL-terminated string.
pub unsafe fn lambdock_launchpad_close_on_right(side: *const c_char) -> c_int {
    eq_ascii(side, "right") as c_int
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    fn c(text: &str) -> CString {
        CString::new(text).expect("no interior NUL")
    }

    fn is_taskbar(mode: Option<&str>) -> bool {
        match mode {
            Some(mode) => {
                let mode = c(mode);
                unsafe { lambdock_dock_mode_is_taskbar(mode.as_ptr()) != 0 }
            }
            None => unsafe { lambdock_dock_mode_is_taskbar(std::ptr::null()) != 0 },
        }
    }

    fn close_on_right(side: Option<&str>) -> bool {
        match side {
            Some(side) => {
                let side = c(side);
                unsafe { lambdock_launchpad_close_on_right(side.as_ptr()) != 0 }
            }
            None => unsafe { lambdock_launchpad_close_on_right(std::ptr::null()) != 0 },
        }
    }

    fn edge_from_name(name: Option<&str>) -> c_int {
        match name {
            Some(name) => {
                let name = c(name);
                unsafe { lambdock_edge_from_name(name.as_ptr()) }
            }
            None => unsafe { lambdock_edge_from_name(std::ptr::null()) },
        }
    }

    fn edge_name(edge: c_int) -> String {
        let text = lambdock_edge_name(edge);
        assert!(!text.is_null());
        unsafe { CStr::from_ptr(text) }.to_string_lossy().into_owned()
    }

    fn sanitize_shelf(raw: Option<ShelfRawValues>, defaults: ShelfParams) -> ShelfParams {
        match raw {
            Some(raw) => unsafe { lambdock_shelf_params_sanitize(&raw, defaults) },
            None => unsafe { lambdock_shelf_params_sanitize(std::ptr::null(), defaults) },
        }
    }

    /// The header's `SHELF_DEFAULT_*`: the classic flat capsule.
    fn shelf_defaults() -> ShelfParams {
        lambdock_shelf_params_default()
    }

    // ======================================================================
    // Clamping
    // ======================================================================

    #[test]
    fn a_value_inside_the_range_is_untouched() {
        assert_eq!(lambdock_clamp_double(5.0, 0.0, 10.0), 5.0);
    }

    #[test]
    fn a_value_outside_the_range_hits_the_bound() {
        assert_eq!(lambdock_clamp_double(-5.0, 0.0, 10.0), 0.0);
        assert_eq!(lambdock_clamp_double(50.0, 0.0, 10.0), 10.0);
    }

    #[test]
    fn a_number_that_is_not_a_number_clamps_to_the_lower_bound() {
        // NaN cannot satisfy any comparison, so it would otherwise leak through.
        assert_eq!(lambdock_clamp_double(f64::NAN, 1.0, 10.0), 1.0);
        assert_eq!(lambdock_clamp_double(f64::INFINITY, 1.0, 10.0), 10.0);
        assert_eq!(lambdock_clamp_double(f64::NEG_INFINITY, 1.0, 10.0), 1.0);
    }

    #[test]
    fn an_integer_clamp_rounds_to_the_nearest_pixel() {
        assert_eq!(lambdock_clamp_int(5.4, 0, 10), 5);
        assert_eq!(lambdock_clamp_int(5.5, 0, 10), 6);
        assert_eq!(lambdock_clamp_int(-3.0, 0, 10), 0);
        assert_eq!(lambdock_clamp_int(99.0, 0, 10), 10);
    }

    #[test]
    fn a_huge_double_never_overflows_the_integer_conversion() {
        assert_eq!(lambdock_clamp_int(1e300, 0, 10), 10);
        assert_eq!(lambdock_clamp_int(-1e300, 0, 10), 0);
    }

    // ======================================================================
    // Sanitising
    // ======================================================================

    #[test]
    fn a_broken_value_falls_back_and_a_large_one_clamps() {
        assert_eq!(lambdock_sanitize_double(f64::NAN, 3.0, 0.0, 10.0), 3.0);
        assert_eq!(lambdock_sanitize_double(42.0, 3.0, 0.0, 10.0), 10.0);
        assert_eq!(lambdock_sanitize_int(f64::INFINITY, 4, 0, 10), 4);
        assert_eq!(lambdock_sanitize_int(2.6, 4, 0, 10), 3);
        assert_eq!(lambdock_sanitize_int(f64::NAN, 7, 0, 10), 7);
    }

    // ======================================================================
    // The dock's own numbers
    // ======================================================================

    #[test]
    fn a_sane_icon_size_passes_through() {
        assert_eq!(lambdock_icon_size_sanitize(64.0), 64);
        assert_eq!(lambdock_icon_size_sanitize(48.4), 48);
    }

    #[test]
    fn a_useless_icon_size_falls_back_to_the_default() {
        assert_eq!(lambdock_icon_size_sanitize(0.0), DEFAULT_ICON_SIZE);
        assert_eq!(lambdock_icon_size_sanitize(-12.0), DEFAULT_ICON_SIZE);
        assert_eq!(lambdock_icon_size_sanitize(f64::NAN), DEFAULT_ICON_SIZE);
        assert_eq!(lambdock_icon_size_sanitize(f64::INFINITY), DEFAULT_ICON_SIZE);
    }

    #[test]
    fn an_extreme_icon_size_is_clamped_at_both_ends() {
        assert_eq!(lambdock_icon_size_sanitize(1.0), 8);
        assert_eq!(lambdock_icon_size_sanitize(10_000.0), ICON_SIZE_MAX);
    }

    #[test]
    fn the_item_spacing_is_clamped_between_none_and_the_maximum() {
        assert_eq!(lambdock_item_spacing_sanitize(12.0), 12);
        assert_eq!(lambdock_item_spacing_sanitize(-5.0), 0);
        assert_eq!(lambdock_item_spacing_sanitize(1000.0), ITEM_SPACING_MAX_PX);
        assert_eq!(lambdock_item_spacing_sanitize(f64::NAN), ITEM_SPACING_PX);
    }

    #[test]
    fn the_capsule_padding_is_clamped_between_none_and_the_maximum() {
        assert_eq!(lambdock_capsule_padding_sanitize(24.0), 24);
        assert_eq!(lambdock_capsule_padding_sanitize(-1.0), 0);
        assert_eq!(
            lambdock_capsule_padding_sanitize(1000.0),
            CAPSULE_PADDING_MAX_PX
        );
        assert_eq!(
            lambdock_capsule_padding_sanitize(f64::INFINITY),
            CAPSULE_PADDING_PX
        );
    }

    // ======================================================================
    // The launchpad
    // ======================================================================

    #[test]
    fn a_launchpad_tile_size_is_kept_inside_its_bounds() {
        assert_eq!(lambdock_launchpad_icon_size_sanitize(80.0), 80);
        assert_eq!(
            lambdock_launchpad_icon_size_sanitize(1.0),
            LAUNCHPAD_ICON_SIZE_MIN
        );
        assert_eq!(
            lambdock_launchpad_icon_size_sanitize(4000.0),
            LAUNCHPAD_ICON_SIZE_MAX
        );
        assert_eq!(
            lambdock_launchpad_icon_size_sanitize(f64::NAN),
            LAUNCHPAD_ICON_SIZE
        );
    }

    #[test]
    fn a_non_positive_column_count_means_as_many_as_fit() {
        assert_eq!(
            lambdock_launchpad_columns_sanitize(0.0),
            LAUNCHPAD_COLUMNS_AUTO
        );
        assert_eq!(
            lambdock_launchpad_columns_sanitize(-4.0),
            LAUNCHPAD_COLUMNS_AUTO
        );
        assert_eq!(
            lambdock_launchpad_columns_sanitize(f64::NAN),
            LAUNCHPAD_COLUMNS_AUTO
        );
        assert_eq!(lambdock_launchpad_columns_sanitize(6.0), 6);
        assert_eq!(
            lambdock_launchpad_columns_sanitize(100.0),
            LAUNCHPAD_COLUMNS_MAX
        );
    }

    // ======================================================================
    // The shelf
    // ======================================================================

    #[test]
    fn no_raw_values_means_all_defaults() {
        let defaults = shelf_defaults();
        let params = sanitize_shelf(None, defaults);

        assert_eq!(params, defaults);
    }

    #[test]
    fn hostile_shelf_values_are_clamped_into_the_range_the_geometry_assumes() {
        let raw = ShelfRawValues {
            tilt_deg: 10_000.0,
            perspective: -5.0,
            margin_px: -10_000.0,
            front_px: 10_000.0,
            reflection_ratio: 99.0,
            reflection_intensity: -3.0,
            reflection_blur_ratio: 99.0,
            flare_px: 10_000.0,
        };
        let params = sanitize_shelf(Some(raw), shelf_defaults());

        assert_eq!(params.tilt_deg, SHELF_TILT_MAX_DEG);
        assert_eq!(params.perspective, SHELF_PERSPECTIVE_MIN);
        // A negative margin is how the icons overhang the shelf.
        assert_eq!(params.margin_px, -SHELF_EXTRA_MAX_PX);
        assert_eq!(params.front_px, SHELF_EXTRA_MAX_PX);
        assert_eq!(params.reflection_ratio, SHELF_REFLECTION_RATIO_MAX);
        assert_eq!(params.reflection_intensity, 0.0);
        assert_eq!(params.reflection_blur_ratio, SHELF_REFLECTION_BLUR_MAX);
        assert_eq!(params.flare_px, SHELF_FLARE_MAX_PX);
    }

    #[test]
    fn a_broken_shelf_value_falls_back_to_the_callers_default() {
        let defaults = shelf_defaults();
        let garbage = ShelfRawValues {
            tilt_deg: f64::NAN,
            perspective: f64::NAN,
            margin_px: f64::NAN,
            front_px: f64::NAN,
            reflection_ratio: f64::NAN,
            reflection_intensity: f64::NAN,
            reflection_blur_ratio: f64::NAN,
            flare_px: f64::NAN,
        };

        assert_eq!(sanitize_shelf(Some(garbage), defaults), defaults);
    }

    // ======================================================================
    // Edges, modes and sides
    // ======================================================================

    #[test]
    fn the_four_edges_are_recognised_whatever_their_case() {
        assert_eq!(edge_from_name(Some("top")), EDGE_TOP);
        assert_eq!(edge_from_name(Some("LEFT")), EDGE_LEFT);
        assert_eq!(edge_from_name(Some("right")), EDGE_RIGHT);
        assert_eq!(edge_from_name(Some("bottom")), EDGE_BOTTOM);
    }

    #[test]
    fn an_unknown_edge_is_the_bottom_one() {
        assert_eq!(edge_from_name(None), EDGE_BOTTOM);
        assert_eq!(edge_from_name(Some("")), EDGE_BOTTOM);
        assert_eq!(edge_from_name(Some("diagonal")), EDGE_BOTTOM);
    }

    #[test]
    fn every_edge_names_itself_and_round_trips() {
        for edge in [EDGE_TOP, EDGE_LEFT, EDGE_RIGHT, EDGE_BOTTOM] {
            let name = edge_name(edge);
            assert_eq!(edge_from_name(Some(&name)), edge);
        }

        assert_eq!(edge_name(EDGE_TOP), "top");
        assert_eq!(edge_name(EDGE_LEFT), "left");
        assert_eq!(edge_name(EDGE_RIGHT), "right");
        assert_eq!(edge_name(EDGE_BOTTOM), "bottom");
        // Anything unknown falls back rather than returning a bogus name.
        assert_eq!(edge_name(99), "bottom");
    }

    #[test]
    fn only_the_taskbar_mode_is_a_taskbar() {
        assert!(is_taskbar(Some("taskbar")));
        assert!(is_taskbar(Some("TASKBAR")));
        assert!(!is_taskbar(Some("dock")));
        assert!(!is_taskbar(Some("")));
        assert!(!is_taskbar(None));
        assert!(!is_taskbar(Some("taskbar-ish")));
    }

    #[test]
    fn the_launchpad_close_button_is_on_the_left_unless_the_side_says_right() {
        assert!(close_on_right(Some("right")));
        assert!(close_on_right(Some("RIGHT")));
        assert!(!close_on_right(Some("left")));
        assert!(!close_on_right(Some("")));
        assert!(!close_on_right(None));
        assert!(!close_on_right(Some("sideways")));
    }
}
