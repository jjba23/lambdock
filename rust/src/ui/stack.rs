// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The stack's path arithmetic: the first half of `src/ui/stack.c` in Rust.
//!
//! A stack is a folder shown as a dock icon, and the whole of its navigation is
//! string arithmetic over paths: expand `~`, join a child, walk back to a parent
//! without ever leaving the stack's own folder, and say where you are in one line.
//! That is a decision about strings, so it is a value here and it is tested; the
//! folder listing, the thumbnails and the view that draws them are still C, and
//! move in their own step.
//!
//! The stack's ceiling is what makes this more than a join: `lambdock_stack_parent_path`
//! refuses to step above the folder the stack was given, and the breadcrumb says
//! only where inside that folder you are.

use crate::glib::strdup_string;
use gtk4::glib;
use std::os::raw::{c_char, c_int};

/// `LambdockStackView`.
pub const VIEW_GRID: c_int = 0;
pub const VIEW_LIST: c_int = 1;

/// The two view modes, by their Scheme names.
const VIEWS: [(&str, c_int); 2] = [("grid", VIEW_GRID), ("list", VIEW_LIST)];

/// `path` with its home directory expanded, or `None` for an empty path.
fn expand_path(path: Option<&str>) -> Option<String> {
    let path = path.filter(|path| !path.is_empty())?;

    let home = glib::home_dir().to_string_lossy().into_owned();

    match path {
        "~" => Some(home),
        _ => match path.strip_prefix("~/") {
            Some(rest) => Some(join(&home, rest)),
            None => Some(path.to_owned()),
        },
    }
}

/// `path` without trailing slashes, but "/" stays "/".
fn strip_trailing_slashes(path: &str) -> String {
    let trimmed = path.trim_end_matches('/');

    if trimmed.is_empty() {
        // Everything was slashes: the path is the root, not the empty string.
        String::from("/")
    } else {
        trimmed.to_owned()
    }
}

/// Joins two path components the way `g_build_filename` does: one separator
/// between them, and none doubled.
fn join(dir: &str, name: &str) -> String {
    let dir = dir.trim_end_matches('/');
    let name = name.trim_start_matches('/');

    if name.is_empty() {
        return dir.to_owned();
    }

    if dir.is_empty() {
        return format!("/{name}");
    }

    format!("{dir}/{name}")
}

/// The path of `name` inside `dir`, or `None` when either is empty.
fn child_path(dir: Option<&str>, name: Option<&str>) -> Option<String> {
    let dir = dir.filter(|dir| !dir.is_empty())?;
    let name = name.filter(|name| !name.is_empty())?;

    Some(join(dir, name))
}

/// The folder above `path`, as long as it is still inside `root`.
///
/// `None` at the stack's own folder, and for a path that is not inside the stack
/// at all: a stack does not walk out of the folder it was given.
fn parent_path(path: Option<&str>, root: Option<&str>) -> Option<String> {
    let path = path.filter(|path| !path.is_empty())?;
    let root = root.filter(|root| !root.is_empty())?;

    let here = strip_trailing_slashes(path);
    let ceiling = strip_trailing_slashes(root);

    if here == ceiling || !is_inside(&here, &ceiling) {
        return None;
    }

    let parent = dirname(&here);

    // Only report a parent that is still inside the stack.
    if parent == ceiling || is_inside(&parent, &ceiling) {
        Some(parent)
    } else {
        Some(ceiling)
    }
}

/// Where `path` is, in one line: the stack's own label, then every folder below it.
fn breadcrumb(path: Option<&str>, root: Option<&str>, root_label: Option<&str>) -> String {
    let here = path
        .filter(|path| !path.is_empty())
        .map(strip_trailing_slashes);
    let ceiling = root
        .filter(|root| !root.is_empty())
        .map(strip_trailing_slashes);

    let fallback = ceiling
        .as_deref()
        .map(basename)
        .filter(|name| !name.is_empty());

    let label = match root_label.filter(|label| !label.is_empty()) {
        Some(label) => label.to_owned(),
        None => match fallback {
            Some(name) => name,
            // Not: a translated default, in a module that is not allowed to be
            // silent about where you are.
            None => translated(c"Stack"),
        },
    };

    // Not inside the stack (or no stack at all): just say where we are.
    let (Some(here), Some(ceiling)) = (here, ceiling) else {
        return label;
    };
    if here == ceiling || !is_inside(&here, &ceiling) {
        return label;
    }

    let rest = &here[ceiling.len()..];
    let mut crumb = label;

    for part in rest.split('/').filter(|part| !part.is_empty()) {
        crumb.push_str(" / ");
        crumb.push_str(part);
    }

    crumb
}

