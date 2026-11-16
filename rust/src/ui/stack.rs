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

use crate::glib::{g_malloc0, strdup_string};
use gtk4::glib;
use gtk4::gdk;
use gtk4::prelude::*;
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

/// One entry of a listed folder: `LambdockStackEntry` from the header, field for
/// field. The C side reads these fields directly, so the order is the contract.
#[repr(C)]
pub struct StackEntry {
    pub name: *mut c_char,
    pub path: *mut c_char,
    pub is_dir: c_int,
}

/// `STACK_MAX_ENTRIES` in `stack.h`: how many entries one panel will show.
const MAX_ENTRIES: u32 = 300;

/// `g_utf8_collate`, so the listing sorts the way the user's locale does.
fn collate(left: &str, right: &str) -> std::cmp::Ordering {
    let left = std::ffi::CString::new(left).unwrap_or_default();
    let right = std::ffi::CString::new(right).unwrap_or_default();

    unsafe { glib::ffi::g_utf8_collate(left.as_ptr(), right.as_ptr()).cmp(&0) }
}

unsafe extern "C" fn free_entry(data: *mut std::ffi::c_void) {
    lambdock_stack_entry_free(data.cast());
}

/// Frees one entry; null is a no-op.
///
/// # Safety
///
/// `entry` must be null or a record this module allocated, not freed twice.
#[no_mangle]
pub unsafe extern "C" fn lambdock_stack_entry_free(entry: *mut StackEntry) {
    if entry.is_null() {
        return;
    }

    crate::glib::g_free((*entry).name.cast());
    crate::glib::g_free((*entry).path.cast());
    crate::glib::g_free(entry.cast());
}

/// Frees a whole listing, entries and array.
///
/// # Safety
///
/// `entries` must be null or an array this module created.
#[no_mangle]
pub unsafe extern "C" fn lambdock_stack_entries_free(entries: *mut glib::ffi::GPtrArray) {
    if !entries.is_null() {
        glib::ffi::g_ptr_array_free(entries, glib::ffi::GTRUE);
    }
}

/// The visible children of `path`, folders first and then by name.
///
/// Hidden files stay hidden, as in any file manager; the listing stops one name
/// past `limit`, because that is what tells the panel the folder has more instead
/// of silently hiding the rest.
///
/// # Safety
///
/// `path` must be null or NUL-terminated; `truncated` must be null or point at a
/// writable `gboolean`. The result is `(transfer full)`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_stack_list_directory_limited(
    path: *const c_char,
    limit: u32,
    truncated: *mut c_int,
) -> *mut glib::ffi::GPtrArray {
    let entries = glib::ffi::g_ptr_array_new_with_free_func(Some(free_entry));

    if !truncated.is_null() {
        *truncated = 0;
    }

    let limit = if limit == 0 { MAX_ENTRIES } else { limit };

    let Some(dir_path) = expand_path(text(path).as_deref()) else {
        return entries;
    };

    let directory = match std::fs::read_dir(&dir_path) {
        Ok(directory) => directory,
        Err(_) => {
            crate::log_warn!("UI", "Stack path is not a readable directory: {dir_path}");
            return entries;
        }
    };

    let mut listed: Vec<*mut StackEntry> = Vec::new();

    for child in directory.flatten() {
        let name = child.file_name().to_string_lossy().into_owned();
        if name.starts_with('.') {
            continue;
        }

        // One name past the cap is enough to know the folder has more.
        if listed.len() as u32 >= limit {
            if !truncated.is_null() {
                *truncated = 1;
            }
            break;
        }

        let child_path = join(&dir_path, &name);
        let is_dir = std::path::Path::new(&child_path).is_dir();

        let entry = g_malloc0(std::mem::size_of::<StackEntry>()).cast::<StackEntry>();
        (*entry).name = strdup_string(&name);
        (*entry).path = strdup_string(&child_path);
        (*entry).is_dir = c_int::from(is_dir);

        listed.push(entry);
    }

    // Folders first, then names: the order a file manager would show.
    listed.sort_by(|left, right| {
        let left_dir = (**left).is_dir != 0;
        let right_dir = (**right).is_dir != 0;

        match left_dir.cmp(&right_dir) {
            std::cmp::Ordering::Equal => collate(
                &text((**left).name).unwrap_or_default(),
                &text((**right).name).unwrap_or_default(),
            ),
            // `true` sorts last, and folders go first.
            ordering => ordering.reverse(),
        }
    });

    for entry in listed {
        glib::ffi::g_ptr_array_add(entries, entry.cast());
    }

    entries
}

