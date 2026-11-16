// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! `AppWindow` ownership: `src/app_window.c` in Rust.
//!
//! One window of a tracked application: a toplevel handle plus its title. It is
//! created in two places and freed in five, and the five used to disagree about
//! NULL checks; these helpers are the ownership rules in one place, so a list of
//! windows can be freed with one call and a NULL window is always a no-op.

use crate::glib::{g_free, g_malloc0, g_strdup};
use std::os::raw::{c_char, c_int, c_void};

/// A toplevel handle, opaque here: the tracker owns it and this only compares
/// pointers.
#[repr(C)]
pub struct LambdockToplevel {
    _private: [u8; 0],
}

/// The record, as `lambdock.h` declares it: the field order is the contract.
#[repr(C)]
pub struct AppWindow {
    pub handle: *mut LambdockToplevel,
    pub title: *mut c_char,
    pub minimized: c_int,
    pub activated: c_int,
}

/// GLib's list node, so the C lists the dock builds can be walked and freed.
#[repr(C)]
pub struct GList {
    pub data: *mut c_void,
    pub next: *mut GList,
    pub prev: *mut GList,
}

extern "C" {
    fn g_list_free(list: *mut GList);

    /// Compares two C strings, with NULL equal to NULL -- the comparison the
    /// title setter needs.
    fn g_strcmp0(left: *const c_char, right: *const c_char) -> c_int;
}

/// Creates a window record for `handle`; `title` may be null.
///
/// # Safety
///
/// `handle` is borrowed and `title` must be null or a C string.
#[no_mangle]
pub unsafe extern "C" fn app_window_new(
    handle: *mut LambdockToplevel,
    title: *const c_char,
) -> *mut AppWindow {
    let window = g_malloc0(std::mem::size_of::<AppWindow>()).cast::<AppWindow>();

    (*window).handle = handle;
    (*window).title = g_strdup(title);

    window
}

/// Replaces the title; a null window or title is ignored, so a late compositor
/// event cannot blank or crash a window that is already gone.
///
/// # Safety
///
/// `window` must be null or a live `AppWindow`; `title` must be null or a C string.
#[no_mangle]
pub unsafe extern "C" fn app_window_set_title(window: *mut AppWindow, title: *const c_char) {
    if window.is_null() || title.is_null() {
        return;
    }

    // Titles arrive on every compositor title event, often with the same text:
    // re-allocating and re-pushing it would wake the label, the accessible name
    // and the tooltip for nothing.
    if g_strcmp0((*window).title, title) == 0 {
        return;
    }

    g_free((*window).title.cast());
    (*window).title = g_strdup(title);
}

/// Records the last known state, which is what the click policy reads.
///
/// # Safety
///
/// `window` must be null or a live `AppWindow`.
#[no_mangle]
pub unsafe extern "C" fn app_window_set_state(
    window: *mut AppWindow,
    minimized: c_int,
    activated: c_int,
) {
    if window.is_null() {
        return;
    }

    (*window).minimized = minimized;
    (*window).activated = activated;
}

/// Whether this record is the one tracking `handle`.
///
/// # Safety
///
/// `window` must be null or a live `AppWindow`.
#[no_mangle]
pub unsafe extern "C" fn app_window_matches(
    window: *const AppWindow,
    handle: *const LambdockToplevel,
) -> c_int {
    (!window.is_null() && (*window).handle == handle as *mut LambdockToplevel) as c_int
}

/// Frees one record; null is a no-op.
///
/// # Safety
///
/// `window` must be null or a record from `app_window_new` that is not freed twice.
#[no_mangle]
pub unsafe extern "C" fn app_window_free(window: *mut AppWindow) {
    if window.is_null() {
        return;
    }

    g_free((*window).title.cast());
    g_free(window.cast());
}

