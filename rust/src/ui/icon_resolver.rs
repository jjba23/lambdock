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
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
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
