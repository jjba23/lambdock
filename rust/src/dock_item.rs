// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! One dock icon: `DockItem` from `src/lambdock.h`, and what an item is.
//!
//! Rust reads this struct through a pointer C owns and never allocates or frees
//! one. What lives here as well is the item's *identity*: which kind it is, the
//! key a layout override remembers it under, the title a menu shows for it, the
//! text a screen reader gets from it, and what activating it means. A kind of
//! item is one row of [`ITEM_KIND_OPS`] instead of four switches to keep in sync.
//!
//! The mirror matches the header field for field, in order. A wrong offset is
//! not a compile error, it is a corrupted field two modules away, so the test at
//! the bottom pins the size and the offsets of the fields the Rust modules read,
//! taken from `offsetof` in C. When `lambdock.h` grows a field, that test is what
//! says so.

use crate::app_window::{AppWindow, GList};
use crate::core::dock_click::{lambdock_click_action_for, ClickAction};
use crate::core::config_values::{EDGE_BOTTOM, EDGE_LEFT, EDGE_RIGHT, EDGE_TOP};
use crate::core::item_text::{
    trash_tooltip, text_app, text_dynamic, text_launchpad, text_stack, text_trash, ItemText,
};
use crate::log_info;
use crate::log_warn;
use crate::state::LambdockState;
use crate::ui::DEFAULT_ICON_SIZE;
use gtk4::gdk;
use gtk4::glib;
use gtk4::glib::translate::{FromGlib, IntoGlib};
use gtk4::glib::translate::{from_glib_borrow, Borrowed};
use gtk4::pango;
use gtk4::prelude::*;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_uint, c_void};
use crate::bridge::{dock_read_settings, lambdock_poll_fn_call, lambdock_poll_fn_is_callable, lambdock_poll_fn_protect, lambdock_poll_fn_release, populate_dock_items_for};
use crate::dock_window::{apply_dock_mode_classes, dock_apply_exclusive_zone, dock_apply_motion_preference, hide_dock, on_popover_closed, refresh_dock_mode, reveal_dock, update_dock_dimensions};
use crate::toplevel_tracker::{activate_toplevel_window, clear_window_tracker_dock_references, minimize_toplevel_window, sync_window_trackers};
use crate::ui::dock_dnd::{dock_dnd_persist_order};
use crate::ui::launchpad::{lambdock_launchpad_hide, lambdock_launchpad_is_open, lambdock_launchpad_show};

/// `LambdockItemKind`: app, dynamic, separator, stack, launchpad, trash.
pub const ITEM_APP: c_int = 0;
pub const ITEM_DYNAMIC: c_int = 1;
pub const ITEM_SEPARATOR: c_int = 2;
pub const ITEM_STACK: c_int = 3;
pub const ITEM_LAUNCHPAD: c_int = 4;
pub const ITEM_TRASH: c_int = 5;

/// The record, field for field.
#[repr(C)]
pub struct DockItem {
    pub name: *mut c_char,
    pub icon: *mut c_char,
    pub exec: *mut c_char,
    pub app_id: *mut c_char,
    pub is_favorite: c_int,
    pub button: *mut c_void,
    pub cell: *mut c_void,
    pub container: *mut c_void,
    pub reflection: *mut c_void,
    pub separator_follow: *mut c_void,
    pub popover: *mut c_void,
    pub windows: *mut GList,
    pub state: *mut LambdockState,
    /// Opaque Guile handle; only the bridge dereferences it.
    pub poll_fn: *mut c_void,
    pub poll_timer_id: c_uint,
    pub label: *mut c_void,
    pub window_title_label: c_int,
    pub hover_animate: c_int,
    pub kind: c_int,
    pub stack_path: *mut c_char,
    pub stack_view: *mut c_char,
    pub dragging: c_int,
    pub meter: *mut c_void,
    pub clock: *mut c_void,
    pub meter_unit: *mut c_char,
    pub meter_max: f64,
    pub desktop_id: *mut c_char,
    pub poll_name: *mut c_char,
    pub poll_interval: c_uint,
    pub show_inspector: c_int,
    pub icon_image: *mut c_void,
    pub trash_timer_id: c_uint,
    pub trash_count: c_uint,
    pub trash_full: c_int,
}


// ============================================================================
// WHAT A KIND OF ITEM IS
// ============================================================================
//
// The dock used to answer "what is this kind?" in four separate switches (key
// name, CSS class, click behaviour, accessibility text), so adding a kind meant
// finding all four. Now a kind is one row of `ITEM_KIND_OPS`, and the switches
// are gone.

/// What activating a kind of item means.
///
/// A tag rather than a function pointer, so that a test which only asks an
/// item for its key does not have to link the C functions a click can land in.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Activation {
    /// Launch, focus, minimize or list, depending on what is running.
    App,
    Stack,
    Launchpad,
    Trash,
}

/// One kind of dock item: the name its key and its Scheme symbol use, the extra
/// CSS class its button wears, what activating it means, and what a screen
/// reader says about it.
struct KindOps {
    key_name: &'static CStr,
    css_class: Option<&'static CStr>,
    activation: Activation,
    /// `None` is an item with nothing to say, which is the separator.
    describe: Option<unsafe fn(&DockItem) -> ItemText>,
}

/// Every kind, in `LambdockItemKind` order.
const ITEM_KIND_OPS: [KindOps; 6] = [
    KindOps {
        key_name: c"app",
        css_class: None,
        activation: Activation::App,
        describe: Some(describe_app),
    },
    KindOps {
        key_name: c"dynamic",
        css_class: Some(c"dynamic-item"),
        activation: Activation::App,
        describe: Some(describe_dynamic),
    },
    KindOps {
        key_name: c"separator",
        css_class: None,
        activation: Activation::App,
        describe: None,
    },
    KindOps {
        key_name: c"stack",
        css_class: Some(c"stack-item"),
        activation: Activation::Stack,
        describe: Some(describe_stack),
    },
    KindOps {
        key_name: c"launchpad",
        css_class: Some(c"launchpad-item"),
        activation: Activation::Launchpad,
        describe: Some(describe_launchpad),
    },
    KindOps {
        key_name: c"trash",
        css_class: Some(c"trash-item"),
        activation: Activation::Trash,
        describe: Some(describe_trash),
    },
];


/// The descriptor of `kind`; unknown kinds behave like a plain application.
fn kind_ops(kind: c_int) -> &'static KindOps {
    usize::try_from(kind)
        .ok()
        .and_then(|kind| ITEM_KIND_OPS.get(kind))
        .unwrap_or(&ITEM_KIND_OPS[0])
}

/// A C string that is there and not empty.
fn non_empty(pointer: *const c_char) -> Option<*const c_char> {
    (!pointer.is_null() && unsafe { *pointer } != 0).then_some(pointer)
}

/// The Scheme-side name of `kind` ("app", "stack", ...).
///
/// # Safety
///
/// The result is a static string; the caller must not free it.
pub fn dock_item_kind_to_string(kind: c_int) -> *const c_char {
    kind_ops(kind).key_name.as_ptr()
}

/// The extra CSS class of `kind`, or null when it has none.
///
/// # Safety
///
/// The result is a static string; the caller must not free it.
pub fn dock_item_kind_css_class(kind: c_int) -> *const c_char {
    kind_ops(kind)
        .css_class
        .map_or(std::ptr::null(), |class| class.as_ptr())
}

/// The stable identity of `item`: "kind:name", matching `(dock-item-key item)` on
/// the Scheme side. The layout overrides remember order, removals and additions
/// by it.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`; the result is `(transfer full)`.
pub unsafe fn dock_item_key(item: *const DockItem) -> *mut c_char {
    let Some(item) = item.as_ref() else {
        return std::ptr::null_mut();
    };

    // A nameless item is still keyed; the empty string keeps the two halves
    // apart without inventing a name.
    let name = if item.name.is_null() {
        c"".as_ptr()
    } else {
        item.name
    };

    crate::glib::g_strdup_printf(c"%s:%s".as_ptr(), kind_ops(item.kind).key_name.as_ptr(), name)
}

/// What to call `item`: its name, else its app id, else the caller's fallback.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`; `fallback` must be null or a C
/// string. Nothing is copied: the result points into the item or at `fallback`.
pub unsafe fn lambdock_item_title(
    item: *const DockItem,
    fallback: *const c_char,
) -> *const c_char {
    if let Some(item) = item.as_ref() {
        if let Some(name) = non_empty(item.name) {
            return name;
        }
        if let Some(app_id) = non_empty(item.app_id) {
            return app_id;
        }
    }

    fallback
}

/// The item's windows, as a count.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn window_count(item: &DockItem) -> u32 {
    let mut count = 0;
    let mut window = item.windows;

    while let Some(current) = window.as_ref() {
        count += 1;
        window = current.next;
    }

    count
}

/// The one window an item tracks, when it tracks exactly one.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn only_window(item: &DockItem) -> Option<*mut AppWindow> {
    let first = item.windows.as_ref()?;
    if !first.next.is_null() {
        return None;
    }

    Some(first.data.cast::<AppWindow>())
}

/// A popover field of an item or of the dock, as a popover.
///
/// # Safety
///
/// `popover` must be null or a live `GtkPopover`.
unsafe fn popover_of(popover: *mut c_void) -> Option<Borrowed<gtk4::Popover>> {
    if popover.is_null() {
        return None;
    }

    Some(from_glib_borrow(popover.cast::<gtk4::ffi::GtkPopover>()))
}

/// A widget field of an item, as a widget.
///
/// # Safety
///
/// `widget` must be null or a live `GtkWidget`.
unsafe fn widget_of(widget: *mut c_void) -> Option<Borrowed<gtk4::Widget>> {
    if widget.is_null() {
        return None;
    }

    Some(from_glib_borrow(widget.cast::<gtk4::ffi::GtkWidget>()))
}

// The per-kind accessible text. The wording is rust/src/core/item_text.rs's;
// these only take the values out of the item.

/// A C string field of an item, borrowed.
///
/// # Safety
///
/// The field must be null or a NUL-terminated string the item owns.
unsafe fn field(text: *mut c_char) -> Option<&'static CStr> {
    text.as_ref().map(|text| CStr::from_ptr(text))
}

/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn describe_app(item: &DockItem) -> ItemText {
    text_app(field(item.name), window_count(item))
}

/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn describe_dynamic(item: &DockItem) -> ItemText {
    // The item's tooltip already carries the latest reading, so that is the
    // value worth describing.
    let reading = widget_of(item.button)
        .and_then(|button| button.tooltip_text())
        .and_then(|tooltip| CString::new(tooltip).ok());

    text_dynamic(field(item.name), reading.as_deref())
}

/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn describe_stack(item: &DockItem) -> ItemText {
    text_stack(field(item.name))
}

/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn describe_launchpad(item: &DockItem) -> ItemText {
    text_launchpad(field(item.name))
}

/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn describe_trash(item: &DockItem) -> ItemText {
    text_trash(field(item.name), item.trash_count)
}

/// Rebuilds what a screen reader says about `item`.
///
/// The labels are rebuilt whenever the item's state changes (a window opened, the
/// trash was filled, a reading arrived), and the update is skipped when nothing
/// changed.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`.
pub unsafe fn dock_item_update_accessible(item: *mut DockItem) {
    let Some(item_ref) = item.as_ref() else {
        return;
    };
    if item_ref.button.is_null() || item_ref.kind == ITEM_SEPARATOR {
        return;
    }

    let Some(describe) = kind_ops(item_ref.kind).describe else {
        return;
    };

    let text = describe(item_ref);
    let label = crate::cstr::cstring(&text.label);
    let description = crate::cstr::cstring(&text.description);

    crate::accessibility::lambdock_accessible_update(
        item_ref.button.cast(),
        label.as_ptr(),
        description.as_ptr(),
    );
}

/// Runs `command`, reporting a failure without taking the dock down.
///
/// # Safety
///
/// `command` must be null or a NUL-terminated string.
unsafe fn launch_command(command: *const c_char) {
    let Some(command) = non_empty(command) else {
        return;
    };
    let command = CStr::from_ptr(command);

    use std::os::unix::ffi::OsStrExt;

    let command_line = std::ffi::OsStr::from_bytes(command.to_bytes());
    if let Err(error) = glib::spawn_command_line_async(command_line) {
        log_warn!(
            "UI",
            "Failed to launch '{}': {error}",
            command.to_string_lossy()
        );
    }
}

/// What a left click on an application item does.
///
/// The decision is rust/src/core/dock_click.rs's -- "launch, minimize, restore or
/// list" -- and this only applies it, so the policy stays testable without a
/// compositor: clicking the icon of the focused app minimizes it, and clicking an
/// icon whose window is minimized or behind another brings it forward.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn activate_app(item: *mut DockItem) {
    let item_ref = &*item;
    let only = only_window(item_ref);

    let action = lambdock_click_action_for(
        window_count(item_ref),
        only.map_or(0, |window| (*window).minimized),
        only.map_or(0, |window| (*window).activated),
        item_ref.exec,
    );

    match (action, only) {
        (ClickAction::Minimize, Some(window)) => minimize_toplevel_window((*window).handle),
        (ClickAction::Activate, Some(window)) => activate_toplevel_window((*window).handle),
        // Several windows: let the user pick one from the menu.
        (ClickAction::Menu, _) => lambdock_dock_item_popup_menu(item),
        (ClickAction::Launch, _) => launch_command(item_ref.exec),
        _ => {}
    }
}

/// # Safety
///
/// `item` must be a live `DockItem` whose kind is a stack.
unsafe fn activate_stack(item: *mut DockItem) {
    crate::ui::stack::lambdock_stack_show(item);
}

