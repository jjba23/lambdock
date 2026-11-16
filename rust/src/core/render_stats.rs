// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! Render statistics: `src/core/render_stats.h` in Rust.
//!
//! The interesting questions about a dock's rendering -- "does hovering relayout
//! the row?", "does the surface keep its size?", "how many draws is one
//! reflection?" -- are measurements, not opinions. Every counter is a plain
//! integer bump on the GTK main thread: no allocation, no locking, no
//! formatting.

use crate::glib::g_strdup;
use std::ffi::{CStr, CString};
use std::fmt::Write as _;
use std::os::raw::{c_char, c_int};

/// The two counters the shelf bumps, by the header's enum order: a reflection
/// repaint and each band it draws inside one.
pub const REFLECTION_PAINTS: c_int = 5;
pub const REFLECTION_DRAWS: c_int = 6;

/// A surface resize (a buffer re-allocation and a compositor reconfigure), and
/// an input region that actually changed (a `wl_region`, a request and a commit).
pub const SURFACE_RESIZES: c_int = 7;
pub const INPUT_REGION_SETS: c_int = 8;

/// The counter the header calls `LAMBDOCK_STAT_COUNT`: the C enum is the
/// contract, and the order below is that enum's order.
const STAT_COUNT: usize = 10;

/// A stable name per counter, for test logs.
const NAMES: [&CStr; STAT_COUNT] = [
    c"hover_frames",
    c"hover_skips",
    c"layout_steps",
    c"geometry_refreshes",
    c"zoom_paints",
    c"reflection_paints",
    c"reflection_draws",
    c"surface_resizes",
    c"input_region_sets",
    c"hover_us",
];

/// One array, one bump per event: this runs on the frame path, so it stays a
/// plain increment. GTK drives everything from the main thread, so there is
/// nothing to synchronise -- the C version was a `static` array for the same
/// reason, and an atomic `fetch_add` would put a locked instruction on that path.
static mut COUNTERS: [u64; STAT_COUNT] = [0; STAT_COUNT];

/// The counters a `LambdockStat` names, or nothing when the value is out of
/// range (which is what the C bounds check said).
fn index(stat: c_int) -> Option<usize> {
    if (0..STAT_COUNT as c_int).contains(&stat) {
        Some(stat as usize)
    } else {
        None
    }
}

/// Clears every counter.
#[no_mangle]
pub extern "C" fn lambdock_stats_reset() {
    // One assignment rather than a loop: the counters are a fixed array, and
    // "all of them, at once" is what the reset means.
    unsafe {
        COUNTERS = [0; STAT_COUNT];
    }
}

/// Adds one to `stat`.
#[no_mangle]
pub extern "C" fn lambdock_stats_bump(stat: c_int) {
    if let Some(index) = index(stat) {
        unsafe {
            COUNTERS[index] += 1;
        }
    }
}

/// Adds `count` to `stat`.
#[no_mangle]
pub extern "C" fn lambdock_stats_add(stat: c_int, count: u64) {
    if let Some(index) = index(stat) {
        unsafe {
            COUNTERS[index] += count;
        }
    }
}

/// The current value of `stat`.
#[no_mangle]
pub extern "C" fn lambdock_stats_get(stat: c_int) -> u64 {
    index(stat).map_or(0, |index| unsafe { COUNTERS[index] })
}

/// A stable name for `stat` ("hover_frames", ...).
#[no_mangle]
pub extern "C" fn lambdock_stats_name(stat: c_int) -> *const c_char {
    index(stat).map_or(c"?".as_ptr(), |index| NAMES[index].as_ptr())
}

