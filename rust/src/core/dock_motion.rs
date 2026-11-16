// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The arithmetic behind the dock's movement: `src/core/dock_motion.h` in Rust.
//!
//! `dock_window.c` keeps the GTK half (frame callbacks, widget mutation) and
//! asks here for every number it needs, which is what makes the easing, the
//! auto-hide margin and the fisheye testable without a compositor.

use std::f64::consts::PI;
use std::os::raw::c_int;

/// The fraction of a slide completed, clamped to `[0, 1]`.
#[no_mangle]
pub extern "C" fn lambdock_slide_progress(start_us: i64, now_us: i64, duration_us: i64) -> f64 {
    if duration_us <= 0 {
        return 1.0;
    }

    let progress = (now_us - start_us) as f64 / duration_us as f64;

    if progress < 0.0 {
        return 0.0;
    }

    if progress > 1.0 {
        1.0
    } else {
        progress
    }
}

/// `1 - (1 - t)^3`.
#[no_mangle]
pub extern "C" fn lambdock_ease_out_cubic(t: f64) -> f64 {
    let remaining = 1.0 - t;
    1.0 - (remaining * remaining * remaining)
}

/// The margin the dock should sit at on this frame.
#[no_mangle]
pub extern "C" fn lambdock_slide_position(from_px: c_int, to_px: c_int, progress: f64) -> c_int {
    let eased = lambdock_ease_out_cubic(progress);
    from_px + ((to_px - from_px) as f64 * eased) as c_int
}

/// The (negative) layer-shell margin that hides the dock.
#[no_mangle]
pub extern "C" fn lambdock_hide_margin(
    measured_main_px: c_int,
    cross_axis_px: c_int,
    tolerance_px: c_int,
    trigger_strip_px: c_int,
) -> c_int {
    // Before the first layout, and right after an orientation change, the
    // measured main-axis size is 0 or a leftover from the other axis; both are
    // unusable and the cross-axis request is the best estimate available.
    let main_px = if measured_main_px <= 0 || measured_main_px > cross_axis_px + tolerance_px {
        cross_axis_px
    } else {
        measured_main_px
    };

    -(main_px - trigger_strip_px)
}

/// Whether crossings are echoes of the dock's own movement.
#[no_mangle]
pub extern "C" fn lambdock_settling(
    sliding: c_int,
    settled_at_us: i64,
    now_us: i64,
    settle_ms: u32,
) -> c_int {
    if sliding != 0 {
        return 1;
    }

    if settled_at_us == 0 {
        return 0;
    }

    ((now_us - settled_at_us) < i64::from(settle_ms) * 1000) as c_int
}

/// Whether the whole surface has to take input.
#[no_mangle]
pub extern "C" fn lambdock_dock_needs_full_input(
    is_hidden: c_int,
    current_margin: c_int,
) -> c_int {
    (is_hidden != 0 || current_margin != 0) as c_int
}

/// A cosine bell from `max_scale` at the centre to `1.0` at `radius_px`.
#[no_mangle]
pub extern "C" fn lambdock_fisheye_scale(distance_px: f64, radius_px: f64, max_scale: f64) -> f64 {
    if radius_px <= 0.0 {
        return 1.0;
    }

    let distance = distance_px.abs();
    if distance >= radius_px {
        return 1.0;
    }

    // Full magnification at the centre, easing to none at the edge of the
    // influence, so neighbouring icons grow smoothly together.
    let factor = 0.5 * (1.0 + (PI * distance / radius_px).cos());
    1.0 + (max_scale - 1.0) * factor
}

/// `base_px * scale`, rounded to the nearest whole pixel.
#[no_mangle]
pub extern "C" fn lambdock_scaled_size(base_px: c_int, scale: f64) -> c_int {
    (f64::from(base_px) * scale).round() as c_int
}

