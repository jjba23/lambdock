// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The executable, and nothing else.
//!
//! `main` exists so the Rust runtime is started before any of the dock's code
//! runs; what it starts is `main_app::run`, which sets the locale and the
//! translation domain up, boots Guile, and hands the process to GTK.

fn main() -> std::process::ExitCode {
    std::process::ExitCode::from(lambdock::main_app::run())
}