/// # Safety
///
/// `item` must be a live `DockItem` whose kind is the launchpad.
unsafe fn activate_launchpad(item: *mut DockItem) {
    lambdock_launchpad_show((*item).state);
}

/// # Safety
///
/// `item` must be a live `DockItem` whose kind is the trash.
unsafe fn activate_trash(_item: *mut DockItem) {
    if crate::ui::trash::lambdock_trash_open() == 0 {
        log_warn!("UI", "Could not open the trash");
    }
}

/// What a left click on `item` does: the kind's own behaviour when it has one,
/// and the application behaviour otherwise.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`.
pub unsafe fn dock_item_activate(item: *mut DockItem) {
    if item.is_null() {
        return;
    }

    match kind_ops((*item).kind).activation {
        Activation::App => activate_app(item),
        Activation::Stack => activate_stack(item),
        Activation::Launchpad => activate_launchpad(item),
        Activation::Trash => activate_trash(item),
    }
}

#[cfg(test)]
mod kind_tests {
    use super::*;

    /// An item with only the fields identity reads.
    fn item(kind: c_int, name: Option<&CStr>, app_id: Option<&CStr>) -> DockItem {
        let mut item = unsafe { std::mem::zeroed::<DockItem>() };
        item.kind = kind;
        item.name = name.map_or(std::ptr::null_mut(), |name| name.as_ptr() as *mut c_char);
        item.app_id = app_id.map_or(std::ptr::null_mut(), |app_id| {
            app_id.as_ptr() as *mut c_char
        });

        item
    }

    fn key_of(item: &DockItem) -> String {
        let key = unsafe { dock_item_key(item) };
        assert!(!key.is_null());

        let text = unsafe { CStr::from_ptr(key) }
            .to_string_lossy()
            .into_owned();
        unsafe { crate::glib::g_free(key.cast()) };

        text
    }

    /// The key is "kind:name": the identity a layout override remembers, and the
    /// same string `(dock-item-key item)` answers on the Scheme side.
    #[test]
    fn a_key_is_the_kind_then_the_name() {
        assert_eq!(key_of(&item(ITEM_APP, Some(c"firefox"), None)), "app:firefox");
        assert_eq!(key_of(&item(ITEM_DYNAMIC, Some(c"cpu"), None)), "dynamic:cpu");
        assert_eq!(
            key_of(&item(ITEM_SEPARATOR, Some(c"sep-1"), None)),
            "separator:sep-1"
        );
        assert_eq!(
            key_of(&item(ITEM_STACK, Some(c"Downloads"), None)),
            "stack:Downloads"
        );
        assert_eq!(
            key_of(&item(ITEM_LAUNCHPAD, Some(c"apps"), None)),
            "launchpad:apps"
        );
        assert_eq!(key_of(&item(ITEM_TRASH, Some(c"Trash"), None)), "trash:Trash");

        // A nameless item is still keyed -- two of them are equally nameless.
        assert_eq!(key_of(&item(ITEM_APP, None, None)), "app:");

        // A null item has no identity at all.
        assert!(unsafe { dock_item_key(std::ptr::null()) }.is_null());
    }

    /// A kind nobody knows behaves like an application: the first row is the
    /// fallback, which is what the C table did.
    #[test]
    fn an_unknown_kind_is_an_application() {
        assert_eq!(key_of(&item(-1, Some(c"odd"), None)), "app:odd");
        assert_eq!(key_of(&item(99, Some(c"odd"), None)), "app:odd");

        let name = dock_item_kind_to_string(99);
        assert_eq!(unsafe { CStr::from_ptr(name) }.to_str(), Ok("app"));

        // Only the kinds that need a class have one.
        assert!(dock_item_kind_css_class(ITEM_APP).is_null());
        assert!(dock_item_kind_css_class(ITEM_SEPARATOR).is_null());
        let class = dock_item_kind_css_class(ITEM_STACK);
        assert_eq!(unsafe { CStr::from_ptr(class) }.to_str(), Ok("stack-item"));
    }

    /// Every kind, in the order of `LambdockItemKind`, has its own name.
    #[test]
    fn every_kind_of_item_has_a_name() {
        let kinds = [
            (ITEM_APP, "app", None),
            (ITEM_DYNAMIC, "dynamic", Some("dynamic-item")),
            (ITEM_SEPARATOR, "separator", None),
            (ITEM_STACK, "stack", Some("stack-item")),
            (ITEM_LAUNCHPAD, "launchpad", Some("launchpad-item")),
            (ITEM_TRASH, "trash", Some("trash-item")),
        ];

        for (kind, name, class) in kinds {
            let actual = unsafe { CStr::from_ptr(dock_item_kind_to_string(kind)) };
            assert_eq!(actual.to_str(), Ok(name), "kind {kind}");

            let actual = dock_item_kind_css_class(kind);
            match class {
                Some(class) => {
                    assert_eq!(unsafe { CStr::from_ptr(actual) }.to_str(), Ok(class))
                }
                None => assert!(actual.is_null(), "kind {kind} has no class"),
            }
        }
    }

    /// Every kind knows what a click on it means: three kinds open something of
    /// their own, and the rest take the application behaviour. Only the
    /// separator has nothing to say about itself.
    #[test]
    fn each_kind_knows_what_a_click_means() {
        let expected = [
            (ITEM_APP, Activation::App, false),
            (ITEM_DYNAMIC, Activation::App, false),
            (ITEM_SEPARATOR, Activation::App, true),
            (ITEM_STACK, Activation::Stack, false),
            (ITEM_LAUNCHPAD, Activation::Launchpad, false),
            (ITEM_TRASH, Activation::Trash, false),
        ];

        for (kind, activation, silent) in expected {
            let ops = kind_ops(kind);
            assert!(ops.activation == activation, "kind {kind}");
            assert_eq!(ops.describe.is_none(), silent, "kind {kind}");
        }

        // An unknown kind is an application in every respect.
        assert!(kind_ops(42).activation == Activation::App);
        assert!(kind_ops(42).describe.is_some());
    }

    /// The title falls back from the item's name to its app id to the caller's
    /// fallback, which is what a menu row shows for an item with neither.
    #[test]
    fn a_title_falls_back_from_the_name_to_the_app_id() {
        let named = item(ITEM_APP, Some(c"Firefox"), Some(c"firefox.desktop"));
        let anonymous = item(ITEM_APP, None, Some(c"firefox.desktop"));
        let bare = item(ITEM_APP, None, None);
        let empty = item(ITEM_APP, Some(c""), Some(c"firefox.desktop"));

        let title = |item: &DockItem| unsafe {
            let title = lambdock_item_title(item, c"fallback".as_ptr());
            CStr::from_ptr(title).to_str().unwrap_or_default().to_owned()
        };

        assert_eq!(title(&named), "Firefox");
        assert_eq!(title(&anonymous), "firefox.desktop");
        // An empty name is not a name.
        assert_eq!(title(&empty), "firefox.desktop");
        assert_eq!(title(&bare), "fallback");

        // A null item is the fallback too.
        let null = unsafe { lambdock_item_title(std::ptr::null(), c"fallback".as_ptr()) };
        assert_eq!(unsafe { CStr::from_ptr(null) }.to_str(), Ok("fallback"));
    }
}


// ============================================================================
// THE KEYBOARD
// ============================================================================
//
// A dock is a row of icons, which a screen reader sees as a row of unnamed
// buttons unless each one says what it is -- and which a keyboard user cannot
// reach at all unless the arrow keys walk it. The keys that move the focus, the
// key that is a right click, and where a step lands are decisions about numbers,
// so they are values with tests; the focus itself is GTK's.

/// Where the focus is, as far as a step cares.
#[derive(Clone, Copy)]
enum Focus {
    /// Nothing in the dock has it.
    None,
    /// The focused item sits at this index among the ones the keyboard reaches.
    At(i32),
    /// Something focused is not one of those items (a separator, or a widget the
    /// dock does not own).
    Elsewhere,
}

/// How far a key moves along the dock: -1, +1, or 0 when the key is not a step
/// on this axis. The cross axis belongs to whatever is focused, not the dock.
fn key_step(orientation: gtk4::Orientation, keyval: gdk::Key) -> i32 {
    let (back, forward) = match orientation {
        gtk4::Orientation::Horizontal => (gdk::Key::Left, gdk::Key::Right),
        _ => (gdk::Key::Up, gdk::Key::Down),
    };

    if keyval == back {
        -1
    } else if keyval == forward {
        1
    } else {
        0
    }
}

/// Whether a key is the keyboard's right click: the Menu key, or Shift+F10.
fn key_opens_menu(keyval: gdk::Key, modifiers: gdk::ModifierType) -> bool {
    keyval == gdk::Key::Menu
        || (keyval == gdk::Key::F10 && modifiers.contains(gdk::ModifierType::SHIFT_MASK))
}

/// Where a step from `focus` lands, clamped into the dock.
///
/// `None` means the key changes nothing -- an empty dock, or already at the end,
/// which is what lets a parent take the key instead. With nothing focused, a step
/// forward starts at the first item and a step back at the last.
fn stepped_index(focus: Focus, delta: i32, count: usize) -> Option<usize> {
    if count == 0 || delta == 0 {
        return None;
    }

    let count = count as i32;
    let base = match focus {
        Focus::At(index) => index,
        // Nothing focused, or something that is not an item: the step starts at
        // the near end, so the first key always lands on an icon.
        Focus::None | Focus::Elsewhere => {
            if delta > 0 {
                -1
            } else {
                count
            }
        }
    };

    let next = (base + delta).clamp(0, count - 1);

    match focus {
        // As far as this key goes: the dock has no business consuming it.
        Focus::At(index) if next == index => None,
        _ => Some(next as usize),
    }
}

/// `state->dock_items`, the list the dock is built from.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`.
unsafe fn dock_items(state: *const LambdockState) -> *mut GList {
    state
        .as_ref()
        .map_or(std::ptr::null_mut(), |state| state.dock_items)
}

/// Whether the keyboard can reach `item`: it has a button, and is not a separator
/// (which is drawn, not focused).
///
/// # Safety
///
/// `item` must be null or a live `DockItem`.
unsafe fn keyboard_reachable(item: *const DockItem) -> bool {
    item.as_ref()
        .is_some_and(|item| !item.button.is_null() && item.kind != ITEM_SEPARATOR)
}

/// The `index`th item the keyboard can reach.
///
/// # Safety
///
/// `state` must be a live `LambdockState`.
unsafe fn item_at_index(state: *const LambdockState, index: usize) -> *mut DockItem {
    let mut seen = 0;
    let mut link = dock_items(state);

    while let Some(entry) = link.as_ref() {
        let item = entry.data.cast::<DockItem>();
        if keyboard_reachable(item) {
            if seen == index {
                return item;
            }
            seen += 1;
        }

        link = entry.next;
    }

    std::ptr::null_mut()
}

/// The index of `target` among the reachable items, when it is one of them.
///
/// # Safety
///
/// `state` must be a live `LambdockState`; `target` must be null or a live
/// `DockItem`.
unsafe fn index_of(state: *const LambdockState, target: *const DockItem) -> Option<i32> {
    if target.is_null() {
        return None;
    }

    let mut index = 0;
    let mut link = dock_items(state);

    while let Some(entry) = link.as_ref() {
        let item = entry.data.cast::<DockItem>();
        if keyboard_reachable(item) {
            if item == target.cast_mut() {
                return Some(index);
            }
            index += 1;
        }

        link = entry.next;
    }

    None
}

/// The item that currently holds the keyboard focus, looking through the button's
/// children so a focused label still resolves to its item.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`.
unsafe fn focused_item(state: *mut LambdockState) -> *mut DockItem {
    let Some(state_ref) = state.as_ref() else {
        return std::ptr::null_mut();
    };

    let Some(window) = widget_of(state_ref.window) else {
        return std::ptr::null_mut();
    };
    let Some(window) = window.downcast_ref::<gtk4::Window>() else {
        return std::ptr::null_mut();
    };

    let root = state_ref.window.cast::<gtk4::ffi::GtkWidget>();
    let mut focus = gtk4::prelude::GtkWindowExt::focus(window);

    while let Some(widget) = focus {
        let item = dock_item_from_widget(widget.as_ptr());
        if !item.is_null() {
            return item;
        }
        if widget.as_ptr() == root {
            break;
        }

        focus = crate::ui::parent_of(widget.as_ref()).map(|parent| parent.as_ref().clone());
    }

    std::ptr::null_mut()
}

/// Where the focus is, for a step's arithmetic.
///
/// # Safety
///
/// `state` must be a live `LambdockState`.
unsafe fn focus_position(state: *mut LambdockState) -> Focus {
    let item = focused_item(state);

    if item.is_null() {
        return Focus::None;
    }

    match index_of(state, item) {
        Some(index) => Focus::At(index),
        None => Focus::Elsewhere,
    }
}

/// Puts the keyboard focus on `item`'s button.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`.
unsafe fn grab_item_focus(item: *mut DockItem) -> bool {
    let Some(button) = item.as_ref().and_then(|item| widget_of(item.button)) else {
        return false;
    };

    button.grab_focus();

    true
}

/// How many items the keyboard can reach.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`.
pub unsafe fn lambdock_dock_item_count(state: *const LambdockState) -> c_int {
    let mut count = 0;
    let mut link = dock_items(state);

    while let Some(entry) = link.as_ref() {
        count += c_int::from(keyboard_reachable(entry.data.cast::<DockItem>()));
        link = entry.next;
    }

    count
}

/// The `index`th item the keyboard can reach, or null.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`.
pub unsafe fn lambdock_dock_item_at_index(
    state: *const LambdockState,
    index: c_int,
) -> *mut DockItem {
    if state.is_null() || index < 0 {
        return std::ptr::null_mut();
    }

    item_at_index(state, index as usize)
}

