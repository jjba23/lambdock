// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The window model behind a dock item: `src/ui/dock_item_windows.c` in Rust.
//!
//! What an item knows about the windows the compositor reported for it: which
//! handles it tracks, how many, whether one holds the focus, whether they are all
//! minimized, and which item a reported app id belongs to.
//!
//! The matching half is pure -- names in, a decision out -- so `cargo test` pins
//! it without a display: an app is matched by every spelling a launcher can be
//! written with (app id, icon, name, desktop id, exec token), and then through
//! the desktop entry the id resolves to, which is what keeps a pinned launcher
//! and the window it opens from showing up as two icons.

use crate::app_window::{AppWindow, GList, LambdockToplevel};
use crate::dock_item::DockItem;
use crate::state::LambdockState;
use std::os::raw::{c_char, c_int, c_void};

/// The names a dock item is known by, borrowed from the C struct that holds them.
///
/// Both sides of a match are described as values so the rules can be tested and
/// read as rules instead of as pointer walks.
#[derive(Default, Clone, Copy)]
pub struct ItemNames<'a> {
    pub app_id: Option<&'a str>,
    pub icon: Option<&'a str>,
    pub name: Option<&'a str>,
    pub desktop_id: Option<&'a str>,
    pub exec: Option<&'a str>,
}

/// The names a desktop entry is known by: the four the matcher reads.
#[derive(Default, Clone, Copy)]
pub struct EntryNames<'a> {
    pub id: Option<&'a str>,
    /// `id` without its `.desktop` suffix, which is what compositors report.
    pub stem: Option<&'a str>,
    pub icon: Option<&'a str>,
    pub exec: Option<&'a str>,
}

/// The leading `char*` fields of `LambdockDesktopEntry`, in the header's order.
///
/// Only the fields the matcher reads are mirrored; the offset test pins them to
/// the header, so a field added *before* them fails that test instead of silently
/// shifting a read.
#[repr(C)]
pub struct DesktopEntryNames {
    pub id: *mut c_char,
    pub stem: *mut c_char,
    pub name: *mut c_char,
    pub exec: *mut c_char,
    pub icon: *mut c_char,
}

/// Whether `app_id` appears in `exec` as a whole token, ignoring case.
///
/// The last-resort heuristic for an item whose launcher was written with the
/// executable instead of the app id. Tokens only: a short id must not match the
/// middle of an unrelated executable, and the reverse direction (exec inside the
/// app id) is not tried, because it produced false positives.
pub fn exec_matches_app_id(exec: Option<&str>, app_id: Option<&str>) -> bool {
    let (Some(exec), Some(app_id)) = (exec, app_id) else {
        return false;
    };

    let id = app_id.as_bytes();
    let exec = exec.as_bytes();

    // A two-letter id ("top", "to") is not evidence of anything.
    if id.len() < 3 || exec.len() < id.len() {
        return false;
    }

    for start in 0..=exec.len() - id.len() {
        // Only at a token boundary, so `top` does not match `ktop`.
        if start > 0 && exec[start - 1].is_ascii_alphanumeric() {
            continue;
        }

        if !exec[start..start + id.len()].eq_ignore_ascii_case(id) {
            continue;
        }

        // The token ends at anything that is not alphanumeric: `firefox-esr` and
        // `firefox %u` both name the executable.
        match exec.get(start + id.len()) {
            None => return true,
            Some(next) if !next.is_ascii_alphanumeric() => return true,
            Some(_) => continue,
        }
    }

    false
}

/// Whether an item is known by `app_id`: any of the spellings a launcher can be
/// written with, plus the exec heuristic.
pub fn item_matches_app_id(item: &ItemNames, app_id: &str) -> bool {
    let equal = |field: Option<&str>| field.is_some_and(|field| field.eq_ignore_ascii_case(app_id));

    equal(item.app_id)
        || equal(item.icon)
        || equal(item.name)
        || equal(item.desktop_id)
        || exec_matches_app_id(item.exec, Some(app_id))
}

