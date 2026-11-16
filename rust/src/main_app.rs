// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The application: `src/main.c` in Rust, starting with the parts that are
//! decisions rather than wiring.
//!
//! `main.c` is the last C file in the project, and it is three kinds of code at
//! once: discovery (which settings files exist, which monitor they name), the
//! docks that follow from them, and the glue that starts GTK, Guile and the file
//! watchers. This module takes the discovery and its decisions first -- they are
//! values, so they are testable -- and the glue follows them file by file.
//!
//! The rule kept here: a `#[no_mangle]` entry point is thin and unsafe only where
//! it must be; the work happens in functions that take `&str`, `&[]` and return
//! values.

use gtk4::gdk;
use gtk4::glib;
use gtk4::glib::translate::IntoGlib;
use gtk4::prelude::*;
use std::os::raw::{c_char, c_double, c_int};

use crate::cstr::text_or_empty;

/// The names a settings file can have: `settings.scm`, and the per-dock
/// `settings-<name>.scm`.
const SETTINGS_STEM: &str = "settings";

/// The suffix every settings file carries.
const SETTINGS_EXTENSION: &str = "scm";

/// Whether `filename` is one of the dock's settings files.
///
/// `settings.scm` is the default dock; `settings-<name>.scm` is a named one.
/// Anything else -- a backup, an editor's swap file, a directory's other
/// content -- is not a dock.
fn is_settings_filename(filename: &str) -> bool {
    let Some(stem) = filename.strip_suffix(&format!(".{SETTINGS_EXTENSION}")) else {
        return false;
    };

    stem == SETTINGS_STEM || stem.starts_with(&format!("{SETTINGS_STEM}-"))
}

/// Whether `filename` is one of the dock's settings files.
///
/// # Safety
///
/// `filename` must be null or NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn is_settings_filename_c(filename: *const c_char) -> c_int {
    if filename.is_null() {
        return 0;
    }

    c_int::from(is_settings_filename(&text_or_empty(filename)))
}

/// The scroll offset a wheel event asks for.
///
/// One notch is thirty pixels, and the result is clamped to the scrollable band
/// so a wheel that keeps turning at the end of the row does not wind the
/// adjustment past its own bounds (GTK would clamp too, but the value is also
/// what a smooth-scroll animation would start from).
fn scrolled_value(value: f64, delta: f64, page_size: f64, upper: f64, lower: f64) -> f64 {
    let max_scroll = upper - page_size;

    (value + delta * 30.0).clamp(lower, max_scroll.max(lower))
}

/// Mouse wheel over the dock: scrolls the row along its main axis.
///
/// The wheel moves the dock's own viewport, not the window: a horizontal dock
/// scrolls on the dominant axis of the event, and a vertical one on `dy`, which
/// is what a wheel on a vertical dock sends.
///
/// # Safety
///
/// `user_data` must be null or a live dock with its scrolled window built.
#[no_mangle]
pub unsafe extern "C" fn on_dock_scroll(
    _controller: *mut gtk4::ffi::GtkEventControllerScroll,
    dx: c_double,
    dy: c_double,
    user_data: *mut std::os::raw::c_void,
) -> c_int {
    let Some(dock) = user_data.cast::<crate::state::LambdockState>().as_ref() else {
        return 0;
    };
    if dock.scrolled_window.is_null() {
        return 0;
    }

    let scrolled: gtk4::ScrolledWindow = gtk4::glib::translate::from_glib_none(dock.scrolled_window.cast());

    let horizontal = dock.orientation == gtk4::Orientation::Horizontal.into_glib();
    let adjustment = if horizontal {
        scrolled.hadjustment()
    } else {
        scrolled.vadjustment()
    };

    if dock.scrolled_window.is_null() {
        return 0;
    }

    // A horizontal dock is scrolled by whichever way the wheel went; a vertical
    // one only by the vertical component.
    let delta = if horizontal && dx.abs() > dy.abs() { dx } else { dy };

    let value = scrolled_value(
        adjustment.value(),
        delta,
        adjustment.page_size(),
        adjustment.upper(),
        adjustment.lower(),
    );
    adjustment.set_value(value);

    c_int::from(true)
}

/// The monitor whose connector is `connector`, if the display has one.
///
/// # Safety
///
/// `connector` must be null or NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn find_monitor_by_connector(
    connector: *const c_char,
) -> *mut gdk::ffi::GdkMonitor {
    if connector.is_null() {
        return std::ptr::null_mut();
    }

    let Some(connector) = crate::cstr::non_empty_text(connector) else {
        return std::ptr::null_mut();
    };

    let Some(display) = gdk::Display::default() else {
        return std::ptr::null_mut();
    };

    let monitors = display.monitors();
    for index in 0..monitors.n_items() {
        let Some(item) = monitors.item(index) else {
            continue;
        };
        let Some(monitor) = item.downcast_ref::<gdk::Monitor>() else {
            continue;
        };

        if monitor.connector().as_deref() == Some(connector.as_str()) {
            // `(transfer full)`: the caller owns the reference it asked for.
            return glib::gobject_ffi::g_object_ref(monitor.as_ptr().cast()).cast();
        }
    }

    std::ptr::null_mut()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Only the two documented shapes are settings files.
    #[test]
    fn a_settings_file_is_named_for_a_dock() {
        assert!(is_settings_filename("settings.scm"));
        assert!(is_settings_filename("settings-work.scm"));
        assert!(is_settings_filename("settings-home-2.scm"));

        // Not settings files: a backup, an editor's leftovers, a prefix, a
        // different extension, and the empty name.
        assert!(!is_settings_filename("settings.scm~"));
        assert!(!is_settings_filename("settings.scm.bak"));
        assert!(!is_settings_filename("settings-.scm.bak"));
        assert!(!is_settings_filename("mysettings.scm"));
        assert!(!is_settings_filename("settings"));
        assert!(!is_settings_filename(""));
        assert!(!is_settings_filename("settings-.scm."));
    }

    /// One notch is thirty pixels, and the offset never leaves the band.
    #[test]
    fn the_wheel_moves_the_row_and_stops_at_the_ends() {
        // A notch down, from the start: thirty pixels in.
        assert_eq!(scrolled_value(0.0, 1.0, 100.0, 400.0, 0.0), 30.0);
        assert_eq!(scrolled_value(300.0, -1.0, 100.0, 400.0, 0.0), 270.0);

        // A wheel that keeps turning at either end stays at the end.
        assert_eq!(scrolled_value(0.0, -5.0, 100.0, 400.0, 0.0), 0.0);
        assert_eq!(scrolled_value(300.0, 5.0, 100.0, 400.0, 0.0), 300.0);

        // A row that does not scroll at all (nothing above the page) is a
        // fixed zero rather than a negative offset.
        assert_eq!(scrolled_value(0.0, 1.0, 100.0, 100.0, 0.0), 0.0);
        assert_eq!(scrolled_value(0.0, 1.0, 100.0, 40.0, 10.0), 10.0);
    }
}