/// The index of `item` among the reachable ones, or -1 when it is not one.
///
/// # Safety
///
/// Both must be null or live.
pub unsafe fn lambdock_dock_item_index_of(
    state: *const LambdockState,
    item: *const DockItem,
) -> c_int {
    if state.is_null() {
        return -1;
    }

    index_of(state, item).unwrap_or(-1)
}

/// The item that holds the keyboard focus, or null.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`.
pub unsafe fn lambdock_dock_item_focused(state: *mut LambdockState) -> *mut DockItem {
    if state.is_null() {
        return std::ptr::null_mut();
    }

    focused_item(state)
}

/// Moves the focus `delta` items along the dock.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`.
pub unsafe fn lambdock_dock_item_focus_step(
    state: *mut LambdockState,
    delta: c_int,
) -> c_int {
    if state.is_null() || delta == 0 {
        return 0;
    }

    let count = lambdock_dock_item_count(state) as usize;
    if count == 0 {
        return 0;
    }

    let Some(next) = stepped_index(focus_position(state), delta, count) else {
        return 0;
    };

    c_int::from(grab_item_focus(item_at_index(state, next)))
}

/// Moves the focus to the first or the last item.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`.
pub unsafe fn lambdock_dock_item_focus_edge(
    state: *mut LambdockState,
    last: c_int,
) -> c_int {
    if state.is_null() {
        return 0;
    }

    let count = lambdock_dock_item_count(state) as usize;
    if count == 0 {
        return 0;
    }

    let index = if last != 0 { count - 1 } else { 0 };

    c_int::from(grab_item_focus(item_at_index(state, index)))
}

/// Lays the dock's child widgets out in the order of `state->dock_items`.
///
/// # Safety
///
/// `state` must be a live `LambdockState`.
unsafe fn reorder_dock_widgets(state: &LambdockState) {
    let Some(box_) = widget_of(state.box_) else {
        return;
    };
    let Some(box_) = box_.downcast_ref::<gtk4::Box>() else {
        return;
    };

    let mut previous: Option<gtk4::Widget> = None;
    let mut link = state.dock_items;

    while let Some(entry) = link.as_ref() {
        let item = entry.data.cast::<DockItem>();
        link = entry.next;

        let Some(item) = item.as_ref() else {
            continue;
        };

        let child = if !item.container.is_null() {
            item.container
        } else {
            item.button
        };
        let Some(child) = widget_of(child) else {
            continue;
        };
        let parent = child.parent().map(|parent| parent.as_ptr());
        if parent != Some(box_.upcast_ref::<gtk4::Widget>().as_ptr()) {
            continue;
        }

        box_.reorder_child_after(child.as_ref(), previous.as_ref());
        previous = Some(child.as_ref().clone());
    }
}

/// Writes the item list back into the dock's state.
///
/// # Safety
///
/// `state` must be a live `LambdockState`; `list` must be the list it should hold.
unsafe fn set_dock_items(state: *mut LambdockState, list: *mut glib::ffi::GList) {
    (*state).dock_items = list.cast();
}

/// Moves `item` `delta` places along the dock, and remembers the order.
///
/// # Safety
///
/// Both must be null or live.
pub unsafe fn lambdock_dock_item_move(
    state: *mut LambdockState,
    item: *mut DockItem,
    delta: c_int,
) -> c_int {
    let (Some(state_ref), false) = (state.as_ref(), item.is_null() || delta == 0) else {
        return 0;
    };

    let count = lambdock_dock_item_count(state) as usize;
    let Some(index) = index_of(state, item) else {
        return 0;
    };
    let target = index + delta;
    if target < 0 || target as usize >= count {
        return 0;
    }

    let Some(other) = item_at_index(state, target as usize).as_ref() else {
        return 0;
    };
    let other = other as *const DockItem as *mut DockItem;

    let list = glib::ffi::g_list_find(state_ref.dock_items.cast(), item.cast());
    if list.is_null() {
        return 0;
    }
    let list = glib::ffi::g_list_delete_link(state_ref.dock_items.cast(), list);
    set_dock_items(state, list);

    // The neighbour may have been the link that was just deleted: look it up
    // again, and put the item back rather than drop it off the dock.
    let other_link = glib::ffi::g_list_find(list, other.cast());
    if other_link.is_null() {
        let list = glib::ffi::g_list_append(list, item.cast());
        set_dock_items(state, list);
        return 0;
    }

    let before = if delta > 0 {
        (*other_link).next
    } else {
        other_link
    };
    let list = glib::ffi::g_list_insert_before(list, before, item.cast());
    set_dock_items(state, list);

    reorder_dock_widgets(&*state);
    dock_dnd_persist_order(state);

    1
}

/// Closes every open panel of `state`: the item menus, the dock's own popover and
/// the launchpad.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`.
pub unsafe fn lambdock_dock_items_close_popovers(state: *mut LambdockState) -> c_int {
    let Some(state_ref) = state.as_ref() else {
        return 0;
    };

    let mut closed = false;
    let mut link = state_ref.dock_items;

    while let Some(entry) = link.as_ref() {
        link = entry.next;

        let item = entry.data.cast::<DockItem>();
        let Some(popover) = item.as_ref().and_then(|item| popover_of(item.popover)) else {
            continue;
        };

        if crate::ui::is_shown(popover.upcast_ref()) {
            popover.popdown();
            closed = true;
        }
    }

    if let Some(popover) = popover_of(state_ref.popover) {
        if crate::ui::is_shown(popover.upcast_ref()) {
            popover.popdown();
            closed = true;
        }
    }

    if lambdock_launchpad_is_open() != 0 {
        lambdock_launchpad_hide();
        closed = true;
    }

    c_int::from(closed)
}

/// What a key does to the dock, as a whole.
///
/// Arrow keys move the focus along the dock's own axis (with Ctrl, they rearrange
/// it), Home and End jump to either end, Menu and Shift+F10 open the focused
/// item's menu, and Escape closes whatever is open. Anything else is not the
/// dock's.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`.
pub unsafe fn lambdock_dock_items_handle_key(
    state: *mut LambdockState,
    keyval: u32,
    modifiers: u32,
) -> c_int {
    let Some(state_ref) = state.as_ref() else {
        return 0;
    };

    let key = gdk::Key::from_glib(keyval);
    let modifiers = gdk::ModifierType::from_bits_truncate(modifiers);

    if key == gdk::Key::Escape {
        return lambdock_dock_items_close_popovers(state);
    }

    let focused = focused_item(state);

    if key_opens_menu(key, modifiers) {
        if focused.is_null() {
            return 0;
        }

        lambdock_dock_item_show_menu(focused);
        return 1;
    }

    // Compared rather than converted: an orientation nobody set is horizontal,
    // which is better than a panic on a value mirrored from C.
    let orientation = if state_ref.orientation == gtk4::Orientation::Vertical.into_glib() {
        gtk4::Orientation::Vertical
    } else {
        gtk4::Orientation::Horizontal
    };

    let step = key_step(orientation, key);
    if step != 0 {
        // Ctrl+arrow rearranges the dock, so dragging an icon is not the only way
        // to move it.
        if modifiers.contains(gdk::ModifierType::CONTROL_MASK) {
            if focused.is_null() {
                return 0;
            }
            return lambdock_dock_item_move(state, focused, step);
        }

        return lambdock_dock_item_focus_step(state, step);
    }

    if key == gdk::Key::Home {
        return lambdock_dock_item_focus_edge(state, 0);
    }
    if key == gdk::Key::End {
        return lambdock_dock_item_focus_edge(state, 1);
    }

    0
}

/// Installs the dock's keyboard handling on `root`, once.
///
/// # Safety
///
/// `root` must be null or a live `GtkWidget`; `state` must be a live
/// `LambdockState` that outlives it.
pub unsafe fn lambdock_dock_attach_key_navigation(
    root: *mut gtk4::ffi::GtkWidget,
    state: *mut LambdockState,
) {
    use gtk4::glib::translate::from_glib_borrow;

    if root.is_null() || state.is_null() {
        return;
    }

    let root: Borrowed<gtk4::Widget> = from_glib_borrow(root);
    if unsafe { root.data::<bool>(KEY_NAVIGATION_KEY) }.is_some() {
        return;
    }

    let keys = gtk4::EventControllerKey::new();
    // Capture phase: the arrow keys must reach the dock before a child (a focused
    // icon, the scroll view) consumes them as its own navigation.
    keys.set_propagation_phase(gtk4::PropagationPhase::Capture);

    keys.connect_key_pressed(move |_, keyval, _, modifiers| {
        let handled = unsafe {
            lambdock_dock_items_handle_key(state, keyval.into_glib(), modifiers.bits())
        };

        if handled != 0 {
            glib::Propagation::Stop
        } else {
            glib::Propagation::Proceed
        }
    });

    root.add_controller(keys);
    root.set_data(KEY_NAVIGATION_KEY, true);
}

/// Where `root` records that the keyboard handling is installed.
const KEY_NAVIGATION_KEY: &str = "lambdock-key-navigation";

#[cfg(test)]
mod key_tests {
    use super::*;

    /// The axis the dock runs along decides which pair of arrows step.
    #[test]
    fn a_step_key_belongs_to_one_axis() {
        use gtk4::Orientation::{Horizontal, Vertical};

        assert_eq!(key_step(Horizontal, gdk::Key::Left), -1);
        assert_eq!(key_step(Horizontal, gdk::Key::Right), 1);
        assert_eq!(key_step(Horizontal, gdk::Key::Up), 0);
        assert_eq!(key_step(Horizontal, gdk::Key::Down), 0);

        assert_eq!(key_step(Vertical, gdk::Key::Up), -1);
        assert_eq!(key_step(Vertical, gdk::Key::Down), 1);
        assert_eq!(key_step(Vertical, gdk::Key::Left), 0);
        assert_eq!(key_step(Vertical, gdk::Key::Right), 0);

        // Not a step at all.
        assert_eq!(key_step(Horizontal, gdk::Key::space), 0);
        assert_eq!(key_step(Vertical, gdk::Key::Escape), 0);
    }

    /// Two spellings of "open the item's menu", and nothing else is one.
    #[test]
    fn the_menu_key_and_shift_f10_open_the_menu() {
        let none = gdk::ModifierType::empty();
        let shift = gdk::ModifierType::SHIFT_MASK;

        assert!(key_opens_menu(gdk::Key::Menu, none));
        assert!(key_opens_menu(gdk::Key::F10, shift));

        // F10 alone is not, and neither is Shift with another key. The Menu key
        // opens the menu whatever else is held: it has no other meaning here.
        assert!(key_opens_menu(gdk::Key::Menu, shift));
        assert!(!key_opens_menu(gdk::Key::F10, none));
        assert!(!key_opens_menu(gdk::Key::a, shift));
    }

    /// A step is clamped into the dock, and with nothing focused it starts at the
    /// near end.
    #[test]
    fn a_step_starts_at_the_near_end_and_stops_at_the_far_one() {
        // Nothing focused: forward is the first item, back is the last.
        assert_eq!(stepped_index(Focus::None, 1, 3), Some(0));
        assert_eq!(stepped_index(Focus::None, -1, 3), Some(2));

        // The middle walks both ways.
        assert_eq!(stepped_index(Focus::At(1), 1, 3), Some(2));
        assert_eq!(stepped_index(Focus::At(1), -1, 3), Some(0));

        // At either end the key does nothing, so a parent can still have it.
        assert_eq!(stepped_index(Focus::At(0), -1, 3), None);
        assert_eq!(stepped_index(Focus::At(2), 1, 3), None);

        // A dock of one is always at its end, and an empty dock has no index.
        assert_eq!(stepped_index(Focus::At(0), 1, 1), None);
        assert_eq!(stepped_index(Focus::None, 1, 0), None);
        assert_eq!(stepped_index(Focus::None, -1, 0), None);

        // The focus can sit on something the dock does not list -- a separator,
        // or a widget that is not an item. That is not a position, so the step
        // starts from the near end instead of refusing to move.
        assert_eq!(stepped_index(Focus::Elsewhere, 1, 3), Some(0));
        assert_eq!(stepped_index(Focus::Elsewhere, -1, 3), Some(2));
    }
}


// ============================================================================
// DYNAMIC ITEMS: THE POLLER AND THE READINGS
// ============================================================================
//
// A dynamic item -- a clock, a CPU gauge, a RAM meter -- is a widget plus a
// Scheme procedure: the procedure answers with a number or a string and the
// widget shows it. Polling is the one place the dock wakes itself up, so it is
// also the one place that asks whether anybody is looking.

/// `LambdockReadingKind`, from `guile_bridge.h`.
const READING_NUMBER: c_int = 1;

/// One poll's answer, as the bridge hands it over.
#[repr(C)]
pub struct Reading {
    pub kind: c_int,
    pub number: f64,
    /// GLib's copy of the text: freed once it has been shown.
    pub text: *mut c_char,
}

/// What a `GSourceFunc` answers: run again, or stop and be forgotten.
const SOURCE_REMOVE: c_int = 0;
const SOURCE_CONTINUE: c_int = 1;


/// Whether `item` still has a callable Scheme poller.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn has_poller(item: &DockItem) -> bool {
    !item.poll_fn.is_null() && lambdock_poll_fn_is_callable(item.poll_fn) != 0
}

/// Whether a dock is off screen: hidden, being destroyed, or never mapped.
fn off_screen(hidden: bool, destroying: bool, mapped: bool) -> bool {
    hidden || destroying || !mapped
}

