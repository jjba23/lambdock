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
use crate::dock_item::{
    create_dock_item, create_dynamic_dock_item, create_launchpad_dock_item,
    create_separator_dock_item, create_stack_dock_item, create_trash_dock_item,
    dock_item_enable_clock, dock_item_enable_meter, dock_item_set_poll_name,
};
use crate::glib::{g_free, g_strdup};
use crate::state::LambdockState;
use crate::{log_info, log_warn};
use crate::guile::{
    free, is_symbol, is_true, scm_apply_0, scm_c_eval_string, scm_c_public_ref, scm_car, scm_cdr,
    scm_call_0, scm_call_1, scm_call_2, scm_call_3, scm_cons, scm_from_double, scm_from_int64,
    scm_from_utf8_string, scm_gc_protect_object, scm_gc_unprotect_object, scm_internal_catch,
    scm_is_number, scm_is_pair, scm_is_real, scm_is_string, scm_list_5, scm_nan_p,
    scm_procedure_p, scm_string_to_symbol, scm_symbol_to_string, scm_to_bool,
    scm_to_double, scm_to_utf8_string, false_value, true_value, Scm,
};
use std::ffi::CStr;
use gtk4::glib::translate::IntoGlib;
use std::os::raw::{c_char, c_int, c_void};
use crate::dock_window::{update_dock_dimensions};


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
pub unsafe fn get_autohide_config_for(config_path: *const c_char) -> c_int {
    bool_for(c"get-dock-auto-hide?", config_path)
}

/// Whether the dock stays visible when nothing is running.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
pub unsafe fn get_show_when_clear_config_for(config_path: *const c_char) -> c_int {
    bool_for(c"get-dock-show-when-clear?", config_path)
}

/// Whether hovering animates the icons.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
pub unsafe fn get_hover_animate_config_for(config_path: *const c_char) -> c_int {
    bool_for(c"get-dock-hover-animate?", config_path)
}

