// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The glass shelf, measured: the first half of `src/ui/shelf.c` in Rust.
//!
//! What the dock *measures* moved first, because it is what everything else asks
//! the shelf for: which parameters are in force, whether the shelf look is on at
//! all, how thick the capsule is and how deep the reflection band runs. The two
//! widgets that paint the glass -- the frame and the reflections -- are still in
//! C, and will move in their own step.
//!
//! Geometry (all values in pixels, `icon` = icon size):
//!
//! ```text
//! band    = round(icon * reflection_ratio)
//! outer   = max(front_px, band)          // glass on the screen-edge side
//! capsule = icon + margin_px + outer
//! ```
//!
//! With the defaults (tilt 0, reflection 0, front 0, margin = capsule padding)
//! that is exactly the classic `icon + 24px` capsule, the frame is a pass-through
//! and the reflections stay hidden.

use crate::core::config_values::{lambdock_shelf_params_default, ShelfParams};
use crate::core::dock_dims::ShelfGeometry;
use crate::state::LambdockState;
use std::os::raw::c_int;

/// `DEFAULT_ICON_SIZE` in `lambdock.h`.
const DEFAULT_ICON_SIZE: c_int = 48;

/// `SHELF_REFLECTION_BANDS` in `lambdock.h`: the most bands one reflection
/// repaint may cost.
const REFLECTION_BANDS: c_int = 16;

/// The fewest bands a reflection is drawn with, however thin the band is.
const MIN_BANDS: c_int = 6;

/// The shelf parameters that should be used for `state`.
///
/// A zero-initialised state (a partially built dock, a unit test) carries no shelf
/// configuration at all; it is treated as the plain dock rather than as a capsule
/// with zero margin, which would collapse the layout.
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn lambdock_shelf_params_effective(
    state: *const LambdockState,
) -> ShelfParams {
    if state.is_null() {
        return lambdock_shelf_params_default();
    }

    let params = (*state).shelf;

    if params.margin_px == 0
        && params.front_px == 0
        && params.reflection_ratio == 0.0
        && params.tilt_deg == 0.0
        && params.flare_px == 0
    {
        return lambdock_shelf_params_default();
    }

    params
}

/// Whether the shelf look is in force for `state`.
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn lambdock_shelf_active(state: *const LambdockState) -> c_int {
    if state.is_null() {
        return 0;
    }

    let defaults = lambdock_shelf_params_default();
    let params = lambdock_shelf_params_effective(state);

    c_int::from(
        params.tilt_deg != defaults.tilt_deg
            || params.margin_px != defaults.margin_px
            || params.front_px != defaults.front_px
            || params.flare_px != defaults.flare_px
            || params.reflection_ratio > defaults.reflection_ratio,
    )
}

/// The shelf measured out for `params` at `icon_size`.
///
/// Pure: the same parameters always measure the same edge, which is what lets the
/// capsule be checked without a dock.
pub fn geometry(params: &ShelfParams, icon_size: c_int) -> ShelfGeometry {
    let icon = if icon_size > 0 {
        icon_size
    } else {
        DEFAULT_ICON_SIZE
    };

    let band = (f64::from(icon) * params.reflection_ratio).round() as c_int;
    let band = band.max(0);
    let outer = params.front_px.max(band);
    let capsule = (icon + params.margin_px + outer).max(0);

    ShelfGeometry {
        band_px: band,
        outer_px: outer,
        capsule_px: capsule,
        overhang_px: (-params.margin_px).max(0),
    }
}

/// The shelf measured out for `state` at `icon_size`.
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn lambdock_shelf_geometry_for(
    state: *const LambdockState,
    icon_size: c_int,
) -> ShelfGeometry {
    geometry(&lambdock_shelf_params_effective(state), icon_size)
}