/// Whether either coordinate moved by at least `epsilon_px`.
#[no_mangle]
pub extern "C" fn lambdock_points_differ(
    ax: f64,
    ay: f64,
    bx: f64,
    by: f64,
    epsilon_px: f64,
) -> c_int {
    ((ax - bx).abs() >= epsilon_px || (ay - by).abs() >= epsilon_px) as c_int
}

/// `size_px` rounded up to the nearest multiple of `step_px`.
#[no_mangle]
pub extern "C" fn lambdock_size_step(size_px: c_int, step_px: c_int) -> c_int {
    // The fisheye paints through a transform, so only the layout has to step:
    // rounding the size request to a few pixels means the dock re-measures its
    // row a fraction as often, and nothing visible changes.
    if step_px <= 1 {
        return size_px;
    }

    ((size_px + step_px - 1) / step_px) * step_px
}

#[cfg(test)]
mod tests {
    use super::*;

    fn differs(ax: f64, ay: f64, bx: f64, by: f64, epsilon: f64) -> bool {
        lambdock_points_differ(ax, ay, bx, by, epsilon) != 0
    }

    // ======================================================================
    // The slide
    // ======================================================================

    #[test]
    fn progress_runs_from_nothing_to_finished() {
        assert_eq!(lambdock_slide_progress(0, 0, 1000), 0.0);
        assert_eq!(lambdock_slide_progress(0, 250, 1000), 0.25);
        assert_eq!(lambdock_slide_progress(0, 1000, 1000), 1.0);
    }

    #[test]
    fn progress_is_clamped_instead_of_driving_the_dock_off_screen() {
        // Frame times past the end, and a clock that jumps backwards.
        assert_eq!(lambdock_slide_progress(0, 5000, 1000), 1.0);
        assert_eq!(lambdock_slide_progress(500, 0, 1000), 0.0);
    }

    #[test]
    fn a_zero_duration_slide_is_already_over() {
        assert_eq!(lambdock_slide_progress(0, 0, 0), 1.0);
        assert_eq!(lambdock_slide_progress(0, 0, -1000), 1.0);
    }

    #[test]
    fn easing_starts_fast_and_ends_gentle() {
        assert_eq!(lambdock_ease_out_cubic(0.0), 0.0);
        assert_eq!(lambdock_ease_out_cubic(1.0), 1.0);
        assert_eq!(lambdock_ease_out_cubic(0.5), 0.875);
    }

    #[test]
    fn easing_covers_more_than_half_the_distance_in_half_the_time() {
        assert!(lambdock_ease_out_cubic(0.25) > 0.25);
        assert!(lambdock_ease_out_cubic(0.25) < lambdock_ease_out_cubic(0.5));
        assert!(lambdock_ease_out_cubic(0.5) < lambdock_ease_out_cubic(0.75));
        assert!(lambdock_ease_out_cubic(0.75) < 1.0);
    }

    #[test]
    fn a_reveal_lands_flat_at_the_end() {
        assert_eq!(lambdock_slide_position(-800, 0, 0.0), -800);
        assert_eq!(lambdock_slide_position(-800, 0, 1.0), 0);
        assert_eq!(lambdock_slide_position(-800, 0, 0.5), -100);
    }

    #[test]
    fn a_hide_moves_out_the_same_way() {
        assert_eq!(lambdock_slide_position(0, -100, 0.5), -87);
    }

    #[test]
    fn a_no_op_slide_stays_put_at_every_frame() {
        assert_eq!(lambdock_slide_position(42, 42, 0.0), 42);
        assert_eq!(lambdock_slide_position(42, 42, 0.5), 42);
        assert_eq!(lambdock_slide_position(42, 42, 1.0), 42);
    }

    // ======================================================================
    // Auto-hide margin
    // ======================================================================

    #[test]
    fn a_laid_out_dock_is_measured_directly() {
        assert_eq!(lambdock_hide_margin(800, 800, 20, 2), -798);
    }

