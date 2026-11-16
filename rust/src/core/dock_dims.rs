// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! Dock dimensions: `src/core/dock_dims.h` in Rust.
//!
//! `update_dock_dimensions()` used to interleave three jobs: query the monitor,
//! decide how big everything should be, and push the numbers into a dozen
//! widgets. The decision half is arithmetic on values, so it lives here as a
//! pure function; the widget half stays in `dock_window.c` and only applies a
//! finished struct.

use super::config_values::{CAPSULE_PADDING_PX, DEFAULT_ICON_SIZE};
use std::os::raw::c_int;

// ---------------------------------------------------------------------------
// The limits, repeated from the headers (`dock_dims.h`, `lambdock.h`). A
// `#define` cannot cross the FFI; `tests/test_dock_dims.c` asserts each of them
// against its macro.
// ---------------------------------------------------------------------------

/// Fallback monitor size, used when no monitor is available at all.
pub const FALLBACK_MONITOR_WIDTH: c_int = 1920;
pub const FALLBACK_MONITOR_HEIGHT: c_int = 1080;
/// The dock never grows past the monitor minus this margin.
pub const MONITOR_EDGE_MARGIN_PX: c_int = 32;
/// ... and never shrinks below this, so a bogus monitor cannot collapse it.
pub const MIN_CONTENT_PX: c_int = 100;
/// `CROSS_DIM_SCALE_FACTOR`, `CROSS_DIM_PADDING_PX`, `FISHEYE_MAX_SCALE` and
/// `FISHEYE_RADIUS_PX` in `lambdock.h`.
pub const CROSS_DIM_SCALE_FACTOR: f64 = 1.65;
pub const CROSS_DIM_PADDING_PX: c_int = 36;
pub const FISHEYE_MAX_SCALE: f64 = 1.45;
pub const FISHEYE_RADIUS_PX: f64 = 140.0;

/// The shelf measured out for one dock and icon size.
///
/// The field order is the contract: `LambdockShelfGeometry` in
/// `core/shelf_params.h`. It lives here because this is the only Rust module
/// that reads it so far.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ShelfGeometry {
    pub band_px: c_int,
    pub outer_px: c_int,
    pub capsule_px: c_int,
    pub overhang_px: c_int,
}

/// Everything the dock's layout needs to know, as a value.
///
/// The field order is the contract: `LambdockDockDims` in the header.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DockDims {
    pub icon_size: c_int,
    pub capsule_cross_px: c_int,
    pub overlay_cross_px: c_int,
    pub hover_headroom_px: c_int,
    pub max_main_h_px: c_int,
    pub max_main_v_px: c_int,
    pub padding_px: c_int,
    pub outer_margin_px: c_int,
}

/// A monitor size of zero (headless run, monitor just unplugged) would produce a
/// negative content area, so it falls back to a sane screen instead.
fn monitor_dim_or_fallback(value: c_int, fallback: c_int) -> c_int {
    if value > 0 {
        value
    } else {
        fallback
    }
}

/// The dock keeps a margin from the monitor's edge, but never shrinks below a
/// usable content size.
fn max_content_dim(monitor_dim: c_int, margin: c_int) -> c_int {
    let dim = monitor_dim - margin;

    if dim < MIN_CONTENT_PX {
        MIN_CONTENT_PX
    } else {
        dim
    }
}

/// How much room, per side, the row can grow into while the fisheye magnifies it.
///
/// The fisheye grows every icon within `FISHEYE_RADIUS_PX` of the pointer by up
/// to `FISHEYE_MAX_SCALE`, and no more than `(2 * radius / icon + 1)` icons fit
/// in that reach, so the row can never get wider than this. Reserving it once is
/// what keeps the surface from being resized on every pointer frame.
fn hover_headroom_px(icon: c_int, hover_magnify: bool) -> c_int {
    if !hover_magnify {
        return 0;
    }

    let reach = 2.0 * FISHEYE_RADIUS_PX / f64::from(icon) + 1.0;
    let growth = (FISHEYE_MAX_SCALE - 1.0) * f64::from(icon) * reach;

    // Half of the total growth per side: the row stays centred as it spreads.
    (growth / 2.0).ceil() as c_int
}

