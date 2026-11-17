// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! `AppWindow`: one window of a tracked application, and the list they live in.
//!
//! It is created in two places and freed in five, and the five used to disagree
//! about NULL checks; these helpers are the ownership rules in one place, so a
//! list of windows can be freed with one call and a NULL window is always a
//! no-op.
//!
//! The record owns its title: the compositor's pointer is only valid for the
//! callback that carried it, so the text is copied once, there, and the dock
//! reads a `CString` afterwards.

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};

/// One window, in terms both backends can describe.
///
/// The trackers write it; the dock reads it through `AppWindow`.  `handle` is the
/// Wayland foreign-toplevel proxy and null on X11, `x11_window` the X11 id and
/// zero on Wayland -- which is also how a backend asks "is this record mine?".
#[derive(Default)]
pub struct LambdockToplevel {
    pub handle: *mut c_void,
    pub x11_window: std::os::raw::c_ulong,
    /// What the compositor last said, copied out of its callback.
    pub title: Option<CString>,
    pub app_id: Option<CString>,
    pub minimized: c_int,
    pub activated: c_int,
}

/// One tracked window: the handle it came from, and the title it last reported.
pub struct AppWindow {
    pub handle: *mut LambdockToplevel,
    pub title: Option<CString>,
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
}

/// Copies a title the caller does not own, when there is one.
///
/// # Safety
///
/// `title` must be null or NUL-terminated.
unsafe fn own_title(title: *const c_char) -> Option<CString> {
    title.as_ref().map(|title| CStr::from_ptr(title).to_owned())
}

/// Creates a window record for `handle`; `title` may be null.
///
/// # Safety
///
/// `handle` is borrowed and `title` must be null or a C string.
pub unsafe fn app_window_new(
    handle: *mut LambdockToplevel,
    title: *const c_char,
) -> *mut AppWindow {
    Box::into_raw(Box::new(AppWindow {
        handle,
        title: own_title(title),
        minimized: 0,
        activated: 0,
    }))
}

/// Replaces the title; a missing one is ignored, so a late compositor event
/// cannot blank a window that is already gone.
pub fn app_window_set_title(window: &mut AppWindow, title: Option<&CStr>) {
    // Titles arrive on every compositor title event, often with the same text:
    // re-allocating and re-pushing it would wake the label, the accessible name
    // and the tooltip for nothing.
    let title = title.map(CStr::to_owned);
    if window.title == title {
        return;
    }

    window.title = title;
}

/// Records the last known state, which is what the click policy reads.
pub fn app_window_set_state(window: &mut AppWindow, minimized: c_int, activated: c_int) {
    window.minimized = minimized;
    window.activated = activated;
}

/// Whether this record is the one tracking `handle`.
pub fn app_window_matches(window: &AppWindow, handle: *const LambdockToplevel) -> bool {
    std::ptr::eq(window.handle, handle)
}

/// Frees one record; null is a no-op.
///
/// # Safety
///
/// `window` must be null or a record from `app_window_new` that is not freed twice.
pub unsafe fn app_window_free(window: *mut AppWindow) {
    if window.is_null() {
        return;
    }

    drop(Box::from_raw(window));
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
pub unsafe fn app_windows_free(windows: *mut GList) {
    let mut node = windows;

    // `as_ref`, not `is_null` then a deref: the loop body then cannot see a
    // pointer it has not just proved.
    while let Some(entry) = node.as_ref() {
        app_window_free(entry.data.cast());
        node = entry.next;
    }

    g_list_free(windows);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn window(title: Option<&str>) -> *mut AppWindow {
        let title = match title {
            Some(title) => std::ffi::CString::new(title).expect("no interior NUL"),
            None => std::ffi::CString::new("").expect("empty"),
        };

        unsafe { app_window_new(std::ptr::null_mut(), title.as_ptr()) }
    }

    fn title_of(window: *mut AppWindow) -> String {
        unsafe { (*window).title.as_deref() }
            .map(|title| title.to_string_lossy().into_owned())
            .unwrap_or_default()
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
        let record = unsafe { window.as_mut() }.expect("a fresh record");
        let before = record.title.as_ref().map(|title| title.as_ptr());

        let same = std::ffi::CString::new("Inbox").expect("no interior NUL");
        app_window_set_title(record, Some(same.as_c_str()));

        // The dedupe is what keeps a title event from waking the label: the same
        // text is the same allocation.
        assert_eq!(record.title.as_ref().map(|title| title.as_ptr()), before);
    }

    #[test]
    fn a_different_title_replaces_it() {
        let window = window(Some("Inbox"));
        let record = unsafe { window.as_mut() }.expect("a fresh record");
        let next = std::ffi::CString::new("Sent").expect("no interior NUL");

        app_window_set_title(record, Some(next.as_c_str()));

        assert_eq!(record.title.as_deref(), Some(next.as_c_str()));
        unsafe { app_window_free(window) };
    }

    #[test]
    fn a_null_pointer_is_a_no_op() {
        // The free helpers are the ones that take a pointer, so they are the ones
        // that have to tolerate null.
        unsafe {
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
        let record = unsafe { window.as_ref() }.expect("a fresh record");

        app_window_set_state(unsafe { window.as_mut() }.expect("a fresh record"), 1, 0);

        assert_eq!(record.minimized, 1);
        assert_eq!(record.activated, 0);

        assert!(app_window_matches(record, handle));
        assert!(
            !app_window_matches(record, std::ptr::null_mut()),
            "a null handle matches nothing"
        );

        unsafe { app_window_free(window) };
    }

    #[test]
    fn a_list_of_windows_is_freed_with_its_records() {
        // Built the way the trackers build it: a GList of borrowed records.
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
    }
}
