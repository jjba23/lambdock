// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The per-dock poller budget, the Rust half of `src/core/poll_budget.h`.
//!
//! Every dynamic item is a Scheme thunk called on the GTK main thread. This is
//! the rule that stops a configuration from scheduling more of them than a dock
//! can afford: the cost of one poll is the poller's own business, how often a
//! dock may call one is the dock's.

use std::os::raw::c_int;

/// The total polls per second a list of intervals schedules.
///
/// A 1000 ms interval is one poll per second, a 2000 ms one half of that. A zero
/// interval means the item has no timer and is ignored: counting it as an
/// infinite rate would make every dock with a disabled poller look over budget.
///
/// # Safety
///
/// `intervals` must point to `count` readable `guint`s, or be null.
pub unsafe fn lambdock_polls_per_second(intervals: *const u32, count: usize) -> f64 {
    if intervals.is_null() {
        return 0.0;
    }

    std::slice::from_raw_parts(intervals, count)
        .iter()
        .filter(|&&interval| interval > 0)
        .map(|&interval| 1000.0 / f64::from(interval))
        .sum()
}

/// Whether the schedule is strictly above the allowed polls per second.
///
/// # Safety
///
/// `intervals` must point to `count` readable `guint`s, or be null.
pub unsafe fn lambdock_poll_budget_exceeded(
    intervals: *const u32,
    count: usize,
    budget: f64,
) -> c_int {
    // `gboolean` is a C `int`, not the one-byte `bool` Rust returns.
    (lambdock_polls_per_second(intervals, count) > budget) as c_int
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rate(intervals: &[u32]) -> f64 {
        unsafe { lambdock_polls_per_second(intervals.as_ptr(), intervals.len()) }
    }

    fn exceeded(intervals: &[u32], budget: f64) -> bool {
        unsafe { lambdock_poll_budget_exceeded(intervals.as_ptr(), intervals.len(), budget) != 0 }
    }

    #[test]
    fn a_one_second_interval_is_one_poll_per_second() {
        assert_eq!(rate(&[1000]), 1.0);
    }

    #[test]
    fn a_two_second_interval_is_half_a_poll_per_second() {
        assert_eq!(rate(&[2000]), 0.5);
    }

    #[test]
    fn four_pollers_add_up() {
        // Four one-second pollers schedule four calls a second between them.
        assert_eq!(rate(&[1000, 1000, 1000, 1000]), 4.0);
    }

    #[test]
    fn mixed_intervals_add_up() {
        assert_eq!(rate(&[1000, 2000, 500]), 3.5);
    }

    #[test]
    fn no_array_schedules_nothing() {
        assert_eq!(unsafe { lambdock_polls_per_second(std::ptr::null(), 4) }, 0.0);
        assert_eq!(unsafe { lambdock_polls_per_second(std::ptr::null(), 0) }, 0.0);
    }

    #[test]
    fn a_zero_interval_schedules_nothing() {
        // Zero means "no timer": counting it as an infinite rate would make
        // every dock with a disabled poller look over budget.
        assert_eq!(rate(&[0, 1000]), 1.0);
    }

    #[test]
    fn above_the_budget_is_exceeded() {
        // 10 + 5 + 4 = 19 polls per second.
        assert!(exceeded(&[100, 200, 250], 10.0));
    }

    #[test]
    fn exactly_at_the_budget_is_allowed() {
        // The rule is about exceeding the budget, so 50/s at 50/s is fine.
        assert!(!exceeded(&[100, 100, 100, 100, 100], 50.0));
    }

    #[test]
    fn under_the_budget_is_allowed() {
        assert!(!exceeded(&[2000, 2000], 12.0));
    }
}