/// Whether an item resolves to the same desktop entry -- the precise match.
pub fn item_matches_entry(item: &ItemNames, entry: &EntryNames) -> bool {
    let equal = |left: Option<&str>, right: Option<&str>| match (left, right) {
        (Some(left), Some(right)) => left.eq_ignore_ascii_case(right),
        _ => false,
    };

    // The item's desktop id against the entry's stem: a launcher written as
    // "org.gnome.Nautilus" and one written as "org.gnome.Nautilus.desktop" are
    // the same application.
    if equal(item.desktop_id, entry.stem) {
        return true;
    }

    if equal(item.app_id, entry.stem) || equal(item.app_id, entry.id) {
        return true;
    }

    if equal(item.icon, entry.icon) {
        return true;
    }

    // The last resort, and the one that catches a pin spelled differently from
    // its window: an exec that names the other's executable.
    item.exec.is_some() && entry.exec.is_some() && exec_matches_app_id(entry.exec, item.exec)
}

/// Whether the item has windows and every one of them is minimized.
///
/// # Safety
///
/// `windows` must be null or a `GList` of live `AppWindow` records.
pub unsafe fn all_windows_minimized(windows: *const GList) -> bool {
    if windows.is_null() {
        return false;
    }

    let mut node = windows;
    while !node.is_null() {
        let window = (*node).data.cast::<AppWindow>();
        if window.is_null() || (*window).minimized == 0 {
            return false;
        }

        node = (*node).next;
    }

    true
}

/// Whether one of the item's windows holds the keyboard focus.
///
/// # Safety
///
/// `windows` must be null or a `GList` of live `AppWindow` records.
pub unsafe fn any_window_focused(windows: *const GList) -> bool {
    let mut node = windows;
    while !node.is_null() {
        let window = (*node).data.cast::<AppWindow>();
        if !window.is_null() && (*window).activated != 0 {
            return true;
        }

        node = (*node).next;
    }

    false
}

// Where the window model reaches back into the modules that are still C.
extern "C" {
    /// Drops one item and everything it owns (`dock_item.c`).
    fn free_dock_item(item: *mut DockItem);

    /// Pushes the item's label and description to the accessibility tree
    /// (`dock_item.c`).
    fn dock_item_update_accessible(item: *mut DockItem);

    /// The glass shelf draws its own running indicator near the outer edge
    /// (`shelf.c`).
    fn shelf_reflection_set_running(reflection: *mut c_void, running: c_int);

    /// Shows the dock again; called when the last tracked window goes away
    /// (`dock_window.c`).
    fn reveal_dock(state: *mut LambdockState);

    /// The desktop entry a reported app id resolves to (`desktop_entry.c`).
    fn lambdock_desktop_entry_lookup(app_id: *const c_char) -> *const DesktopEntryNames;

    fn g_list_append(list: *mut GList, data: *mut c_void) -> *mut GList;
    fn g_list_free(list: *mut GList);
    fn g_list_delete_link(list: *mut GList, link: *mut GList) -> *mut GList;
    fn g_list_remove(list: *mut GList, data: *const c_void) -> *mut GList;

    fn gtk_label_set_text(label: *mut gtk4::ffi::GtkLabel, text: *const c_char);
    fn gtk_widget_set_visible(widget: *mut gtk4::ffi::GtkWidget, visible: c_int);
}

/// One of the item's names as a `&str`, or `None` when it is unset.
///
/// # Safety
///
/// `text` must be null or a NUL-terminated C string that outlives the borrow.
unsafe fn name<'a>(text: *const c_char) -> Option<&'a str> {
    if text.is_null() {
        return None;
    }

    // A name that is not UTF-8 cannot equal one that is, so it matches nothing.
    std::ffi::CStr::from_ptr(text).to_str().ok()
}

/// The spellings an item is known by.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn item_names<'a>(item: *const DockItem) -> ItemNames<'a> {
    ItemNames {
        app_id: name((*item).app_id),
        icon: name((*item).icon),
        name: name((*item).name),
        desktop_id: name((*item).desktop_id),
        exec: name((*item).exec),
    }
}

