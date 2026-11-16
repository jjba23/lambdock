// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! Layer 1: decisions from plain values, the Rust home of `src/core/`.
//!
//! A module here answers "given these values, what should happen?" and never
//! touches a widget, a display or the Guile runtime, so its tests are ordinary
//! Rust tests and need nothing but the compiler.

pub mod poll_budget;