/// The dimensions for this dock. Pure: same inputs, same output.
#[no_mangle]
pub extern "C" fn lambdock_dock_dims_compute(
    icon_size: c_int,
    capsule_padding: c_int,
    shelf: ShelfGeometry,
    monitor_width: c_int,
    monitor_height: c_int,
    hover_magnify: c_int,
) -> DockDims {
    let icon = if icon_size > 0 {
        icon_size
    } else {
        DEFAULT_ICON_SIZE
    };

    let width = monitor_dim_or_fallback(monitor_width, FALLBACK_MONITOR_WIDTH);
    let height = monitor_dim_or_fallback(monitor_height, FALLBACK_MONITOR_HEIGHT);

    DockDims {
        icon_size: icon,
        capsule_cross_px: shelf.capsule_px,
        // Fisheye magnification headroom, plus the glass, plus any overhang.
        overlay_cross_px: (f64::from(icon) * CROSS_DIM_SCALE_FACTOR) as c_int
            + CROSS_DIM_PADDING_PX
            + shelf.outer_px
            + shelf.overhang_px,
        hover_headroom_px: hover_headroom_px(icon, hover_magnify != 0),
        max_main_h_px: max_content_dim(width, MONITOR_EDGE_MARGIN_PX),
        max_main_v_px: max_content_dim(height, MONITOR_EDGE_MARGIN_PX),
        padding_px: if capsule_padding >= 0 {
            capsule_padding
        } else {
            CAPSULE_PADDING_PX
        },
        outer_margin_px: {
            let margin = shelf.outer_px - shelf.band_px;
            if margin > 0 {
                margin
            } else {
                0
            }
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The classic capsule: no glass, no reflection band, no overhang.
    fn flat_shelf() -> ShelfGeometry {
        ShelfGeometry {
            band_px: 0,
            outer_px: CAPSULE_PADDING_PX,
            capsule_px: DEFAULT_ICON_SIZE + CAPSULE_PADDING_PX,
            overhang_px: 0,
        }
    }

    fn compute(
        icon_size: c_int,
        capsule_padding: c_int,
        shelf: ShelfGeometry,
        width: c_int,
        height: c_int,
        hover_magnify: bool,
    ) -> DockDims {
        lambdock_dock_dims_compute(
            icon_size,
            capsule_padding,
            shelf,
            width,
            height,
            hover_magnify as c_int,
        )
    }

    #[test]
    fn a_missing_icon_size_and_padding_mean_the_documented_defaults() {
        let dims = compute(0, -1, flat_shelf(), 1920, 1080, false);

        assert_eq!(dims.icon_size, DEFAULT_ICON_SIZE);
        assert_eq!(dims.padding_px, CAPSULE_PADDING_PX);
        assert_eq!(dims.capsule_cross_px, flat_shelf().capsule_px);
    }

    #[test]
    fn the_content_area_keeps_its_margin_from_both_monitor_edges() {
        let dims = compute(0, -1, flat_shelf(), 1920, 1080, false);

        assert_eq!(dims.max_main_h_px, 1920 - MONITOR_EDGE_MARGIN_PX);
        assert_eq!(dims.max_main_v_px, 1080 - MONITOR_EDGE_MARGIN_PX);
    }

    #[test]
    fn the_front_glass_pushes_the_icons_off_the_edge() {
        // There is no reflection band to subtract, so the whole glass thickness
        // is the margin.
        let dims = compute(0, -1, flat_shelf(), 1920, 1080, false);
        assert_eq!(dims.outer_margin_px, CAPSULE_PADDING_PX);
    }

    #[test]
    fn the_overlay_holds_the_magnification_headroom_plus_the_glass() {
        let shelf = flat_shelf();
        let dims = compute(64, 12, shelf, 2560, 1440, false);

        let expected = (64.0 * CROSS_DIM_SCALE_FACTOR) as c_int
            + CROSS_DIM_PADDING_PX
            + shelf.outer_px
            + shelf.overhang_px;

        assert_eq!(dims.overlay_cross_px, expected);
        assert_eq!(dims.padding_px, 12);
    }

    #[test]
    fn a_missing_monitor_size_falls_back_to_a_sane_screen() {
        // A headless run, or a monitor that just disappeared, must not produce a
        // negative content size.
        let dims = compute(48, 0, flat_shelf(), 0, 0, false);

        assert_eq!(
            dims.max_main_h_px,
            FALLBACK_MONITOR_WIDTH - MONITOR_EDGE_MARGIN_PX
        );
        assert_eq!(
            dims.max_main_v_px,
            FALLBACK_MONITOR_HEIGHT - MONITOR_EDGE_MARGIN_PX
        );
    }

    #[test]
    fn a_tiny_monitor_cannot_shrink_the_content_area_to_nothing() {
        let dims = compute(48, 0, flat_shelf(), 50, 40, false);

        assert_eq!(dims.max_main_h_px, MIN_CONTENT_PX);
        assert_eq!(dims.max_main_v_px, MIN_CONTENT_PX);
    }

    #[test]
    fn a_reflection_band_taller_than_the_glass_leaves_a_positive_margin() {
        let shelf = ShelfGeometry {
            band_px: 40,
            outer_px: 90,
            capsule_px: 120,
            overhang_px: 10,
        };
        let dims = compute(48, 8, shelf, 1920, 1080, false);

        assert_eq!(dims.outer_margin_px, 50);
    }

    #[test]
    fn a_margin_never_goes_negative_and_push_the_icons_into_the_capsule() {
        let shelf = ShelfGeometry {
            band_px: 60,
            outer_px: 20,
            capsule_px: 120,
            overhang_px: 10,
        };
        let dims = compute(48, 8, shelf, 1920, 1080, false);

        assert_eq!(dims.outer_margin_px, 0);
    }

    #[test]
    fn without_magnification_there_is_nothing_to_reserve() {
        // The surface hugs the dock and the pointer cannot change its size.
        let still = compute(48, 8, flat_shelf(), 1920, 1080, false);
        assert_eq!(still.hover_headroom_px, 0);
    }

    #[test]
    fn magnification_gets_room_on_both_sides_to_spread_into() {
        let hovered = compute(48, 8, flat_shelf(), 1920, 1080, true);
        assert!(hovered.hover_headroom_px > 0);
    }

    #[test]
    fn the_headroom_covers_the_widest_the_row_can_get() {
        // Checked against the formula rather than a magic number, so changing
        // the fisheye constants keeps the promise honest.
        let hovered = compute(48, 8, flat_shelf(), 1920, 1080, true);
        let reach = 2.0 * FISHEYE_RADIUS_PX / 48.0 + 1.0;
        let growth = (FISHEYE_MAX_SCALE - 1.0) * 48.0 * reach;

        assert_eq!(hovered.hover_headroom_px, (growth / 2.0).ceil() as c_int);
    }

    #[test]
    fn smaller_icons_magnify_over_a_shorter_reach() {
        let shelf = flat_shelf();
        let large = compute(48, 8, shelf, 1920, 1080, true);
        let small = compute(32, 8, shelf, 1920, 1080, true);

        assert!(small.hover_headroom_px < large.hover_headroom_px);
    }

    #[test]
    fn the_default_icon_size_needs_no_special_casing() {
        let defaults = compute(0, 8, flat_shelf(), 1920, 1080, true);
        let explicit = compute(DEFAULT_ICON_SIZE, 8, flat_shelf(), 1920, 1080, true);

        assert_eq!(defaults.hover_headroom_px, explicit.hover_headroom_px);
    }

    #[test]
    fn the_same_inputs_give_the_same_dimensions() {
        let shelf = flat_shelf();
        assert_eq!(
            compute(48, 12, shelf, 1920, 1080, false),
            compute(48, 12, shelf, 1920, 1080, false)
        );
    }
}