/// The visible children of `path`, capped at `STACK_MAX_ENTRIES`.
///
/// # Safety
///
/// `path` must be null or NUL-terminated; the result is `(transfer full)`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_stack_list_directory(
    path: *const c_char,
) -> *mut glib::ffi::GPtrArray {
    lambdock_stack_list_directory_limited(path, MAX_ENTRIES, std::ptr::null_mut())
}

// ============================================================================
// WHAT DESERVES A THUMBNAIL
// ============================================================================
//
// A thumbnail decodes a real image, so only files that are plausibly images are
// worth opening: no giant files, and no images so large that decoding them would
// stall the dock for a second. Reading an image header is cheap, which is how the
// view decides up front which cells deserve a decode and which keep their type
// icon.

/// `STACK_THUMBNAIL_MAX_BYTES` and `STACK_THUMBNAIL_MAX_PIXELS` in `stack.c`.
const THUMBNAIL_MAX_BYTES: u64 = 32 * 1024 * 1024;
const THUMBNAIL_MAX_PIXELS: i64 = 80 * 1000 * 1000;

// GdkPixbuf's header reader, which is what decides whether a file is an image at
// all. The dock links the library through GTK already; only the C type of the
// format it returns is ignored here.
extern "C" {
    fn gdk_pixbuf_get_file_info(
        filename: *const c_char,
        width: *mut c_int,
        height: *mut c_int,
    ) -> *mut std::ffi::c_void;
}

/// Whether `path` is a file worth decoding into a thumbnail.
///
/// Directories, FIFOs and sockets have no thumbnail; a file over the size cap is
/// not read at all; and an image whose dimensions would take a second to decode
/// keeps its type icon instead.
pub fn path_has_thumbnail(path: &std::path::Path) -> bool {
    let Ok(metadata) = std::fs::metadata(path) else {
        return false;
    };
    if !metadata.is_file() {
        return false;
    }

    if metadata.len() == 0 || metadata.len() > THUMBNAIL_MAX_BYTES {
        return false;
    }

    // Only the header is read here, and only GdkPixbuf's formats count: a text
    // file with a .png name is not an image.
    let Some(path) = std::ffi::CString::new(path.to_string_lossy().as_bytes())
        .ok()
        .filter(|path| !path.as_bytes().is_empty())
    else {
        return false;
    };

    let (mut width, mut height) = (0, 0);
    let format = unsafe { gdk_pixbuf_get_file_info(path.as_ptr(), &mut width, &mut height) };
    if format.is_null() || width <= 0 || height <= 0 {
        return false;
    }

    i64::from(width) * i64::from(height) <= THUMBNAIL_MAX_PIXELS
}

/// Whether the file at `path` is worth decoding into a thumbnail.
///
/// # Safety
///
/// `path` must be null or NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lambdock_stack_path_has_thumbnail(path: *const c_char) -> c_int {
    match text(path).filter(|path| !path.is_empty()) {
        Some(path) => c_int::from(path_has_thumbnail(std::path::Path::new(&path))),
        None => 0,
    }
}


// ============================================================================
// THE VIEW'S KEYBOARD
// ============================================================================
//
// A grid of tiles is expected to answer the arrow keys as a grid, and a list as a
// list: one step is one entry sideways, and one *row* up or down -- which is why
// the number of columns the wrapped grid put on its first row matters. The
// controller captures the keys before the FlowBox does, because a flow box has its
// own cursor bindings and those would otherwise swallow what the stack needs.

/// The entry at `index`, or `None` when the view is shorter than that.
///
/// A flow box wraps each entry in a `GtkFlowBoxChild`, and the focusable widget is
/// the button inside it.
fn child_at(view: &gtk4::Widget, index: usize) -> Option<gtk4::Widget> {
    if let Some(flow) = view.downcast_ref::<gtk4::FlowBox>() {
        let child = flow.child_at_index(index as i32)?;
        return Some(
            child
                .child()
                .unwrap_or_else(|| child.clone().upcast::<gtk4::Widget>()),
        );
    }

    let mut child = view.first_child();
    let mut seen = 0;
    while let Some(each) = child {
        if seen == index {
            return Some(each);
        }

        seen += 1;
        child = each.next_sibling();
    }

    None
}

