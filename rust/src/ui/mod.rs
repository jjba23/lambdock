// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! Layers 3-4: one dock's widgets and its panels, the Rust home of `src/ui/`.
//!
//! A module here is still reached through the C header beside it, so the C
//! callers do not move; what changes is who implements the body. Widgets are
//! borrowed from C as `gtk4::Widget` and never owned here.

pub mod widget_utils;