/// Toggles the `running-app` class and the shelf's running indicator together,
/// plus the two states every theme can style on its own.
///
/// # Safety
///
/// `item` must be a live `DockItem` whose button is set.
unsafe fn apply_running_state(item: *const DockItem) {
    let running = !(*item).windows.is_null();
    let button = (*item).button.cast::<gtk4::ffi::GtkWidget>();

    crate::ui::widget_utils::lambdock_set_css_class(
        button,
        c"running-app".as_ptr(),
        running as c_int,
    );

    // `window-focused`: the app's window is the one being looked at, which is the
    // depressed task button in a taskbar-style theme. `window-minimized`: all of
    // its windows are tucked away (overlays.css dims the icon for every theme).
    crate::ui::widget_utils::lambdock_set_css_class(
        button,
        c"window-focused".as_ptr(),
        any_window_focused((*item).windows) as c_int,
    );
    crate::ui::widget_utils::lambdock_set_css_class(
        button,
        c"window-minimized".as_ptr(),
        all_windows_minimized((*item).windows) as c_int,
    );

    // The glass shelf draws its own running indicator near the outer edge.
    if !(*item).reflection.is_null() {
        shelf_reflection_set_running((*item).reflection, running as c_int);
    }
}

/// Shows the running window's title inside the button in taskbar mode, and hides
/// the label otherwise. App items only: a dynamic item's label carries its
/// reading, and separators have none.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn apply_window_title_label(item: *const DockItem) {
    if (*item).window_title_label == 0 || (*item).label.is_null() {
        return;
    }

    let window = if (*item).windows.is_null() {
        std::ptr::null_mut()
    } else {
        (*(*item).windows).data.cast::<AppWindow>()
    };

    let show_titles = !(*item).state.is_null() && (*(*item).state).show_window_titles != 0;
    let title = if window.is_null() {
        std::ptr::null()
    } else {
        (*window).title
    };

    let text = crate::core::item_text::lambdock_window_title_text(
        show_titles as c_int,
        (!(*item).windows.is_null()) as c_int,
        title,
        (*item).name,
        (*item).app_id,
    );

    let label = (*item).label.cast::<gtk4::ffi::GtkLabel>();

    if !text.is_null() {
        gtk_label_set_text(label, text);
    }
    gtk_widget_set_visible(label.cast(), (!text.is_null()) as c_int);
}

/// Drops every tracked window and handle, and removes the items that existed
/// only because a window was tracked.
///
/// Used when the Wayland foreign-toplevel manager goes away: its handles become
/// invalid, so no dock may keep pointers to them.
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn clear_tracked_windows(state: *mut LambdockState) {
    if state.is_null() {
        return;
    }

    let mut node = (*state).dock_items.cast::<GList>();
    while !node.is_null() {
        let item = (*node).data.cast::<DockItem>();
        if !item.is_null() {
            crate::app_window::app_windows_free((*item).windows);
            (*item).windows = std::ptr::null_mut();
            update_dock_item_status(item);
        }

        node = (*node).next;
    }

    // Then the items that only a tracked window brought into being. Favourites
    // stay: they were in the settings file.
    let mut node = (*state).dock_items.cast::<GList>();
    while !node.is_null() {
        let next = (*node).next;
        let item = (*node).data.cast::<DockItem>();

        if !item.is_null() && (*item).is_favorite == 0 {
            (*state).dock_items = g_list_delete_link((*state).dock_items.cast(), node).cast();
            free_dock_item(item);
        }

        node = next;
    }

    g_list_free((*state).window_trackers.cast());
    (*state).window_trackers = std::ptr::null_mut();

    if (*state).show_when_clear != 0 {
        reveal_dock(state);
    }
}

/// Attaches a tracked window to an item.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`; `handle` must be null or a live
/// toplevel handle owned by the tracker.
#[no_mangle]
pub unsafe extern "C" fn dock_item_add_window(
    item: *mut DockItem,
    handle: *mut LambdockToplevel,
) -> *mut AppWindow {
    if item.is_null() || handle.is_null() {
        return std::ptr::null_mut();
    }

    // Idempotency guard: a handle maps to exactly one AppWindow. Callers such as
    // the tracker's reload and repeated app-id events can re-add the same handle;
    // without this they would duplicate entries, leak records and inflate the
    // window count.
    let mut node = (*item).windows.cast::<GList>();
    while !node.is_null() {
        let existing = (*node).data.cast::<AppWindow>();
        if !existing.is_null() && crate::app_window::app_window_matches(existing, handle) != 0 {
            return existing;
        }

        node = (*node).next;
    }

    let title = if (*item).name.is_null() {
        (*item).app_id
    } else {
        (*item).name
    };
    let window = crate::app_window::app_window_new(handle, title);

    (*item).windows = g_list_append((*item).windows.cast(), window.cast()).cast();

    update_dock_item_status(item);

    window
}

