// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! Icon lookup: the search-path half of `src/ui/icon_resolver.h` in Rust.
//!
//! The fallback pipeline itself (`create_image_for_icon`) and the theme setup
//! still live in C: they need a `GtkIconTheme` for the active display, a
//! `GSettings` lookup and the colourised `LOG_C_*` diagnostics, none of which has
//! a Rust home yet. What does move here is the part that is a decision -- which
//! directories to search, in what order, and only if they exist -- because it is
//! built from explicit inputs and checked without a display.

use crate::glib::{g_malloc0, g_strdup};
use crate::log_debug;
use crate::log_error;
use crate::log_info;
use crate::log_warn;
use gtk4::gdk;
use gtk4::gio;
use gtk4::glib::translate::{from_glib_borrow, Borrowed, ToGlibPtr};
use gtk4::prelude::*;
use gtk4::{ffi, IconTheme, Settings};
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int};
use std::path::Path;

/// Joins a base directory and a relative path the way `g_build_filename` did,
/// without doubling a slash the caller already put at the end.
fn join(base: &str, rest: &str) -> String {
    if base.ends_with('/') {
        format!("{base}{rest}")
    } else {
        format!("{base}/{rest}")
    }
}

/// The icon directories the dock should search, in the order it should search
/// them, as a NULL-terminated array of owned paths.
///
/// Each data directory's `icons/`, then `$HOME/.local/share/icons`, then the
/// legacy `$HOME/.icons`. Only directories that exist are listed, so the order
/// is the precedence: the first one an icon is found in wins.
///
/// # Safety
///
/// `system_data_dirs` must be null or a NULL-terminated array of C strings;
/// `home` must be null or a C string.
#[no_mangle]
pub unsafe extern "C" fn lambdock_icon_search_paths_build(
    system_data_dirs: *const *const c_char,
    home: *const c_char,
) -> *mut *mut c_char {
    let mut paths: Vec<String> = Vec::new();

    if !system_data_dirs.is_null() {
        let mut index = 0;

        loop {
            let directory = *system_data_dirs.add(index);
            if directory.is_null() {
                break;
            }

            let icons = join(&CStr::from_ptr(directory).to_string_lossy(), "icons");
            if Path::new(&icons).is_dir() {
                paths.push(icons);
            }

            index += 1;
        }
    }

    if !home.is_null() && *home != 0 {
        let home = CStr::from_ptr(home).to_string_lossy();

        // A user's own icons come last, because the first match wins.
        for rest in [".local/share/icons", ".icons"] {
            let directory = join(&home, rest);
            if Path::new(&directory).is_dir() {
                paths.push(directory);
            }
        }
    }

    // NULL-terminated, the way GtkIconTheme's API wants a list, and allocated
    // with GLib so the caller's g_strfreev is the right free.
    let array = g_malloc0((paths.len() + 1) * std::mem::size_of::<*mut c_char>())
        as *mut *mut c_char;

    for (index, path) in paths.iter().enumerate() {
        if let Ok(path) = CString::new(path.as_bytes()) {
            *array.add(index) = g_strdup(path.as_ptr());
        }
    }

    array
}

/// The same list for the running process: the real XDG data directories and
/// `$HOME`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_icon_search_paths_new() -> *mut *mut c_char {
    lambdock_icon_search_paths_build(
        crate::glib::g_get_system_data_dirs(),
        crate::glib::g_get_home_dir(),
    )
}

// ===========================================================================
// The fallback pipeline
// ===========================================================================

/// Sets the pixel size on a freshly created image.
///
/// The constructors are called through the C API rather than through
/// `gtk4::Image`, because of the reference: `gtk_image_new_*` hands back a
/// *floating* reference, and the C callers sink it by appending the widget to a
/// box. `gtk4::Image` sinks on construction, so returning one would leave the
/// caller's `g_object_ref_sink` taking a second reference -- one leaked widget
/// per icon.
unsafe fn sized(image: *mut ffi::GtkWidget, icon_size: c_int) -> *mut ffi::GtkWidget {
    if !image.is_null() {
        ffi::gtk_image_set_pixel_size(image.cast(), icon_size);
    }

    image
}

/// The icon theme name from GNOME's settings, for a desktop whose GTK settings
/// carry none.
fn gsettings_icon_theme() -> Option<String> {
    let Some(source) = gio::SettingsSchemaSource::default() else {
        log_warn!(
            "UI",
            "GSettingsSchemaSource default source is NULL. Cannot read gsettings."
        );
        return None;
    };

    if source
        .lookup("org.gnome.desktop.interface", true)
        .is_none()
    {
        log_warn!(
            "UI",
            "GSettings schema 'org.gnome.desktop.interface' not found on system."
        );
        return None;
    }

    let settings = gio::Settings::new("org.gnome.desktop.interface");
    let theme = settings.string("icon-theme").to_string();

    if theme.is_empty() {
        None
    } else {
        Some(theme)
    }
}