/// Whether `path` sits under `root` (and not merely shares its prefix).
fn is_inside(path: &str, root: &str) -> bool {
    match path.strip_prefix(root) {
        Some(rest) => rest.starts_with('/') || root.ends_with('/'),
        None => false,
    }
}

/// The last component of a path.
fn basename(path: &str) -> String {
    path.rsplit('/').next().unwrap_or(path).to_owned()
}

/// The folder a path is in, as `g_path_get_dirname` gives it.
fn dirname(path: &str) -> String {
    match path.rsplit_once('/') {
        Some(("", _)) => String::from("/"),
        Some((dir, _)) => dir.to_owned(),
        None => String::from("."),
    }
}

fn translated(msgid: &std::ffi::CStr) -> String {
    unsafe {
        let message = crate::glib::tr(msgid);
        if message.is_null() {
            return String::new();
        }

        std::ffi::CStr::from_ptr(message)
            .to_string_lossy()
            .into_owned()
    }
}

unsafe fn text(pointer: *const c_char) -> Option<String> {
    if pointer.is_null() {
        return None;
    }

    Some(
        std::ffi::CStr::from_ptr(pointer)
            .to_string_lossy()
            .into_owned(),
    )
}

/// `path` with its home directory expanded.
///
/// # Safety
///
/// `path` must be null or NUL-terminated; the result is `(transfer full)`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_stack_expand_path(path: *const c_char) -> *mut c_char {
    match expand_path(text(path).as_deref()) {
        Some(path) => strdup_string(&path),
        None => std::ptr::null_mut(),
    }
}

/// The path of `name` inside `dir`.
///
/// # Safety
///
/// Both must be null or NUL-terminated; the result is `(transfer full)`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_stack_child_path(
    dir: *const c_char,
    name: *const c_char,
) -> *mut c_char {
    match child_path(text(dir).as_deref(), text(name).as_deref()) {
        Some(path) => strdup_string(&path),
        None => std::ptr::null_mut(),
    }
}

/// The folder above `path`, as long as it is still inside `root`.
///
/// # Safety
///
/// Both must be null or NUL-terminated; the result is `(transfer full)`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_stack_parent_path(
    path: *const c_char,
    root: *const c_char,
) -> *mut c_char {
    match parent_path(text(path).as_deref(), text(root).as_deref()) {
        Some(path) => strdup_string(&path),
        None => std::ptr::null_mut(),
    }
}

/// Where `path` is, in one line.
///
/// # Safety
///
/// Every argument must be null or NUL-terminated; the result is `(transfer full)`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_stack_breadcrumb(
    path: *const c_char,
    root: *const c_char,
    root_label: *const c_char,
) -> *mut c_char {
    let crumb = breadcrumb(
        text(path).as_deref(),
        text(root).as_deref(),
        text(root_label).as_deref(),
    );

    strdup_string(&crumb)
}

/// The view mode a Scheme name names; an unknown name is the grid.
///
/// # Safety
///
/// `name` must be null or NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lambdock_stack_view_from_string(name: *const c_char) -> c_int {
    let Some(name) = text(name) else {
        return VIEW_GRID;
    };

    VIEWS
        .iter()
        .find(|(candidate, _)| candidate.eq_ignore_ascii_case(&name))
        .map_or(VIEW_GRID, |(_, view)| *view)
}