/// The same rule, asked of a dock's state and its window.
///
/// # Safety
///
/// `state` must be a live `LambdockState`.
unsafe fn state_is_off_screen(state: &LambdockState) -> bool {
    let mapped = widget_of(state.window).is_some_and(|window| crate::ui::is_mapped(&window));

    off_screen(state.is_hidden != 0, state.is_destroying != 0, mapped)
}

/// Draws a numeric reading on the item's gauge, and says so in the tooltip.
///
/// Returns whether the item is a meter and the reading was a number.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn apply_meter_reading(item: &DockItem, reading: &Reading) -> bool {
    if item.meter.is_null() || reading.kind != READING_NUMBER {
        return false;
    }

    let printed = crate::core::item_text::meter_reading_text(
        reading.number,
        unsafe { field(item.meter_unit) },
    );

    let fraction =
        crate::core::item_text::meter_reading_fraction(reading.number, item.meter_max);
    let printed_c = crate::cstr::cstring(&printed);
    crate::ui::meter::lambdock_meter_set_value(item.meter.cast(), fraction, printed_c.as_ptr());

    let name = crate::cstr::text_or_empty(item.name);
    if let Some(button) = widget_of(item.button) {
        button.set_tooltip_text(Some(&format!("{name}: {printed}")));
    }

    dock_item_update_accessible(item as *const DockItem as *mut DockItem);

    true
}

/// Writes a text reading into the item's label and tooltip.
///
/// # Safety
///
/// `item` must be a live `DockItem`; `text` must be null or NUL-terminated.
unsafe fn apply_text_reading(item: &DockItem, text: *const c_char) {
    let Some(text) = crate::cstr::text(text) else {
        return;
    };

    if let Some(label) = widget_of(item.label) {
        if let Some(label) = label.as_ref().downcast_ref::<gtk4::Label>() {
            label.set_text(&text);
        }
    }
    if let Some(button) = widget_of(item.button) {
        button.set_tooltip_text(Some(&text));
    }

    dock_item_update_accessible(item as *const DockItem as *mut DockItem);
}

/// Points the item's clock face at the current local time.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn apply_clock_time(item: &DockItem) {
    let Ok(now) = glib::DateTime::now_local() else {
        return;
    };

    let seconds = now.hour() * 3600 + now.minute() * 60 + now.second();
    crate::ui::clock::lambdock_clock_set_time(item.clock.cast(), seconds);
}

/// Polls `item` once and applies whatever the poller returned.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`.
unsafe fn refresh(item: *mut DockItem) {
    let Some(item_ref) = item.as_ref() else {
        return;
    };

    // An item whose Scheme side gave it no procedure has nothing to poll. Calling
    // it anyway would apply `#f` -- a "Wrong type to apply" raised on the GTK
    // thread, where Guile has no handler, which aborts the process.
    if !has_poller(item_ref) {
        return;
    }

    let mut reading = Reading {
        kind: 0,
        number: 0.0,
        text: std::ptr::null_mut(),
    };
    let have_value = lambdock_poll_fn_call(item_ref.poll_fn, &mut reading) != 0;

    // A clock's hands come from the system clock, not from the Scheme value: the
    // poller only formats the reading the tooltip and the screen reader announce.
    if !item_ref.clock.is_null() {
        apply_clock_time(item_ref);
    }

    if have_value && !apply_meter_reading(item_ref, &reading) {
        apply_text_reading(item_ref, reading.text);
    }

    if !reading.text.is_null() {
        crate::glib::g_free(reading.text.cast());
    }
}

/// Whether `item` still has a Scheme poller.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`.
pub unsafe fn dock_item_has_poller(item: *const DockItem) -> c_int {
    c_int::from(item.as_ref().is_some_and(|item| has_poller(item)))
}

/// Whether the dock `item` belongs to is off screen.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`.
pub unsafe fn dock_item_off_screen(item: *const DockItem) -> c_int {
    let Some(state) = item.as_ref().and_then(|item| item.state.as_ref()) else {
        return 0;
    };

    c_int::from(state_is_off_screen(state))
}

/// Polls `item` once and applies the reading.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`.
pub unsafe fn dock_item_refresh_readings(item: *mut DockItem) {
    refresh(item);
}

/// The timer behind a dynamic item.
///
/// # Safety
///
/// `user_data` must be null or a live `DockItem` that outlives the source.
pub unsafe extern "C" fn lambdock_dock_item_poll(user_data: *mut c_void) -> c_int {
    let item = user_data.cast::<DockItem>();

    let pollable = item.as_ref().is_some_and(|item| has_poller(item));
    if !pollable {
        // The source goes away once this returns: forget its id, so teardown does
        // not try to remove it -- and cannot remove an unrelated source that has
        // reused the number.
        if let Some(item) = item.as_mut() {
            item.poll_timer_id = 0;
        }

        return SOURCE_REMOVE;
    }

    // A hidden dock with a clock, a CPU gauge and a trash count would wake the
    // process several times a second to update widgets nobody can see, so the
    // work is skipped. `lambdock_dock_items_refresh_polls` catches every item up
    // the moment the dock comes back, so the values are current when they show.
    if item
        .as_ref()
        .and_then(|item| item.state.as_ref())
        .is_some_and(|state| state_is_off_screen(state))
    {
        return SOURCE_CONTINUE;
    }

    refresh(item);

    SOURCE_CONTINUE
}

/// Catches every polled item up, and re-counts every trash, in one pass.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`.
pub unsafe fn dock_items_refresh_polls(state: *mut LambdockState) {
    let Some(state_ref) = state.as_ref() else {
        return;
    };

    let mut link = state_ref.dock_items;
    while let Some(entry) = link.as_ref() {
        link = entry.next;

        let item = entry.data.cast::<DockItem>();
        let Some(item_ref) = item.as_ref() else {
            continue;
        };

        if has_poller(item_ref) {
            refresh(item);
        }
        if item_ref.trash_timer_id != 0 {
            dock_item_refresh_trash(item);
        }
    }
}

#[cfg(test)]
mod poll_tests {
    use super::*;

    /// A dock nobody can see is not worth refreshing: hidden, going away, or not
    /// mapped at all. Everything a hidden dock would do -- a clock, a CPU gauge, a
    /// trash count -- is work for a widget that is not on screen.
    #[test]
    fn a_dock_nobody_can_see_is_off_screen() {
        assert!(off_screen(false, false, false), "not mapped");
        assert!(off_screen(true, false, true), "hidden");
        assert!(off_screen(false, true, true), "going away");

        // Hidden wins over everything: the flags are not exclusive in practice.
        assert!(off_screen(true, false, false));

        // The one answer that means "refresh it".
        assert!(!off_screen(false, false, true));
    }
}


// ============================================================================
// ALIGNMENT
// ============================================================================
//
// Alignment is a decision (which edge is the dock on?) followed by three widget
// calls (a size request, an alignment, centring the content). The decision is a
// value, so it is written once and read by every applier.

/// How an item's widgets align themselves on a dock attached to an edge.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
struct Alignment {
    halign: gtk4::Align,
    valign: gtk4::Align,
}

/// The alignment a dock's edge asks for: centred along the dock, anchored to the
/// edge it sits on. Any other edge centres both ways, rather than pinning an item
/// to an edge the dock is not on.
fn alignment_for_edge(edge: c_int) -> Alignment {
    let (halign, valign) = match edge {
        EDGE_BOTTOM => (gtk4::Align::Center, gtk4::Align::End),
        EDGE_TOP => (gtk4::Align::Center, gtk4::Align::Start),
        EDGE_LEFT => (gtk4::Align::Start, gtk4::Align::Center),
        EDGE_RIGHT => (gtk4::Align::End, gtk4::Align::Center),
        _ => (gtk4::Align::Center, gtk4::Align::Center),
    };

    Alignment { halign, valign }
}

/// The cell request for one item: one icon across the dock, free along it.
///
/// The theme's button padding sits outside that cell, so an icon, a date label
/// ("Thu Oct 29") and a gauge all end up exactly as tall as each other. Sizing the
/// *button* instead leaves the theme's padding out of the sum, and the shorter
/// items then hang at the bottom of the row.
fn size_request(icon_size: i32, horizontal: bool) -> (i32, i32) {
    if horizontal {
        (-1, icon_size)
    } else {
        (icon_size, -1)
    }
}

/// The icon size a dock lays out at, with the default for partly built states.
fn icon_size(state: Option<&LambdockState>) -> i32 {
    match state.map(|state| state.icon_size) {
        Some(size) if size > 0 => size,
        _ => DEFAULT_ICON_SIZE,
    }
}

/// The icon size a dock lays out at.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`.
pub unsafe fn dock_item_icon_size(state: *const LambdockState) -> c_int {
    icon_size(state.as_ref())
}

/// Whether a dock runs along the top or bottom of the screen.
fn is_horizontal(state: &LambdockState) -> bool {
    state.orientation != gtk4::Orientation::Vertical.into_glib()
}

/// Asks for the item's cell: one icon across the dock, free along it.
///
/// # Safety
///
/// `item` must be a live `DockItem` with a state.
unsafe fn apply_size_request(item: &DockItem) {
    let Some(state) = item.state.as_ref() else {
        return;
    };

    let cell = if item.cell.is_null() {
        item.button
    } else {
        item.cell
    };
    let Some(cell) = widget_of(cell) else {
        return;
    };

    let (width, height) = size_request(icon_size(Some(state)), is_horizontal(state));
    cell.set_size_request(width, height);
}

/// Points `widget` at the dock's edge.
fn apply_alignment(widget: &gtk4::Widget, alignment: Alignment) {
    widget.set_halign(alignment.halign);
    widget.set_valign(alignment.valign);
}

/// Centres the button's content inside it, and everything in that content on the
/// button's cross axis.
///
/// # Safety
///
/// `button` must be a live `GtkWidget`.
fn centre_button_content(button: &gtk4::Widget) {
    let Some(button) = button.downcast_ref::<gtk4::Button>() else {
        return;
    };
    let Some(child) = crate::ui::button_child(button) else {
        return;
    };

    child.set_halign(gtk4::Align::Center);
    child.set_valign(gtk4::Align::Center);

    if !child.as_ref().is::<gtk4::Box>() {
        return;
    }

    let mut widget = crate::ui::first_child_of(child.as_ref());
    while let Some(each) = widget {
        each.set_valign(gtk4::Align::Center);
        widget = crate::ui::next_sibling_of(each.as_ref());
    }
}

/// Applies the dock's edge rules to one item: its cell, how it leans, and how its
/// content sits inside it.
///
/// A separator is the exception: its line fills the cell it is given -- the cell is
/// what follows the dock edge and the line is all that is inside it -- so it spans
/// the icons' height instead of collapsing to the one-pixel separator default.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`.
pub unsafe fn apply_dock_item_alignment(item: *mut DockItem) {
    let Some(item_ref) = item.as_ref() else {
        return;
    };
    if item_ref.button.is_null() {
        return;
    }
    let Some(state) = item_ref.state.as_ref() else {
        return;
    };

    apply_size_request(item_ref);

    let alignment = alignment_for_edge(state.edge);
    if let Some(button) = widget_of(item_ref.button) {
        apply_alignment(&button, alignment);

        if item_ref.kind == ITEM_SEPARATOR {
            let horizontal = is_horizontal(state);
            button.set_valign(if horizontal {
                gtk4::Align::Fill
            } else {
                gtk4::Align::Center
            });
            button.set_halign(if horizontal {
                gtk4::Align::Center
            } else {
                gtk4::Align::Fill
            });
        }
    }

    // The container holds the button plus (when reflections are on) its glass
    // shelf reflection, so it hugs the same dock edge as the button.
    if let Some(container) = widget_of(item_ref.container) {
        apply_alignment(&container, alignment);
    }

    if let Some(button) = widget_of(item_ref.button) {
        centre_button_content(&button);
    }
}

#[cfg(test)]
mod align_tests {
    use super::*;

    /// A dock on an edge anchors its items to that edge and centres them along
    /// it, which is what makes a row of different heights look like one row.
    #[test]
    fn the_edge_decides_which_way_an_item_lears() {
        assert_eq!(
            alignment_for_edge(EDGE_BOTTOM),
            Alignment {
                halign: gtk4::Align::Center,
                valign: gtk4::Align::End
            }
        );
        assert_eq!(
            alignment_for_edge(EDGE_TOP),
            Alignment {
                halign: gtk4::Align::Center,
                valign: gtk4::Align::Start
            }
        );
        assert_eq!(
            alignment_for_edge(EDGE_LEFT),
            Alignment {
                halign: gtk4::Align::Start,
                valign: gtk4::Align::Center
            }
        );
        assert_eq!(
            alignment_for_edge(EDGE_RIGHT),
            Alignment {
                halign: gtk4::Align::End,
                valign: gtk4::Align::Center
            }
        );

        // An edge nobody set is a dock that centres both ways rather than an
        // item pinned to an edge it is not on.
        for edge in [-1, 4, 99] {
            assert_eq!(
                alignment_for_edge(edge),
                Alignment {
                    halign: gtk4::Align::Center,
                    valign: gtk4::Align::Center
                }
            );
        }
    }

    /// The cell is one icon across the dock and free along it: the theme's button
    /// padding sits outside that cell, so an icon, a date label and a gauge all
    /// end up exactly as tall as each other.
    #[test]
    fn the_cell_is_one_icon_across_the_dock() {
        // A horizontal dock: the height is fixed, the width is not.
        assert_eq!(size_request(48, true), (-1, 48));
        // A vertical one: the same request, turned over.
        assert_eq!(size_request(48, false), (48, -1));
    }

