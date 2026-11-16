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
use std::os::raw::{c_char, c_ulong, c_void};

extern "C" {
    fn gettext(msgid: *const c_char) -> *mut c_char;
    fn ngettext(singular: *const c_char, plural: *const c_char, count: c_ulong) -> *mut c_char;

    pub fn g_strdup(text: *const c_char) -> *mut c_char;
    pub fn g_strdup_printf(format: *const c_char, ...) -> *mut c_char;
    pub fn g_free(memory: *mut c_void);
    pub fn g_malloc0(size: usize) -> *mut c_void;

    /// The XDG data directories, which the icon search path is built from.
    pub fn g_get_system_data_dirs() -> *const *const c_char;
    pub fn g_get_home_dir() -> *const c_char;
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
/// # Safety
///
/// `msgid` is a valid C string by construction; the result is owned by gettext
/// or by the static message table.
pub(crate) unsafe fn tr(msgid: &CStr) -> *mut c_char {
    gettext(msgid.as_ptr())
}

/// The plural form of [`tr`], chosen by the locale's plural rules.
///
/// # Safety
///
/// The result is owned by gettext or by the static message table.
pub(crate) unsafe fn trn(singular: &CStr, plural: &CStr, count: u32) -> *mut c_char {
    ngettext(singular.as_ptr(), plural.as_ptr(), c_ulong::from(count))
}