/// Registers the search paths and resolves the icon theme name for the display.
#[no_mangle]
pub extern "C" fn setup_icon_theme() {
    crate::ui::ensure_gtk_initialized();

    let Some(display) = gdk::Display::default() else {
        log_error!("UI", "setup_icon_theme: No default GdkDisplay detected!");
        return;
    };

    let icon_theme = IconTheme::for_display(&display);
    log_info!(
        "UI",
        "Initializing Icon Theme system for display context {display:?}..."
    );

    // Steps 1 and 2: the search paths, built as a value and then applied.
    let paths = unsafe { lambdock_icon_search_paths_new() };
    if !paths.is_null() {
        let mut index = 0;

        loop {
            let path = unsafe { *paths.add(index) };
            if path.is_null() {
                break;
            }

            let path = unsafe { CStr::from_ptr(path) }
                .to_string_lossy()
                .into_owned();
            log_debug!("UI", "Adding icon search path: {path}");
            icon_theme.add_search_path(&path);
            index += 1;
        }

        unsafe { crate::glib::g_strfreev(paths) };
    }

    // Step 3: the theme name. GTK's own setting first, GSettings second.
    let settings = Settings::for_display(&display);
    let current = settings
        .gtk_icon_theme_name()
        .map(|name| name.to_string())
        .filter(|name| !name.is_empty());

    if let Some(name) = current {
        log_info!("UI", "GTK already has an active icon theme set: '{name}'");
        return;
    }

    log_info!(
        "UI",
        "No active GTK icon theme name detected. Falling back to GSettings / dconf..."
    );

    match gsettings_icon_theme() {
        Some(theme) => {
            log_info!(
                "UI",
                "Found GSettings icon theme: '{theme}'. Applying to GtkSettings..."
            );
            settings.set_gtk_icon_theme_name(Some(&theme));
        }
        None => log_warn!(
            "UI",
            "Fallback GSettings lookup failed or returned an empty string!"
        ),
    }
}