/// How many entries the view holds.
fn child_count(view: &gtk4::Widget) -> usize {
    if let Some(flow) = view.downcast_ref::<gtk4::FlowBox>() {
        let mut count = 0;
        while flow.child_at_index(count as i32).is_some() {
            count += 1;
        }
        return count as usize;
    }

    let mut count = 0;
    let mut child = view.first_child();
    while let Some(each) = child {
        count += 1;
        child = each.next_sibling();
    }

    count
}

/// How many cards the wrapped grid put on its first row, which is how far one
/// "row" is for Up/Down. A grid that has not been allocated yet reports one.
fn grid_columns(flow: &gtk4::FlowBox) -> i32 {
    let Some(first) = flow.child_at_index(0) else {
        return 1;
    };
    let flow_widget = flow.upcast_ref::<gtk4::Widget>();

    let Some(first_bounds) = first.compute_bounds(flow_widget) else {
        return 1;
    };

    let mut columns = 0;
    let mut index = 0;
    while let Some(child) = flow.child_at_index(index) {
        if let Some(bounds) = child.compute_bounds(flow_widget) {
            if (bounds.y() - first_bounds.y()).abs() > 1.0 {
                break;
            }
            columns += 1;
        }

        index += 1;
    }

    columns.max(1)
}

/// Index of the entry that holds the keyboard focus, if any.
fn focused_index(view: &gtk4::Widget) -> Option<usize> {
    let window = view.root().and_then(|root| root.downcast::<gtk4::Window>().ok())?;
    let focus: gtk4::Widget = gtk4::prelude::GtkWindowExt::focus(&window)?;

    let mut index = 0;
    let mut child = view.first_child();
    while let Some(each) = child {
        // The focus can sit on a label inside an entry, so the entry is the nearest
        // ancestor that matches -- and the view itself is the wall.
        let mut widget = Some(focus.clone());
        while let Some(candidate) = widget {
            if candidate == each {
                return Some(index);
            }
            if candidate == *view {
                break;
            }

            widget = candidate.parent();
        }

        index += 1;
        child = each.next_sibling();
    }

    None
}

/// The index one step lands on, clamped into the view.
fn stepped_index(current: usize, dx: i32, dy: i32, columns: i32, count: usize) -> usize {
    let next = current as i32 + dx + dy * columns;

    next.clamp(0, count.saturating_sub(1) as i32) as usize
}

/// Moves the focus by one step: `dx` entries sideways and `dy` rows down, where a
/// row is as wide as the grid.
fn focus_step(view: &gtk4::Widget, dx: i32, dy: i32) -> bool {
    let count = child_count(view);
    if count == 0 {
        return false;
    }

    let Some(current) = focused_index(view) else {
        return focus_edge(view, false);
    };

    let columns = match view.downcast_ref::<gtk4::FlowBox>() {
        Some(flow) => grid_columns(flow),
        None => 1,
    };

    let next = stepped_index(current, dx, dy, columns, count);
    if next == current {
        return true;
    }

    let Some(target) = child_at(view, next) else {
        return false;
    };

    target.grab_focus();

    true
}

/// Moves the focus to the first or last entry.
fn focus_edge(view: &gtk4::Widget, last: bool) -> bool {
    let count = child_count(view);
    if count == 0 {
        return false;
    }

    let Some(target) = child_at(view, if last { count - 1 } else { 0 }) else {
        return false;
    };

    target.grab_focus();

    true
}

/// Installs the view's arrow-key handling, once per view.
///
/// # Safety
///
/// `view` must be null or a live `GtkWidget`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_stack_view_attach_keys(view: *mut gtk4::ffi::GtkWidget) {
    if view.is_null() {
        return;
    }

    crate::ui::ensure_gtk_initialized();

    let view: gtk4::Widget = crate::ui::widget_from_pointer(view);

    let keys = gtk4::EventControllerKey::new();
    // Capture: a flow box has its own arrow-key bindings for moving its cursor, and
    // those would otherwise swallow the keys the stack navigation needs.
    keys.set_propagation_phase(gtk4::PropagationPhase::Capture);

    let target = view.clone();
    keys.connect_key_pressed(move |_, keyval, _, _| {
        let handled = match keyval {
            gdk::Key::Left => focus_step(&target, -1, 0),
            gdk::Key::Right => focus_step(&target, 1, 0),
            gdk::Key::Up => focus_step(&target, 0, -1),
            gdk::Key::Down => focus_step(&target, 0, 1),
            gdk::Key::Home => focus_edge(&target, false),
            gdk::Key::End => focus_edge(&target, true),
            _ => return glib::Propagation::Proceed,
        };

        // The keys are the view's whether or not there was somewhere to move to.
        let _ = handled;
        glib::Propagation::Stop
    });

    view.add_controller(keys);
}

