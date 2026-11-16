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

pub mod app_window;
pub mod bridge;
pub mod core;
pub mod dock_registry;
pub(crate) mod glib;
pub(crate) mod guile;
pub mod logging;
pub mod ui;
