// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The GLib and libc surface the ported modules use, in one place.
//!
//! Two contracts live here:
//!
//! - A string that leaves the process is allocated with GLib's allocator, because
//!   whoever receives it frees it with `g_free` -- GTK's own caches do. Inside the
//!   crate, strings are Rust's own (`String`), and this module is where the two
//!   meet: `take_printf` frees what GLib allocated, `read_c_str` copies what it
//!   only borrowed.
//! - A translated message with substitutions is formatted by `g_strdup_printf`, so
//!   a translation may reorder its placeholders -- something Rust's `format!`
//!   cannot express.
//!
//! `cargo test` links against GLib on its own, so `build.rs` asks pkg-config where
//! it is.

use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_ulong, c_void};

extern "C" {
    fn gettext(msgid: *const c_char) -> *mut c_char;
    fn ngettext(singular: *const c_char, plural: *const c_char, count: c_ulong) -> *mut c_char;

    pub fn g_strdup(text: *const c_char) -> *mut c_char;
    pub fn g_strdup_printf(format: *const c_char, ...) -> *mut c_char;
    pub fn g_free(memory: *mut c_void);
    pub fn g_malloc0(size: usize) -> *mut c_void;

    /// Compares two C strings, with NULL equal to NULL.
    pub fn g_strcmp0(left: *const c_char, right: *const c_char) -> c_int;

    /// The XDG data directories, which the icon search path is built from.
    pub fn g_get_system_data_dirs() -> *const *const c_char;
    pub fn g_get_home_dir() -> *const c_char;
    pub fn g_strfreev(strings: *mut *mut c_char);

    /// The `g_print`/`g_log` the C logger macros use, so the Rust half writes the
    /// same lines through the same doors. `g_warning` and `g_critical` are
    /// macros over `g_log`, so the level is passed explicitly.
    pub fn g_print(format: *const c_char, ...);
    pub fn g_log(domain: *const c_char, level: c_int, format: *const c_char, ...);
}

/// A Rust string as a C string, cut at an interior NUL.
///
/// `CString::new` refuses an interior NUL outright, and does not say which half
/// it kept; `g_strdup` stops at it. A log line or a CSS class that somehow
/// carries a NUL should come out truncated, as `g_strdup` would have, rather than
/// disappear entirely.
pub(crate) fn c_string(text: &str) -> std::ffi::CString {
    let whole = text.split_once('\0').map_or(text, |(head, _)| head);

    std::ffi::CString::new(whole).unwrap_or_default()
}

/// `g_strdup` of a Rust string, for a `(transfer full)` field that C frees with
/// `g_free`.
pub(crate) fn strdup_string(text: &str) -> *mut c_char {
    unsafe { g_strdup(c_string(text).as_ptr()) }
}

/// Translate `msgid`, whose literal stays at the call site.
///
/// A `&CStr` is a fat pointer, so handing one to `gettext` would shift every
/// following argument: the conversion to a plain `const char *` happens here, at
/// the FFI boundary, and never at a call site.
///
/// The wrapper is also where an extractor attaches -- `xgettext` learns the name
/// with `--keyword=tr:1` (and `trn:1,2`) once this crate is in `POTFILES.in`.
///
/// Safe to call: the argument is a `&CStr`, so it is a valid C string, and what
/// comes back is either the static message table or memory gettext owns. Nothing
/// a caller can do with the pointer makes it unsound -- only *freeing* it would,
/// which is why every function here hands it to `g_strdup` or reads it.
pub(crate) fn tr(msgid: &CStr) -> *mut c_char {
    unsafe { gettext(msgid.as_ptr()) }
}

/// The plural form of [`tr`], chosen by the locale's plural rules.
pub(crate) fn trn(singular: &CStr, plural: &CStr, count: u32) -> *mut c_char {
    unsafe { ngettext(singular.as_ptr(), plural.as_ptr(), c_ulong::from(count)) }
}

/// A copy of `text` in GLib's allocator: what a `(transfer full)` field holds.
///
/// Safe, unlike the `g_strdup` it wraps: a `&CStr` cannot be NULL or unterminated,
/// and the copy it returns belongs to the caller.
pub(crate) fn dup_c_str(text: &CStr) -> *mut c_char {
    unsafe { g_strdup(text.as_ptr()) }
}

/// Reads a GLib-allocated string into a Rust one and frees it.
///
/// # Safety
///
/// `pointer` must be null or a string this module had GLib allocate.
unsafe fn take_printf(pointer: *mut c_char) -> String {
    if pointer.is_null() {
        return String::new();
    }

    let text = CStr::from_ptr(pointer).to_string_lossy().into_owned();
    g_free(pointer.cast());

    text
}

/// Reads a string this module only borrows -- gettext's, say -- without freeing
/// it.
///
/// # Safety
///
/// `pointer` must be null or a NUL-terminated string that outlives the read.
unsafe fn read_c_str(pointer: *const c_char) -> String {
    if pointer.is_null() {
        return String::new();
    }

    CStr::from_ptr(pointer).to_string_lossy().into_owned()
}

/// [`tr`] as a `String`.
///
/// A *copy*: what `tr` returns belongs to gettext -- the message table, or memory
/// it keeps -- and freeing it is a heap corruption, which is exactly what an
/// earlier version of this function did.
pub(crate) fn translated(msgid: &CStr) -> String {
    unsafe { read_c_str(tr(msgid)) }
}

/// A plural message with its count filled in, as a `String`.
///
/// The count goes through `g_strdup_printf` rather than `format!` for the same
/// reason as the rest: a translation may reorder its placeholders.
pub(crate) fn translated_plural(singular: &CStr, plural: &CStr, count: u32) -> String {
    let format = trn(singular, plural, count);

    unsafe { take_printf(g_strdup_printf(format, count)) }
}

/// A translated `%s` message, as a `String`.
pub(crate) fn translated_with(msgid: &CStr, argument: &CStr) -> String {
    let format = tr(msgid);

    unsafe { take_printf(g_strdup_printf(format, argument.as_ptr())) }
}

/// A translated message with two `%s` placeholders, as a `String`.
pub(crate) fn translated_with_two(msgid: &CStr, first: &CStr, second: &CStr) -> String {
    let format = tr(msgid);

    unsafe { take_printf(g_strdup_printf(format, first.as_ptr(), second.as_ptr())) }
}

/// A translated message with one `%s` and one `%u`, as a `String`.
pub(crate) fn translated_with_count(msgid: &CStr, argument: &CStr, count: u32) -> String {
    let format = tr(msgid);

    unsafe { take_printf(g_strdup_printf(format, argument.as_ptr(), count)) }
}

/// A format that is *not* translated -- a joining of two values -- as a `String`.
pub(crate) fn formatted_with_two(format: &CStr, first: &CStr, second: &CStr) -> String {
    unsafe { take_printf(g_strdup_printf(format.as_ptr(), first.as_ptr(), second.as_ptr())) }
}

/// A number printed as a whole, glued to a unit, as a `String`.
pub(crate) fn formatted_number_with_unit(shown: f64, unit: &CStr) -> String {
    unsafe { take_printf(g_strdup_printf(c"%.0f%s".as_ptr(), shown, unit.as_ptr())) }
}
