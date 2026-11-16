// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! Clock face geometry: `src/core/clock_face.h` in Rust.
//!
//! Where the hands point and where a squircle runs, as plain numbers. The widget
//! in `src/ui/clock.c` paints what these return, which keeps the part that can
//! be wrong in a way nobody notices -- an hour hand on the wrong minute, a
//! "squircle" that is really an ellipse -- testable without a display.

use std::f64::consts::PI;
use std::os::raw::c_int;

/// Seconds in a day: the unit the hands are derived from.
///
/// Mirrors `LAMBDOCK_SECONDS_PER_DAY` in the header; a macro cannot cross the FFI.
pub const SECONDS_PER_DAY: c_int = 86_400;

/// Hand positions for one reading, in turns clockwise from 12 o'clock.
///
/// The layout is the contract: `LambdockClockTurns` in the header is the same
/// three doubles in the same order, returned by value.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ClockTurns {
    pub hour: f64,
    pub minute: f64,
    pub second: f64,
}

/// A hand position is a fraction of a turn, and a fraction that is 1.0 wraps to
/// 0.0: the minute hand at 60 minutes belongs at the top of the face, not a full
/// turn past it.
fn wrap_turn(turn: f64) -> f64 {
    let wrapped = turn % 1.0;

    if wrapped < 0.0 {
        wrapped + 1.0
    } else {
        wrapped
    }
}

/// Where the three hands point at that moment.
///
/// The hour hand creeps forward with the minutes and the minute hand with the
/// seconds, which is what makes the face read as a clock instead of three
/// independent dials.
#[no_mangle]
pub extern "C" fn lambdock_clock_turns_for_seconds(seconds_of_day: c_int) -> ClockTurns {
    // A poll that reads the clock across midnight, or a caller whose arithmetic
    // went negative, still produces a possible face.
    let mut seconds = seconds_of_day % SECONDS_PER_DAY;
    if seconds < 0 {
        seconds += SECONDS_PER_DAY;
    }

    let hours = seconds / 3600;
    let minutes = (seconds % 3600) / 60;
    let rest = seconds % 60;

    ClockTurns {
        hour: wrap_turn(
            (f64::from(hours) + f64::from(minutes) / 60.0 + f64::from(rest) / 3600.0) / 12.0,
        ),
        minute: wrap_turn((f64::from(minutes) + f64::from(rest) / 60.0) / 60.0),
        second: wrap_turn(f64::from(rest) / 60.0),
    }
}