/// How many clipped draws one reflection repaint costs.
///
/// The fade is painted as a stack of bands, so this is the per-repaint budget of
/// the whole widget: a pure function, and tested, because "a few bands" is not a
/// budget.
#[no_mangle]
pub extern "C" fn lambdock_reflection_band_count(axis_len: f64) -> c_int {
    if !(axis_len > 0.0) {
        return REFLECTION_BANDS;
    }

    (axis_len.round() as c_int).clamp(MIN_BANDS, REFLECTION_BANDS)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The capsule is the icon plus the margin plus whatever the glass takes on
    /// the outer side, and a reflection deeper than the front is what decides
    /// `outer`.
    #[test]
    fn the_geometry_is_the_capsule_and_its_band() {
        let params = ShelfParams {
            margin_px: 12,
            front_px: 8,
            reflection_ratio: 0.25,
            ..lambdock_shelf_params_default()
        };

        let geometry = crate::ui::shelf::geometry(&params, 48);

        // 48 * 0.25 = 12 of band, which is deeper than the 8px front.
        assert_eq!(geometry.band_px, 12);
        assert_eq!(geometry.outer_px, 12);
        assert_eq!(geometry.capsule_px, 48 + 12 + 12);
        assert_eq!(
            geometry.overhang_px, 0,
            "a positive margin does not overhang"
        );
    }

    /// A negative margin puts the icons past the capsule's inner edge.
    #[test]
    fn a_negative_margin_is_an_overhang() {
        let params = ShelfParams {
            margin_px: -18,
            front_px: 14,
            reflection_ratio: 0.0,
            ..lambdock_shelf_params_default()
        };

        let geometry = geometry(&params, 48);

        assert_eq!(geometry.band_px, 0);
        assert_eq!(geometry.overhang_px, 18);
        assert_eq!(geometry.capsule_px, 48 - 18 + 14);
    }

    /// A nonsense icon size falls back to the dock's default, and nothing comes
    /// out negative.
    #[test]
    fn a_missing_icon_size_uses_the_default() {
        let params = ShelfParams {
            margin_px: 0,
            front_px: 0,
            reflection_ratio: 0.0,
            ..lambdock_shelf_params_default()
        };

        assert_eq!(geometry(&params, 0).capsule_px, 48);
        assert_eq!(geometry(&params, -5).capsule_px, 48);
    }

    /// The band has a floor and a ceiling: a hairline band is still drawn in
    /// enough strips to fade, and a deep one does not cost an unbounded number of
    /// draws.
    #[test]
    fn the_band_count_is_a_budget() {
        assert_eq!(lambdock_reflection_band_count(0.0), REFLECTION_BANDS);
        assert_eq!(lambdock_reflection_band_count(-4.0), REFLECTION_BANDS);
        assert_eq!(lambdock_reflection_band_count(f64::NAN), REFLECTION_BANDS);

        assert_eq!(lambdock_reflection_band_count(1.0), MIN_BANDS);
        assert_eq!(lambdock_reflection_band_count(10.4), 10);
        assert_eq!(lambdock_reflection_band_count(99.0), REFLECTION_BANDS);
    }

    /// The geometry the C asks for is the same one these tests measure, from a
    /// state whose shelf is all zeroes and from one that configures it.
    #[test]
    fn a_zeroed_state_measures_the_plain_dock() {
        let mut state: LambdockState = unsafe { std::mem::zeroed() };

        unsafe {
            assert_eq!(
                lambdock_shelf_active(&state),
                0,
                "all zeroes is not a shelf"
            );

            // A zeroed shelf is treated as the defaults, not as a zero-margin
            // capsule that would collapse the layout.
            let geometry = lambdock_shelf_geometry_for(&state, 48);
            assert_eq!(
                geometry.capsule_px,
                48 + lambdock_shelf_params_default().margin_px
            );

            // A reflection and a tilt turn the shelf look on.
            state.shelf.reflection_ratio = 0.3;
            assert_ne!(lambdock_shelf_active(&state), 0);
            assert!(lambdock_shelf_geometry_for(&state, 48).band_px > 0);

            state.shelf.reflection_ratio = 0.0;
            state.shelf.tilt_deg = 8.0;
            assert_ne!(lambdock_shelf_active(&state), 0);
        }
    }

    /// The parameters in force are the dock's, unless it configured none.
    #[test]
    fn an_unconfigured_dock_gets_the_defaults() {
        unsafe {
            let defaults = lambdock_shelf_params_default();
            let mut state: LambdockState = std::mem::zeroed();

            let effective = lambdock_shelf_params_effective(&state);
            assert_eq!(effective.margin_px, defaults.margin_px);
            assert_eq!(effective.front_px, defaults.front_px);

            state.shelf.front_px = 9;
            assert_eq!(lambdock_shelf_params_effective(&state).front_px, 9);

            // A null state is the defaults too.
            assert_eq!(
                lambdock_shelf_params_effective(std::ptr::null()).front_px,
                defaults.front_px
            );
        }
    }
}
