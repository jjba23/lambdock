// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The C→Scheme config readers: the first half of `src/guile_bridge.c`.
//!
//! Every function here asks a procedure in `(lambdock core)` a question about one
//! dock's configuration and converts the answer to a C value. Strings leave as
//! `g_strdup` allocations so the callers free them with `g_free`; numbers go
//! through the sanitisers in `core/config_values.rs`, so a broken or hostile
//! settings file can never push garbage into the dock's geometry.
//!
//! Nothing here stores an `Scm`: a value is converted the moment it arrives, so
//! the collector never has to be told about a Rust-side owner.

use crate::core::config_values::{
    lambdock_capsule_padding_sanitize, lambdock_dock_mode_is_taskbar, lambdock_edge_from_name,
    lambdock_icon_size_sanitize, lambdock_item_spacing_sanitize,
    lambdock_launchpad_close_on_right, lambdock_launchpad_columns_sanitize,
    lambdock_launchpad_icon_size_sanitize, lambdock_shelf_params_default,
    lambdock_shelf_params_sanitize, ShelfParams, ShelfRawValues, CAPSULE_PADDING_PX,
    DEFAULT_ICON_SIZE, ITEM_SPACING_PX, LAUNCHPAD_COLUMNS_AUTO, LAUNCHPAD_ICON_SIZE,
};
use crate::glib::{g_free, g_strdup};
use crate::guile::{
    free, scm_c_public_ref, scm_call_0, scm_call_1, scm_car, scm_cdr, scm_from_utf8_string,
    is_symbol, is_true, scm_is_pair, scm_is_real, scm_is_string, scm_nan_p,
    scm_procedure_p, scm_symbol_to_string, scm_to_bool, scm_to_double, scm_to_utf8_string, Scm,
};
use std::ffi::CStr;
use std::os::raw::{c_char, c_int};

/// The module every setting is read from.
const CORE_MODULE: &CStr = c"lambdock core";

/// Calls `procedure` with the config path, or without an argument when there is
/// none: the dock's default configuration is the one registered under `'default`.
unsafe fn call_with_path(procedure: Scm, path: *const c_char) -> Scm {
    if !path.is_null() && *path != 0 {
        scm_call_1(procedure, scm_from_utf8_string(path))
    } else {
        scm_call_0(procedure)
    }
}

/// Calls `name` in `(lambdock core)` with the config path, or nothing when the
/// procedure does not exist (an old or partial Scheme side).
unsafe fn call_core(name: &CStr, path: *const c_char) -> Option<Scm> {
    let procedure = scm_c_public_ref(CORE_MODULE.as_ptr(), name.as_ptr());

    if !is_true(procedure) {
        return None;
    }

    Some(call_with_path(procedure, path))
}

/// A Scheme predicate's answer as a C boolean; anything unreadable is false.
unsafe fn bool_for(name: &CStr, path: *const c_char) -> c_int {
    match call_core(name, path) {
        Some(result) if is_true(result) => scm_to_bool(result),
        _ => 0,
    }
}

/// A Scheme number, or `fallback` when the answer is missing, not a real number,
/// or NaN.
unsafe fn number_for(name: &CStr, path: *const c_char, fallback: f64) -> f64 {
    match call_core(name, path) {
        Some(result) if scm_is_real(result) != 0 && !is_true(scm_nan_p(result)) => {
            scm_to_double(result)
        }
        _ => fallback,
    }
}

/// A Scheme string or symbol as a `g_strdup`ed C string, or `default` when the
/// Scheme side has nothing usable. A null `default` stays null.
unsafe fn string_for(
    name: &CStr,
    module: Option<&CStr>,
    path: *const c_char,
    default: *const c_char,
) -> *mut c_char {
    let module = module.unwrap_or(CORE_MODULE);
    let procedure = scm_c_public_ref(module.as_ptr(), name.as_ptr());

    if !is_true(procedure) {
        return g_strdup(default);
    }

    let result = call_with_path(procedure, path);
    if !is_true(result) {
        return g_strdup(default);
    }

    let value = if is_symbol(result) {
        scm_symbol_to_string(result)
    } else {
        result
    };

    if scm_is_string(value) != 0 {
        // Guile hands back memory it malloc'ed; the copy that leaves this module
        // is GLib's, so the caller never has to know the difference.
        let text = scm_to_utf8_string(value);
        let copy = if !text.is_null() && *text != 0 {
            g_strdup(text)
        } else {
            std::ptr::null_mut()
        };
        free(text.cast());

        if !copy.is_null() {
            return copy;
        }
    }

    g_strdup(default)
}