    /// A dock that has not been configured yet still lays its items out at the
    /// default icon size, not at zero.
    #[test]
    fn the_icon_size_falls_back_to_the_default() {
        let dock = |icon_size| {
            let mut state = unsafe { std::mem::zeroed::<LambdockState>() };
            state.icon_size = icon_size;
            state
        };

        assert_eq!(icon_size(None), DEFAULT_ICON_SIZE);
        assert_eq!(icon_size(Some(&dock(0))), DEFAULT_ICON_SIZE);
        assert_eq!(icon_size(Some(&dock(-4))), DEFAULT_ICON_SIZE);
        assert_eq!(icon_size(Some(&dock(32))), 32);
    }
}


// ============================================================================
// THE TRASH
// ============================================================================
//
// The trash is the one item the rest of the desktop changes under it, so it is
// re-counted on a timer rather than on a signal nobody sends. Everything except
// the counting itself (which is `ui/trash.rs`) lives here: which artwork a count
// calls for, and when the artwork may be swapped at all.

/// The full-trash name in the same family as `icon`: the plain and symbolic
/// spellings both come from the Icon Naming Specification, and a theme set up for
/// symbolic icons carries the symbolic one.
fn trash_full_icon_for(icon: Option<&str>) -> &'static str {
    match icon {
        Some(icon) if icon.ends_with("-symbolic") => "user-trash-full-symbolic",
        _ => "user-trash-full",
    }
}

/// The artwork an item shows when the trash holds `count` items.
///
/// Empty is what the item was configured with -- or the theme's own trash when it
/// was given nothing. Full is always the family's full icon, because the
/// configured one *is* the empty artwork.
fn trash_icon_for(icon: Option<&str>, full: bool) -> &str {
    if full {
        return trash_full_icon_for(icon);
    }

    icon.filter(|icon| !icon.is_empty()).unwrap_or("user-trash")
}

/// Whether the item's artwork can be swapped between the two themed trash icons.
///
/// A configured file, resource or unknown icon is left alone: replacing it would
/// lose the artwork the user chose, and an icon name is not a path.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn trash_artwork_is_swappable(item: &DockItem) -> bool {
    let Some(image) = widget_of(item.icon_image) else {
        return false;
    };
    let Some(image) = image.as_ref().downcast_ref::<gtk4::Image>() else {
        return false;
    };
    if image.storage_type() != gtk4::ImageType::IconName {
        return false;
    }

    let icon = crate::cstr::text_or_empty(item.icon);
    if icon.is_empty() {
        return true;
    }

    gdk::Display::default()
        .map(|display| gtk4::IconTheme::for_display(&display))
        .is_some_and(|theme| theme.has_icon(&icon))
}

/// Re-counts the trash and, when the count changed, swaps the artwork and the
/// tooltip.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`.
pub unsafe fn dock_item_refresh_trash(item: *mut DockItem) {
    let Some(item_ref) = item.as_mut() else {
        return;
    };
    if item_ref.kind != ITEM_TRASH {
        return;
    }

    let count = crate::ui::trash::lambdock_trash_count();
    let full = count > 0;

    if count == item_ref.trash_count && full == (item_ref.trash_full != 0) {
        return;
    }

    item_ref.trash_count = count;
    item_ref.trash_full = c_int::from(full);

    if trash_artwork_is_swappable(item_ref) {
        let configured = crate::cstr::text(item_ref.icon);
        let icon = trash_icon_for(configured.as_deref(), full);

        if let Some(image) = widget_of(item_ref.icon_image) {
            if let Some(image) = image.as_ref().downcast_ref::<gtk4::Image>() {
                if image.icon_name().as_deref() != Some(icon) {
                    image.set_icon_name(Some(icon));
                }
            }
        }
    }

    // The wording (and its plural form) is `core/item_text.rs`'s, the same place
    // the screen-reader description comes from.
    let tooltip = trash_tooltip(unsafe { field(item_ref.name) }, count);

    if let Some(button) = widget_of(item_ref.button) {
        button.set_tooltip_text(Some(&tooltip));
    }

    dock_item_update_accessible(item);
}

/// The timer behind a trash item.
///
/// # Safety
///
/// `user_data` must be null or a live `DockItem` that outlives the source.
pub unsafe extern "C" fn lambdock_dock_item_trash_poll(user_data: *mut c_void) -> c_int {
    let item = user_data.cast::<DockItem>();
    if item.as_ref().is_none() {
        return SOURCE_REMOVE;
    }

    // Same rule as the poll timer: do not scan the trash directories for a dock
    // nobody can see.
    if dock_item_off_screen(item) != 0 {
        return SOURCE_CONTINUE;
    }

    dock_item_refresh_trash(item);

    SOURCE_CONTINUE
}

#[cfg(test)]
mod trash_tests {
    use super::*;

    /// The Icon Naming Specification has a name for the empty trash and one for
    /// the full trash, in both the plain and the symbolic spelling -- so the full
    /// icon is the one from the same family as the icon the item was given.
    #[test]
    fn the_full_trash_icon_stays_in_its_family() {
        assert_eq!(trash_full_icon_for(Some("user-trash")), "user-trash-full");
        assert_eq!(
            trash_full_icon_for(Some("user-trash-symbolic")),
            "user-trash-full-symbolic"
        );

        // Nothing configured is still a family: the theme's own trash.
        assert_eq!(trash_full_icon_for(None), "user-trash-full");
        assert_eq!(trash_full_icon_for(Some("")), "user-trash-full");
    }

    /// An empty trash shows what the item was configured with, and the theme's
    /// trash when it was given nothing; a full one never shows the configured
    /// icon, because the configured icon is the *empty* artwork.
    #[test]
    fn the_empty_trash_shows_the_configured_icon() {
        assert_eq!(
            trash_icon_for(Some("user-trash-symbolic"), false),
            "user-trash-symbolic"
        );
        assert_eq!(trash_icon_for(None, false), "user-trash");
        assert_eq!(trash_icon_for(Some(""), false), "user-trash");

        assert_eq!(
            trash_icon_for(Some("user-trash-symbolic"), true),
            "user-trash-full-symbolic"
        );
        // A custom icon has no full sibling: the theme's full trash stands in.
        assert_eq!(
            trash_icon_for(Some("my-custom-trash"), true),
            "user-trash-full"
        );
    }
}

// ============================================================================
// THE ITEM'S SHELL
// ============================================================================
//
// Every kind of item is the same GTK scaffold around different content:
//
//   button -> content box -> [icon, label?] -> glass reflection
//          -> shelf container -> the dock row
//
// That scaffold used to be copy-pasted into three constructors of fifty lines
// each, so touching the popover, the gestures or the drop targets meant touching
// all three. A constructor now builds only its content and calls `mount`.
//
// The widgets are built through the C constructors, as the rest of the port does:
// a floating reference is what the container that appends it expects, and the item
// itself owns no reference at all -- it holds the pointers, and the widget tree
// holds the widgets. That is what makes `free_item_widgets` a matter of taking them
// out of their parents.

/// `WINDOW_TITLE_MAX_CHARS`, `SEPARATOR_MARGIN_PX`, `FISHEYE_MAX_SCALE` and
/// `TRASH_REFRESH_INTERVAL_MS` from the headers.
const WINDOW_TITLE_MAX_CHARS: i32 = 28;
const SEPARATOR_MARGIN_PX: i32 = 4;
const FISHEYE_MAX_SCALE: f64 = 1.45;
const TRASH_REFRESH_INTERVAL_MS: u32 = 2000;

/// Where deferred callbacks find the item behind a widget.
///
/// A `&CStr`, not a `&str`: the key is handed to `g_object_set_data` as a plain
/// `const char *`, and a `&str`'s bytes are not NUL-terminated.
const ITEM_DATA_KEY: &std::ffi::CStr = c"lambdock-item";

/// Where a teardown finds the handlers it has to disconnect first: a signal that
/// fires after its item is gone would read freed memory.
const CLICKED_HANDLER_KEY: &str = "lambdock-item-clicked-handler";
const CLOSED_HANDLER_KEY: &str = "lambdock-item-closed-handler";


/// What a constructor tells the shared scaffold about the item it is building.
struct Shell<'a> {
    state: *mut LambdockState,
    kind: c_int,
    name: Option<&'a str>,
    icon: Option<&'a str>,
    exec: Option<&'a str>,
    app_id: Option<&'a str>,
    is_favorite: bool,
    hover_animate: bool,
}

/// An item allocated the way C allocates one, so `free_dock_item` can hand it back
/// to `g_free`.
unsafe fn new_item(shell: &Shell<'_>) -> *mut DockItem {
    let item = crate::glib::g_malloc0(std::mem::size_of::<DockItem>()).cast::<DockItem>();
    let item_ref = &mut *item;

    item_ref.state = shell.state;
    item_ref.kind = shell.kind;
    item_ref.name = shell.name.map_or(std::ptr::null_mut(), crate::glib::strdup_string);
    item_ref.icon = shell.icon.map_or(std::ptr::null_mut(), crate::glib::strdup_string);
    item_ref.exec = shell.exec.map_or(std::ptr::null_mut(), crate::glib::strdup_string);
    item_ref.app_id = shell.app_id.map_or(std::ptr::null_mut(), crate::glib::strdup_string);
    item_ref.is_favorite = c_int::from(shell.is_favorite);
    item_ref.hover_animate = c_int::from(shell.hover_animate);

    item
}

/// The button an item is clicked through, and the empty content box inside it:
/// the classes every item shares, plus the one its kind adds.
unsafe fn begin_content(item: &mut DockItem, spacing: i32) -> Option<Borrowed<gtk4::Widget>> {
    use gtk4::glib::translate::IntoGlib;

    let button = gtk4::ffi::gtk_button_new();
    item.button = button.cast();

    let button_view: Borrowed<gtk4::Widget> = from_glib_borrow(button);
    button_view.add_css_class("dock-item");
    button_view.add_css_class("flat");
    if let Some(class) = kind_ops(item.kind).css_class {
        button_view.add_css_class(&class.to_string_lossy());
    }
    // A magnified icon is drawn outside the box it was allocated.
    button_view.set_overflow(gtk4::Overflow::Visible);

    let content = gtk4::ffi::gtk_box_new(gtk4::Orientation::Horizontal.into_glib(), spacing);
    item.cell = content.cast();

    let content_view: Borrowed<gtk4::Widget> = from_glib_borrow(content);
    content_view.set_valign(gtk4::Align::Center);

    // The button owns its child from here on, which is what sinks the box.
    gtk4::ffi::gtk_button_set_child(button.cast(), content);

    Some(content_view)
}

/// Appends `child` to `box_`, which is the box's own `gtk_box_append`: a widget is
/// what the item keeps, and the cast to `GtkBox` is the one thing the container
/// knows that the item does not.
///
/// # Safety
///
/// `box_` must be a live `GtkBox`; `child` a live, unparented widget.
unsafe fn box_append(box_: &gtk4::Widget, child: *mut gtk4::ffi::GtkWidget) {
    gtk4::ffi::gtk_box_append(box_.as_ptr().cast(), child);
}

/// An item's artwork from the icon theme, at the dock's icon size.
unsafe fn load_icon(state: *const LambdockState, icon: *const c_char) -> *mut gtk4::ffi::GtkWidget {
    let theme = crate::ui::icon_theme();

    crate::ui::icon_resolver::create_image_for_icon(
        theme.map_or(std::ptr::null_mut(), |theme| theme.as_ptr().cast()),
        icon,
        icon_size(state.as_ref()),
    )
}

/// Wraps `image` so the fisheye can magnify it by painting a transform instead of
/// re-resolving the artwork at every pixel size.
///
/// # Safety
///
/// `image` must be null or a live, unparented `GtkWidget`.
unsafe fn wrap_icon_for_zoom(
    image: *mut gtk4::ffi::GtkWidget,
    base_size: i32,
) -> *mut gtk4::ffi::GtkWidget {
    if image.is_null() {
        return std::ptr::null_mut();
    }

    let base_size = if base_size <= 0 {
        DEFAULT_ICON_SIZE
    } else {
        base_size
    };
    let max_size = (f64::from(base_size) * FISHEYE_MAX_SCALE).ceil() as i32;

    crate::ui::icon_zoom::lambdock_icon_zoom_new(image, base_size, max_size)
}

/// Appends `image` to the content box, wrapped so the fisheye can scale it.
///
/// # Safety
///
/// `image` must be null or a live, unparented `GtkWidget`.
unsafe fn append_icon(content: &gtk4::Widget, image: *mut gtk4::ffi::GtkWidget, icon_size: i32) {
    if image.is_null() {
        return;
    }

    if let Some(image) = widget_of(image.cast()) {
        image.set_overflow(gtk4::Overflow::Visible);
    }

    let zoom = wrap_icon_for_zoom(image, icon_size);
    if !zoom.is_null() {
        box_append(content, zoom);
    }
}

/// The item's glass-shelf reflection slot.
///
/// Every item gets one, including the ones with nothing to mirror -- a gauge, a
/// separator, an item whose icon did not load. The band the slot reserves is part
/// of the cell's height, so dropping it would leave those items a band lower than
/// the icons next to them on a shelf theme.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn attach_reflection(item: &mut DockItem, image: *mut gtk4::ffi::GtkWidget) {
    item.reflection = crate::ui::shelf::shelf_reflection_new(image).cast();

    if let Some(reflection) = widget_of(item.reflection) {
        reflection.set_visible(false);
    }
}

