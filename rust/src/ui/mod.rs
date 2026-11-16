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

/// Whether `widget` is shown, on its own.
///
/// gtk-rs's `WidgetExt::is_visible()` is `gtk_widget_is_visible()`, which also
/// asks every ancestor -- and a popover is nobody's child, so an open menu
/// answers "no" there. This is the widget's own flag, which is what the C read
/// with `gtk_widget_get_visible()`, and what decides whether there is a panel to
/// close or a menu to redraw.
pub(crate) fn is_shown(widget: &gtk4::Widget) -> bool {
    use gtk4::glib::translate::{from_glib, ToGlibPtr};

    unsafe { from_glib(gtk4::ffi::gtk_widget_get_visible(widget.to_glib_none().0)) }
}

/// Whether `widget` is mapped, on its own flag.
///
/// The same distinction as [`is_shown`]: `WidgetExt::is_mapped()` also asks the
/// ancestors. For the toplevel a dock polls for, both answers agree -- but a
/// helper that says which one it means is a helper that cannot become a bug when
/// somebody passes a child widget.
pub(crate) fn is_mapped(widget: &gtk4::Widget) -> bool {
    use gtk4::glib::translate::{from_glib, ToGlibPtr};

    unsafe { from_glib(gtk4::ffi::gtk_widget_get_mapped(widget.to_glib_none().0)) }
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
