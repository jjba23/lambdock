// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The diagnostics the C modules write with `LOG_C_*`, for the Rust half.
//!
//! Same shape as `src/logger.h`: a dim/green/yellow/red tag, the program name,
//! and the subsystem in magenta. The one deliberate difference is the origin tag
//! -- `[R:UI]` here, `[C:UI]` from the C macros -- so a log line says which side
//! of the port emitted it.
//!
//! The two loud levels keep their C behaviour: `log_warn!` and `log_error!` go
//! through `g_warning`/`g_critical`, so `G_DEBUG=fatal-warnings` still turns them
//! into aborts, while the quiet two are `g_print`.

use crate::glib::{g_log, g_print};
use std::ffi::CString;
use std::os::raw::c_int;

/// `G_LOG_LEVEL_WARNING` and `G_LOG_LEVEL_CRITICAL` from `glib/gmessages.h`.
const LEVEL_WARNING: c_int = 1 << 4;
const LEVEL_CRITICAL: c_int = 1 << 3;

const RESET: &str = "\x1b[0m";
const MAGENTA: &str = "\x1b[35m";
pub const DIM: &str = "\x1b[2m";
pub const GREEN: &str = "\x1b[32m";
pub const YELLOW: &str = "\x1b[33m";
pub const RED: &str = "\x1b[31m";

/// Builds one line: `<colour>[LEVEL]\x1b[0m[lambdock]<magenta>[R:SUB]\x1b[0m msg`.
fn line(colour: &str, level: &str, subsystem: &str, message: &str) -> CString {
    let text = format!("{colour}[{level}]{RESET}[lambdock]{MAGENTA}[R:{subsystem}]{RESET} {message}");

    // A message carrying a NUL is a broken message, not a reason to panic.
    CString::new(text).unwrap_or_default()
}

/// Writes a quiet line (`debug!`, `info!`) to stdout.
pub fn print_line(colour: &str, level: &str, subsystem: &str, message: &str) {
    let line = line(colour, level, subsystem, message);

    // The line is an argument, never the format: a message may contain a `%` (a
    // gauge reading, a CSS selector) and that is text.
    unsafe { g_print(c"%s\n".as_ptr(), line.as_ptr()) };
}

/// Writes a `warn!` line at `G_LOG_LEVEL_WARNING`, which is what `g_warning` is.
pub fn warn_line(colour: &str, level: &str, subsystem: &str, message: &str) {
    let line = line(colour, level, subsystem, message);
    unsafe { g_log(std::ptr::null(), LEVEL_WARNING, c"%s".as_ptr(), line.as_ptr()) };
}

/// Writes an `error!` line at `G_LOG_LEVEL_CRITICAL`, which is what
/// `g_critical` is.
pub fn error_line(colour: &str, level: &str, subsystem: &str, message: &str) {
    let line = line(colour, level, subsystem, message);
    unsafe { g_log(std::ptr::null(), LEVEL_CRITICAL, c"%s".as_ptr(), line.as_ptr()) };
}

/// A diagnostic that only matters while reading the source.
#[macro_export]
macro_rules! log_debug {
    ($subsystem:expr, $($arg:tt)*) => {
        $crate::logging::print_line(
            $crate::logging::DIM,
            "DEBUG",
            $subsystem,
            &format!($($arg)*),
        )
    };
}

/// Something a user might want to see in a terminal.
#[macro_export]
macro_rules! log_info {
    ($subsystem:expr, $($arg:tt)*) => {
        $crate::logging::print_line(
            $crate::logging::GREEN,
            "INFO",
            $subsystem,
            &format!($($arg)*),
        )
    };
}

/// Something recoverable that the user should know about.
#[macro_export]
macro_rules! log_warn {
    ($subsystem:expr, $($arg:tt)*) => {
        $crate::logging::warn_line(
            $crate::logging::YELLOW,
            "WARN",
            $subsystem,
            &format!($($arg)*),
        )
    };
}

/// Something that should not have happened.
#[macro_export]
macro_rules! log_error {
    ($subsystem:expr, $($arg:tt)*) => {
        $crate::logging::error_line(
            $crate::logging::RED,
            "ERROR",
            $subsystem,
            &format!($($arg)*),
        )
    };
}
