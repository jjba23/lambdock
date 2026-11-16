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
use crate::core::item_text::{
    lambdock_item_text_app, lambdock_item_text_clear, lambdock_item_text_dynamic,
    lambdock_item_text_launchpad, lambdock_item_text_stack, lambdock_item_text_trash, ItemText,
};
use crate::log_warn;
use crate::state::LambdockState;
use gtk4::gio;
use gtk4::glib;
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
