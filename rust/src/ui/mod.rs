// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! Layers 3-4: one dock's widgets and its panels, the Rust home of `src/ui/`.
//!
//! A module here is still reached through the C header beside it, so the C
//! callers do not move; what changes is who implements the body. Widgets are
//! borrowed from C as `gtk4::Widget` and never owned here.

pub mod icon_resolver;
pub mod icon_zoom;
pub mod meter;
pub mod poof;
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