/// The icon size, sanitised into the usable range.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
pub unsafe fn get_icon_size_config_for(config_path: *const c_char) -> c_int {
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
pub unsafe fn get_item_spacing_config_for(config_path: *const c_char) -> c_int {
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
pub unsafe fn get_capsule_padding_config_for(config_path: *const c_char) -> c_int {
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
pub unsafe fn get_launchpad_icon_size_config_for(config_path: *const c_char) -> c_int {
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
pub unsafe fn get_launchpad_columns_config_for(config_path: *const c_char) -> c_int {
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
pub unsafe fn get_launchpad_blur_config_for(config_path: *const c_char) -> c_int {
    bool_for(c"get-dock-launchpad-blur?", config_path)
}

/// Whether the launchpad's close button belongs on the right.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
pub unsafe fn get_launchpad_close_on_right_config_for(
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
pub unsafe fn get_reduce_motion_config_for(config_path: *const c_char) -> c_int {
    bool_for(c"get-dock-reduce-motion?", config_path)
}

/// Whether this dock starts a REPL.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
pub unsafe fn get_repl_config_for(config_path: *const c_char) -> c_int {
    bool_for(c"get-dock-enable-repl?", config_path)
}

/// The dock's CSS theme name, as a `g_strdup`ed string.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
pub unsafe fn get_theme_config_for(config_path: *const c_char) -> *mut c_char {
    string_for(c"get-dock-theme", None, config_path, c"vanilla".as_ptr())
}

/// Whether the dock tracks the compositor's windows.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
pub unsafe fn get_track_windows_config_for(config_path: *const c_char) -> c_int {
    bool_for(c"get-dock-track-windows?", config_path)
}

/// Whether the dock renders as a full-width taskbar.
///
/// # Safety
///
/// `config_path` must be null or a NUL-terminated string.
pub unsafe fn get_taskbar_config_for(config_path: *const c_char) -> c_int {
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
pub unsafe fn get_show_window_titles_config_for(config_path: *const c_char) -> c_int {
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
pub unsafe fn get_shelf_config_for(config_path: *const c_char, out: *mut ShelfParams) {
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
pub unsafe fn get_monitor_config_for(config_path: *const c_char) -> *mut c_char {
    string_for(c"get-dock-monitor", None, config_path, c"default".as_ptr())
}

/// The settings file the default dock came from, or null.
///
/// # Safety
///
/// The result is `(transfer full)`: free it with `g_free`.
pub unsafe fn get_config_path_c() -> *mut c_char {
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
pub unsafe fn get_position_config_for(config_path: *const c_char) -> c_int {
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
pub unsafe fn export_dock_settings_to_file(config_path: *const c_char) -> *mut c_char {
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
pub unsafe fn load_dock_config_scm(file_path: *const c_char) {
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
pub unsafe fn is_app_excluded(
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

// ============================================================================
// THE C→SCHEME WRITES: ITEMS, SETTINGS, LAYOUT, POLLERS, PREFERENCES
// ============================================================================
//
// The other direction of the seam. Everything here hands *values* to Scheme and
// reads answers back as plain C -- an item builder asks `(lambdock core)` what an
// item is and creates the matching widget, the layout writes record a drag in the
// override file, a poller is a procedure the dock calls on a timer.
//
// Two rules from the C version stay, and they are the reason this lives in one
// module:
//
// - *Every string that leaves here is GLib's*, so a caller always pairs it with
//   `g_free`. Guile's `scm_to_utf8_string` returns `malloc`ed memory; the
//   conversion stays local so no caller has to remember which allocator it holds.
// - *No `Scm` is stored in a Rust value.* A value is converted the moment it
//   arrives. The one handle that outlives its call -- a poll procedure -- is kept
//   by Guile itself (`scm_gc_protect_object`) and held as an opaque word in
//   `DockItem.poll_fn`, exactly as before.

/// `LAMBDOCK_SCHEME_DIR`, the installed Scheme path, when the build named it.
const SCHEME_DIR: Option<&str> = option_env!("LAMBDOCK_SCHEME_DIR");

/// `SCM_EOL`: the empty list, asked of the interpreter because the constant itself
/// is not an exported symbol. Quoted -- `scm_c_eval_string` evaluates what it is
/// given, and `()` is a syntax error as an expression.
unsafe fn empty_list() -> Scm {
    scm_c_eval_string(c"'()".as_ptr())
}

/// An installed dock's data directory, derived from the executable's own
/// location: `<prefix>/bin/lambdock` → `<prefix>/share/<what>`.
///
/// `None` for an executable with no parent, and for a path with nothing above
/// the binary (`lambdock`, or `/lambdock`).
fn share_dir_for_exe(exe: &std::path::Path, what: &str) -> Option<std::path::PathBuf> {
    let prefix = exe.parent()?.parent()?;

    (!prefix.as_os_str().is_empty()).then(|| prefix.join("share").join(what))
}

/// [`share_dir_for_exe`] for this process, when it exists on disk.
pub(crate) fn installed_share_dir(what: &str) -> Option<std::path::PathBuf> {
    let exe = std::env::current_exe().ok()?;
    let directory = share_dir_for_exe(&exe, what)?;

    directory.is_dir().then_some(directory)
}

/// Tells Guile where to look for `(lambdock core)`.
///
/// The local `scheme` directory first, so a checkout runs from its own sources,
/// then the installed one -- the path the build named, or failing that the prefix
/// the executable sits in. That second fallback is what makes a plain
/// `cargo run -p xtask -- install --prefix ~/.local` work without the installer
/// having to know that a build-time variable exists.
///
/// # Safety
///
/// Guile must be initialized.
pub unsafe fn setup_guile_load_path() {
    scm_c_eval_string(c"(set! %load-path (cons \"scheme\" %load-path))".as_ptr());

    let directory = SCHEME_DIR
        .filter(|directory| !directory.is_empty())
        .map(std::path::PathBuf::from)
        .or_else(|| installed_share_dir("guile/site/3.0"));

    if let Some(directory) = directory {
        let expression = crate::cstr::cstring(&format!(
            "(set! %load-path (cons \"{}\" %load-path))",
            directory
                .to_string_lossy()
                .replace('\\', "\\\\")
                .replace('"', "\\\"")
        ));
        scm_c_eval_string(expression.as_ptr());
    }
}

#[cfg(test)]
mod share_dir_tests {
    use super::share_dir_for_exe;
    use std::path::Path;

    /// The prefix is the directory the binary's own directory sits in.
    #[test]
    fn a_binary_in_a_prefix_names_that_prefix() {
        let derived = share_dir_for_exe(
            Path::new("/home/me/.nix-profile/bin/lambdock"),
            "guile/site/3.0",
        );

        assert_eq!(
            derived.as_deref(),
            Some(Path::new("/home/me/.nix-profile/share/guile/site/3.0"))
        );
    }

    /// A bare name has no prefix, and neither has a path with nothing above it.
    #[test]
    fn a_path_without_a_prefix_derives_nothing() {
        assert_eq!(
            share_dir_for_exe(Path::new("lambdock"), "guile/site/3.0"),
            None
        );
        assert_eq!(
            share_dir_for_exe(Path::new("/lambdock"), "guile/site/3.0"),
            None
        );
    }
}

/// Re-evaluates the default Scheme configuration.
///
/// # Safety
///
/// Guile must be initialized.
pub unsafe fn reload_guile_config() {
    log_info!("GuileBridge", "Re-evaluating Scheme configuration file...");

    let init = scm_c_public_ref(CORE_MODULE.as_ptr(), c"init-dock!".as_ptr());
    if is_true(init) {
        scm_call_0(init);
    }
}

/// A Scheme value that names something, as a `g_strdup`ed string.
///
/// `None` for `#f`, for a non-string and for the empty string, which is what the
/// C meant by returning null.
unsafe fn text_of(value: Scm) -> Option<*mut c_char> {
    if !is_true(value) {
        return None;
    }

    // `scm_symbol_p` answers `#t`/`#f`, which are tagged words rather than null,
    // so the test is `is_true` and not a null check.
    let text = if is_symbol(value) {
        scm_symbol_to_string(value)
    } else {
        value
    };
    if scm_is_string(text) == 0 {
        return None;
    }

    let raw = scm_to_utf8_string(text);
    if raw.is_null() {
        return None;
    }

    let copy = if *raw != 0 {
        Some(crate::glib::g_strdup(raw))
    } else {
        None
    };
    free(raw.cast());

    copy
}

/// The name of a symbol (or a string), and nothing for anything else.
///
/// A list, `#f` or a number yields `None` instead of raising a Guile type error,
/// which would longjmp out of the caller.
unsafe fn symbol_name(value: Scm) -> Option<*mut c_char> {
    text_of(value)
}

/// A poll interval in milliseconds, clamped to something a timer can use.
unsafe fn poll_interval(value: Scm, fallback: u32) -> u32 {
    if scm_is_number(value) == 0 {
        return fallback;
    }

    let interval = scm_to_double(value);
    if !interval.is_finite() || interval < 1.0 {
        return fallback;
    }
    if interval > f64::from(u32::MAX) {
        return u32::MAX;
    }

    interval as u32
}

/// Every `dock-item-*` accessor, resolved once per populate pass.
#[derive(Default, Clone, Copy)]
struct ItemAccessors {
    name: Scm,
    icon: Scm,
    exec: Scm,
    poll: Scm,
    poll_interval: Scm,
    hover_animate: Scm,
    path: Scm,
    view: Scm,
    meter: Scm,
    clock: Scm,
    unit: Scm,
    max: Scm,
    poll_name: Scm,
    style: Scm,
}

/// What one item builder is given.
struct BuildContext<'a> {
    state: *mut LambdockState,
    item: Scm,
    hover_animate: bool,
    separator_orientation: gtk4::Orientation,
    accessors: &'a ItemAccessors,
    /// Poll periods of the dynamic items built this pass, for the budget check.
    poll_intervals: &'a std::cell::RefCell<Vec<u32>>,
}

/// The value of one `dock-item-*` accessor for this item.
unsafe fn accessor(procedure: Scm, item: Scm) -> Scm {
    scm_call_1(procedure, item)
}

/// The text one accessor answers, as a `g_strdup`ed string.
unsafe fn accessor_text(procedure: Scm, item: Scm) -> Option<String> {
    let value = accessor(procedure, item);
    let text = text_of(value)?;

    Some(crate::cstr::text_or_empty(text))
}

unsafe fn build_app_item(context: &BuildContext<'_>) {
    let accessors = context.accessors;

    let name = accessor_text(accessors.name, context.item);
    let icon = accessor_text(accessors.icon, context.item);
    let exec = accessor_text(accessors.exec, context.item);

    let config_path = (*context.state).config_path;
    let excluded = |value: Option<&String>| {
        value.is_some_and(|value| {
            let value = crate::cstr::cstring(value);
            is_app_excluded(value.as_ptr(), config_path) != 0
        })
    };

    if excluded(name.as_ref()) || excluded(icon.as_ref()) || excluded(exec.as_ref()) {
        log_info!(
            "GuileBridge",
            "Skipping excluded launcher item: {}",
            name.as_deref().unwrap_or("")
        );
        return;
    }

    let (name_c, icon_c, exec_c) = (cstring_of(&name), cstring_of(&icon), cstring_of(&exec));
    create_dock_item(
        context.state,
        name_c.as_ptr(),
        icon_c.as_ptr(),
        exec_c.as_ptr(),
        name_c.as_ptr(),
        1,
        c_int::from(context.hover_animate),
    );

    log_info!(
        "GuileBridge",
        "Created favorite item: {} ({})",
        name.as_deref().unwrap_or("unnamed"),
        exec.as_deref().unwrap_or("no-exec")
    );
}

unsafe fn build_dynamic_item(context: &BuildContext<'_>) {
    let accessors = context.accessors;

    let name = accessor_text(accessors.name, context.item);
    let icon = accessor_text(accessors.icon, context.item);
    let exec = accessor_text(accessors.exec, context.item);
    let poll_fn = accessor(accessors.poll, context.item);
    let interval_ms = poll_interval(accessor(accessors.poll_interval, context.item), 1000);

    let config_path = (*context.state).config_path;
    let excluded = |value: Option<&String>| {
        value.is_some_and(|value| {
            let value = crate::cstr::cstring(value);
            is_app_excluded(value.as_ptr(), config_path) != 0
        })
    };
    if excluded(name.as_ref()) || excluded(exec.as_ref()) {
        return;
    }

    let (name_c, icon_c, exec_c) = (cstring_of(&name), cstring_of(&icon), cstring_of(&exec));
    let dynamic = create_dynamic_dock_item(
        context.state,
        name_c.as_ptr(),
        icon_c.as_ptr(),
        exec_c.as_ptr(),
        poll_fn.cast(),
        interval_ms,
        c_int::from(context.hover_animate),
    );
    if dynamic.is_null() {
        return;
    }

    if let Some(poll_name) = accessor_text(accessors.poll_name, context.item) {
        let poll_name = crate::cstr::cstring(&poll_name);
        dock_item_set_poll_name(dynamic, poll_name.as_ptr());
    }
    context.poll_intervals.borrow_mut().push(interval_ms);

    // Meter items draw a gauge instead of text: the poll function returns a
    // number, which fills the ring up to `max' and is shown with `unit'.
    if is_true(accessor(accessors.meter, context.item)) {
        let unit = accessor_text(accessors.unit, context.item);
        let max = accessor(accessors.max, context.item);
        let max_value = if scm_is_number(max) != 0 {
            scm_to_double(max)
        } else {
            100.0
        };

        let unit = cstring_of(&unit);
        dock_item_enable_meter(dynamic, unit.as_ptr(), max_value);
    }

    // Clock items draw an analogue face: the poll function still formats the
    // reading the tooltip and the screen reader use, the hands follow the system
    // clock.
    if is_true(accessor(accessors.clock, context.item)) {
        dock_item_enable_clock(dynamic);
    }

    log_info!(
        "GuileBridge",
        "Created dynamic item: {} ({}, poll interval: {interval_ms} ms)",
        name.as_deref().unwrap_or("unnamed"),
        exec.as_deref().unwrap_or("no-exec")
    );
}

unsafe fn build_separator_item(context: &BuildContext<'_>) {
    // The style is a symbol in the settings file ("line", "groove", ...) and a
    // CSS class on the line; the Rust side validates it against its own list.
    let style = symbol_name(accessor(context.accessors.style, context.item))
        .map(|style| crate::cstr::text_or_empty(style));
    let style_c = cstring_of(&style);

    create_separator_dock_item(
        context.state,
        context.separator_orientation.into_glib(),
        style_c.as_ptr(),
    );

    log_info!(
        "GuileBridge",
        "Created layout separator ({})",
        style.as_deref().unwrap_or("line")
    );
}

unsafe fn build_stack_item(context: &BuildContext<'_>) {
    let accessors = context.accessors;

    let name = accessor_text(accessors.name, context.item);
    let icon = accessor_text(accessors.icon, context.item);
    let path = accessor_text(accessors.path, context.item);
    let view = accessor(accessors.view, context.item);
    let view = symbol_name(view).map(|view| crate::cstr::text_or_empty(view));

    let (name_c, icon_c, path_c) = (cstring_of(&name), cstring_of(&icon), cstring_of(&path));
    let view_c = cstring_of(&view);
    create_stack_dock_item(
        context.state,
        name_c.as_ptr(),
        icon_c.as_ptr(),
        path_c.as_ptr(),
        view_c.as_ptr(),
        c_int::from(context.hover_animate),
    );
}

unsafe fn build_launchpad_item(context: &BuildContext<'_>) {
    let accessors = context.accessors;

    let name = accessor_text(accessors.name, context.item);
    let icon = accessor_text(accessors.icon, context.item);

    let (name_c, icon_c) = (cstring_of(&name), cstring_of(&icon));
    create_launchpad_dock_item(
        context.state,
        name_c.as_ptr(),
        icon_c.as_ptr(),
        c_int::from(context.hover_animate),
    );
}

unsafe fn build_trash_item(context: &BuildContext<'_>) {
    let accessors = context.accessors;

    let name = accessor_text(accessors.name, context.item);
    let icon = accessor_text(accessors.icon, context.item);

    let (name_c, icon_c) = (cstring_of(&name), cstring_of(&icon));
    create_trash_dock_item(
        context.state,
        name_c.as_ptr(),
        icon_c.as_ptr(),
        c_int::from(context.hover_animate),
    );
}

/// A string as a `CString`, or empty: the builders treat an absent value as the
/// empty string, which is what the C did with its nullable `g_strdup`s.
fn cstring_of(value: &Option<String>) -> std::ffi::CString {
    crate::cstr::cstring(value.as_deref().unwrap_or(""))
}

/// One row per Scheme item type: the type names are the `dock-item-type` symbols
/// the Scheme side produces.
type ItemBuilder = unsafe fn(&BuildContext<'_>);

const ITEM_BUILDERS: [(&str, ItemBuilder); 6] = [
    ("app", build_app_item),
    ("dynamic", build_dynamic_item),
    ("separator", build_separator_item),
    ("stack", build_stack_item),
    ("launchpad", build_launchpad_item),
    ("trash", build_trash_item),
];

/// The builder for a Scheme type name, or `None` for a type nobody knows.
fn builder_for_type(type_name: &str) -> Option<ItemBuilder> {
    ITEM_BUILDERS
        .iter()
        .find(|(name, _)| *name == type_name)
        .map(|(_, build)| *build)
}

/// Resolves every accessor the builders use.
///
/// `None` when `(lambdock core)` is not loaded, so the dock stays empty instead of
/// crashing. Every accessor is checked, not a sample: applying a `#f` that
/// `scm_c_public_ref` could not resolve raises "wrong type to apply" on the GTK
/// thread -- where Guile has no handler and aborts the process -- so a module that
/// failed to export one accessor has to degrade to "no items", never to a crash.
unsafe fn resolve_item_accessors() -> Option<ItemAccessors> {
    let accessors = ItemAccessors {
        name: scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-item-name".as_ptr()),
        icon: scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-item-icon".as_ptr()),
        exec: scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-item-exec".as_ptr()),
        poll: scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-item-poll-fn".as_ptr()),
        poll_interval: scm_c_public_ref(
            CORE_MODULE.as_ptr(),
            c"dock-item-poll-interval".as_ptr(),
        ),
        hover_animate: scm_c_public_ref(
            CORE_MODULE.as_ptr(),
            c"dock-item-hover-animate?".as_ptr(),
        ),
        path: scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-item-path".as_ptr()),
        view: scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-item-view-mode".as_ptr()),
        meter: scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-item-meter?".as_ptr()),
        clock: scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-item-clock?".as_ptr()),
        unit: scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-item-unit".as_ptr()),
        max: scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-item-max".as_ptr()),
        poll_name: scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-item-poll-name".as_ptr()),
        style: scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-item-style".as_ptr()),
    };

    let procedures = [
        accessors.name,
        accessors.icon,
        accessors.exec,
        accessors.poll,
        accessors.hover_animate,
        accessors.path,
        accessors.view,
        accessors.meter,
        accessors.clock,
        accessors.unit,
        accessors.max,
        accessors.poll_interval,
        accessors.poll_name,
        accessors.style,
    ];

    procedures
        .iter()
        .all(|procedure| is_true(scm_procedure_p(*procedure)))
        .then_some(accessors)
}

/// Reads every configured item and builds one dock item per entry.
///
/// # Safety
///
/// `state` must be a live `LambdockState` with its row already built.
pub unsafe fn populate_dock_items_for(state: &mut LambdockState) {
    // The builders keep the pointer: an item remembers the dock that made it.
    let pointer = state as *mut LambdockState;
    let state_ref = state;
    if state_ref.box_.is_none() {
        return;
    }

    let Some(accessors) = resolve_item_accessors() else {
        return;
    };

    let get_type = scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-item-type".as_ptr());
    if !is_true(get_type) {
        return;
    }

    // A separator runs across the dock, so it is the other orientation.
    let horizontal = state_ref.orientation != gtk4::Orientation::Vertical.into_glib();
    let separator_orientation = if horizontal {
        gtk4::Orientation::Vertical
    } else {
        gtk4::Orientation::Horizontal
    };

    let poll_intervals: std::cell::RefCell<Vec<u32>> = std::cell::RefCell::new(Vec::new());

    let items = call_with_path(
        scm_c_public_ref(CORE_MODULE.as_ptr(), c"get-dock-items".as_ptr()),
        state_ref.config_path,
    );

    let mut rest = items;
    while scm_is_pair(rest) != 0 {
        let item = scm_car(rest);

        let type_name = symbol_name(scm_call_1(get_type, item)).map(|name| crate::cstr::text_or_empty(name));

        match type_name.as_deref().and_then(builder_for_type) {
            Some(build) => {
                let context = BuildContext {
                    state: pointer,
                    item,
                    hover_animate: is_true(accessor(accessors.hover_animate, item)),
                    separator_orientation,
                    accessors: &accessors,
                    poll_intervals: &poll_intervals,
                };
                build(&context);
            }
            None => {
                if let Some(type_name) = type_name.filter(|name| !name.is_empty()) {
                    log_warn!("GuileBridge", "Ignoring unknown dock item type: {type_name}");
                }
            }
        }

        rest = scm_cdr(rest);
    }

    // A configuration can schedule more polling work than one dock should: the
    // rule the developer guide used to only warn about. Nothing is disabled -- the
    // dock says what it is doing and lets the user decide.
    let poll_intervals = poll_intervals.into_inner();
    if !poll_intervals.is_empty() {
        let count = poll_intervals.len();
        let rate = crate::core::poll_budget::lambdock_polls_per_second(
            poll_intervals.as_ptr(),
            count,
        );
        if crate::core::poll_budget::lambdock_poll_budget_exceeded(
            poll_intervals.as_ptr(),
            count,
            LAMBDOCK_POLL_BUDGET_PER_SECOND,
        ) != 0
        {
            log_warn!(
                "Poller",
                "{count} polling item(s) in [{}] schedule {rate:.1} polls/second \
                 (budget {LAMBDOCK_POLL_BUDGET_PER_SECOND:.0}): raise a poll interval \
                 or drop a widget",
                crate::cstr::text_or_empty(state_ref.config_path)
            );
        }
    }

    // The row exists only now, and the derived layout is measured from it: every
    // item's shelf slot is created hidden and the band it reserves is part of the
    // row's cross-axis size. Re-applying the layout here is the same pass a reload
    // runs, and without it the first frame kept every reflection hidden -- so the
    // icons sat one band lower (and unmirrored) until something reloaded the dock.
    update_dock_dimensions(state_ref);
}

/// `LAMBDOCK_POLL_BUDGET_PER_SECOND`, from `core/poll_budget.h`.
const LAMBDOCK_POLL_BUDGET_PER_SECOND: f64 = 10.0;

/// Reads every value the dock takes from the settings file, in one place.
///
/// The theme and the mode flags the stylesheet depends on, the shelf geometry, and
/// the sizes the row is built from. Startup and a reload both go through here, so
/// neither can drift into reading a different subset of the other -- and the sizes
/// are already current when the stylesheet asks the shelf for its band.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`.
pub unsafe fn dock_read_settings(state: &mut LambdockState) {
    let state_ref = state;
    let path = state_ref.config_path;

    g_free(state_ref.theme.cast());
    state_ref.theme = get_theme_config_for(path);

    // Mode flags: the taskbar and window-title modifiers are attached from them, so
    // they are read before the stylesheet is loaded.
    state_ref.taskbar = get_taskbar_config_for(path);
    state_ref.show_window_titles = get_show_window_titles_config_for(path);
    get_shelf_config_for(path, &mut state_ref.shelf);

    state_ref.auto_hide = get_autohide_config_for(path);
    state_ref.track_windows = get_track_windows_config_for(path);
    state_ref.show_when_clear = get_show_when_clear_config_for(path);
    state_ref.hover_animate = get_hover_animate_config_for(path);
    state_ref.reduce_motion = get_reduce_motion_config_for(path);
    state_ref.icon_size = get_icon_size_config_for(path);
    state_ref.item_spacing = get_item_spacing_config_for(path);
    state_ref.capsule_padding = get_capsule_padding_config_for(path);
}

/// A string as a Scheme value, with the empty string for anything absent.
fn optional_string(value: Option<&str>) -> Scm {
    let value = crate::cstr::cstring(value.unwrap_or_default());

    unsafe { scm_from_utf8_string(value.as_ptr()) }
}

/// Records a dragged-in item in the layout overrides.
///
/// # Safety
///
/// The strings must be null or NUL-terminated.
pub unsafe fn dock_layout_add_item(
    config_path: *const c_char,
    kind: *const c_char,
    name: *const c_char,
    icon: *const c_char,
    arg: *const c_char,
    view: *const c_char,
) {
    let Some(config_path) = crate::cstr::non_empty_text(config_path) else {
        return;
    };

    let procedure = scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-layout-add!".as_ptr());
    if !is_true(procedure) {
        return;
    }

    let arguments = scm_cons(
        optional_string(Some(&config_path)),
        scm_list_5(
            optional_string(crate::cstr::text(kind).as_deref()),
            optional_string(crate::cstr::text(name).as_deref()),
            optional_string(crate::cstr::text(icon).as_deref()),
            optional_string(crate::cstr::text(arg).as_deref()),
            optional_string(crate::cstr::text(view).as_deref()),
        ),
    );

    scm_apply_0(procedure, arguments);
}

/// Records a whole preset's items in the layout overrides.
///
/// # Safety
///
/// The strings must be null or NUL-terminated.
pub unsafe fn dock_layout_add_preset_item(
    config_path: *const c_char,
    preset: *const c_char,
) {
    let Some(preset) = crate::cstr::non_empty_text(preset) else {
        return;
    };

    let procedure = scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-layout-add!".as_ptr());
    if !is_true(procedure) {
        return;
    }

    // An empty path is the default dock: the Scheme side resolves it to the default
    // settings file's layout, which is the same file a path-ed dock would use.
    let arguments = scm_cons(
        optional_string(crate::cstr::text(config_path).as_deref()),
        scm_list_5(
            scm_from_utf8_string(c"preset".as_ptr()),
            optional_string(Some(&preset)),
            optional_string(Some("")),
            optional_string(Some("")),
            optional_string(Some("")),
        ),
    );

    scm_apply_0(procedure, arguments);
}

/// Forgets a removed item in the layout overrides.
///
/// # Safety
///
/// The strings must be null or NUL-terminated.
pub unsafe fn dock_layout_remove_item(
    config_path: *const c_char,
    key: *const c_char,
) {
    let (Some(config_path), Some(key)) = (
        crate::cstr::non_empty_text(config_path),
        crate::cstr::text(key),
    ) else {
        return;
    };

    let procedure = scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-layout-remove!".as_ptr());
    if !is_true(procedure) {
        return;
    }

    scm_call_2(
        procedure,
        optional_string(Some(&config_path)),
        optional_string(Some(&key)),
    );
}

/// Records a new order in the layout overrides.
///
/// # Safety
///
/// `config_path` must be null or NUL-terminated.
pub unsafe fn dock_layout_set_order(config_path: *const c_char, keys: &[String]) {
    let Some(config_path) = crate::cstr::non_empty_text(config_path) else {
        return;
    };

    let procedure = scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-layout-set-order!".as_ptr());
    if !is_true(procedure) {
        return;
    }

    // Built back to front, so the conses come out in the order they were given.
    let list = keys
        .iter()
        .rev()
        .fold(empty_list(), |list, key| {
            scm_cons(optional_string(Some(key)), list)
        });

    scm_call_2(procedure, optional_string(Some(&config_path)), list);
}

/// Remembers a stack's view mode in the layout overrides.
///
/// # Safety
///
/// The strings must be null or NUL-terminated.
pub unsafe fn dock_layout_set_view(
    config_path: *const c_char,
    key: *const c_char,
    view: *const c_char,
) {
    let (Some(config_path), Some(key)) = (
        crate::cstr::non_empty_text(config_path),
        crate::cstr::text(key),
    ) else {
        return;
    };

    let procedure = scm_c_public_ref(CORE_MODULE.as_ptr(), c"dock-layout-set-view!".as_ptr());
    if !is_true(procedure) {
        return;
    }

    let view = crate::cstr::non_empty_text(view).unwrap_or_else(|| String::from("grid"));
    let view = scm_string_to_symbol(optional_string(Some(&view)));

    scm_call_3(
        procedure,
        optional_string(Some(&config_path)),
        optional_string(Some(&key)),
        view,
    );
}

// ============================================================================
// ITEM POLLERS
// ============================================================================
//
// The one place a dynamic item's Scheme procedure is kept and called: the item
// stores an opaque handle, and what comes back is a `LambdockReading`.

/// Adapter: Guile's catch helper takes the procedure as a `void *`.
unsafe extern "C" fn call_poll_proc(data: *mut c_void) -> Scm {
    scm_call_0(data.cast())
}

/// Adapter: an error inside a poller is a missing reading, not a dead daemon.
unsafe extern "C" fn handle_poll_error(_data: *mut c_void, _key: Scm, _args: Scm) -> Scm {
    scm_c_eval_string(c"#f".as_ptr())
}

/// Returns the handle to keep, or null when the value is not a procedure.
///
/// # Safety
///
/// `procedure` must be null or an `Scm`.
pub unsafe fn lambdock_poll_fn_protect(procedure: *mut c_void) -> *mut c_void {
    // Null is the dock's own "no poller" handle, not a Scheme value: handing it to
    // `scm_*` would be a null dereference, since Scheme's false is a tagged
    // pointer rather than null.
    if procedure.is_null() {
        return std::ptr::null_mut();
    }

    let procedure: Scm = procedure.cast();
    if !is_true(procedure) || !is_true(scm_procedure_p(procedure)) {
        return std::ptr::null_mut();
    }

    scm_gc_protect_object(procedure);

    procedure.cast()
}

/// Whether a handle is still a callable procedure.
///
/// # Safety
///
/// `poll_fn` must be null or a handle from [`lambdock_poll_fn_protect`].
pub unsafe fn lambdock_poll_fn_is_callable(poll_fn: *mut c_void) -> c_int {
    if poll_fn.is_null() {
        return 0;
    }

    let procedure: Scm = poll_fn.cast();

    c_int::from(is_true(procedure) && is_true(scm_procedure_p(procedure)))
}

/// Calls the procedure once, turning a Scheme error into "no reading".
///
/// # Safety
///
/// `poll_fn` must be null or a protected handle; `out` must be null or writable.
pub unsafe fn lambdock_poll_fn_call(
    poll_fn: *mut c_void,
    out: *mut crate::dock_item::Reading,
) -> c_int {
    let Some(out) = out.as_mut() else {
        return 0;
    };
    *out = crate::dock_item::Reading {
        kind: 0,
        number: 0.0,
        text: std::ptr::null_mut(),
    };

    if lambdock_poll_fn_is_callable(poll_fn) == 0 {
        return 0;
    }

    // `#t` as the catch tag is Guile's "catch everything".
    let value = scm_internal_catch(
        true_value(),
        Some(call_poll_proc),
        poll_fn,
        Some(handle_poll_error),
        std::ptr::null_mut(),
    );

    if scm_is_number(value) != 0 {
        out.kind = 1; // `LAMBDOCK_READING_NUMBER`
        out.number = scm_to_double(value);
        return 1;
    }

    if scm_is_string(value) != 0 {
        out.kind = 2; // `LAMBDOCK_READING_TEXT`
        // malloc'ed, freed with `free` by the caller.
        out.text = scm_to_utf8_string(value);
        return 1;
    }

    0
}

/// Releases the GC protection a handle holds.
///
/// # Safety
///
/// `poll_fn` must be null or a handle from [`lambdock_poll_fn_protect`], released
/// once.
pub unsafe fn lambdock_poll_fn_release(poll_fn: *mut c_void) {
    if lambdock_poll_fn_is_callable(poll_fn) != 0 {
        scm_gc_unprotect_object(poll_fn.cast());
    }
}

// ============================================================================
// GUI PREFERENCES
// ============================================================================
//
// The GTK preferences pane never edits `settings.scm`: it asks `(lambdock prefs)`
// to apply the value to the running dock (through the ordinary setters) and to
// remember it in the GUI override file, which the loader layers on top of the
// settings file on the next start.

/// The `default` symbol targets the fallback dock, which has no settings file and
/// therefore nothing to persist against.
fn preference_target(config_path: Option<&str>) -> Scm {
    match config_path {
        Some(path) => optional_string(Some(path)),
        None => unsafe { scm_string_to_symbol(optional_string(Some("default"))) },
    }
}

/// Hands one value to `set-dock-preference!` with its key and target.
unsafe fn call_set_preference(key: *const c_char, value: Scm, config_path: *const c_char) {
    let Some(key) = crate::cstr::non_empty_text(key) else {
        return;
    };

    let procedure = scm_c_public_ref(CORE_MODULE.as_ptr(), c"set-dock-preference!".as_ptr());
    if !is_true(procedure) {
        return;
    }

    let key = scm_string_to_symbol(optional_string(Some(&key)));
    let target = preference_target(crate::cstr::text(config_path).as_deref());

    scm_call_3(procedure, key, value, target);
}

/// Sets one boolean preference.
///
/// # Safety
///
/// The strings must be null or NUL-terminated.
pub unsafe fn set_dock_setting_bool(
    key: *const c_char,
    config_path: *const c_char,
    value: c_int,
) {
    call_set_preference(
        key,
        if value != 0 { true_value() } else { false_value() },
        config_path,
    );
}

/// Sets one integer preference.
///
/// # Safety
///
/// The strings must be null or NUL-terminated.
pub unsafe fn set_dock_setting_int(
    key: *const c_char,
    config_path: *const c_char,
    value: c_int,
) {
    call_set_preference(key, scm_from_int64(i64::from(value)), config_path);
}

/// Sets one real preference.
///
/// # Safety
///
/// The strings must be null or NUL-terminated.
pub unsafe fn set_dock_setting_double(
    key: *const c_char,
    config_path: *const c_char,
    value: f64,
) {
    call_set_preference(key, scm_from_double(value), config_path);
}

/// Sets one symbol preference (a theme, a view mode, an edge).
///
/// # Safety
///
/// The strings must be null or NUL-terminated.
pub unsafe fn set_dock_setting_symbol(
    key: *const c_char,
    config_path: *const c_char,
    value: *const c_char,
) {
    let Some(value) = crate::cstr::non_empty_text(value) else {
        return;
    };

    call_set_preference(
        key,
        scm_string_to_symbol(optional_string(Some(&value))),
        config_path,
    );
}

/// Forgets every GUI override of one dock, so its settings file is the only word.
///
/// # Safety
///
/// `config_path` must be null or NUL-terminated.
pub unsafe fn reset_dock_preferences(config_path: *const c_char) {
    let Some(config_path) = crate::cstr::non_empty_text(config_path) else {
        return;
    };

    let procedure = scm_c_public_ref(CORE_MODULE.as_ptr(), c"reset-dock-preferences!".as_ptr());
    if !is_true(procedure) {
        return;
    }

    scm_call_1(procedure, optional_string(Some(&config_path)));
}
