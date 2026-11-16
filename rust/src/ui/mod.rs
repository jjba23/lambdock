// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! Layers 3-4: one dock's widgets and its panels, the Rust home of `src/ui/`.
//!
//! A module here is still reached through the C header beside it, so the C
//! callers do not move; what changes is who implements the body. Widgets are
//! borrowed from C as `gtk4::Widget` and never owned here.

use gtk4::glib::translate::Borrowed;

pub mod app_actions;
pub mod clock;
pub mod desktop_entry;
pub mod dock_dnd;
pub mod dock_item_windows;
pub mod icon_resolver;
pub mod icon_zoom;
pub mod meter;
pub mod poof;
pub mod popover_menu;
pub mod shelf;
pub mod shelf_follow;
pub mod stack;
pub mod theme_engine;
pub mod trash;
pub mod widget_utils;

/// Makes GTK initialized *as far as gtk4-rs is concerned*.
///
/// gtk-rs keeps its own flag and sets it only when it runs `gtk_init` itself;
/// `main.c` initializes GTK through C, so the first Rust call that reaches an API
/// carrying `assert_initialized_main_thread!` would abort with "GTK has not been
/// initialized". `gtk4::init()` notices GTK is already up, adopts it, and is a
/// cheap check afterwards, so it is safe to call at every entry point.
///
/// Main thread only, like everything else that touches a widget.
pub(crate) fn ensure_gtk_initialized() {
    let _ = gtk4::init();
}

/// A widget borrowed from C for the length of one call.
///
/// `from_glib_borrow`, never `from_glib_none`: the latter sinks a floating
/// reference and unrefs on drop, so it would take the caller's widget away --
/// a widget C still holds but has not sunk yet (a box built and not yet
/// appended) comes back freed. A borrow touches neither the count nor the flag.
pub(crate) unsafe fn widget_from_pointer(
    pointer: *mut gtk4::ffi::GtkWidget,
) -> Borrowed<gtk4::Widget> {
    use gtk4::glib::translate::from_glib_borrow;

    from_glib_borrow(pointer)
}
