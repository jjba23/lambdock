// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The Rust half of lambdock.
//!
//! The crate is a `staticlib` so meson can link it into the C executable and an
//! `rlib` so `cargo test` can link the unit tests. Each module exports the C ABI
//! that its header under `src/` declares, which is what lets the port go one
//! module at a time: from C's point of view nothing moved.

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
