// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! Layers 3-4: one dock's widgets and its panels, the Rust home of `src/ui/`.
//!
//! A module here is still reached through the C header beside it, so the C
//! callers do not move; what changes is who implements the body. Widgets are
//! borrowed from C as `gtk4::Widget` and never owned here.

use gtk4::glib::translate::{from_glib_borrow, Borrowed};
use gtk4::prelude::ObjectExt;

/// `DEFAULT_ICON_SIZE` in `lambdock.h`: the icon size a dock lays out at when its
/// settings do not say otherwise, and the answer for a dock that is halfway built.
pub(crate) const DEFAULT_ICON_SIZE: i32 = 48;

pub mod app_actions;
pub mod clock;
pub mod desktop_entry;
pub mod dock_dnd;
pub mod dock_item_windows;
pub mod icon_resolver;
pub mod icon_zoom;
pub mod launchpad;
pub mod layer_shell;
pub mod meter;
pub mod poof;
pub mod popover_menu;
pub mod preferences;
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

/// The icon theme of the default display, or `None` while it is starting up or
/// when there is no display at all.
pub(crate) fn icon_theme() -> Option<gtk4::IconTheme> {
    gtk4::gdk::Display::default().map(|display| gtk4::IconTheme::for_display(&display))
}

/// A widget's parent, borrowed rather than owned.
///
/// gtk-rs's `WidgetExt::parent()` returns an *owned* wrapper, which goes through
/// `from_glib_none`: that sinks a floating reference and releases it when the
/// wrapper drops, so asking a floating widget for its parent destroys it. A box a
/// constructor has built but not appended yet is exactly that, and the dock's own
/// row is a widget somebody may not have sunk yet -- which is how it disappeared
/// in a test before this existed.
pub(crate) fn parent_of(widget: &gtk4::Widget) -> Option<Borrowed<gtk4::Widget>> {
    use gtk4::glib::translate::ToGlibPtr;

    borrow_widget(unsafe { gtk4::ffi::gtk_widget_get_parent(widget.to_glib_none().0) })
}

/// The first child of `widget`, borrowed. See [`parent_of`].
pub(crate) fn first_child_of(widget: &gtk4::Widget) -> Option<Borrowed<gtk4::Widget>> {
    use gtk4::glib::translate::ToGlibPtr;

    borrow_widget(unsafe { gtk4::ffi::gtk_widget_get_first_child(widget.to_glib_none().0) })
}

/// The next sibling of `widget`, borrowed. See [`parent_of`].
pub(crate) fn next_sibling_of(widget: &gtk4::Widget) -> Option<Borrowed<gtk4::Widget>> {
    use gtk4::glib::translate::ToGlibPtr;

    borrow_widget(unsafe { gtk4::ffi::gtk_widget_get_next_sibling(widget.to_glib_none().0) })
}

/// The child of `button`, borrowed: `gtk_button_get_child`.
pub(crate) fn button_child(button: &gtk4::Button) -> Option<Borrowed<gtk4::Widget>> {
    use gtk4::glib::translate::ToGlibPtr;

    borrow_widget(unsafe { gtk4::ffi::gtk_button_get_child(button.to_glib_none().0) })
}/// A widget pointer from C, borrowed and never owned.
fn borrow_widget(pointer: *mut gtk4::ffi::GtkWidget) -> Option<Borrowed<gtk4::Widget>> {
    if pointer.is_null() {
        return None;
    }

    Some(unsafe { from_glib_borrow(pointer) })
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

/// The child of a flow box's wrapper, borrowed: `gtk_flow_box_child_get_child`.
///
/// A flow box wraps whatever it is given, so a tile is either the wrapper or the
/// one widget inside it. See [`parent_of`] for why this is borrowed.
pub(crate) fn flow_box_child(wrapper: &gtk4::Widget) -> Option<Borrowed<gtk4::Widget>> {
    use gtk4::glib::translate::ToGlibPtr;

    if !wrapper.is::<gtk4::FlowBoxChild>() {
        return None;
    }

    let pointer: *mut gtk4::ffi::GtkWidget = wrapper.to_glib_none().0;
    let wrapper = pointer.cast::<gtk4::ffi::GtkFlowBoxChild>();

    borrow_widget(unsafe { gtk4::ffi::gtk_flow_box_child_get_child(wrapper) })
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