/// Detaches a tracked window from an item, if it has one.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`; `handle` must be null or a handle the
/// tracker owns.
#[no_mangle]
pub unsafe extern "C" fn dock_item_remove_window(
    item: *mut DockItem,
    handle: *const LambdockToplevel,
) {
    if item.is_null() || handle.is_null() {
        return;
    }

    let mut node = (*item).windows.cast::<GList>();
    while !node.is_null() {
        let window = (*node).data.cast::<AppWindow>();
        if !window.is_null() && crate::app_window::app_window_matches(window, handle) != 0 {
            (*item).windows = g_list_remove((*item).windows.cast(), window.cast()).cast();
            crate::app_window::app_window_free(window);
            break;
        }

        node = (*node).next;
    }

    update_dock_item_status(item);
}

/// Whether the item still tracks that handle.
///
/// The tracker owns and frees the records, so a handle stored elsewhere -- a
/// popover row's action data, built when the menu opened -- can be freed while
/// that menu is still on screen. Callers that act on a captured handle must ask
/// this first, or they would send a request for a window that is gone.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`; `handle` must be null or a
/// `LambdockToplevel` pointer to compare, never dereferenced here.
#[no_mangle]
pub unsafe extern "C" fn dock_item_has_window(
    item: *const DockItem,
    handle: *const LambdockToplevel,
) -> c_int {
    if item.is_null() || handle.is_null() {
        return 0;
    }

    let mut node = (*item).windows.cast::<GList>();
    while !node.is_null() {
        if crate::app_window::app_window_matches((*node).data.cast::<AppWindow>(), handle) != 0 {
            return 1;
        }

        node = (*node).next;
    }

    0
}

/// Re-applies the styling that depends on the item's windows, and the text that
/// goes with it.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`.
#[no_mangle]
pub unsafe extern "C" fn update_dock_item_status(item: *mut DockItem) {
    if item.is_null() || (*item).button.is_null() {
        return;
    }

    apply_running_state(item);

    // The running state and the title are both part of what a screen reader
    // announces, so refresh the label before anything else can return early.
    dock_item_update_accessible(item);
    apply_window_title_label(item);
}

/// Re-applies only the styling that depends on what the compositor reports.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`.
#[no_mangle]
pub unsafe extern "C" fn dock_item_refresh_window_state(item: *mut DockItem) {
    if item.is_null() || (*item).button.is_null() {
        return;
    }

    // Only the CSS classes: titles and accessible text are handled by the
    // tracker's title visitor, and re-deriving them here would defeat its
    // memoisation.
    apply_running_state(item);
}

/// The first item of `state` the predicate accepts, walking the row in order.
///
/// # Safety
///
/// `state` must be a live dock.
unsafe fn find_item(
    state: *const LambdockState,
    predicate: impl Fn(*const DockItem) -> bool,
) -> *mut DockItem {
    let mut node = (*state).dock_items.cast::<GList>();
    while !node.is_null() {
        let item = (*node).data.cast::<DockItem>();
        if !item.is_null() && predicate(item) {
            return item;
        }

        node = (*node).next;
    }

    std::ptr::null_mut()
}

