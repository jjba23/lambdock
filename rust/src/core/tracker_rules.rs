// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The foreign-toplevel tracker's decisions: `src/core/tracker_rules.h` in Rust.
//!
//! These used to be conditions buried inside the Wayland protocol listeners,
//! where the only way to test them was to fake a compositor. `toplevel_tracker.c`
//! reads the state, asks here, and then does the widget work.

use crate::glib::tr;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int};

/// Ways the compositor spells "this is the dock itself". Tracking one of these
/// would put an icon for the dock inside the dock.
const OWN_APP_IDS: [&str; 2] = ["lambdock", "org.jointhefreeworld.lambdock"];

/// Whether @app_id carries no tracking information.
///
/// # Safety
///
/// `app_id` must be null or a NUL-terminated string.
pub unsafe fn lambdock_tracker_ignores_app_id(app_id: *const c_char) -> c_int {
    if app_id.is_null() {
        return 1;
    }

    let app_id = CStr::from_ptr(app_id).to_bytes();
    if app_id.is_empty() {
        return 1;
    }

    // ASCII-only folding, because the ids are byte strings that need not be
    // UTF-8 and g_ascii_strcasecmp was ASCII-only too.
    let own_window = OWN_APP_IDS
        .iter()
        .any(|own| app_id.eq_ignore_ascii_case(own.as_bytes()));

    own_window as c_int
}

/// Whether an item that exists only because a window did should go.
pub fn lambdock_tracker_item_should_be_dropped(
    has_windows: c_int,
    is_favorite: c_int,
) -> c_int {
    (has_windows == 0 && is_favorite == 0) as c_int
}

/// Whether a window just appeared and the dock should slide away.
pub fn lambdock_tracker_should_hide(
    auto_hide: c_int,
    show_when_clear: c_int,
    has_tracked_windows: c_int,
) -> c_int {
    (auto_hide != 0 && show_when_clear != 0 && has_tracked_windows != 0) as c_int
}

/// Whether the last tracked window closed and the dock should come back.
pub fn lambdock_tracker_should_reveal(
    show_when_clear: c_int,
    has_tracked_windows: c_int,
) -> c_int {
    (show_when_clear != 0 && has_tracked_windows == 0) as c_int
}

/// The sentence the dock shows while the compositor's window list is gone.
///
/// A restart drops every proxy; the tracker binds the new manager when it is
/// announced.
///
/// # Safety
///
/// Returns a pointer owned by gettext, or null when there is nothing to announce.
pub fn lambdock_tracker_status_text(connected: c_int) -> *const c_char {
    // NULL means "connected: show nothing", so the caller can use the result
    // directly as "is there something to say?".
    if connected != 0 {
        return std::ptr::null();
    }

    // The msgid is the one already in po/*.po, so the translations keep working.
    tr(c"Reconnecting…")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ignores(app_id: Option<&'static [u8]>) -> bool {
        let app_id = match app_id {
            Some(bytes) => bytes.as_ptr() as *const c_char,
            None => std::ptr::null(),
        };

        unsafe { lambdock_tracker_ignores_app_id(app_id) != 0 }
    }

    fn status(connected: bool) -> Option<String> {
        let text = lambdock_tracker_status_text(connected as c_int);
        if text.is_null() {
            return None;
        }

        Some(unsafe { CStr::from_ptr(text) }.to_string_lossy().into_owned())
    }

    #[test]
    fn an_unset_app_id_is_not_tracked() {
        assert!(ignores(None));
        assert!(ignores(Some(b"\0")));
    }

    #[test]
    fn the_docks_own_windows_are_not_tracked() {
        assert!(ignores(Some(b"lambdock\0")));
        assert!(ignores(Some(b"LambDock\0")));
        assert!(ignores(Some(b"org.jointhefreeworld.lambdock\0")));
        assert!(ignores(Some(b"org.jointhefreeworld.Lambdock\0")));
    }

    #[test]
    fn everyone_else_is_tracked() {
        assert!(!ignores(Some(b"org.gnome.Nautilus\0")));
        assert!(!ignores(Some(b"firefox\0")));
        // A similar id is a different application.
        assert!(!ignores(Some(b"lambdock-helper\0")));
    }

    #[test]
    fn an_item_with_no_window_is_dropped_unless_it_is_pinned() {
        assert!(lambdock_tracker_item_should_be_dropped(0, 0) != 0);
        assert!(lambdock_tracker_item_should_be_dropped(0, 1) == 0);
        assert!(lambdock_tracker_item_should_be_dropped(1, 1) == 0);
        assert!(lambdock_tracker_item_should_be_dropped(1, 0) == 0);
    }

    #[test]
    fn the_dock_hides_only_when_both_settings_ask_for_it() {
        assert!(lambdock_tracker_should_hide(1, 1, 1) != 0);
        assert!(lambdock_tracker_should_hide(0, 1, 1) == 0);
        assert!(lambdock_tracker_should_hide(1, 0, 1) == 0);
        assert!(lambdock_tracker_should_hide(1, 1, 0) == 0);
    }

    #[test]
    fn the_dock_reveals_only_for_settings_that_hide_it() {
        assert!(lambdock_tracker_should_reveal(1, 0) != 0);
        assert!(lambdock_tracker_should_reveal(1, 1) == 0);
        assert!(lambdock_tracker_should_reveal(0, 0) == 0);
    }

    #[test]
    fn there_is_nothing_to_say_while_the_window_list_is_tracked() {
        assert_eq!(status(true), None);
    }

    #[test]
    fn a_compositor_restart_says_reconnecting() {
        assert_eq!(status(false).as_deref(), Some("Reconnecting…"));
    }
}