    #[test]
    fn before_the_first_layout_the_request_is_the_best_estimate() {
        assert_eq!(lambdock_hide_margin(0, 800, 20, 2), -798);
        // A negative measurement is as useless as zero.
        assert_eq!(lambdock_hide_margin(-5, 800, 20, 2), -798);
    }

    #[test]
    fn a_stale_measurement_from_the_other_orientation_is_discarded() {
        assert_eq!(lambdock_hide_margin(1200, 800, 20, 2), -798);
    }

    #[test]
    fn a_measurement_just_past_the_request_is_still_believed() {
        assert_eq!(lambdock_hide_margin(815, 800, 20, 2), -813);
        assert_eq!(lambdock_hide_margin(500, 800, 20, 2), -498);
    }

    #[test]
    fn a_hidden_dock_leaves_exactly_the_trigger_strip_on_screen() {
        // The strip is the whole of a tucked-away dock: it is the only thing the
        // pointer can reach, so anything less is a dock that never comes back.
        const STRIP: c_int = 4;

        for surface in [44, 72, 148, 220] {
            assert_eq!(
                surface + lambdock_hide_margin(surface, surface, 20, STRIP),
                STRIP
            );
        }
    }

    // ======================================================================
    // Settling
    // ======================================================================

    #[test]
    fn a_slide_in_flight_is_settling() {
        assert!(lambdock_settling(1, 0, 1_000_000, 300) != 0);
    }

    #[test]
    fn a_dock_that_never_slid_has_no_echo_window() {
        assert!(lambdock_settling(0, 0, 1_000_000, 300) == 0);
    }

    #[test]
    fn the_echo_window_is_exactly_as_wide_as_asked() {
        assert!(lambdock_settling(0, 1_000_000, 1_000_000, 300) != 0);
        assert!(lambdock_settling(0, 1_000_000, 1_299_000, 300) != 0);
        // Once it is over, a leave is the user.
        assert!(lambdock_settling(0, 1_000_000, 1_300_000, 300) == 0);
        assert!(lambdock_settling(0, 1_000_000, 1_400_000, 300) == 0);
    }

    // ======================================================================
    // Input region
    // ======================================================================

    #[test]
    fn a_parked_or_sliding_dock_takes_the_whole_surface() {
        // The trigger strip lives in the headroom a dock-shaped input region
        // cuts away, so cutting it while hidden would make auto-hide final.
        assert!(lambdock_dock_needs_full_input(1, 0) != 0);
        assert!(lambdock_dock_needs_full_input(1, -120) != 0);
        assert!(lambdock_dock_needs_full_input(0, -60) != 0);
    }

    #[test]
    fn a_dock_at_rest_takes_only_its_own_shape() {
        assert!(lambdock_dock_needs_full_input(0, 0) == 0);
    }

    // ======================================================================
    // Fisheye
    // ======================================================================

    #[test]
    fn the_fisheye_is_full_size_under_the_pointer() {
        let scale = lambdock_fisheye_scale(0.0, 140.0, 1.45);
        assert!((scale - 1.45).abs() < 1e-9);
    }

    #[test]
    fn there_is_no_magnification_past_the_edge_of_the_influence() {
        assert!((lambdock_fisheye_scale(140.0, 140.0, 1.45) - 1.0).abs() < 1e-9);
        assert!((lambdock_fisheye_scale(560.0, 140.0, 1.45) - 1.0).abs() < 1e-9);
    }

    #[test]
    fn the_bell_is_halfway_up_halfway_out() {
        let scale = lambdock_fisheye_scale(70.0, 140.0, 1.45);
        assert!((scale - (1.0 + 0.45 / 2.0)).abs() < 1e-9);
    }

    #[test]
    fn distance_is_a_magnitude_not_a_direction() {
        let left = lambdock_fisheye_scale(-70.0, 140.0, 1.45);
        let right = lambdock_fisheye_scale(70.0, 140.0, 1.45);
        assert!((left - right).abs() < 1e-9);
    }

