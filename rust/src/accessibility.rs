// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! Accessibility: `src/accessibility.c` in Rust.
//!
//! Two jobs: decide whether the dock may animate (the user's setting, the
//! desktop's `gtk-enable-animations`, or neither), and push an item's label and
//! description to the accessibility bus only when they changed.

use crate::glib::{g_free, g_strcmp0, g_strdup};
use crate::state::LambdockState;
use gtk4::glib::translate::from_glib_borrow;
use gtk4::prelude::*;
use gtk4::accessible::Property;
use gtk4::{Accessible, AccessibleAnnouncementPriority, Settings, Widget};
use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_uint, c_void};
use std::sync::atomic::{AtomicBool, Ordering};

/// Cached `gtk-enable-animations`: this is read once per animation frame, and a
/// property lookup by name is avoidable work on the one path with a frame
/// budget. The notify handler below refreshes it.
static ANIMATIONS_ENABLED: AtomicBool = AtomicBool::new(true);
static ANIMATIONS_QUERIED: AtomicBool = AtomicBool::new(false);
static ANIMATIONS_WATCHED: AtomicBool = AtomicBool::new(false);

/// The per-widget cache of what was last pushed, so identical properties are not
/// re-sent: that is pure AT-SPI traffic that can make a screen reader stutter.
const LABEL_KEY: &CStr = c"lambdock-a11y-label";
const DESCRIPTION_KEY: &CStr = c"lambdock-a11y-description";

extern "C" {
    fn g_object_get_data(object: *mut c_void, key: *const c_char) -> *mut c_void;
    fn g_object_set_data_full(
        object: *mut c_void,
        key: *const c_char,
        data: *mut c_void,
        destroy: Option<unsafe extern "C" fn(*mut c_void)>,
    );

    /// C's `GCallback` is `void (*)(void)` only because C cannot say better: what
    /// a `notify::` handler really receives is the settings object, the property
    /// spec and the data it was connected with, so that is the type declared
    /// here. The ABI is a function pointer either way, and the one place that
    /// relies on it is this file.
    fn g_signal_connect_data(
        instance: *mut c_void,
        detailed_signal: *const c_char,
        handler: Option<unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void)>,
        data: *mut c_void,
        destroy: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
        flags: c_int,
    ) -> std::os::raw::c_ulong;
}

/// The settings object for the default display, or nothing when there is no
/// display yet.
///
/// `gtk_settings_get_default` rather than `gtk4::Settings::default()`: the gtk-rs
/// constructor asserts that *gtk-rs* initialized GTK, which C's `gtk_init` does
/// not do, and the C code tolerates a null settings object anyway.
fn default_settings() -> Option<gtk4::glib::translate::Borrowed<Settings>> {
    let settings = unsafe { gtk4::ffi::gtk_settings_get_default() };

    if settings.is_null() {
        None
    } else {
        Some(unsafe { from_glib_borrow(settings) })
    }
}

/// Whether the desktop asks for animations.
pub fn lambdock_system_animations_enabled() -> c_int {
    if ANIMATIONS_QUERIED.load(Ordering::Relaxed) {
        return ANIMATIONS_ENABLED.load(Ordering::Relaxed) as c_int;
    }

    // No settings object (no display yet) keeps the documented default, which is
    // "animations on".
    let enabled = default_settings()
        .map(|settings| settings.property::<bool>("gtk-enable-animations"))
        .unwrap_or(true);

    ANIMATIONS_ENABLED.store(enabled, Ordering::Relaxed);
    ANIMATIONS_QUERIED.store(true, Ordering::Relaxed);

    enabled as c_int
}

/// Whether the dock should reduce its motion: the dock's setting first, then the
/// desktop's.
pub fn lambdock_reduce_motion(state: &LambdockState) -> c_int {
    if state.reduce_motion != 0 {
        return 1;
    }

    (lambdock_system_animations_enabled() == 0) as c_int
}

/// Whether the dock may animate.
pub fn lambdock_animations_enabled(state: &LambdockState) -> c_int {
    (lambdock_reduce_motion(state) == 0) as c_int
}

