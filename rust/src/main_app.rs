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
use std::os::raw::{c_char, c_double, c_int, c_void};

use crate::cstr::{cstring, text_or_empty};
use crate::dock_window::dock_apply_motion_preference;
use crate::state::LambdockState;

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

// ===========================================================================
// DIRECTORY DISCOVERY
// ===========================================================================

/// The application's own directory under a GLib-provided base directory.
fn app_dir(base: std::path::PathBuf) -> std::path::PathBuf {
    base.join("lambdock")
}

/// `~/.config/lambdock`: the settings files.
fn config_dir() -> std::path::PathBuf {
    app_dir(glib::user_config_dir())
}

/// `~/.local/state/lambdock`: the runtime state the settings are layered with.
///
/// The preferences the Preferences window remembers and the layout drag and drop
/// writes live here, not in the config directory: they are runtime state, and
/// `(lambdock layout)` and `(lambdock prefs)` name the same place.
fn state_dir() -> std::path::PathBuf {
    // GLib grew `g_get_user_state_dir` in 2.72 and the vendored bindings do not
    // expose it, so the XDG rule it implements is written out here: an absolute
    // `XDG_STATE_HOME`, else `~/.local/state`.
    let base = std::env::var_os("XDG_STATE_HOME")
        .map(std::path::PathBuf::from)
        .filter(|path| path.is_absolute())
        .or_else(|| {
            std::env::var_os("HOME")
                .map(|home| std::path::PathBuf::from(home).join(".local").join("state"))
        })
        .unwrap_or_default();

    app_dir(base)
}

/// The settings files in `directory`, as absolute paths, sorted by name.
///
/// Sorted because a dock is spawned per file: a directory listing's order is the
/// filesystem's business, and the docks should appear in the same order on every
/// run. A directory that cannot be read has no settings files -- a fresh install
/// has not made one yet -- which is the same answer the C's `g_dir_open` gave.
fn settings_files_in(directory: &std::path::Path) -> Vec<std::path::PathBuf> {
    let Ok(entries) = std::fs::read_dir(directory) else {
        return Vec::new();
    };

    let mut files: Vec<std::path::PathBuf> = entries
        .filter_map(|entry| entry.ok())
        .filter(|entry| {
            entry
                .file_name()
                .to_str()
                .is_some_and(is_settings_filename)
        })
        .map(|entry| directory.join(entry.file_name()))
        .collect();

    files.sort();

    files
}

/// Every settings file in the dock's configuration directory.
fn settings_files() -> Vec<std::path::PathBuf> {
    settings_files_in(&config_dir())
}

/// A path as a `(transfer full)` C string.
fn dup_path(path: &std::path::Path) -> *mut c_char {
    unsafe { crate::glib::g_strdup(cstring(&path.to_string_lossy()).as_ptr()) }
}

/// `~/.config/lambdock`, for the C half. `(transfer full)`.
#[no_mangle]
pub extern "C" fn get_config_dir_c() -> *mut c_char {
    dup_path(&config_dir())
}

/// `~/.local/state/lambdock`, for the C half. `(transfer full)`.
#[no_mangle]
pub extern "C" fn get_state_dir_c() -> *mut c_char {
    dup_path(&state_dir())
}

/// The settings files, as a `GList` of `g_strdup`ed paths the caller frees with
/// `g_list_free_full(list, g_free)`.
#[no_mangle]
pub extern "C" fn scan_settings_files() -> *mut crate::app_window::GList {
    extern "C" {
        fn g_list_append(list: *mut crate::app_window::GList, data: *mut c_void) -> *mut crate::app_window::GList;
    }

    let mut list: *mut crate::app_window::GList = std::ptr::null_mut();

    for path in settings_files() {
        unsafe { list = g_list_append(list, dup_path(&path).cast()) };
    }

    list
}

// ===========================================================================
// SMALL ACTION HANDLERS
// ===========================================================================

/// Every dock re-reads the desktop's motion setting.
extern "C" fn apply_motion_visitor(state: *mut LambdockState, _data: *mut c_void) {
    unsafe { dock_apply_motion_preference(state) };
}

/// The desktop's "reduce motion" (GTK's `gtk-enable-animations`) can change while
/// the dock is running, so every dock re-reads it: the next slide or hover is
/// then immediate instead of animated.
///
/// # Safety
///
/// The arguments are the signal's own and are ignored.
#[no_mangle]
pub unsafe extern "C" fn on_system_motion_changed(
    _settings: *mut glib::gobject_ffi::GObject,
    _pspec: *mut c_void,
    _user_data: *mut c_void,
) {
    crate::dock_registry::dock_registry_foreach(Some(apply_motion_visitor), std::ptr::null_mut());
}

/// Writes the dock's resolved configuration as settings-file text and opens it in
/// the user's editor, so "what is my dock actually doing" is answerable without a
/// REPL.
///
/// # Safety
///
/// `parameter` must be null or a string `GVariant` holding the dock's settings
/// path, "" for the fallback dock.
#[no_mangle]
pub unsafe extern "C" fn on_export_settings_action(
    _action: *mut gtk4::gio::ffi::GSimpleAction,
    parameter: *mut glib::ffi::GVariant,
    _user_data: *mut c_void,
) {
    let config_path = if parameter.is_null() {
        std::ptr::null()
    } else {
        glib::ffi::g_variant_get_string(parameter, std::ptr::null_mut())
    };

    let written = crate::bridge::export_dock_settings_to_file(config_path);
    if written.is_null() {
        crate::log_warn!("Main", "Could not capture the dock's settings");
        return;
    }

    let path = crate::cstr::text_or_empty(written);
    crate::log_info!("Main", "Captured the running dock in {path}");

    if crate::ui::desktop_entry::lambdock_open_path(written) == 0 {
        crate::log_warn!("Main", "Could not open {path}");
    }

    crate::glib::g_free(written.cast());
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

    /// Only settings files are picked up, sorted, and an unreadable directory
    /// has none -- which is what a fresh install looks like.
    #[test]
    fn a_directory_is_scanned_for_settings_files() {
        let root = std::env::temp_dir().join(format!("lambdock-scan-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).expect("a temporary directory");

        for name in [
            "settings-work.scm",
            "settings.scm",
            "notes.txt",
            "settings.scm.bak",
            "settings-home.scm",
        ] {
            std::fs::write(root.join(name), "").expect("a file");
        }

        let found: Vec<String> = settings_files_in(&root)
            .iter()
            .map(|path| path.file_name().unwrap().to_string_lossy().into_owned())
            .collect();

        assert_eq!(
            found,
            ["settings-home.scm", "settings-work.scm", "settings.scm"],
            "sorted, and only the two documented shapes"
        );

        std::fs::remove_dir_all(&root).expect("cleanup");
        assert!(settings_files_in(&root).is_empty(), "a directory that is gone has none");
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