/// Samples the superellipse at `turn`, into whichever outputs are non-null.
///
/// The parametrisation walks the curve monotonically, so a loop over 0..1 turns
/// builds a closed path with no crossings -- which is what the bezel, the face
/// and the glass are drawn from.
///
/// # Safety
///
/// `out_x` and `out_y` must each be null or point to a writable `double`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_squircle_point(
    turn: f64,
    half_width: f64,
    half_height: f64,
    squareness: f64,
    out_x: *mut f64,
    out_y: *mut f64,
) {
    // Below 2 the curve would be star-shaped and turn the bezel inside out.
    let exponent = if squareness > 2.0 {
        squareness
    } else {
        2.0
    };
    let angle = wrap_turn(turn) * 2.0 * PI;
    let cosine = angle.cos();
    let sine = angle.sin();

    // |cos|^(2/n) with its sign kept: n = 2 gives the unit circle and a larger n
    // pushes the curve towards the corners of the bounding box. The near-zero
    // snap keeps the four vertices exactly on the axes: cos(pi/2) is 6e-17, and
    // raising that to a fractional power leaves a visible sliver at large sizes.
    let mut x = 0.0;
    let mut y = 0.0;
    if cosine.abs() > 1e-12 {
        x = cosine.abs().powf(2.0 / exponent).copysign(cosine);
    }
    if sine.abs() > 1e-12 {
        y = sine.abs().powf(2.0 / exponent).copysign(sine);
    }

    if !out_x.is_null() {
        *out_x = x * half_width;
    }
    if !out_y.is_null() {
        *out_y = y * half_height;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn turns(seconds: c_int) -> ClockTurns {
        lambdock_clock_turns_for_seconds(seconds)
    }

    fn near(actual: f64, expected: f64) {
        assert!(
            (actual - expected).abs() < 1e-9,
            "expected {expected}, got {actual}"
        );
    }

    fn squircle(turn: f64, half_width: f64, half_height: f64, squareness: f64) -> (f64, f64) {
        let (mut x, mut y) = (0.0, 0.0);
        unsafe {
            lambdock_squircle_point(turn, half_width, half_height, squareness, &mut x, &mut y);
        }
        (x, y)
    }

    // ======================================================================
    // Where the hands point
    // ======================================================================

    #[test]
    fn midnight_points_every_hand_at_the_top() {
        let midnight = turns(0);
        assert_eq!(midnight.hour, 0.0);
        assert_eq!(midnight.minute, 0.0);
        assert_eq!(midnight.second, 0.0);
    }

    #[test]
    fn a_quarter_past_three_is_a_quarter_turn() {
        let quarter = turns(3 * 3600);
        near(quarter.hour, 0.25);
        near(quarter.minute, 0.0);
    }

    #[test]
    fn noon_is_up_and_not_a_full_turn_round() {
        near(turns(12 * 3600).hour, 0.0);
    }

    #[test]
    fn half_past_midnight_is_half_way_between_twelve_and_one() {
        let half_past = turns(12 * 3600 + 1800);
        near(half_past.hour, 0.5 / 12.0);
        near(half_past.minute, 0.5);
    }

    #[test]
    fn the_hour_hand_creeps_with_the_minutes_and_the_minute_hand_with_the_seconds() {
        // 10:10:30: three independent readings that all come from one moment.
        let moment = turns(10 * 3600 + 10 * 60 + 30);
        near(moment.hour, (10.0 + 10.0 / 60.0 + 30.0 / 3600.0) / 12.0);
        near(moment.minute, (10.0 + 30.0 / 60.0) / 60.0);
        near(moment.second, 0.5);
    }

    #[test]
    fn the_last_second_of_the_day_stays_short_of_a_full_turn() {
        let last = turns(SECONDS_PER_DAY - 1);
        assert!(last.hour < 1.0 && last.hour > 0.999);
        assert!(last.minute < 1.0 && last.minute > 0.999);
        near(last.second, 59.0 / 60.0);
    }

    #[test]
    fn seconds_out_of_range_wrap_into_a_possible_face() {
        // A poll that reads the clock across midnight, and a negative reading
        // from a timezone change or a caller's arithmetic.
        assert_eq!(turns(SECONDS_PER_DAY).hour, 0.0);
        near(turns(-3600).hour, 11.0 / 12.0);
    }

    // ======================================================================
    // The squircle
    // ======================================================================

    #[test]
    fn the_squircle_passes_through_the_axes() {
        let (x, y) = squircle(0.0, 10.0, 8.0, 4.0);
        near(x, 10.0);
        near(y, 0.0);

        let (x, y) = squircle(0.25, 10.0, 8.0, 4.0);
        near(x, 0.0);
        near(y, 8.0);

        let (x, _) = squircle(0.5, 10.0, 8.0, 4.0);
        near(x, -10.0);

        let (_, y) = squircle(0.75, 10.0, 8.0, 4.0);
        near(y, -8.0);
    }

    #[test]
    fn squareness_two_is_a_circle() {
        let (x, y) = squircle(0.125, 10.0, 10.0, 2.0);
        let expected = 10.0 * std::f64::consts::SQRT_2 / 2.0;
        near(x, expected);
        near(y, expected);
    }

    #[test]
    fn a_higher_squareness_rounds_further_out_than_a_circle() {
        // This is the whole point of the shape: the curve stays nearer the
        // corner of its box instead of bowing in like an ellipse.
        let (circle_x, circle_y) = squircle(0.125, 10.0, 10.0, 2.0);
        let (square_x, square_y) = squircle(0.125, 10.0, 10.0, 4.0);

        assert!(square_x > circle_x);
        assert!(square_y > circle_y);
        assert!(square_x < 10.0);
    }

    #[test]
    fn a_squareness_below_two_is_clamped_to_a_circle() {
        // Below 2 the "squircle" would be star-shaped and turn the bezel inside
        // out.
        let (clamped, _) = squircle(0.125, 10.0, 10.0, 0.5);
        let (circle, _) = squircle(0.125, 10.0, 10.0, 2.0);
        near(clamped, circle);
    }

    #[test]
    fn a_full_turn_wraps_onto_the_start() {
        let (start_x, start_y) = squircle(0.0, 10.0, 8.0, 4.0);
        let (wrapped_x, wrapped_y) = squircle(1.0, 10.0, 8.0, 4.0);
        near(wrapped_x, start_x);
        near(wrapped_y, start_y);
    }

    #[test]
    fn a_caller_may_ask_for_one_coordinate_only() {
        let mut y = 0.0;
        unsafe {
            lambdock_squircle_point(0.25, 10.0, 8.0, 4.0, std::ptr::null_mut(), &mut y);
        }
        near(y, 8.0);
    }

    #[test]
    fn the_turn_is_wrapped_not_trusted() {
        // The bezel is drawn in a loop over 0..1 turns; a caller that overshoots
        // by a whole turn has to land on the same point.
        let (x, y) = squircle(2.25, 10.0, 8.0, 4.0);
        near(x, 0.0);
        near(y, 8.0);
    }

    #[test]
    fn the_day_length_mirrors_the_headers_macro() {
        // `LAMBDOCK_SECONDS_PER_DAY` in the header and this constant are the
        // same number, and nothing but this line keeps them so.
        assert_eq!(SECONDS_PER_DAY, 86_400);
    }
}