    #[test]
    fn a_degenerate_radius_cannot_magnify_anything() {
        assert_eq!(lambdock_fisheye_scale(10.0, 0.0, 1.45), 1.0);
        assert_eq!(lambdock_fisheye_scale(10.0, -140.0, 1.45), 1.0);
    }

    #[test]
    fn closer_always_means_larger() {
        let near = lambdock_fisheye_scale(20.0, 140.0, 1.45);
        let middle = lambdock_fisheye_scale(60.0, 140.0, 1.45);
        let far = lambdock_fisheye_scale(120.0, 140.0, 1.45);
        assert!(near > middle);
        assert!(middle > far);
    }

    // ======================================================================
    // Sizes and movement
    // ======================================================================

    #[test]
    fn a_scaled_size_is_a_whole_number_of_pixels() {
        assert_eq!(lambdock_scaled_size(48, 1.0), 48);
        // 48 * 1.45 = 69.6, 48 * 1.1 = 52.8.
        assert_eq!(lambdock_scaled_size(48, 1.45), 70);
        assert_eq!(lambdock_scaled_size(48, 1.1), 53);
        assert_eq!(lambdock_scaled_size(0, 1.45), 0);
    }

    #[test]
    fn a_repeated_position_is_not_movement() {
        assert!(!differs(10.0, 10.0, 10.0, 10.0, 1.0));
        assert!(!differs(10.0, 10.0, 10.5, 10.5, 1.0));
        assert!(!differs(10.0, 10.0, 9.5, 9.5, 1.0));
    }

    #[test]
    fn a_whole_pixel_along_either_axis_is_movement() {
        assert!(differs(10.0, 10.0, 11.0, 10.0, 1.0));
        assert!(differs(10.0, 10.0, 10.0, 11.0, 1.0));
        assert!(differs(10.0, 10.0, 8.0, 10.0, 1.0));
    }

    #[test]
    fn a_step_of_one_or_less_means_no_quantising() {
        assert_eq!(lambdock_size_step(53, 1), 53);
        assert_eq!(lambdock_size_step(53, 0), 53);
        assert_eq!(lambdock_size_step(53, -4), 53);
    }

    #[test]
    fn sizes_round_up_to_the_next_multiple() {
        assert_eq!(lambdock_size_step(48, 2), 48);
        assert_eq!(lambdock_size_step(60, 4), 60);
        assert_eq!(lambdock_size_step(49, 2), 50);
        assert_eq!(lambdock_size_step(53, 2), 54);
        assert_eq!(lambdock_size_step(61, 4), 64);
        assert_eq!(lambdock_size_step(1, 2), 2);
    }

    #[test]
    fn quantising_halves_the_layout_passes_of_a_hover_sweep() {
        // The layout cost of a sweep is the number of *distinct* sizes the row
        // is asked to re-measure for, while the painted scale keeps changing
        // every pixel: that is what the fisheye is for.
        const BASE: c_int = 48;
        const RADIUS: f64 = 140.0;

        let (mut exact, mut stepped, mut changed_scale) = (0, 0, 0);
        let (mut previous_exact, mut previous_stepped, mut previous_scale) =
            (-1, -1, -1.0);

        let mut offset = 0;
        while offset <= RADIUS as c_int {
            let scale = lambdock_fisheye_scale(f64::from(offset), RADIUS, 1.45);
            let size = lambdock_scaled_size(BASE, scale);

            if size != previous_exact {
                exact += 1;
                previous_exact = size;
            }

            let quantised = lambdock_size_step(size, 2);
            if quantised != previous_stepped {
                stepped += 1;
                previous_stepped = quantised;
            }

            if (scale - previous_scale).abs() > 0.0 {
                changed_scale += 1;
                previous_scale = scale;
            }

            offset += 2;
        }

        assert!(stepped < exact);
        // Every pointer position still gets its own magnification.
        assert_eq!(changed_scale, (RADIUS / 2.0) as c_int + 1);
    }
}