/// The item a window belongs to.
///
/// Matched by the spellings a launcher can be written with (app id, icon, name,
/// desktop id, exec), then through the desktop entry the id resolves to -- which
/// is what keeps a pinned launcher and the window it opens from showing up as two
/// icons.
///
/// # Safety
///
/// `state` must be null or a live dock; `app_id` must be null or a C string.
#[no_mangle]
pub unsafe extern "C" fn find_dock_item_by_app_id(
    state: *mut LambdockState,
    app_id: *const c_char,
) -> *mut DockItem {
    if state.is_null() || app_id.is_null() || *app_id == 0 {
        return std::ptr::null_mut();
    }

    // An app id that is not UTF-8 is not one the compositor sends; lossy
    // conversion keeps the comparison total instead of dropping the window.
    let id = std::ffi::CStr::from_ptr(app_id).to_string_lossy();

    let match_by_id = find_item(state, |item| item_matches_app_id(&item_names(item), &id));
    if !match_by_id.is_null() {
        return match_by_id;
    }

    // Nothing matched the raw id. The desktop entry behind it knows the names the
    // application is actually installed under, so use those for a second pass: a
    // pinned "Files" (icon "nautilus") tracks a window whose app id is
    // "org.gnome.Nautilus".
    let entry = lambdock_desktop_entry_lookup(app_id);
    if entry.is_null() {
        return std::ptr::null_mut();
    }

    let entry_names = EntryNames {
        id: name((*entry).id),
        stem: name((*entry).stem),
        icon: name((*entry).icon),
        exec: name((*entry).exec),
    };

    find_item(state, |item| {
        item_matches_entry(&item_names(item), &entry_names)
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::app_window::{app_window_free, app_window_new, app_window_set_state};

    fn item(
        app_id: &str,
        icon: &str,
        name: &str,
        desktop_id: &str,
        exec: &str,
    ) -> ItemNames<'static> {
        // Leaked on purpose: these live for the process, like the C strings they
        // stand in for.
        fn leaked(text: &str) -> Option<&'static str> {
            if text.is_empty() {
                None
            } else {
                Some(Box::leak(text.to_owned().into_boxed_str()))
            }
        }

        ItemNames {
            app_id: leaked(app_id),
            icon: leaked(icon),
            name: leaked(name),
            desktop_id: leaked(desktop_id),
            exec: leaked(exec),
        }
    }

    fn entry(id: &str, stem: &str, icon: &str, exec: &str) -> EntryNames<'static> {
        let leaked = |text: &str| -> Option<&'static str> {
            if text.is_empty() {
                None
            } else {
                Some(Box::leak(text.to_owned().into_boxed_str()))
            }
        };

        EntryNames {
            id: leaked(id),
            stem: leaked(stem),
            icon: leaked(icon),
            exec: leaked(exec),
        }
    }

    /// A list of records whose state the tests can set, as the tracker leaves them.
    fn windows(states: &[(bool, bool)]) -> *mut GList {
        extern "C" {
            fn g_list_append(list: *mut GList, data: *mut std::ffi::c_void) -> *mut GList;
        }

        let mut list: *mut GList = std::ptr::null_mut();
        for (minimized, activated) in states {
            let title = std::ffi::CString::new("Window").expect("no interior NUL");
            let window = unsafe { app_window_new(std::ptr::null_mut(), title.as_ptr()) };
            unsafe {
                app_window_set_state(window, i32::from(*minimized), i32::from(*activated));
                list = g_list_append(list, window.cast());
            }
        }

        list
    }

    fn free_windows(list: *mut GList) {
        extern "C" {
            fn g_list_free(list: *mut GList);
        }

        let mut node = list;
        while !node.is_null() {
            unsafe {
                app_window_free((*node).data.cast());
                node = (*node).next;
            }
        }
        unsafe { g_list_free(list) };
    }

    /// A launcher written with the executable still tracks its window -- but only
    /// where the id is a whole token, or `top` would match `ktop`.
    #[test]
    fn an_exec_match_needs_a_whole_token() {
        assert!(exec_matches_app_id(
            Some("/usr/bin/firefox %u"),
            Some("firefox")
        ));
        assert!(exec_matches_app_id(
            Some("env GTK_THEME=x org.gnome.Terminal"),
            Some("org.gnome.Terminal")
        ));
        assert!(exec_matches_app_id(
            Some("/usr/bin/Firefox"),
            Some("firefox")
        ));

        // The token ends at anything that is not alphanumeric, so a flag or a
        // suffix still counts as the executable's name.
        assert!(exec_matches_app_id(
            Some("/usr/bin/firefox-esr"),
            Some("firefox")
        ));

        // Mine inside a longer word: not the executable.
        assert!(!exec_matches_app_id(
            Some("/usr/bin/libfirefox.so"),
            Some("firefox")
        ));
        assert!(!exec_matches_app_id(Some("ktop"), Some("top")));

        // A short id cannot be a heuristic, or it would match anything.
        assert!(!exec_matches_app_id(Some("/usr/bin/top"), Some("to")));
        assert!(!exec_matches_app_id(Some("/usr/bin/top"), None));
        assert!(!exec_matches_app_id(None, Some("firefox")));
        assert!(!exec_matches_app_id(Some(""), Some("firefox")));
    }

    /// Any spelling a launcher can be written with finds its item.
    #[test]
    fn an_item_is_matched_by_every_spelling() {
        let firefox = item(
            "org.mozilla.firefox",
            "firefox",
            "Firefox",
            "firefox.desktop",
            "/usr/bin/firefox %u",
        );

        assert!(item_matches_app_id(&firefox, "org.mozilla.firefox"));
        assert!(item_matches_app_id(&firefox, "Firefox")); // icon
        assert!(item_matches_app_id(&firefox, "FIREFOX")); // name, case-insensitive
        assert!(item_matches_app_id(&firefox, "firefox.desktop")); // desktop id
        assert!(item_matches_app_id(&firefox, "firefox")); // exec token

        assert!(!item_matches_app_id(&firefox, "chromium"));
        assert!(!item_matches_app_id(&ItemNames::default(), "firefox"));
    }

    /// The precise match: a pinned "Files" whose icon is nautilus tracks the
    /// window that reports org.gnome.Nautilus, because both land on the same
    /// desktop entry.
    #[test]
    fn an_item_is_matched_through_its_desktop_entry() {
        let resolved = entry(
            "org.gnome.Nautilus.desktop",
            "org.gnome.Nautilus",
            "org.gnome.Nautilus",
            "nautilus --new-window",
        );

        // The desktop id, the app id and the icon, one at a time.
        assert!(item_matches_entry(
            &item("", "system-file-manager", "Files", "org.gnome.Nautilus", ""),
            &resolved
        ));
        assert!(item_matches_entry(
            &item("org.gnome.Nautilus", "system-file-manager", "Files", "", ""),
            &resolved
        ));
        assert!(item_matches_entry(
            &item("", "org.gnome.Nautilus", "Files", "", ""),
            &resolved
        ));

        // The launcher written with the executable: the last resort, and the one
        // that catches a launcher spelled differently from its window. The
        // entry's exec is searched for the item's, so the item's has to be the
        // shorter of the two.
        assert!(item_matches_entry(
            &item("", "nautilus", "Files", "", "nautilus"),
            &resolved
        ));

        // Longer than the entry's own exec: there is nothing to find in it.
        assert!(!item_matches_entry(
            &item("", "nautilus", "Files", "", "nautilus --new-window %u"),
            &resolved
        ));

        // Nothing in common: no match.
        assert!(!item_matches_entry(
            &item("", "nautilus", "Files", "", "nautilus"),
            &entry("gimp.desktop", "gimp", "gimp", "gimp-2.10")
        ));
        assert!(!item_matches_entry(
            &item("", "nautilus", "Files", "", "nautilus"),
            &EntryNames::default()
        ));
    }

    /// Only an item whose windows are *all* tucked away is dimmed.
    #[test]
    fn windows_are_minimized_only_when_every_one_is() {
        unsafe {
            assert!(!all_windows_minimized(std::ptr::null()));

            let all = windows(&[(true, false), (true, false)]);
            assert!(all_windows_minimized(all));
            free_windows(all);

            let some = windows(&[(true, false), (false, true)]);
            assert!(!all_windows_minimized(some));
            free_windows(some);

            let none = windows(&[(false, true)]);
            assert!(!all_windows_minimized(none));
            free_windows(none);
        }
    }

    /// The focused class follows any of the item's windows, not just the first.
    #[test]
    fn a_window_holds_the_focus_when_any_one_is_activated() {
        unsafe {
            assert!(!any_window_focused(std::ptr::null()));

            let focused = windows(&[(false, false), (false, true)]);
            assert!(any_window_focused(focused));
            free_windows(focused);

            let unfocused = windows(&[(true, false), (false, false)]);
            assert!(!any_window_focused(unfocused));
            free_windows(unfocused);
        }
    }

    /// The entry mirror reads the header's leading fields, so it is a prefix of
    /// `LambdockDesktopEntry`: id, stem, name, exec, icon, in that order.
    #[test]
    fn the_entry_mirror_matches_the_header_prefix() {
        use std::mem::{offset_of, size_of};

        assert_eq!(size_of::<DesktopEntryNames>(), 5 * size_of::<*mut c_char>());
        assert_eq!(offset_of!(DesktopEntryNames, id), 0);
        assert_eq!(offset_of!(DesktopEntryNames, stem), 8);
        assert_eq!(offset_of!(DesktopEntryNames, name), 16);
        assert_eq!(offset_of!(DesktopEntryNames, exec), 24);
        assert_eq!(offset_of!(DesktopEntryNames, icon), 32);
    }
}