/// Moves the view's focus by one step.
///
/// # Safety
///
/// `view` must be null or a live `GtkWidget`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_stack_view_focus_step(
    view: *mut gtk4::ffi::GtkWidget,
    dx: c_int,
    dy: c_int,
) -> c_int {
    if view.is_null() {
        return 0;
    }

    let view: gtk4::Widget = crate::ui::widget_from_pointer(view);
    c_int::from(focus_step(&view, dx, dy))
}

/// Moves the view's focus to its first or last entry.
///
/// # Safety
///
/// `view` must be null or a live `GtkWidget`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_stack_view_focus_edge(
    view: *mut gtk4::ffi::GtkWidget,
    last: c_int,
) -> c_int {
    if view.is_null() {
        return 0;
    }

    let view: gtk4::Widget = crate::ui::widget_from_pointer(view);
    c_int::from(focus_edge(&view, last != 0))
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

    // ======================================================================
    // The listing
    // ======================================================================

    /// What a listing of `root` holds, as `(name, is_dir)`.
    unsafe fn listed(root: &std::path::Path) -> (Vec<(String, bool)>, bool) {
        let path = std::ffi::CString::new(root.to_string_lossy().as_bytes()).expect("no NUL");
        let mut truncated = 0;
        let entries = lambdock_stack_list_directory_limited(path.as_ptr(), 0, &mut truncated);

        let mut out = Vec::new();
        for index in 0..(*entries).len as usize {
            let entry = *(*entries).pdata.add(index) as *mut StackEntry;
            out.push((
                text((*entry).name).unwrap_or_default(),
                (*entry).is_dir != 0,
            ));
        }

        lambdock_stack_entries_free(entries);
        (out, truncated != 0)
    }

    /// A listing is folders first and then by name, with dot files left out.
    #[test]
    fn the_listing_is_folders_first_then_names() {
        let root = std::env::temp_dir().join("lambdock-stack-listing");
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(root.join("zulu")).expect("dir");
        std::fs::create_dir_all(root.join("alpha")).expect("dir");
        std::fs::write(root.join("beta.txt"), "x").expect("file");
        std::fs::write(root.join("Alpha.txt"), "x").expect("file");
        std::fs::write(root.join(".hidden"), "x").expect("file");

        unsafe {
            let (entries, truncated) = listed(&root);

            assert!(!truncated);
            assert_eq!(
                entries,
                [
                    (String::from("alpha"), true),
                    (String::from("zulu"), true),
                    (String::from("Alpha.txt"), false),
                    (String::from("beta.txt"), false),
                ],
                "folders first, then names collated"
            );
        }

        let _ = std::fs::remove_dir_all(&root);
    }

    /// The cap stops the listing one name past the limit and says so, instead of
    /// silently hiding the rest of the folder.
    #[test]
    fn a_listing_stops_at_its_cap_and_says_so() {
        let root = std::env::temp_dir().join("lambdock-stack-capped");
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).expect("dir");
        for name in ["a", "b", "c"] {
            std::fs::write(root.join(name), "x").expect("file");
        }

        let path = std::ffi::CString::new(root.to_string_lossy().as_bytes()).expect("no NUL");

        unsafe {
            let mut truncated = 0;
            let entries = lambdock_stack_list_directory_limited(path.as_ptr(), 2, &mut truncated);
            assert_eq!((*entries).len, 2);
            assert_ne!(truncated, 0, "the folder has more than the cap");
            lambdock_stack_entries_free(entries);

            // Exactly the cap is not truncation.
            let mut truncated = 0;
            let entries = lambdock_stack_list_directory_limited(path.as_ptr(), 3, &mut truncated);
            assert_eq!((*entries).len, 3);
            assert_eq!(truncated, 0);
            lambdock_stack_entries_free(entries);
        }

        let _ = std::fs::remove_dir_all(&root);
    }

    /// A missing path lists nothing instead of crashing. The unreadable-directory
    /// case is not checked here on purpose: it warns (as a stack whose folder went
    /// away should), and `cargo test` output stays pristine.
    #[test]
    fn a_missing_path_lists_nothing() {
        unsafe {
            let entries = lambdock_stack_list_directory(std::ptr::null());
            assert_eq!((*entries).len, 0);
            lambdock_stack_entries_free(entries);
            lambdock_stack_entries_free(std::ptr::null_mut());
        }
    }

    /// A small image is worth a decode; a file that is not an image is not, and
    /// neither is one too large to read.
    #[test]
    fn only_plausible_images_deserve_a_thumbnail() {
        let root = std::env::temp_dir().join("lambdock-stack-thumbnails");
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).expect("dir");

        // A real one-pixel PNG: the bytes are the format, so nothing has to be
        // installed to write one.
        const ONE_PIXEL_PNG: [u8; 69] = [
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
            0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00,
            0x00, 0x90, 0x77, 0x53, 0xde, 0x00, 0x00, 0x00, 0x0c, 0x49, 0x44, 0x41, 0x54, 0x08,
            0xd7, 0x63, 0xf8, 0xcf, 0xc0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xdd, 0x8d,
            0xb0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
        ];

        let image = root.join("tiny.png");
        std::fs::write(&image, ONE_PIXEL_PNG).expect("write png");

        let note = root.join("notes.txt");
        std::fs::write(&note, "not an image").expect("write");

        assert!(path_has_thumbnail(&image));

        // Not an image GdkPixbuf can read.
        assert!(!path_has_thumbnail(&note));
        // A directory, a missing file, and an empty file.
        assert!(!path_has_thumbnail(&root));
        assert!(!path_has_thumbnail(&root.join("missing.png")));
        let empty = root.join("empty.png");
        std::fs::write(&empty, "").expect("write");
        assert!(!path_has_thumbnail(&empty));

        let _ = std::fs::remove_dir_all(&root);
    }

    /// A file over the size cap is not even opened.
    #[test]
    fn a_file_too_large_to_read_is_not_a_thumbnail() {
        let root = std::env::temp_dir().join("lambdock-stack-big");
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).expect("dir");

        let big = root.join("big.png");
        let file = std::fs::File::create(&big).expect("create");
        // Sparse: the size is what this decision reads, not the contents.
        file.set_len(THUMBNAIL_MAX_BYTES + 1).expect("set_len");
        drop(file);

        assert!(!path_has_thumbnail(&big));

        let _ = std::fs::remove_dir_all(&root);
    }


    /// One step is one entry sideways and one *row* down, and it stops at the ends
    /// instead of wrapping: a grid's edge is where the pointer stops too.
    #[test]
    fn one_step_is_one_entry_or_one_row() {
        // A grid three cards wide with seven entries in it.
        let grid = (3, 7);

        assert_eq!(stepped_index(0, 1, 0, grid.0, grid.1), 1, "right");
        assert_eq!(stepped_index(2, 1, 0, grid.0, grid.1), 3, "right wraps to the next row");
        assert_eq!(stepped_index(3, -1, 0, grid.0, grid.1), 2, "left");
        assert_eq!(stepped_index(0, 0, 1, grid.0, grid.1), 3, "down is one row");
        assert_eq!(stepped_index(4, 0, -1, grid.0, grid.1), 1, "up is one row");
        assert_eq!(stepped_index(5, 0, 1, grid.0, grid.1), 8.min(6), "clamped at the last");
        assert_eq!(stepped_index(0, 0, -1, grid.0, grid.1), 0, "clamped at the first");

        // A list is a grid one wide.
        assert_eq!(stepped_index(0, 0, 1, 1, 4), 1);
        assert_eq!(stepped_index(3, 0, 1, 1, 4), 3);
        assert_eq!(stepped_index(1, 1, 0, 1, 4), 2);

        // One entry: every step lands on it.
        assert_eq!(stepped_index(0, 1, 1, 3, 1), 0);
    }

    /// The step is clamped into the view, never past its last entry.
    #[test]
    fn a_step_never_leaves_the_view() {
        for count in 1..8usize {
            for current in 0..count {
                for (dx, dy) in [(1, 0), (-1, 0), (0, 1), (0, -1), (3, 2), (-3, -2)] {
                    let next = stepped_index(current, dx, dy, 3, count);
                    assert!(next < count, "count {count} from {current} by ({dx},{dy})");
                }
            }
        }
    }
}