// ===========================================================================
// The dock's settings
// ===========================================================================

/// Whether auto-hiding is enabled for this dock.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_autohide_config_for(config_path: *const c_char) -> c_int {
    bool_for(c"get-dock-auto-hide?", config_path)
}

/// Whether the dock stays visible when nothing is running.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_show_when_clear_config_for(config_path: *const c_char) -> c_int {
    bool_for(c"get-dock-show-when-clear?", config_path)
}

/// Whether hovering animates the icons.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_hover_animate_config_for(config_path: *const c_char) -> c_int {
    bool_for(c"get-dock-hover-animate?", config_path)
}

/// The icon size, sanitised into the usable range.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_icon_size_config_for(config_path: *const c_char) -> c_int {
    let raw = number_for(
        c"get-dock-icon-size",
        config_path,
        f64::from(DEFAULT_ICON_SIZE),
    );

    lambdock_icon_size_sanitize(raw)
}

/// The gap between dock items, in pixels.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_item_spacing_config_for(config_path: *const c_char) -> c_int {
    let raw = number_for(
        c"get-dock-item-spacing",
        config_path,
        f64::from(ITEM_SPACING_PX),
    );

    lambdock_item_spacing_sanitize(raw)
}

/// The room between the capsule's ends and the icons.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_capsule_padding_config_for(config_path: *const c_char) -> c_int {
    let raw = number_for(
        c"get-dock-capsule-padding",
        config_path,
        f64::from(CAPSULE_PADDING_PX),
    );

    lambdock_capsule_padding_sanitize(raw)
}

/// The launchpad's tile size.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_launchpad_icon_size_config_for(config_path: *const c_char) -> c_int {
    let raw = number_for(
        c"get-dock-launchpad-icon-size",
        config_path,
        f64::from(LAUNCHPAD_ICON_SIZE),
    );

    lambdock_launchpad_icon_size_sanitize(raw)
}

/// The launchpad's column count, where zero means "as many as fit".
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_launchpad_columns_config_for(config_path: *const c_char) -> c_int {
    let raw = number_for(
        c"get-dock-launchpad-columns",
        config_path,
        f64::from(LAUNCHPAD_COLUMNS_AUTO),
    );

    lambdock_launchpad_columns_sanitize(raw)
}

/// Whether the launchpad's backdrop is blurred.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_launchpad_blur_config_for(config_path: *const c_char) -> c_int {
    bool_for(c"get-dock-launchpad-blur?", config_path)
}

/// Whether the launchpad's close button belongs on the right.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_launchpad_close_on_right_config_for(
    config_path: *const c_char,
) -> c_int {
    let side = string_for(
        c"get-dock-launchpad-close-side",
        None,
        config_path,
        c"left".as_ptr(),
    );
    let on_right = lambdock_launchpad_close_on_right(side);
    g_free(side.cast());

    on_right
}

/// Whether the dock reduces its motion for accessibility.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_reduce_motion_config_for(config_path: *const c_char) -> c_int {
    bool_for(c"get-dock-reduce-motion?", config_path)
}

/// Whether this dock starts a REPL.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_repl_config_for(config_path: *const c_char) -> c_int {
    bool_for(c"get-dock-enable-repl?", config_path)
}

/// The dock's CSS theme name, as a `g_strdup`ed string.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_theme_config_for(config_path: *const c_char) -> *mut c_char {
    string_for(c"get-dock-theme", None, config_path, c"vanilla".as_ptr())
}

/// Whether the dock tracks the compositor's windows.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_track_windows_config_for(config_path: *const c_char) -> c_int {
    bool_for(c"get-dock-track-windows?", config_path)
}

/// Whether the dock renders as a full-width taskbar.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_taskbar_config_for(config_path: *const c_char) -> c_int {
    let mode = string_for(c"get-dock-mode", None, config_path, c"dock".as_ptr());
    let taskbar = lambdock_dock_mode_is_taskbar(mode);
    g_free(mode.cast());

    taskbar
}

/// Whether running items show their window's title.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_show_window_titles_config_for(config_path: *const c_char) -> c_int {
    bool_for(c"get-dock-show-window-titles?", config_path)
}