/// "name=value name=value ..." for every counter, for test logs.
#[no_mangle]
pub extern "C" fn lambdock_stats_describe() -> *mut c_char {
    let mut text = String::new();

    for (index, name) in NAMES.iter().enumerate() {
        if index > 0 {
            text.push(' ');
        }

        let value = unsafe { COUNTERS[index] };
        let _ = write!(text, "{}={}", name.to_string_lossy(), value);
    }

    // The caller frees this with g_free, so it is GLib's allocation.
    let text = CString::new(text).unwrap_or_default();
    unsafe { g_strdup(text.as_ptr()) }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The header's `LAMBDOCK_STAT_*` values, in the enum's order.
    const HOVER_FRAMES: c_int = 0;
    const LAYOUT_STEPS: c_int = 2;
    const HOVER_US: c_int = 9;

    fn take(pointer: *mut c_char) -> Option<String> {
        if pointer.is_null() {
            return None;
        }

        let text = unsafe { CStr::from_ptr(pointer) }
            .to_string_lossy()
            .into_owned();
        unsafe { crate::glib::g_free(pointer.cast()) };
        Some(text)
    }

    fn name(stat: c_int) -> Option<String> {
        let pointer = lambdock_stats_name(stat);
        if pointer.is_null() {
            return None;
        }

        Some(
            unsafe { CStr::from_ptr(pointer) }
                .to_string_lossy()
                .into_owned(),
        )
    }

    /// Every test starts from zero; they share one global, so they take it.
    fn reset() -> std::sync::MutexGuard<'static, ()> {
        static GUARD: std::sync::Mutex<()> = std::sync::Mutex::new(());
        let guard = GUARD
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        lambdock_stats_reset();
        guard
    }

    #[test]
    fn a_fresh_counter_reads_zero() {
        let _guard = reset();
        assert_eq!(lambdock_stats_get(HOVER_FRAMES), 0);
    }

    #[test]
    fn a_bump_adds_one_and_an_add_adds_the_count() {
        let _guard = reset();

        lambdock_stats_bump(HOVER_FRAMES);
        lambdock_stats_bump(HOVER_FRAMES);
        assert_eq!(lambdock_stats_get(HOVER_FRAMES), 2);

        lambdock_stats_add(LAYOUT_STEPS, 5);
        assert_eq!(lambdock_stats_get(LAYOUT_STEPS), 5);

        // Bumping one counter leaves the others alone.
        assert_eq!(lambdock_stats_get(HOVER_US), 0);
    }

    #[test]
    fn reset_clears_every_counter() {
        let _guard = reset();
        lambdock_stats_bump(HOVER_FRAMES);
        lambdock_stats_add(HOVER_US, 120);

        lambdock_stats_reset();

        assert_eq!(lambdock_stats_get(HOVER_FRAMES), 0);
        assert_eq!(lambdock_stats_get(HOVER_US), 0);
    }

    #[test]
    fn every_counter_in_range_has_its_own_name() {
        let _guard = reset();
        let expected = [
            "hover_frames",
            "hover_skips",
            "layout_steps",
            "geometry_refreshes",
            "zoom_paints",
            "reflection_paints",
            "reflection_draws",
            "surface_resizes",
            "input_region_sets",
            "hover_us",
        ];

        for (stat, expected) in expected.iter().enumerate() {
            assert_eq!(name(stat as c_int).as_deref(), Some(*expected));
        }
    }

    #[test]
    fn an_out_of_range_counter_is_named_and_valued_by_nothing() {
        let _guard = reset();

        for stat in [STAT_COUNT as c_int, 99, -1] {
            assert_eq!(name(stat).as_deref(), Some("?"));
            assert_eq!(lambdock_stats_get(stat), 0);
            // ... and bumping it is a no-op rather than a panic or a scribble.
            lambdock_stats_bump(stat);
            lambdock_stats_add(stat, 7);
            assert_eq!(lambdock_stats_get(stat), 0);
        }
    }

    #[test]
    fn describe_prints_every_counter_on_one_line() {
        let _guard = reset();
        lambdock_stats_bump(HOVER_FRAMES);
        lambdock_stats_add(HOVER_US, 42);

        let text = take(lambdock_stats_describe()).expect("a description");

        assert_eq!(
            text,
            "hover_frames=1 hover_skips=0 layout_steps=0 geometry_refreshes=0 \
             zoom_paints=0 reflection_paints=0 reflection_draws=0 \
             surface_resizes=0 input_region_sets=0 hover_us=42"
        );
    }
}
