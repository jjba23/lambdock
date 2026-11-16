// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The layer-shell surface: `gtk4-layer-shell` as Rust sees it.
//!
//! Two questions the dock's window already answered are answered here now,
//! because the launchpad covers the screen too: *can this display host a layer
//! surface at all*, and *is the monitor a dock remembers still on the display*.
//! Both are reached through the header (`ui/dock_window.h`), so the C callers
//! read exactly what they read before and only the body moved.
//!
//! The library itself stays a C library, so its contract is declared as FFI --
//! the same choice the trackers made for libwayland and libX11.

use gtk4::gdk;
use gtk4::glib;
use gtk4::prelude::*;
use std::ffi::CStr;
use std::os::raw::c_int;

/// `GtkLayerShellLayer`: above everything, which is what an overlay wants.
pub const LAYER_OVERLAY: c_int = 3;

/// `GtkLayerShellEdge`, in the library's order.
pub const EDGE_LEFT: c_int = 0;
pub const EDGE_RIGHT: c_int = 1;
pub const EDGE_TOP: c_int = 2;
pub const EDGE_BOTTOM: c_int = 3;

/// `GtkLayerShellKeyboardMode`: the compositor hands this surface the keyboard.
pub const KEYBOARD_MODE_EXCLUSIVE: c_int = 1;

extern "C" {
    fn gtk_layer_is_supported() -> c_int;
    fn gtk_layer_init_for_window(window: *mut gtk4::ffi::GtkWindow);
    fn gtk_layer_set_namespace(window: *mut gtk4::ffi::GtkWindow, namespace: *const std::os::raw::c_char);
    fn gtk_layer_set_layer(window: *mut gtk4::ffi::GtkWindow, layer: c_int);
    fn gtk_layer_set_exclusive_zone(window: *mut gtk4::ffi::GtkWindow, zone: c_int);
    fn gtk_layer_set_keyboard_mode(window: *mut gtk4::ffi::GtkWindow, mode: c_int);
    fn gtk_layer_set_anchor(window: *mut gtk4::ffi::GtkWindow, edge: c_int, anchored: c_int);
    fn gtk_layer_set_monitor(
        window: *mut gtk4::ffi::GtkWindow,
        monitor: *mut gdk::ffi::GdkMonitor,
    );
}

/// Whether the current display is Wayland *and* the compositor implements the
/// wlr-layer-shell protocol.
///
/// `gtk_layer_is_supported()` asserts that the display is a Wayland one, so it
/// cannot be the guard on X11: it logs a critical right there, before it has a
/// chance to answer "no". The backend is therefore checked first.
#[no_mangle]
pub extern "C" fn lambdock_layer_shell_available() -> c_int {
    let Some(display) = gdk::Display::default() else {
        return 0;
    };

    if display.backend() != gdk::Backend::Wayland {
        return 0;
    }

    unsafe { gtk_layer_is_supported() }
}

/// Whether `monitor` is one of the display's monitors *right now*.
///
/// # Safety
///
/// `monitor` must be null or a `GdkMonitor` this dock was handed; it may already
/// have been destroyed by a hot unplug, which is exactly what this answers.
#[no_mangle]
pub unsafe extern "C" fn is_monitor_valid(monitor: *mut gdk::ffi::GdkMonitor) -> c_int {
    if monitor.is_null() {
        return 0;
    }

    let Some(display) = gdk::Display::default() else {
        return 0;
    };

    let monitors = display.monitors();
    for index in 0..monitors.n_items() {
        let Some(item) = monitors.item(index) else {
            continue;
        };

        // Plain pointer comparison on purpose: `monitor` may be a stale (freed)
        // monitor, and asking it for its type would dereference it.
        if item.as_ptr() == monitor.cast::<glib::gobject_ffi::GObject>() {
            return 1;
        }
    }

    0
}

/// Turns `window` into a layer surface.
///
/// # Safety
///
/// `window` must be a live `GtkWindow` of the default display.
pub unsafe fn init_for_window(window: &gtk4::Window) {
    gtk_layer_init_for_window(window.as_ptr().cast());
}

/// The namespace the compositor reports, e.g. `lambdock-launchpad`.
///
/// # Safety
///
/// `window` must be a live `GtkWindow` that is already a layer surface.
pub unsafe fn set_namespace(window: &gtk4::Window, namespace: &CStr) {
    gtk_layer_set_namespace(window.as_ptr().cast(), namespace.as_ptr());
}

/// # Safety
///
/// `window` must be a live `GtkWindow` that is already a layer surface.
pub unsafe fn set_layer(window: &gtk4::Window, layer: c_int) {
    gtk_layer_set_layer(window.as_ptr().cast(), layer);
}

/// # Safety
///
/// `window` must be a live `GtkWindow` that is already a layer surface.
pub unsafe fn set_exclusive_zone(window: &gtk4::Window, zone: c_int) {
    gtk_layer_set_exclusive_zone(window.as_ptr().cast(), zone);
}

/// # Safety
///
/// `window` must be a live `GtkWindow` that is already a layer surface.
pub unsafe fn set_keyboard_mode(window: &gtk4::Window, mode: c_int) {
    gtk_layer_set_keyboard_mode(window.as_ptr().cast(), mode);
}

/// # Safety
///
/// `window` must be a live `GtkWindow` that is already a layer surface.
pub unsafe fn set_anchor(window: &gtk4::Window, edge: c_int, anchored: bool) {
    gtk_layer_set_anchor(window.as_ptr().cast(), edge, c_int::from(anchored));
}

/// # Safety
///
/// `window` must be a live `GtkWindow` that is already a layer surface, and
/// `monitor` must be valid (see [`is_monitor_valid`]).
pub unsafe fn set_monitor(window: &gtk4::Window, monitor: *mut gdk::ffi::GdkMonitor) {
    gtk_layer_set_monitor(window.as_ptr().cast(), monitor);
}
