// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! How the item menu answers the keyboard: the first half of
//! `src/ui/popover_menu.c` in Rust.
//!
//! A context menu is expected to answer the arrow keys, and to have somewhere
//! sensible to start: the focus lands on the first row when the menu opens, Up and
//! Down move it, Home and End jump, and Escape closes the popover.
//!
//! Both controllers live on the popover widget, which is also what makes them
//! survive a rebuild: a reload replaces the menu's contents, not the popover. They
//! only act on an *item menu* -- a stack panel reuses the same popover and has its
//! own arrow handling in its view, so a menu-level handler there would swallow the
//! keys it needs.
//!
//! What is still C is the menu's content: the rows, the sections (windows, app
//! actions, views, folders, removals) and their callbacks. Those move in their own
//! step.
//!
//! There are no `cargo test` cases here on purpose: every one of these decisions
//! needs a popover with rows in it and a window to hold the focus, and `cargo
//! test` stays display-free. `tests/test_popover_menu.c` drives this code through
//! the C ABI -- its `keyboard` and `escape_controller` subtests are exactly these
//! two controllers.

use gtk4::ffi;
use gtk4::gdk;
use gtk4::glib;
use gtk4::glib::object::ObjectExt;
use gtk4::glib::translate::from_glib_none;
use gtk4::prelude::*;

/// Class of the popover child a menu builds, which is how the navigation below
/// tells an item menu from a stack panel.
const MENU_CLASS: &str = "popover-menu";

/// Where the marker for "this popover already has its controllers" lives.
const ESCAPE_MARKER: &str = "lambdock-escape-close";
const KEYS_MARKER: &str = "lambdock-menu-keys";

/// Every sensitive, focusable button of the menu, in top-to-bottom order.
fn collect_rows(widget: &gtk4::Widget, out: &mut Vec<gtk4::Widget>) {
    let mut child = widget.first_child();

    while let Some(each) = child {
        if each.is::<gtk4::Button>() && each.is_sensitive() && each.is_focusable() {
            out.push(each.clone());
        }

        collect_rows(&each, out);
        child = each.next_sibling();
    }
}

/// The rows of an item menu, or none when the popover is showing something else.
fn rows(popover: &gtk4::Popover) -> Vec<gtk4::Widget> {
    let mut rows = Vec::new();

    if let Some(child) = popover
        .child()
        .filter(|child| child.has_css_class(MENU_CLASS))
    {
        collect_rows(&child, &mut rows);
    }

    rows
}

/// Index of the row that holds the focus, if any.
fn focused_row(popover: &gtk4::Popover, rows: &[gtk4::Widget]) -> Option<usize> {
    let window = popover
        .root()
        .and_then(|root| root.downcast::<gtk4::Window>().ok())?;
    let focus: gtk4::Widget = gtk4::prelude::GtkWindowExt::focus(&window)?;

    for (index, row) in rows.iter().enumerate() {
        // The focus can sit on the label inside a row's box, so the row is the
        // nearest ancestor that matches -- and the popover itself is the wall.
        let mut widget = Some(focus.clone());
        while let Some(each) = widget {
            if &each == row {
                return Some(index);
            }
            if each == *popover.upcast_ref::<gtk4::Widget>() {
                break;
            }

            widget = each.parent();
        }
    }

    None
}

/// Moves the focus to `index`, clamped into the rows.
fn focus_row(popover: &gtk4::Popover, index: usize) -> bool {
    let rows = rows(popover);

    let Some(row) = rows.get(index.min(rows.len().saturating_sub(1))) else {
        return false;
    };

    row.grab_focus();

    true
}

/// Moves the focus by `delta` rows from wherever it is, or to the end it is
/// heading for when nothing holds it yet.
fn focus_step(popover: &gtk4::Popover, delta: i32) -> bool {
    let rows = rows(popover);
    if rows.is_empty() {
        return false;
    }

    let last = rows.len() as i32 - 1;

    let next = match focused_row(popover, &rows) {
        Some(current) => (current as i32 + delta).clamp(0, last),
        None if delta > 0 => 0,
        None => last,
    };

    focus_row(popover, next as usize)
}

/// Installs Escape dismissal on the popover, once.
///
/// # Safety
///
/// `popover` must be null or a live `GtkPopover`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_popover_close_on_escape(popover: *mut ffi::GtkPopover) {
    if popover.is_null() {
        return;
    }

    // gtk-rs asserts its own initialization flag inside the controller machinery,
    // and C's gtk_init does not set it.
    crate::ui::ensure_gtk_initialized();

    let popover: gtk4::Popover = from_glib_none(popover);

    // Called once per popover, at creation: a reload rebuilds menu contents but
    // reuses the popover widget.
    if popover.data::<bool>(ESCAPE_MARKER).is_some() {
        return;
    }

    let keys = gtk4::EventControllerKey::new();
    keys.set_propagation_phase(gtk4::PropagationPhase::Capture);

    let target = popover.clone();
    keys.connect_key_pressed(move |_, keyval, _, _| {
        if keyval != gdk::Key::Escape {
            return glib::Propagation::Proceed;
        }

        if target.is_visible() {
            target.popdown();
        }

        glib::Propagation::Stop
    });

    // The marker goes on the controller as well as on the popover: a controller is
    // what a caller can look up by name (`test_popover_menu` does), and the popover
    // is what makes the install idempotent.
    keys.set_data(ESCAPE_MARKER, true);
    popover.add_controller(keys);
    popover.set_data(ESCAPE_MARKER, true);
}

/// Installs the arrow-key navigation on the popover, once.
///
/// # Safety
///
/// `popover` must be null or a live `GtkPopover`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_popover_menu_keys(popover: *mut ffi::GtkPopover) {
    if popover.is_null() {
        return;
    }

    crate::ui::ensure_gtk_initialized();

    let popover: gtk4::Popover = from_glib_none(popover);

    // Once per popover: a rebuild replaces the contents, not the widget.
    if popover.data::<bool>(KEYS_MARKER).is_some() {
        return;
    }

    let keys = gtk4::EventControllerKey::new();
    keys.set_propagation_phase(gtk4::PropagationPhase::Capture);

    let target = popover.clone();
    keys.connect_key_pressed(move |_, keyval, _, _| {
        let moved = match keyval {
            gdk::Key::Down => focus_step(&target, 1),
            gdk::Key::Up => focus_step(&target, -1),
            gdk::Key::Home => focus_row(&target, 0),
            gdk::Key::End => {
                let last = rows(&target).len().saturating_sub(1);
                focus_row(&target, last)
            }
            _ => return glib::Propagation::Proceed,
        };

        if moved {
            glib::Propagation::Stop
        } else {
            glib::Propagation::Proceed
        }
    });

    popover.add_controller(keys);
    popover.set_data(KEYS_MARKER, true);
}

/// Puts the focus on the menu's first row, if it has one.
///
/// A stack panel focuses its first entry itself, so an item menu is the only one
/// this does anything for.
///
/// # Safety
///
/// `popover` must be null or a live `GtkPopover`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_popover_menu_focus_first(popover: *mut ffi::GtkPopover) {
    if popover.is_null() {
        return;
    }

    crate::ui::ensure_gtk_initialized();

    let popover: gtk4::Popover = from_glib_none(popover);
    focus_row(&popover, 0);
}
