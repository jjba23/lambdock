// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! lambdock, as a crate.
//!
//! It is a `staticlib` and an `rlib`: the staticlib is what a C build of the dock
//! would link, and the rlib is what `cargo test` links the unit tests against.
//! Nothing outside the crate calls into it any more -- the port from C is
//! finished -- so the modules are ordinary Rust modules, and the `lambdock_*`
//! names that remain are history rather than exports.

pub mod accessibility;
pub mod app_window;
pub mod bridge;
pub mod core;
pub(crate) mod cstr;
pub mod dock_item;
pub mod dock_registry;
pub mod dock_window;
pub(crate) mod glib;
pub(crate) mod guile;
pub mod logging;
pub mod main_app;
pub mod state;
pub mod toplevel_tracker;
pub mod ui;
pub mod x11_tracker;