/// The item's own context popover, parented to its button.
///
/// # Safety
///
/// `item` must be a live `DockItem` with a button.
unsafe fn install_popover(item: &mut DockItem) {
    let popover = gtk4::ffi::gtk_popover_new();
    item.popover = popover.cast();

    let popover_widget: Borrowed<gtk4::Widget> = from_glib_borrow(popover);
    if let Some(popover_view) = popover_widget.as_ref().downcast_ref::<gtk4::Popover>() {
        popover_view.set_has_arrow(false);
    }

    if let Some(button) = widget_of(item.button) {
        // Takes ownership of the floating reference the constructor returned.
        gtk4::ffi::gtk_widget_set_parent(popover, button.as_ptr().cast());
    }

    crate::ui::popover_menu::lambdock_popover_close_on_escape(popover.cast());

    // `on_popover_closed()` releases the dock's popover slot, so it is only
    // connected for items that belong to a dock -- and remembered, because a
    // teardown has to disconnect it before the popover is taken down.
    if let Some(popover_view) = popover_widget.as_ref().downcast_ref::<gtk4::Popover>() {
        if !item.state.is_null() {
            let state = item.state.cast();
            let closed = popover_view.connect_closed(move |popover| {
                on_popover_closed(popover.as_ptr().cast(), state);
            });
            popover_view.set_data(CLOSED_HANDLER_KEY, closed);
        }
    }
}

/// Left click activates the item, right click opens its context menu.
///
/// # Safety
///
/// `item` must be a live `DockItem` with a button.
unsafe fn install_gestures(item: *mut DockItem) {
    let Some(button) = widget_of((*item).button) else {
        return;
    };
    let button_ref: &gtk4::Widget = button.as_ref();

    if let Some(button_view) = button_ref.downcast_ref::<gtk4::Button>() {
        let clicked = button_view.connect_clicked(move |_| dock_item_activate(item));
        button_ref.set_data(CLICKED_HANDLER_KEY, clicked);
    }

    let right_click = gtk4::GestureClick::new();
    right_click.set_button(gdk::BUTTON_SECONDARY);
    right_click.connect_pressed(move |_, _, _, _| lambdock_dock_item_popup_menu(item));
    button_ref.add_controller(right_click);
}

/// Finishes an item once its content exists: the glass-shelf container, its
/// alignment, the tooltip, the context menu, drag and drop, and its place in the
/// dock row.
///
/// # Safety
///
/// `item` must be a live `DockItem` with its widgets built.
unsafe fn mount(item: *mut DockItem) -> *mut DockItem {
    let item_ref = &mut *item;
    let state = item_ref.state;

    item_ref.container = crate::ui::shelf::shelf_wrap_item(
        state,
        item_ref.button.cast(),
        item_ref.reflection.cast(),
    )
    .cast();

    if !state.is_null() {
        apply_dock_item_alignment(item);
    }

    if let (Some(name), Some(button)) = (crate::cstr::non_empty_text(item_ref.name), widget_of(item_ref.button)) {
        button.set_tooltip_text(Some(&name));
    }

    install_gestures(item);
    install_popover(item_ref);

    // The container goes into the row before drag and drop looks it up: the bar
    // sinks the floating reference a constructor hands over, and a reader that
    // arrives first would sink it itself and let it go.
    if let Some(state) = state.as_mut() {
        if let Some(dock_box) = widget_of(state.box_).and_then(|dock_box| {
            dock_box.as_ref().downcast_ref::<gtk4::Box>().map(|dock_box| dock_box.as_ptr())
        }) {
            let child = if item_ref.container.is_null() {
                item_ref.button
            } else {
                item_ref.container
            };
            if !child.is_null() {
                gtk4::ffi::gtk_box_append(dock_box, child.cast());
            }
        }

        state.dock_items = glib::ffi::g_list_append(state.dock_items.cast(), item.cast()).cast();
        // A new item moves every centre after it.
        state.centres_dirty = 1;
    }

    crate::ui::dock_dnd::dock_dnd_setup_item(item);

    attach_data(item);
    dock_item_update_accessible(item);

    item
}

/// Stores `owner` (or, with null, clears it) on every widget of the item that a
/// deferred callback may hold on to.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn set_backpointers(item: &DockItem, owner: *mut DockItem) {
    for widget in [item.button, item.container] {
        if !widget.is_null() {
            glib::gobject_ffi::g_object_set_data(
                widget.cast(),
                ITEM_DATA_KEY.as_ptr(),
                owner.cast(),
            );
        }
    }
}

/// Stores the item on its own widgets, so a drag callback can find it.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn attach_data(item: *mut DockItem) {
    set_backpointers(&*item, item);
}

/// Clears the back pointers, so deferred callbacks see "no item".
///
/// The widget survives a hot reload -- the drag machinery references it -- but the
/// data becomes null, so the callback bails out instead of touching freed memory.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn detach_data(item: &DockItem) {
    set_backpointers(item, std::ptr::null_mut());
}

/// The item behind a widget, or null when there is none.
///
/// # Safety
///
/// `widget` must be null or a live `GtkWidget`.
pub unsafe fn dock_item_from_widget(widget: *mut gtk4::ffi::GtkWidget) -> *mut DockItem {
    if widget.is_null() {
        return std::ptr::null_mut();
    }

    glib::gobject_ffi::g_object_get_data(widget.cast(), ITEM_DATA_KEY.as_ptr()).cast()
}

/// The app-item label that taskbar mode shows a running window's title in.
fn add_window_title_label(item: &mut DockItem, content: &gtk4::Widget) {
    let label = unsafe { gtk4::ffi::gtk_label_new(std::ptr::null()) };
    item.label = label.cast();
    item.window_title_label = 1;

    let label_view: Borrowed<gtk4::Widget> = unsafe { from_glib_borrow(label) };
    label_view.add_css_class("window-title-label");
    label_view.set_valign(gtk4::Align::Center);
    label_view.set_visible(false);

    if let Some(label) = label_view.as_ref().downcast_ref::<gtk4::Label>() {
        label.set_ellipsize(pango::EllipsizeMode::End);
        label.set_max_width_chars(WINDOW_TITLE_MAX_CHARS);
        label.set_single_line_mode(true);
    }

    unsafe { box_append(content, label) };
}

/// The label a dynamic item writes its polled reading into.
fn add_dynamic_label(item: &mut DockItem, content: &gtk4::Widget, name: &str) {
    let name = crate::cstr::cstring(name);
    let label = unsafe { gtk4::ffi::gtk_label_new(name.as_ptr()) };
    item.label = label.cast();

    let label_view: Borrowed<gtk4::Widget> = unsafe { from_glib_borrow(label) };
    label_view.add_css_class("dynamic-item-label");
    label_view.set_valign(gtk4::Align::Center);

    if let Some(label) = label_view.as_ref().downcast_ref::<gtk4::Label>() {
        label.set_yalign(0.5);
    }

    unsafe { box_append(content, label) };
}

/// The desktop entry an item's launcher refers to.
///
/// The exec line is the most reliable hint (it is what the entry itself runs),
/// then the icon (entries name their icon after the app), then the app id.
///
/// # Safety
///
/// The candidates must be null or NUL-terminated.
unsafe fn resolve_desktop_entry(
    exec: *const c_char,
    icon: *const c_char,
    app_id: *const c_char,
) -> *const crate::ui::desktop_entry::DesktopEntry {
    for candidate in [exec, icon, app_id] {
        let Some(candidate) = crate::cstr::non_empty_text(candidate) else {
            continue;
        };

        let key = crate::cstr::cstring(&candidate);
        let entry = crate::ui::desktop_entry::lambdock_desktop_entry_lookup(key.as_ptr());
        if !entry.is_null() && !(*entry).stem.is_null() {
            return entry;
        }
    }

    std::ptr::null()
}

/// Whether an icon name will draw: a resource, a file on disk, an icon in the
/// theme, or its symbolic sibling.
///
/// It mirrors the first stages of `create_image_for_icon`, and is only used to
/// decide whether a desktop entry's icon is a better bet than a configured one.
fn icon_is_resolvable(theme: Option<&gtk4::IconTheme>, icon: Option<&str>) -> bool {
    let Some(icon) = icon.filter(|icon| !icon.is_empty()) else {
        return false;
    };

    if icon.starts_with("resource://") || std::path::Path::new(icon).exists() {
        return true;
    }

    theme.is_some_and(|theme| theme.has_icon(icon) || theme.has_icon(&format!("{icon}-symbolic")))
}

/// Gives an application item the desktop entry behind it: its canonical id (what
/// keeps a pinned launcher and the window it opens one icon) and, when the
/// configured icon is not in the theme, the entry's icon.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn apply_desktop_entry_identity(item: &mut DockItem) {
    let entry = resolve_desktop_entry(item.exec, item.icon, item.app_id);
    if entry.is_null() {
        return;
    }
    let entry = &*entry;

    if !entry.stem.is_null() {
        item.desktop_id = crate::glib::strdup_string(&crate::cstr::text_or_empty(entry.stem));
    }

    // A preset may name "pavucontrol" while the icon lives under
    // "org.pulseaudio.pavucontrol".
    let Some(entry_icon) = crate::cstr::non_empty_text(entry.icon) else {
        return;
    };

    let theme = crate::ui::icon_theme();
    let configured = crate::cstr::text(item.icon);

    if icon_is_resolvable(theme.as_ref(), configured.as_deref())
        || !icon_is_resolvable(theme.as_ref(), Some(&entry_icon))
    {
        return;
    }

    log_info!(
        "UI",
        "Icon '{}' is not in the icon theme; using '{entry_icon}' from {}",
        configured.as_deref().unwrap_or(""),
        crate::cstr::text_or_empty(entry.id)
    );

    crate::glib::g_free(item.icon.cast());
    item.icon = crate::glib::strdup_string(&entry_icon);
}

// ============================================================================
// THE CONSTRUCTORS
// ============================================================================

/// Creates an item with its widgets. Nothing is shown until `mount` has run.
///
/// # Safety
///
/// `item` must be a live `DockItem` whose kind shows an icon; `image` is the
/// artwork, or null when the icon did not load.
unsafe fn build_icon_content(item: &mut DockItem, image: *mut gtk4::ffi::GtkWidget, spacing: i32) {
    let Some(content) = begin_content(item, spacing) else {
        return;
    };

    // Kept so an item whose artwork depends on state (the trash) can swap it.
    item.icon_image = image.cast();

    append_icon(content.as_ref(), image, icon_size(item.state.as_ref()));
    attach_reflection(item, image);
}

/// An application item: its icon and an (initially hidden) window-title label.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`; the strings are C strings.
pub unsafe fn create_dock_item(
    state: *mut LambdockState,
    name: *const c_char,
    icon: *const c_char,
    exec: *const c_char,
    app_id: *const c_char,
    is_favorite: c_int,
    hover_animate: c_int,
) -> *mut DockItem {
    crate::ui::ensure_gtk_initialized();

    let name = crate::cstr::text(name);
    let icon = crate::cstr::text(icon);
    let exec = crate::cstr::text(exec);
    let app_id = crate::cstr::text(app_id);

    // A launcher with no app id is known by its icon.
    let app_id = app_id.as_deref().or(icon.as_deref());

    let item = new_item(&Shell {
        state,
        kind: ITEM_APP,
        name: name.as_deref(),
        icon: icon.as_deref(),
        exec: exec.as_deref(),
        app_id,
        is_favorite: is_favorite != 0,
        hover_animate: hover_animate != 0,
    });

    apply_desktop_entry_identity(&mut *item);

    let Some(content) = begin_content(&mut *item, 6) else {
        return item;
    };
    let image = load_icon(state, (*item).icon);
    append_icon(content.as_ref(), image, icon_size(state.as_ref()));
    add_window_title_label(&mut *item, content.as_ref());
    attach_reflection(&mut *item, image);

    mount(item)
}

/// A dynamic item: an icon, a label the polled reading goes into, and a Scheme
/// procedure that produces it.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`; the strings are C strings.
pub unsafe fn create_dynamic_dock_item(
    state: *mut LambdockState,
    name: *const c_char,
    icon: *const c_char,
    exec: *const c_char,
    poll_fn: *mut c_void,
    interval_ms: u32,
    hover_animate: c_int,
) -> *mut DockItem {
    crate::ui::ensure_gtk_initialized();

    let name_text = crate::cstr::text(name);
    let icon_text = crate::cstr::text(icon);
    let exec_text = crate::cstr::text(exec);

    let item = new_item(&Shell {
        state,
        kind: ITEM_DYNAMIC,
        name: name_text.as_deref(),
        icon: icon_text.as_deref(),
        exec: exec_text.as_deref(),
        // Dynamic items are known by their name.
        app_id: name_text.as_deref(),
        is_favorite: true,
        hover_animate: hover_animate != 0,
    });

    // The bridge protects the procedure from the GC; this module never sees an SCM
    // value, only the handle.
    if !poll_fn.is_null() {
        (*item).poll_fn = lambdock_poll_fn_protect(poll_fn);
    }

    if let Some(content) = begin_content(&mut *item, 6) {
        let image = if (*item).icon.is_null() || *(*item).icon == 0 {
            std::ptr::null_mut()
        } else {
            load_icon(state, (*item).icon)
        };
        append_icon(content.as_ref(), image, icon_size(state.as_ref()));
        add_dynamic_label(&mut *item, content.as_ref(), name_text.as_deref().unwrap_or(""));
        attach_reflection(&mut *item, image);
    }

    mount(item);

    (*item).poll_interval = interval_ms;

    // Paint the first reading now instead of after the first interval.
    dock_item_refresh_readings(item);

    if interval_ms > 0 && dock_item_has_poller(item) != 0 {
        (*item).poll_timer_id =
            glib::ffi::g_timeout_add(interval_ms, Some(lambdock_dock_item_poll), item.cast());
    }

    dock_item_update_accessible(item);

    item
}

