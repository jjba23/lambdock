// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! CSS-class toggles: `src/ui/widget_utils.h` in Rust.
//!
//! The dock's styling is state-driven: an item gains `running-app` when a window
//! appears and loses it when the last one closes, and the class list is what the
//! stylesheets key on. These two helpers are the only place a class is set, so
//! the "does it already have it?" check lives in one place instead of at every
//! call site.

use gtk4::ffi::GtkWidget;
use gtk4::glib::translate::{from_glib_borrow, Borrowed};
use gtk4::prelude::*;
use gtk4::Widget;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int};

/// Sets or clears one CSS class, touching the widget only when it changes.
///
/// # Safety
///
/// `widget` must be null or a valid `GtkWidget`; `name` must be null or a
/// NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn lambdock_set_css_class(
    widget: *mut GtkWidget,
    name: *const c_char,
    enabled: c_int,
) {
    if widget.is_null() || name.is_null() {
        return;
    }

    // A class name that is not UTF-8 is not a class name: GTK's API takes UTF-8,
    // so passing the bytes through would be the only thing worse than dropping
    // them.
    let Ok(name) = CStr::from_ptr(name).to_str() else {
        return;
    };

    let widget: Borrowed<Widget> = from_glib_borrow(widget);

    if enabled != 0 {
        if !widget.has_css_class(name) {
            widget.add_css_class(name);
        }
    } else if widget.has_css_class(name) {
        widget.remove_css_class(name);
    }
}

/// Sets or clears the same state for every class in a null-terminated list.
///
/// # Safety
///
/// `widget` must be null or a valid `GtkWidget`; `names` must be null or point to
/// `n_names` C strings.
#[no_mangle]
pub unsafe extern "C" fn lambdock_toggle_css_classes(
    widget: *mut GtkWidget,
    names: *const *const c_char,
    n_names: usize,
    enabled: c_int,
) {
    if names.is_null() {
        return;
    }

    for index in 0..n_names {
        lambdock_set_css_class(widget, *names.add(index), enabled);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    /// Nothing here builds a widget: what the toggles *do* to a class list is
    /// covered by the C suite (=test_dock_item=, =test_accessibility=,
    /// =test_theme_engine=), which already owns real widgets and a display. What
    /// is worth pinning down in Rust is that a missing widget or list is ignored
    /// instead of dereferenced, and that needs no display at all.
    #[test]
    fn a_missing_widget_or_list_is_ignored() {
        let name = CString::new("running-app").expect("no interior NUL");
        let names = [name.as_ptr()];

        unsafe {
            lambdock_set_css_class(std::ptr::null_mut(), name.as_ptr(), 1);
            lambdock_set_css_class(std::ptr::null_mut(), std::ptr::null(), 1);
            lambdock_toggle_css_classes(std::ptr::null_mut(), names.as_ptr(), names.len(), 1);
            lambdock_toggle_css_classes(std::ptr::null_mut(), std::ptr::null(), 3, 0);
        }
    }
}
