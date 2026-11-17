// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The GLib and libc surface the ported modules use, in one place.
//!
//! Two contracts come from C and have to keep working across the seam:
//!
//! - A `(transfer full)` string is allocated with GLib's allocator, because the C
//!   callers free it with `g_free` and `lambdock_item_text_clear`. A `cargo test`
//!   binary links on its own, so `build.rs` asks pkg-config where GLib is.
//! - A translated message with substitutions is formatted by `g_strdup_printf`,
//!   so a translation may reorder its placeholders -- something Rust's `format!`
//!   cannot express.

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