/// A separator: a line in the same icon-sized cell every other item's content
/// sits in, so it is centred on the row instead of hanging at its bottom edge.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`.
pub unsafe fn create_separator_dock_item(
    state: *mut LambdockState,
    orientation: c_int,
) -> *mut DockItem {
    use gtk4::glib::translate::IntoGlib;

    crate::ui::ensure_gtk_initialized();

    let item = new_item(&Shell {
        state,
        kind: ITEM_SEPARATOR,
        name: None,
        icon: None,
        exec: None,
        app_id: None,
        is_favorite: true,
        hover_animate: false,
    });
    let item_ref = &mut *item;

    let vertical = orientation == gtk4::Orientation::Vertical.into_glib();

    // A separator is a widget, not a button: it is drawn, never pressed.
    let line = gtk4::ffi::gtk_separator_new(orientation);
    item_ref.button = line.cast();

    let line_view: Borrowed<gtk4::Widget> = from_glib_borrow(line);
    line_view.add_css_class("dock-separator");
    if vertical {
        line_view.set_margin_start(SEPARATOR_MARGIN_PX);
        line_view.set_margin_end(SEPARATOR_MARGIN_PX);
    } else {
        line_view.set_margin_top(SEPARATOR_MARGIN_PX);
        line_view.set_margin_bottom(SEPARATOR_MARGIN_PX);
    }

    // Along the cell's own axis the line has to claim the spare room; across it
    // the cell is exactly as wide as the line plus its margins.
    line_view.set_vexpand(vertical);
    line_view.set_hexpand(!vertical);

    let cell = gtk4::ffi::gtk_box_new(orientation, 0);
    item_ref.cell = cell.cast();

    let cell_view: Borrowed<gtk4::Widget> = from_glib_borrow(cell);
    cell_view.set_halign(gtk4::Align::Center);
    cell_view.set_valign(gtk4::Align::Center);
    gtk4::ffi::gtk_box_append(cell_view.as_ptr().cast(), line);

    // A separator stands *on* the shelf, so it follows the slab's projection: on
    // a keystoned or tilted dock it leans with the glass instead of standing
    // upright in front of it. On a flat capsule the follower is a pass-through.
    let mut body = cell;
    if let Some(state) = state.as_ref() {
        if !state.bg_shelf.is_null() {
            body = crate::ui::shelf_follow::shelf_follow_new(cell, state.bg_shelf.cast(), state.bg_capsule.cast());
            item_ref.separator_follow = body.cast();
        }
    }

    // Nothing to mirror, but the slot still reserves the shelf's band, which is
    // what keeps the line level with the icons.
    attach_reflection(item_ref, std::ptr::null_mut());
    item_ref.container = crate::ui::shelf::shelf_wrap_item(state, body.cast(), item_ref.reflection.cast()).cast();

    if !state.is_null() {
        // After the cell exists: the size request goes to the cell, not the line.
        apply_dock_item_alignment(item);
    }

    if let Some(state) = state.as_mut() {
        if !state.box_.is_null() {
            let child = if item_ref.container.is_null() {
                item_ref.cell
            } else {
                item_ref.container
            };
            gtk4::ffi::gtk_box_append(state.box_.cast(), child.cast());
        }

        state.dock_items = glib::ffi::g_list_append(state.dock_items.cast(), item.cast()).cast();
        state.centres_dirty = 1;
    }

    attach_data(item);

    item
}

/// The shell shared by the icon-only kinds: an icon, no label, and the kind's own
/// CSS class.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`; the strings are C strings.
unsafe fn create_icon_dock_item(
    state: *mut LambdockState,
    kind: c_int,
    name: Option<&str>,
    icon: Option<&str>,
    hover_animate: bool,
) -> *mut DockItem {
    let item = new_item(&Shell {
        state,
        kind,
        name,
        icon,
        exec: Some(""),
        app_id: name,
        is_favorite: true,
        hover_animate,
    });

    let image = load_icon(state, (*item).icon);
    build_icon_content(&mut *item, image, 0);

    mount(item)
}

/// A stack: a folder shown as an icon.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`; the strings are C strings.
pub unsafe fn create_stack_dock_item(
    state: *mut LambdockState,
    name: *const c_char,
    icon: *const c_char,
    path: *const c_char,
    view: *const c_char,
    hover_animate: c_int,
) -> *mut DockItem {
    crate::ui::ensure_gtk_initialized();

    let name = crate::cstr::text(name);
    let icon = crate::cstr::non_empty_text(icon);
    let path = crate::cstr::text_or_empty(path);
    let view = crate::cstr::non_empty_text(view);

    let item = create_icon_dock_item(
        state,
        ITEM_STACK,
        name.as_deref(),
        Some(icon.as_deref().unwrap_or("folder")),
        hover_animate != 0,
    );

    (*item).stack_path = crate::glib::strdup_string(&path);
    (*item).stack_view = crate::glib::strdup_string(view.as_deref().unwrap_or("grid"));

    log_info!(
        "UI",
        "Created stack item: {} ({path}, {} view)",
        crate::cstr::text_or_empty((*item).name),
        crate::cstr::text_or_empty((*item).stack_view)
    );

    item
}

/// The launchpad: every installed application, in a grid.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`; the strings are C strings.
pub unsafe fn create_launchpad_dock_item(
    state: *mut LambdockState,
    name: *const c_char,
    icon: *const c_char,
    hover_animate: c_int,
) -> *mut DockItem {
    crate::ui::ensure_gtk_initialized();

    let name = crate::cstr::text(name);
    let icon = crate::cstr::non_empty_text(icon);

    let item = create_icon_dock_item(
        state,
        ITEM_LAUNCHPAD,
        name.as_deref(),
        Some(icon.as_deref().unwrap_or("view-app-grid-symbolic")),
        hover_animate != 0,
    );

    log_info!(
        "UI",
        "Created launchpad item: {}",
        crate::cstr::text_or_empty((*item).name)
    );

    item
}

/// The trash, which counts itself on a timer.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`; the strings are C strings.
pub unsafe fn create_trash_dock_item(
    state: *mut LambdockState,
    name: *const c_char,
    icon: *const c_char,
    hover_animate: c_int,
) -> *mut DockItem {
    crate::ui::ensure_gtk_initialized();

    let name = crate::cstr::text(name);
    let icon = crate::cstr::non_empty_text(icon);

    let item = create_icon_dock_item(
        state,
        ITEM_TRASH,
        name.as_deref(),
        Some(icon.as_deref().unwrap_or("user-trash")),
        hover_animate != 0,
    );

    // The trash is changed by every other program on the desktop, so the item
    // counts it again on a timer instead of waiting for a signal nobody sends.
    (*item).trash_timer_id = glib::ffi::g_timeout_add(
        TRASH_REFRESH_INTERVAL_MS,
        Some(lambdock_dock_item_trash_poll),
        item.cast(),
    );
    dock_item_refresh_trash(item);

    log_info!(
        "UI",
        "Created trash item: {}",
        crate::cstr::text_or_empty((*item).name)
    );
    item
}

/// Replaces a dynamic item's text form (its icon and label) with one widget that
/// carries the reading itself: a gauge, a clock face.
///
/// The label goes with it -- it would otherwise keep a stale reading next to the
/// widget -- and the reflection slot stays, cleared of artwork: the band that slot
/// reserves is what holds the row at one height.
///
/// # Safety
///
/// `item` must be a live `DockItem`; `widget` a live, unparented widget.
unsafe fn replace_dynamic_content(
    item: &mut DockItem,
    widget: *mut gtk4::ffi::GtkWidget,
) -> bool {
    if item.button.is_null() || widget.is_null() {
        return false;
    }

    let Some(button) = widget_of(item.button) else {
        return false;
    };
    let Some(button) = button.as_ref().downcast_ref::<gtk4::Button>() else {
        return false;
    };
    let Some(content) = crate::ui::button_child(button) else {
        return false;
    };
    let content_ref: &gtk4::Widget = content.as_ref();

    item.label = std::ptr::null_mut();
    while let Some(child) = crate::ui::first_child_of(content_ref) {
        gtk4::ffi::gtk_box_remove(content_ref.as_ptr().cast(), child.as_ptr().cast());
    }

    crate::ui::shelf::shelf_reflection_clear_source(item.reflection.cast());
    box_append(content_ref, widget);

    true
}

/// Turns a dynamic item into a gauge: one widget that is both the "icon" and the
/// reading, instead of an icon next to a changing string.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`.
pub unsafe fn dock_item_enable_meter(
    item: *mut DockItem,
    unit: *const c_char,
    max_value: f64,
) {
    let Some(item_ref) = item.as_mut() else {
        return;
    };
    if item_ref.button.is_null() {
        return;
    }

    let meter = crate::ui::meter::lambdock_meter_new();
    crate::ui::meter::lambdock_meter_set_icon_size(meter, icon_size(item_ref.state.as_ref()));

    if !replace_dynamic_content(item_ref, meter) {
        // Never adopted: hand the floating reference back.
        glib::gobject_ffi::g_object_ref_sink(meter.cast());
        glib::gobject_ffi::g_object_unref(meter.cast());
        return;
    }

    item_ref.meter = meter.cast();
    crate::glib::g_free(item_ref.meter_unit.cast());
    item_ref.meter_unit = crate::glib::strdup_string(&crate::cstr::text_or_empty(unit));
    item_ref.meter_max = if max_value > 0.0 { max_value } else { 100.0 };

    if let Some(button) = widget_of(item_ref.button) {
        button.add_css_class("dynamic-meter");
    }

    // Paint the first reading now instead of after the first interval. This is the
    // explicit read, not the timer callback, so it is not skipped when the dock is
    // still unmapped (startup, tests).
    dock_item_refresh_readings(item);
}

/// Turns a dynamic item into an analogue clock face.
///
/// The text form goes the same way as a meter's: a face in an icon slot reads as
/// part of the row, where a string of digits pushes everything after it out of
/// line. The item keeps its Scheme poller -- its reading is the face's tooltip and
/// accessible name ("Thu Oct 29 13:07") -- and the hands are set from the system
/// clock on every poll.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`.
pub unsafe fn dock_item_enable_clock(item: *mut DockItem) {
    let Some(item_ref) = item.as_mut() else {
        return;
    };
    if item_ref.button.is_null() || !item_ref.clock.is_null() {
        return;
    }

    let clock = crate::ui::clock::lambdock_clock_new();
    crate::ui::clock::lambdock_clock_set_icon_size(clock, icon_size(item_ref.state.as_ref()));

    if !replace_dynamic_content(item_ref, clock) {
        glib::gobject_ffi::g_object_ref_sink(clock.cast());
        glib::gobject_ffi::g_object_unref(clock.cast());
        return;
    }

    item_ref.clock = clock.cast();

    if let Some(button) = widget_of(item_ref.button) {
        button.add_css_class("dynamic-clock");
    }

    // Point the hands now instead of after the first interval, so a dock that is
    // unmapped at startup still shows the right time when it appears.
    dock_item_refresh_readings(item);
}


// ============================================================================
// THE ITEM MENU
// ============================================================================
//
// An item's popover is both its right-click menu and (for a stack) its folder
// panel, so opening it is one path: fill it, count it against the dock's popover
// slot, pop it up, and put the keyboard on its first row.

/// Pops the item's menu up.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`.
pub unsafe fn lambdock_dock_item_popup_menu(item: *mut DockItem) {
    let Some(item_ref) = item.as_ref() else {
        return;
    };
    let Some(popover) = popover_of(item_ref.popover) else {
        return;
    };
    if item_ref.state.is_null() {
        return;
    }

    crate::ui::popover_menu::populate_popover_menu(item);

    if !crate::ui::is_shown(popover.as_ref().upcast_ref()) {
        (*item_ref.state).active_popovers += 1;
    }

    gtk4::ffi::gtk_popover_popup(popover.as_ptr().cast());

    // Put the keyboard on the menu's first row, so the arrow keys work from the
    // moment it opens (a stack panel focuses its own first entry instead).
    crate::ui::popover_menu::lambdock_popover_menu_focus_first(item_ref.popover.cast());
}

/// Pops the item's menu up, or down when it is already open.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`.
pub unsafe fn lambdock_dock_item_show_menu(item: *mut DockItem) {
    let Some(item_ref) = item.as_ref() else {
        return;
    };
    let Some(popover) = popover_of(item_ref.popover) else {
        return;
    };
    if item_ref.state.is_null() {
        return;
    }

    if crate::ui::is_shown(popover.as_ref().upcast_ref()) {
        gtk4::ffi::gtk_popover_popdown(popover.as_ptr().cast());
        return;
    }

    lambdock_dock_item_popup_menu(item);
}

// ============================================================================
// TEARDOWN
// ============================================================================

/// Stops the item's timers.
///
/// `g_clear_handle_id` only removes a source that is still alive, so a timer that
/// already fired cannot make us remove an unrelated (recycled) id.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn free_item_timers(item: &mut DockItem) {
    if item.poll_timer_id != 0 {
        glib::ffi::g_source_remove(item.poll_timer_id);
        item.poll_timer_id = 0;
    }
    if item.trash_timer_id != 0 {
        glib::ffi::g_source_remove(item.trash_timer_id);
        item.trash_timer_id = 0;
    }
}

/// Releases the Scheme poller's GC protection.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn free_item_poller(item: &mut DockItem) {
    if item.poll_fn.is_null() {
        return;
    }

    lambdock_poll_fn_release(item.poll_fn);
    item.poll_fn = std::ptr::null_mut();
}