/// The Scheme name of a view mode.
#[no_mangle]
pub extern "C" fn lambdock_stack_view_to_string(view: c_int) -> *const c_char {
    match view {
        VIEW_LIST => c"list".as_ptr(),
        _ => c"grid".as_ptr(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// `~` means the home directory, and only at the front.
    #[test]
    fn a_leading_tilde_is_the_home_directory() {
        let home = glib::home_dir().to_string_lossy().into_owned();

        assert_eq!(expand_path(Some("~")).as_deref(), Some(home.as_str()));
        assert_eq!(
            expand_path(Some("~/Downloads")).as_deref(),
            Some(format!("{home}/Downloads").as_str())
        );

        // A tilde anywhere else is a character in a file name.
        assert_eq!(expand_path(Some("/tmp/~x")).as_deref(), Some("/tmp/~x"));
        assert_eq!(expand_path(Some("~x")).as_deref(), Some("~x"));
        assert_eq!(expand_path(None), None);
        assert_eq!(expand_path(Some("")), None);
    }

    /// Trailing slashes go, and the root stays the root.
    #[test]
    fn trailing_slashes_are_stripped_but_the_root_is_not() {
        assert_eq!(strip_trailing_slashes("/a/b/"), "/a/b");
        assert_eq!(strip_trailing_slashes("/a/b"), "/a/b");
        assert_eq!(strip_trailing_slashes("/"), "/");
        assert_eq!(strip_trailing_slashes("///"), "/");
    }

    /// A child path has one separator, never two.
    #[test]
    fn a_child_path_joins_with_one_separator() {
        assert_eq!(
            child_path(Some("/home/me/Pictures"), Some("cat.png")).as_deref(),
            Some("/home/me/Pictures/cat.png")
        );
        assert_eq!(
            child_path(Some("/home/me/Pictures/"), Some("/cat.png")).as_deref(),
            Some("/home/me/Pictures/cat.png")
        );
        assert_eq!(child_path(Some(""), Some("x")), None);
        assert_eq!(child_path(Some("/a"), Some("")), None);
        assert_eq!(child_path(None, Some("x")), None);
    }

    /// Going up stops at the stack's own folder, and a path outside the stack is
    /// not walked at all.
    #[test]
    fn going_up_stops_at_the_stacks_folder() {
        let root = "/home/me/Pictures";

        assert_eq!(
            parent_path(Some("/home/me/Pictures/a/b"), Some(root)).as_deref(),
            Some("/home/me/Pictures/a")
        );
        assert_eq!(
            parent_path(Some("/home/me/Pictures/a"), Some(root)).as_deref(),
            Some(root)
        );

        // At the ceiling there is nowhere to go back to.
        assert_eq!(parent_path(Some(root), Some(root)), None);
        assert_eq!(parent_path(Some("/home/me/Pictures/"), Some(root)), None);

        // Outside the stack: not a path this walk may take.
        assert_eq!(parent_path(Some("/etc"), Some(root)), None);

        // A folder that merely shares the prefix is not inside the stack.
        assert_eq!(parent_path(Some("/home/me/Pictures2/a"), Some(root)), None);
    }

    /// The breadcrumb names the stack, then every folder below it.
    #[test]
    fn the_breadcrumb_names_the_stack_and_the_folders_below_it() {
        let root = "/home/me/Pictures";

        // At the folder itself: just the label.
        assert_eq!(
            breadcrumb(Some(root), Some(root), Some("Pictures")),
            "Pictures"
        );

        // Below it: the label and one part per folder, in order.
        assert_eq!(
            breadcrumb(
                Some("/home/me/Pictures/trips/alps"),
                Some(root),
                Some("Pictures")
            ),
            "Pictures / trips / alps"
        );

        // A label of its own wins over the folder's name...
        assert_eq!(
            breadcrumb(
                Some("/home/me/Pictures/trips"),
                Some(root),
                Some("My Pictures")
            ),
            "My Pictures / trips"
        );
        // ...and without one the folder's name is used.
        assert_eq!(
            breadcrumb(Some("/home/me/Pictures/trips"), Some(root), None),
            "Pictures / trips"
        );
        assert_eq!(
            breadcrumb(Some("/home/me/Pictures/trips"), Some(root), Some("")),
            "Pictures / trips"
        );

        // Not inside the stack: only where we are, never a misbreadcrumb.
        assert_eq!(
            breadcrumb(Some("/etc"), Some(root), Some("Pictures")),
            "Pictures"
        );
        assert_eq!(breadcrumb(None, Some(root), Some("Pictures")), "Pictures");
        // No stack at all: the translated default.
        assert_eq!(breadcrumb(None, None, None), translated(c"Stack"));
    }

    /// The view modes round-trip through their Scheme names, and an unknown name
    /// is the grid.
    #[test]
    fn the_view_names_round_trip() {
        unsafe {
            assert_eq!(lambdock_stack_view_from_string(c"grid".as_ptr()), VIEW_GRID);
            assert_eq!(lambdock_stack_view_from_string(c"list".as_ptr()), VIEW_LIST);
            assert_eq!(lambdock_stack_view_from_string(c"LIST".as_ptr()), VIEW_LIST);
            assert_eq!(
                lambdock_stack_view_from_string(c"tiles".as_ptr()),
                VIEW_GRID
            );
            assert_eq!(lambdock_stack_view_from_string(std::ptr::null()), VIEW_GRID);
        }

        unsafe {
            assert_eq!(
                std::ffi::CStr::from_ptr(lambdock_stack_view_to_string(VIEW_LIST)),
                c"list"
            );
            assert_eq!(
                std::ffi::CStr::from_ptr(lambdock_stack_view_to_string(VIEW_GRID)),
                c"grid"
            );
        }
    }
}