/// The six-stage icon fallback: GResource, file, icon theme, XDG pixmaps, the
/// symbolic variant, then the generic icon.
///
/// # Safety
///
/// `theme` must be a valid `GtkIconTheme`; `icon` must be null or a C string.
/// The result is a new floating widget reference, exactly like
/// `gtk_image_new_*`, or null for a missing icon name.
#[no_mangle]
pub unsafe extern "C" fn create_image_for_icon(
    theme: *mut ffi::GtkIconTheme,
    icon: *const c_char,
    icon_size: c_int,
) -> *mut ffi::GtkWidget {
    crate::ui::ensure_gtk_initialized();

    if icon.is_null() || *icon == 0 {
        log_debug!("UI", "create_image_for_icon: Passed NULL or empty icon name.");
        return std::ptr::null_mut();
    }

    let icon = CStr::from_ptr(icon).to_string_lossy().into_owned();
    log_debug!("UI", "Resolving icon: '{icon}' (target size: {icon_size}px)");

    // Step 1: the GResource bundle.
    if let Some(resource) = icon.strip_prefix("resource://") {
        log_debug!("UI", "Attempting load from resource scheme: {icon}");
        return sized(
            ffi::gtk_image_new_from_resource(resource.to_glib_none().0),
            icon_size,
        );
    }

    if icon.starts_with("/org/jointhefreeworld/lambdock/")
        && gio::resources_get_info(&icon, gio::ResourceLookupFlags::NONE).is_ok()
    {
        log_debug!("UI", "Found embedded internal resource: {icon}");
        return sized(
            ffi::gtk_image_new_from_resource(icon.to_glib_none().0),
            icon_size,
        );
    }

    // Step 2: an absolute path on disk.
    if std::path::Path::new(&icon).exists() {
        log_debug!("UI", "Found direct file path on disk: {icon}");
        return sized(
            ffi::gtk_image_new_from_file(icon.to_glib_none().0),
            icon_size,
        );
    }

    let theme: Borrowed<IconTheme> = from_glib_borrow(theme);

    // Step 3: the icon theme.
    let has_icon = theme.has_icon(&icon);
    log_debug!(
        "UI",
        "gtk_icon_theme_has_icon(theme, '{icon}'): {}",
        if has_icon { "YES" } else { "NO" }
    );

    if has_icon {
        log_debug!("UI", "Resolved '{icon}' successfully via GTK Icon Theme!");
        return sized(
            ffi::gtk_image_new_from_icon_name(icon.to_glib_none().0),
            icon_size,
        );
    }

    // Step 4: the XDG pixmap directories.
    let directories = crate::glib::g_get_system_data_dirs();

    if !directories.is_null() {
        let mut index = 0;

        loop {
            let directory = *directories.add(index);
            if directory.is_null() {
                break;
            }

            let directory = CStr::from_ptr(directory).to_string_lossy();
            let pixmap = format!("{directory}/pixmaps/{icon}.png");

            if std::path::Path::new(&pixmap).exists() {
                log_debug!("UI", "Resolved '{icon}' in pixmaps folder: {pixmap}");
                return sized(
                    ffi::gtk_image_new_from_file(pixmap.to_glib_none().0),
                    icon_size,
                );
            }

            index += 1;
        }
    }

    // Step 5: the symbolic variant of the same name.
    let symbolic = format!("{icon}-symbolic");
    let has_symbolic = theme.has_icon(&symbolic);
    log_debug!(
        "UI",
        "gtk_icon_theme_has_icon(theme, '{symbolic}'): {}",
        if has_symbolic { "YES" } else { "NO" }
    );

    if has_symbolic {
        log_debug!("UI", "Resolved '{icon}' using symbolic fallback: {symbolic}");
        return sized(
            ffi::gtk_image_new_from_icon_name(symbolic.to_glib_none().0),
            icon_size,
        );
    }

    // Step 6: the generic executable icon, so an item is never iconless.
    log_warn!(
        "UI",
        "Failed to resolve icon '{icon}'. Falling back to 'application-x-executable'"
    );
    sized(
        ffi::gtk_image_new_from_icon_name(c"application-x-executable".as_ptr()),
        icon_size,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::os::raw::c_void;
    use std::path::PathBuf;

    extern "C" {
        fn g_free(memory: *mut c_void);
    }

    fn temporary_root(name: &str) -> PathBuf {
        let root = std::env::temp_dir().join(format!("lambdock-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("a temporary directory");
        root
    }

    /// Reads a `char**` list and frees it the way `g_strfreev` would.
    fn take(paths: *mut *mut c_char) -> Vec<String> {
        assert!(!paths.is_null());

        let mut read = Vec::new();
        let mut index = 0;

        loop {
            let path = unsafe { *paths.add(index) };
            if path.is_null() {
                break;
            }

            read.push(
                unsafe { CStr::from_ptr(path) }
                    .to_string_lossy()
                    .into_owned(),
            );
            unsafe { g_free(path.cast()) };
            index += 1;
        }

        unsafe { g_free(paths.cast()) };
        read
    }

    fn build(system: &[CString], home: Option<&CString>) -> Vec<String> {
        let mut pointers: Vec<*const c_char> = system.iter().map(|dir| dir.as_ptr()).collect();
        pointers.push(std::ptr::null());

        take(unsafe {
            lambdock_icon_search_paths_build(
                pointers.as_ptr(),
                home.map_or(std::ptr::null(), |home| home.as_ptr()),
            )
        })
    }

    #[test]
    fn only_directories_that_exist_are_listed_in_search_order() {
        let root = temporary_root("icons");
        let system_a = root.join("a");
        let system_b = root.join("b");
        let home = root.join("home");

        fs::create_dir_all(system_a.join("icons")).expect("a/icons");
        fs::create_dir_all(&system_b).expect("b (no icons/ inside)");
        fs::create_dir_all(home.join(".icons")).expect("legacy icons");

        let (a, b) = (
            CString::new(system_a.to_str().unwrap()).unwrap(),
            CString::new(system_b.to_str().unwrap()).unwrap(),
        );
        let home = CString::new(home.to_str().unwrap()).unwrap();

        let paths = build(&[a.clone(), b.clone()], Some(&home));

        // b has no icons/ subdirectory and the XDG one does not exist yet.
        assert_eq!(
            paths,
            vec![
                system_a.join("icons").to_str().unwrap().to_owned(),
                root.join("home/.icons").to_str().unwrap().to_owned(),
            ]
        );

        // Creating the XDG directory slots it in before the legacy one.
        fs::create_dir_all(root.join("home/.local/share/icons")).expect("xdg icons");
        let paths = build(&[a, b], Some(&home));

        assert_eq!(
            paths,
            vec![
                system_a.join("icons").to_str().unwrap().to_owned(),
                root.join("home/.local/share/icons")
                    .to_str()
                    .unwrap()
                    .to_owned(),
                root.join("home/.icons").to_str().unwrap().to_owned(),
            ]
        );

        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn no_inputs_is_an_empty_list_not_a_null_one() {
        assert!(build(&[], None).is_empty());
    }

    #[test]
    fn an_empty_home_is_no_home() {
        let root = temporary_root("icons-empty-home");
        let system = root.join("a");
        fs::create_dir_all(system.join("icons")).expect("a/icons");

        let a = CString::new(system.to_str().unwrap()).unwrap();
        let empty = CString::new("").unwrap();

        assert_eq!(build(&[a], Some(&empty)).len(), 1);

        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn a_trailing_slash_does_not_double() {
        assert_eq!(join("/usr/share/", "icons"), "/usr/share/icons");
        assert_eq!(join("/usr/share", "icons"), "/usr/share/icons");
    }

    #[test]
    fn the_process_list_only_names_directories_that_exist() {
        let paths = take(unsafe { lambdock_icon_search_paths_new() });

        for path in paths {
            assert!(Path::new(&path).is_dir(), "{path}");
        }
    }
}
