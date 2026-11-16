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

use crate::app_window::{AppWindow, GList, LambdockToplevel};
use crate::core::dock_click::{lambdock_click_action_for, ClickAction};
use crate::core::config_values::{EDGE_BOTTOM, EDGE_LEFT, EDGE_RIGHT, EDGE_TOP};
use crate::core::item_text::{
    lambdock_item_text_app, lambdock_item_text_clear, lambdock_item_text_dynamic,
    lambdock_item_text_launchpad, lambdock_item_text_stack, lambdock_item_text_trash, ItemText,
};
use crate::log_warn;
use crate::state::LambdockState;
use crate::ui::DEFAULT_ICON_SIZE;
use gtk4::gdk;
use gtk4::glib;
use gtk4::glib::translate::{FromGlib, IntoGlib};
use gtk4::glib::translate::{from_glib_borrow, Borrowed};
use gtk4::prelude::*;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_uint, c_void};

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

extern "C" {
    /// `dock_item_from_widget` in `src/ui/dock_item.c`: the widget back pointer.
    fn dock_item_from_widget(widget: *mut gtk4::ffi::GtkWidget) -> *mut DockItem;

    /// `lambdock_dock_item_show_menu` in `src/ui/dock_item.c`: the toggle half of
    /// the keyboard's right click.
    fn lambdock_dock_item_show_menu(item: *mut DockItem);

    /// `dock_dnd_persist_order` in `src/guile_bridge.c`: remembers the new order.
    fn dock_dnd_persist_order(state: *mut LambdockState);

    /// `src/ui/launchpad.c`: the one launchpad, which Escape also closes.
    fn lambdock_launchpad_is_open() -> c_int;
    fn lambdock_launchpad_hide();

    /// `lambdock_launchpad_show` in `src/ui/launchpad.c`.
    fn lambdock_launchpad_show(state: *mut LambdockState);

    /// `lambdock_dock_item_popup_menu` in `src/ui/dock_item.c`: the shared "show
    /// the menu" path, which the several-windows click lands on.
    fn lambdock_dock_item_popup_menu(item: *mut DockItem);

    /// `src/toplevel_tracker.c`: what a click does to one window.
    fn activate_toplevel_window(window: *mut LambdockToplevel);
    fn minimize_toplevel_window(window: *mut LambdockToplevel);
}

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
#[no_mangle]
pub unsafe extern "C" fn dock_item_kind_to_string(kind: c_int) -> *const c_char {
    kind_ops(kind).key_name.as_ptr()
}