/// Pushes `label` and `description` to the widget's accessible, when they are not
/// already there.
///
/// # Safety
///
/// `widget` must be null or a valid `GtkWidget`; the strings must be null or
/// NUL-terminated.
pub unsafe fn lambdock_accessible_update(
    widget: *mut gtk4::ffi::GtkWidget,
    label: *const c_char,
    description: *const c_char,
) {
    if widget.is_null() {
        return;
    }

    let widget: gtk4::glib::translate::Borrowed<Widget> = from_glib_borrow(widget);
    if !widget.is::<Accessible>() {
        return;
    }

    let object = widget.as_ptr().cast::<c_void>();
    let last_label = g_object_get_data(object, LABEL_KEY.as_ptr());
    let last_description = g_object_get_data(object, DESCRIPTION_KEY.as_ptr());

    if g_strcmp0(last_label.cast(), label) == 0 && g_strcmp0(last_description.cast(), description) == 0
    {
        return;
    }

    g_object_set_data_full(object, LABEL_KEY.as_ptr(), g_strdup(label).cast(), Some(g_free));
    g_object_set_data_full(
        object,
        DESCRIPTION_KEY.as_ptr(),
        g_strdup(description).cast(),
        Some(g_free),
    );

    // A null label is not a label; the callers always have one.
    let label = if label.is_null() {
        std::borrow::Cow::Borrowed("")
    } else {
        CStr::from_ptr(label).to_string_lossy()
    };

    let accessible: &Accessible = widget.upcast_ref();

    if description.is_null() {
        accessible.update_property(&[Property::Label(&label)]);
    } else {
        let description = CStr::from_ptr(description).to_string_lossy();
        accessible.update_property(&[Property::Label(&label), Property::Description(&description)]);
    }
}

/// Announces `message` to a screen reader, politely or assertively.
///
/// # Safety
///
/// `widget` must be null or a valid `GtkWidget`; `message` must be null or
/// NUL-terminated.
pub unsafe fn lambdock_accessible_announce(
    widget: *mut gtk4::ffi::GtkWidget,
    message: *const c_char,
    assertive: c_int,
) {
    if widget.is_null() || message.is_null() || *message == 0 {
        return;
    }

    let widget: gtk4::glib::translate::Borrowed<Widget> = from_glib_borrow(widget);
    if !widget.is::<Accessible>() {
        return;
    }

    let message = CStr::from_ptr(message).to_string_lossy();
    let priority = if assertive != 0 {
        AccessibleAnnouncementPriority::High
    } else {
        AccessibleAnnouncementPriority::Low
    };

    let accessible: &Accessible = widget.upcast_ref();
    accessible.announce(&message, priority);
}

/// Drops the cached `gtk-enable-animations` so the next read is fresh.
///
/// GLib calls this with the settings object, the property spec and the data
/// pointer it was connected with; the dock's setting is read from the new value
/// elsewhere, so all three are ignored.
unsafe extern "C" fn animations_changed(
    _settings: *mut c_void,
    _pspec: *mut c_void,
    _data: *mut c_void,
) {
    ANIMATIONS_QUERIED.store(false, Ordering::Relaxed);
}

/// The signature GLib calls a `notify::` handler with.
pub type MotionListener = unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void);

/// Watches the desktop's animation setting.
///
/// One cache-invalidating handler is installed per settings object for the
/// lifetime of the process -- the dock reads the value every animation frame, so
/// it must not go stale when the desktop toggles animations -- and `callback` is
/// connected on top of it.
///
/// # Safety
///
/// `callback` must be null or a valid `notify::` handler for the lifetime of the
/// process.
pub unsafe fn lambdock_watch_system_motion(
    callback: Option<MotionListener>,
    data: *mut c_void,
) -> c_uint {
    let Some(settings) = default_settings() else {
        return 0;
    };

    let instance = settings.as_ptr().cast::<c_void>();
    let signal = c"notify::gtk-enable-animations".as_ptr();

    if !ANIMATIONS_WATCHED.swap(true, Ordering::Relaxed) {
        g_signal_connect_data(
            instance,
            signal,
            Some(animations_changed),
            std::ptr::null_mut(),
            None,
            0,
        );
    }

    let Some(callback) = callback else {
        return 0;
    };

    g_signal_connect_data(instance, signal, Some(callback), data, None, 0) as c_uint
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A zeroed state is what a partially built dock or a unit test has: no
    /// reduce-motion setting of its own.
    fn empty_state() -> Box<LambdockState> {
        Box::new(LambdockState::default())
    }

    #[test]
    fn the_docks_own_setting_reduces_motion() {
        let mut state = empty_state();
        state.reduce_motion = 1;

        assert_eq!(lambdock_reduce_motion(&state), 1);
        assert_eq!(lambdock_animations_enabled(&state), 0);
    }

    #[test]
    fn a_dock_without_the_setting_follows_the_desktop() {
        // No display in a unit test, so there is no settings object: the
        // documented default is "animations on".
        let state = empty_state();

        assert_eq!(lambdock_system_animations_enabled(), 1);
        assert_eq!(lambdock_reduce_motion(&state), 0);
        assert_eq!(lambdock_animations_enabled(&state), 1);
    }

    #[test]
    fn a_null_widget_or_message_is_ignored() {
        unsafe {
            lambdock_accessible_update(std::ptr::null_mut(), std::ptr::null(), std::ptr::null());
            lambdock_accessible_announce(std::ptr::null_mut(), std::ptr::null(), 0);
        }
    }
}