/// Frees every record in a `GList` and the list itself.
///
/// The caller keeps ownership of the list pointer and should pass null after
/// calling this.
///
/// # Safety
///
/// `windows` must be null or a list whose data are all `AppWindow*` from
/// `app_window_new`.
#[no_mangle]
pub unsafe extern "C" fn app_windows_free(windows: *mut GList) {
    let mut node = windows;

    while !node.is_null() {
        app_window_free((*node).data.cast());
        node = (*node).next;
    }

    g_list_free(windows);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::glib::g_malloc0;

    fn window(title: Option<&str>) -> *mut AppWindow {
        let title = match title {
            Some(title) => std::ffi::CString::new(title).expect("no interior NUL"),
            None => std::ffi::CString::new("").expect("empty"),
        };

        unsafe { app_window_new(std::ptr::null_mut(), title.as_ptr()) }
    }

    fn title_of(window: *mut AppWindow) -> String {
        unsafe { std::ffi::CStr::from_ptr((*window).title) }
            .to_string_lossy()
            .into_owned()
    }

    #[test]
    fn a_new_window_carries_its_title_and_no_state() {
        let window = window(Some("Inbox"));

        assert_eq!(title_of(window), "Inbox");
        unsafe {
            assert_eq!((*window).minimized, 0);
            assert_eq!((*window).activated, 0);
            app_window_free(window);
        }
    }

    #[test]
    fn setting_the_same_title_keeps_the_same_allocation() {
        let window = window(Some("Inbox"));
        let before = unsafe { (*window).title };

        let same = std::ffi::CString::new("Inbox").expect("no interior NUL");
        unsafe { app_window_set_title(window, same.as_ptr()) };

        // The dedupe is what keeps a title event from waking the label.
        assert_eq!(unsafe { (*window).title }, before);

        unsafe { app_window_free(window) };
    }

    #[test]
    fn a_different_title_replaces_it() {
        let window = window(Some("Inbox"));
        let next = std::ffi::CString::new("Sent").expect("no interior NUL");

        unsafe { app_window_set_title(window, next.as_ptr()) };

        assert_eq!(title_of(window), "Sent");
        unsafe { app_window_free(window) };
    }

    #[test]
    fn a_null_window_or_title_is_ignored() {
        let title = std::ffi::CString::new("Inbox").expect("no interior NUL");

        unsafe {
            app_window_set_title(std::ptr::null_mut(), title.as_ptr());
            app_window_set_state(std::ptr::null_mut(), 1, 1);
            app_window_free(std::ptr::null_mut());
            app_windows_free(std::ptr::null_mut());
        }
    }

    #[test]
    fn state_is_recorded_and_matched_by_handle() {
        let mut handle = 0u8;
        let handle = (&mut handle) as *mut u8 as *mut LambdockToplevel;
        let title = std::ffi::CString::new("Inbox").expect("no interior NUL");

        let window = unsafe { app_window_new(handle, title.as_ptr()) };

        unsafe {
            app_window_set_state(window, 1, 0);
            assert_eq!((*window).minimized, 1);
            assert_eq!((*window).activated, 0);

            assert_ne!(app_window_matches(window, handle), 0);
            assert_eq!(
                app_window_matches(window, std::ptr::null_mut()),
                0,
                "a null handle matches nothing"
            );
            assert_eq!(app_window_matches(std::ptr::null(), handle), 0);

            app_window_free(window);
        }
    }

    #[test]
    fn a_list_of_windows_is_freed_with_its_records() {
        // Built the way the C side builds it: a GList of borrowed records.
        extern "C" {
            fn g_list_append(list: *mut GList, data: *mut c_void) -> *mut GList;
        }

        let mut list: *mut GList = std::ptr::null_mut();
        for title in ["One", "Two"] {
            let record = window(Some(title));
            unsafe { list = g_list_append(list, record.cast()) };
        }

        assert!(!list.is_null());
        unsafe { app_windows_free(list) };

        // The allocation the test made above, so the helper is not the only
        // caller of g_malloc0 the compiler keeps.
        unsafe { g_free(g_malloc0(8)) };
    }
}