/// The extra CSS class of `kind`, or null when it has none.
///
/// # Safety
///
/// The result is a static string; the caller must not free it.
#[no_mangle]
pub unsafe extern "C" fn dock_item_kind_css_class(kind: c_int) -> *const c_char {
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
#[no_mangle]
pub unsafe extern "C" fn dock_item_key(item: *const DockItem) -> *mut c_char {
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
#[no_mangle]
pub unsafe extern "C" fn lambdock_item_title(
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

/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn describe_app(item: &DockItem) -> ItemText {
    lambdock_item_text_app(item.name, window_count(item))
}

/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn describe_dynamic(item: &DockItem) -> ItemText {
    // The item's tooltip already carries the latest reading, so that is the
    // value worth describing.
    let tooltip = widget_of(item.button).and_then(|button| button.tooltip_text());

    lambdock_item_text_dynamic(
        item.name,
        tooltip
            .as_ref()
            .map_or(std::ptr::null(), |tooltip| tooltip.as_ptr()),
    )
}

/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn describe_stack(item: &DockItem) -> ItemText {
    lambdock_item_text_stack(item.name)
}

/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn describe_launchpad(item: &DockItem) -> ItemText {
    lambdock_item_text_launchpad(item.name)
}

/// # Safety
///
/// `item` must be a live `DockItem`.
unsafe fn describe_trash(item: &DockItem) -> ItemText {
    lambdock_item_text_trash(item.name, item.trash_count)
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
#[no_mangle]
pub unsafe extern "C" fn dock_item_update_accessible(item: *mut DockItem) {
    let Some(item_ref) = item.as_ref() else {
        return;
    };
    if item_ref.button.is_null() || item_ref.kind == ITEM_SEPARATOR {
        return;
    }

    let Some(describe) = kind_ops(item_ref.kind).describe else {
        return;
    };

    let mut text = describe(item_ref);
    crate::accessibility::lambdock_accessible_update(
        item_ref.button.cast(),
        text.label,
        text.description,
    );
    lambdock_item_text_clear(&mut text);
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
#[no_mangle]
pub unsafe extern "C" fn dock_item_activate(item: *mut DockItem) {
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

        let name = unsafe { dock_item_kind_to_string(99) };
        assert_eq!(unsafe { CStr::from_ptr(name) }.to_str(), Ok("app"));

        // Only the kinds that need a class have one.
        assert!(unsafe { dock_item_kind_css_class(ITEM_APP) }.is_null());
        assert!(unsafe { dock_item_kind_css_class(ITEM_SEPARATOR) }.is_null());
        let class = unsafe { dock_item_kind_css_class(ITEM_STACK) };
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

            let actual = unsafe { dock_item_kind_css_class(kind) };
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
        .map_or(std::ptr::null_mut(), |state| state.dock_items.cast::<GList>())
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

        focus = widget.parent();
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
#[no_mangle]
pub unsafe extern "C" fn lambdock_dock_item_count(state: *const LambdockState) -> c_int {
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
#[no_mangle]
pub unsafe extern "C" fn lambdock_dock_item_at_index(
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
#[no_mangle]
pub unsafe extern "C" fn lambdock_dock_item_index_of(
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
#[no_mangle]
pub unsafe extern "C" fn lambdock_dock_item_focused(state: *mut LambdockState) -> *mut DockItem {
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
#[no_mangle]
pub unsafe extern "C" fn lambdock_dock_item_focus_step(
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
#[no_mangle]
pub unsafe extern "C" fn lambdock_dock_item_focus_edge(
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
    let mut link = state.dock_items.cast::<GList>();

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
#[no_mangle]
pub unsafe extern "C" fn lambdock_dock_item_move(
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
#[no_mangle]
pub unsafe extern "C" fn lambdock_dock_items_close_popovers(state: *mut LambdockState) -> c_int {
    let Some(state_ref) = state.as_ref() else {
        return 0;
    };

    let mut closed = false;
    let mut link = state_ref.dock_items.cast::<GList>();

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
#[no_mangle]
pub unsafe extern "C" fn lambdock_dock_items_handle_key(
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
#[no_mangle]
pub unsafe extern "C" fn lambdock_dock_attach_key_navigation(
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

extern "C" {
    /// `guile_bridge.h`: whether the handle is still a callable procedure.
    fn lambdock_poll_fn_is_callable(poll_fn: *mut c_void) -> c_int;

    /// `guile_bridge.h`: calls the procedure once, turning a Scheme error into
    /// `LAMBDOCK_READING_NONE`.
    fn lambdock_poll_fn_call(poll_fn: *mut c_void, out: *mut Reading) -> c_int;

    /// `src/ui/dock_item.c`: re-counts the trash and swaps its artwork.
    fn dock_item_refresh_trash(item: *mut DockItem);
}

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

    let printed_pointer =
        crate::core::item_text::lambdock_meter_reading_text(reading.number, item.meter_unit);
    let printed = crate::cstr::text_or_empty(printed_pointer);
    if !printed_pointer.is_null() {
        // `(transfer full)`: the reading's text is ours once it has been read.
        crate::glib::g_free(printed_pointer.cast());
    }

    let fraction =
        crate::core::item_text::lambdock_meter_reading_fraction(reading.number, item.meter_max);
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
#[no_mangle]
pub unsafe extern "C" fn dock_item_has_poller(item: *const DockItem) -> c_int {
    c_int::from(item.as_ref().is_some_and(|item| has_poller(item)))
}

/// Whether the dock `item` belongs to is off screen.
///
/// # Safety
///
/// `item` must be null or a live `DockItem`.
#[no_mangle]
pub unsafe extern "C" fn dock_item_off_screen(item: *const DockItem) -> c_int {
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
#[no_mangle]
pub unsafe extern "C" fn dock_item_refresh_readings(item: *mut DockItem) {
    refresh(item);
}

/// The timer behind a dynamic item.
///
/// # Safety
///
/// `user_data` must be null or a live `DockItem` that outlives the source.
#[no_mangle]
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
#[no_mangle]
pub unsafe extern "C" fn dock_items_refresh_polls(state: *mut LambdockState) {
    let Some(state_ref) = state.as_ref() else {
        return;
    };

    let mut link = state_ref.dock_items.cast::<GList>();
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
#[no_mangle]
pub unsafe extern "C" fn dock_item_icon_size(state: *const LambdockState) -> c_int {
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
unsafe fn centre_button_content(button: &gtk4::Widget) {
    let Some(button) = button.downcast_ref::<gtk4::Button>() else {
        return;
    };
    let Some(child) = button.child() else {
        return;
    };

    child.set_halign(gtk4::Align::Center);
    child.set_valign(gtk4::Align::Center);

    if !child.is::<gtk4::Box>() {
        return;
    }

    let mut widget = child.first_child();
    while let Some(each) = widget {
        each.set_valign(gtk4::Align::Center);
        widget = each.next_sibling();
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
#[no_mangle]
pub unsafe extern "C" fn apply_dock_item_alignment(item: *mut DockItem) {
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