/// Every shelf and reflection parameter for one dock, sanitised.
///
/// The Scheme side already merges "explicit setting > special-mode preset >
/// default"; this only reads the numbers out and hands them to the pure
/// sanitiser.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string, and `out` must be null
/// or point to a writable `LambdockShelfParams`.
#[no_mangle]
pub unsafe extern "C" fn get_shelf_config_for(config_path: *const c_char, out: *mut ShelfParams) {
    if out.is_null() {
        return;
    }

    let defaults = lambdock_shelf_params_default();

    let raw = ShelfRawValues {
        tilt_deg: number_for(c"get-dock-shelf-tilt", config_path, defaults.tilt_deg),
        perspective: number_for(c"get-dock-shelf-perspective", config_path, defaults.perspective),
        margin_px: number_for(
            c"get-dock-shelf-margin",
            config_path,
            f64::from(defaults.margin_px),
        ),
        front_px: number_for(
            c"get-dock-shelf-front",
            config_path,
            f64::from(defaults.front_px),
        ),
        reflection_ratio: number_for(
            c"get-dock-reflection",
            config_path,
            defaults.reflection_ratio,
        ),
        reflection_intensity: number_for(
            c"get-dock-reflection-intensity",
            config_path,
            defaults.reflection_intensity,
        ),
        reflection_blur_ratio: number_for(
            c"get-dock-reflection-blur",
            config_path,
            defaults.reflection_blur_ratio,
        ),
        flare_px: number_for(
            c"get-dock-shelf-flare",
            config_path,
            f64::from(defaults.flare_px),
        ),
    };

    *out = lambdock_shelf_params_sanitize(&raw, defaults);
}

/// The monitor the dock belongs to ("default" or a connector name).
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_monitor_config_for(config_path: *const c_char) -> *mut c_char {
    string_for(c"get-dock-monitor", None, config_path, c"default".as_ptr())
}

/// The settings file the default dock came from, or null.
#[no_mangle]
pub unsafe extern "C" fn get_config_path_c() -> *mut c_char {
    string_for(
        c"get-config-path",
        Some(c"lambdock loader"),
        std::ptr::null(),
        std::ptr::null(),
    )
}

/// The screen edge the dock is docked to.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_position_config_for(config_path: *const c_char) -> c_int {
    let position = string_for(
        c"get-dock-position",
        None,
        config_path,
        c"bottom".as_ptr(),
    );
    let edge = lambdock_edge_from_name(position);
    g_free(position.cast());

    edge
}

// ===========================================================================
// Actions on the Scheme side
// ===========================================================================

/// Asks Scheme to write the dock's resolved settings to a file.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn export_dock_settings_to_file(config_path: *const c_char) -> *mut c_char {
    let procedure = scm_c_public_ref(CORE_MODULE.as_ptr(), c"export-dock-settings!".as_ptr());

    if !is_true(scm_procedure_p(procedure)) {
        return std::ptr::null_mut();
    }

    let result = call_with_path(procedure, config_path);
    if scm_is_string(result) == 0 {
        return std::ptr::null_mut();
    }

    // The path out of Scheme is malloc'ed; the copy that leaves this module is
    // GLib's, like every other string it hands to C.
    let text = scm_to_utf8_string(result);
    let copy = g_strdup(text);
    free(text.cast());

    copy
}

/// Loads one settings file into the Scheme side.
///
/// # Safety
///
/// `file_path` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn load_dock_config_scm(file_path: *const c_char) {
    if file_path.is_null() {
        return;
    }

    let procedure = scm_c_public_ref(CORE_MODULE.as_ptr(), c"load-dock-config!".as_ptr());
    if is_true(procedure) {
        scm_call_1(procedure, scm_from_utf8_string(file_path));
    }
}

/// Whether the dock is configured to leave this application out.
///
/// # Safety
///
/// Each argument must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn is_app_excluded(
    app_id: *const c_char,
    config_path: *const c_char,
) -> c_int {
    if app_id.is_null() || *app_id == 0 {
        return 0;
    }

    let mut list = match call_core(c"get-dock-exclude-apps", config_path) {
        Some(list) => list,
        None => return 0,
    };

    let app_id = CStr::from_ptr(app_id).to_bytes();

    while scm_is_pair(list) != 0 {
        let element = scm_car(list);
        let value = if is_symbol(element) {
            scm_symbol_to_string(element)
        } else {
            element
        };

        if scm_is_string(value) != 0 {
            let text = scm_to_utf8_string(value);
            let matches =
                !text.is_null() && CStr::from_ptr(text).to_bytes().eq_ignore_ascii_case(app_id);
            free(text.cast());

            if matches {
                return 1;
            }
        }

        list = scm_cdr(list);
    }

    0
}