/// Unparents the item's context menu.
///
/// A popover torn down while it is still open never emits "closed", so the dock's
/// popover slot is released here; otherwise `active_popovers` stays above zero and
/// the dock can never auto-hide again. The handler is disconnected first, because
/// unparenting an open popover *does* emit "closed" -- and that would release the
/// slot a second time.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn free_item_popover(item: &mut DockItem) {
    let Some(popover) = widget_of(item.popover) else {
        return;
    };
    let popover_ref: &gtk4::Widget = popover.as_ref();

    if let Some(closed) = popover_ref.steal_data::<glib::SignalHandlerId>(CLOSED_HANDLER_KEY) {
        popover_ref.disconnect(closed);
    }

    if crate::ui::is_shown(popover_ref) {
        if let Some(state) = item.state.as_mut() {
            if state.active_popovers > 0 {
                state.active_popovers -= 1;
            }
        }
    }

    if crate::ui::parent_of(popover_ref).is_some() {
        popover_ref.unparent();
    }

    item.popover = std::ptr::null_mut();
}

/// Detaches the button from the dock, then the container that held it.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn free_item_widgets(item: &mut DockItem) {
    if let Some(button) = widget_of(item.button) {
        let button_ref: &gtk4::Widget = button.as_ref();

        // The drag machinery can hold the button after it leaves the row, so the
        // click handler goes with the item rather than waiting to be finalized.
        if let Some(clicked) = button_ref.steal_data::<glib::SignalHandlerId>(CLICKED_HANDLER_KEY) {
            button_ref.disconnect(clicked);
        }

        if let Some(parent) = crate::ui::parent_of(button_ref)
            .and_then(|parent| parent.as_ref().downcast_ref::<gtk4::Box>().map(|parent| parent.as_ptr()))
        {
            gtk4::ffi::gtk_box_remove(parent, button_ref.as_ptr().cast());
        }
    }
    item.button = std::ptr::null_mut();

    // The reflection (if any) is owned by the container and dies with it, so the
    // pointer is dropped before the container goes.
    item.reflection = std::ptr::null_mut();

    let Some(container) = widget_of(item.container) else {
        return;
    };
    let container_ref: &gtk4::Widget = container.as_ref();

    // Removing it from the box drops the box's reference; this one keeps it alive
    // until the removal has finished.
    glib::gobject_ffi::g_object_ref(container_ref.as_ptr().cast());

    if let Some(parent) = crate::ui::parent_of(container_ref)
        .and_then(|parent| parent.as_ref().downcast_ref::<gtk4::Box>().map(|parent| parent.as_ptr()))
    {
        gtk4::ffi::gtk_box_remove(parent, container_ref.as_ptr().cast());
    }

    glib::gobject_ffi::g_object_unref(container_ref.as_ptr().cast());
    item.container = std::ptr::null_mut();
}

/// Frees the item's strings.
///
/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn free_item_strings(item: &mut DockItem) {
    for pointer in [
        &mut item.name,
        &mut item.icon,
        &mut item.exec,
        &mut item.app_id,
        &mut item.stack_path,
        &mut item.stack_view,
        &mut item.meter_unit,
        &mut item.desktop_id,
        &mut item.poll_name,
    ] {
        if !pointer.is_null() {
            crate::glib::g_free((*pointer).cast());
            *pointer = std::ptr::null_mut();
        }
    }
}

/// Remembers the Scheme-side name of the item's poller.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`; `name` a C string.
pub unsafe fn dock_item_set_poll_name(item: *mut DockItem, name: *const c_char) {
    let Some(item) = item.as_mut() else {
        return;
    };

    crate::glib::g_free(item.poll_name.cast());
    item.poll_name = name.cast_mut();
}

/// Frees one item: its timers, its Scheme handle, its widgets and its strings.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`, and no longer used afterwards.
pub unsafe fn free_dock_item(item: *mut DockItem) {
    let Some(item_ref) = item.as_mut() else {
        return;
    };

    // The row is about to lose an item: every centre after it moves.
    if let Some(state) = item_ref.state.as_mut() {
        state.centres_dirty = 1;
    }

    free_item_timers(item_ref);

    // Back pointers go before anything is unparented: deferred drag callbacks may
    // still hold the widgets, and must see "no item" instead of a freed one.
    detach_data(item_ref);

    free_item_poller(item_ref);
    free_item_popover(item_ref);
    free_item_widgets(item_ref);

    crate::app_window::app_windows_free(item_ref.windows);
    item_ref.windows = std::ptr::null_mut();

    free_item_strings(item_ref);
    crate::glib::g_free(item.cast());
}

/// Frees the pinned launchers while keeping the running-window tracking alive.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`.
pub unsafe fn clear_favorite_dock_items(state: *mut LambdockState) {
    let Some(state_ref) = state.as_mut() else {
        return;
    };
    if state_ref.dock_items.is_null() {
        return;
    }

    clear_window_tracker_dock_references(state);

    let mut list = state_ref.dock_items;
    let mut link = list;

    while let Some(entry) = link.as_ref() {
        let next = entry.next;
        let item = entry.data.cast::<DockItem>();

        if item.as_ref().is_some_and(|item| item.is_favorite != 0) {
            list = glib::ffi::g_list_delete_link(list.cast(), link.cast()).cast();
            free_dock_item(item);
        }

        link = next;
    }

    state_ref.dock_items = list.cast();
}


// ============================================================================
// HOT RELOAD
// ============================================================================
//
// `reload_dock_instance` is a sequence of named stages rather than one long body:
// what a reload *is* is "re-read the settings, move the surface, rebuild the row,
// re-apply the derived state, put the dock where it belongs". Each stage below is
// one of those sentences, and every one of them belongs to the window rather than
// to an item -- which is why this is the last thing in the item module.

/// `GTK_LAYER_SHELL_EDGE_LEFT` and `..._RIGHT`, which are what make a dock
/// vertical. The other two are `EDGE_RIGHT` and `EDGE_BOTTOM` in
/// `core/config_values`.
const LAYER_EDGE_LEFT: c_int = 0;
const LAYER_EDGE_RIGHT: c_int = 1;

extern "C" {




    /// `gtk4-layer-shell`: the anchors that put the surface on an edge. The crate
    /// is a C library with no bindings here, so these are the three calls the dock
    /// makes of it.
    fn gtk_layer_is_layer_window(window: *mut gtk4::ffi::GtkWindow) -> c_int;
    fn gtk_layer_set_margin(window: *mut gtk4::ffi::GtkWindow, edge: c_int, margin: c_int);
    fn gtk_layer_set_anchor(window: *mut gtk4::ffi::GtkWindow, edge: c_int, anchor: c_int);
}

/// Mode-relevant settings, captured before a reload so it can compare afterwards.
///
/// A mode class and the dock's dimensions depend on these *and* on the row that is
/// rebuilt, so the reload only refreshes them when one of them actually moved.
#[derive(Clone, Copy)]
struct ModeSnapshot {
    taskbar: bool,
    shelf: crate::core::config_values::ShelfParams,
}

impl ModeSnapshot {
    /// # Safety
    ///
    /// `state` must be a live `LambdockState`.
    unsafe fn of(state: &LambdockState) -> Self {
        Self {
            taskbar: state.taskbar != 0,
            shelf: state.shelf,
        }
    }
}

/// Re-reads the settings CSS depends on, and reloads the stylesheets.
///
/// # Safety
///
/// `state` must be a live `LambdockState`.
unsafe fn reload_appearance(state: &mut LambdockState) -> ModeSnapshot {
    let before = ModeSnapshot::of(state);

    // One reader, before the CSS: it fills the mode flags *and* the icon size the
    // shelf's band is computed from, so the stylesheet is never built from the
    // size the previous reload happened to leave behind.
    dock_read_settings(state);
    crate::ui::theme_engine::load_custom_css(state);

    before
}

/// Applies the settings a widget takes once it exists.
///
/// # Safety
///
/// `state` must be a live `LambdockState`.
unsafe fn apply_settings(state: &mut LambdockState) {
    // A pending auto-hide timeout would fire against the layout being replaced.
    if state.hide_timer_id != 0 {
        glib::ffi::g_source_remove(state.hide_timer_id);
        state.hide_timer_id = 0;
    }

    if let Some(dock_box) = widget_of(state.box_).and_then(|dock_box| {
        dock_box
            .as_ref()
            .downcast_ref::<gtk4::Box>()
            .map(|dock_box| dock_box.as_ptr())
    }) {
        gtk4::ffi::gtk_box_set_spacing(dock_box, state.item_spacing);
    }
}

/// Moves the surface to a new screen edge, re-orienting the dock with it.
///
/// # Safety
///
/// `state` must be a live `LambdockState`.
unsafe fn apply_edge_change(state: &mut LambdockState, edge: c_int) {
    if edge == state.edge {
        return;
    }

    if state.tick_cb_id != 0 && !state.window.is_null() {
        gtk4::ffi::gtk_widget_remove_tick_callback(state.window.cast(), state.tick_cb_id);
        state.tick_cb_id = 0;
    }

    // The edge lives in layer-shell anchors; without a layer surface (X11, GNOME)
    // there is nothing to re-anchor, and every call would log a critical.
    let window = state.window.cast::<gtk4::ffi::GtkWindow>();
    if !window.is_null() && gtk_layer_is_layer_window(window) != 0 {
        gtk_layer_set_margin(window, state.edge, 0);
        gtk_layer_set_anchor(window, state.edge, 0);

        state.edge = edge;
        gtk_layer_set_anchor(window, state.edge, 1);
    } else {
        state.edge = edge;
    }

    let vertical = edge == LAYER_EDGE_LEFT || edge == LAYER_EDGE_RIGHT;
    let orientation = if vertical {
        gtk4::Orientation::Vertical
    } else {
        gtk4::Orientation::Horizontal
    };
    state.orientation = orientation.into_glib();

    // Both are orientables: the row itself and the capsule drawn behind it.
    for widget in [state.box_, state.bg_capsule] {
        if !widget.is_null() {
            gtk4::ffi::gtk_orientable_set_orientation(widget.cast(), orientation.into_glib());
        }
    }

    // A dock that just changed edges is on screen, at rest.
    state.current_margin = 0;
    state.target_margin = 0;
    state.start_margin = 0;
    state.is_hidden = 0;
}

/// Rebuilds the row from the configuration and re-syncs the tracked windows.
///
/// # Safety
///
/// `state` must be a live `LambdockState`.
unsafe fn reload_items(state: *mut LambdockState) {
    clear_favorite_dock_items(state);
    populate_dock_items_for(state);

    // Re-sync Wayland foreign window handles back to the new launchers.
    sync_window_trackers(state);

    let mut link = (*state).dock_items;
    while let Some(entry) = link.as_ref() {
        link = entry.next;

        let item = entry.data.cast::<DockItem>();
        if item.as_ref().is_none_or(|item| item.button.is_null()) {
            continue;
        }

        apply_dock_item_alignment(item);
        crate::ui::dock_item_windows::update_dock_item_status(item);
    }
}

/// Reserves space for the dock (or not) and puts it where the setting wants it.
///
/// # Safety
///
/// `state` must be a live `LambdockState`.
unsafe fn apply_auto_hide_state(state: *mut LambdockState) {
    dock_apply_exclusive_zone(state);

    let state_ref = &*state;
    let stay_visible = state_ref.auto_hide == 0
        || (state_ref.show_when_clear != 0 && state_ref.window_trackers.is_null());

    if stay_visible {
        reveal_dock(state);
    } else {
        hide_dock(state);
    }
}

/// Hot-reloads one dock: its appearance, its configuration, its CSS, its
/// auto-hide behaviour, its edge and its row -- without restarting the dock.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`.
pub unsafe fn reload_dock_instance(state: *mut LambdockState) {
    let Some(state_ref) = state.as_mut() else {
        return;
    };

    log_info!(
        "UI",
        "Hot-reloading UI layout for config: {}",
        crate::cstr::text_or_empty(state_ref.config_path)
    );

    let before = reload_appearance(state_ref);
    apply_settings(state_ref);

    let config_path = crate::cstr::cstring(&crate::cstr::text_or_empty(state_ref.config_path));
    apply_edge_change(
        state_ref,
        crate::bridge::get_position_config_for(config_path.as_ptr()),
    );
    reload_items(state);

    // Mode classes and dimensions depend on both the new flags and the new row.
    apply_dock_mode_classes(state);
    if before.taskbar != (state_ref.taskbar != 0) || before.shelf != state_ref.shelf {
        refresh_dock_mode(state);
    }

    update_dock_dimensions(state);

    // A toggle from the preferences window takes effect on the running dock.
    dock_apply_motion_preference(state);

    apply_auto_hide_state(state);
}


#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{offset_of, size_of};

    /// The numbers come from a `printf("%zu", sizeof/offsetof)` against
    /// `lambdock.h`, next to the `LambdockState` ones in `state.rs`.
    #[test]
    fn the_mirror_matches_the_header() {
        assert_eq!(size_of::<DockItem>(), 248);
        assert_eq!(offset_of!(DockItem, windows), 88);
        assert_eq!(offset_of!(DockItem, poll_fn), 104);
        assert_eq!(offset_of!(DockItem, kind), 136);
        assert_eq!(offset_of!(DockItem, meter_max), 192);
        assert_eq!(offset_of!(DockItem, show_inspector), 220);
        assert_eq!(offset_of!(DockItem, trash_full), 240);
    }

    /// `AppWindow` is mirrored in `app_window.rs`; its layout is pinned to the
    /// same measurement.
    #[test]
    fn the_window_record_matches_the_header() {
        assert_eq!(size_of::<crate::app_window::AppWindow>(), 24);
        assert_eq!(offset_of!(crate::app_window::AppWindow, activated), 20);
    }

    #[test]
    fn the_item_kinds_are_the_headers_enum() {
        assert_eq!(
            [ITEM_APP, ITEM_DYNAMIC, ITEM_SEPARATOR, ITEM_STACK, ITEM_LAUNCHPAD, ITEM_TRASH],
            [0, 1, 2, 3, 4, 5]
        );
    }
}
