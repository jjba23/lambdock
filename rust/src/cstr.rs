// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The C string boundary, in one place.
//!
//! Every ported module reads C strings and each one used to carry its own
//! `text()`: five spellings of "null is the empty string", three of "null is
//! `None`", two of a translated message. They disagreed in the one way that
//! matters -- whether a missing string is absent or empty -- so here there is one
//! function per answer, named after the answer:
//!
//! - [`text`] is `Option`: a null pointer is absent, and an empty string is still
//!   a string. Use it when the difference can matter.
//! - [`non_empty_text`] is `Option` too, but treats an empty string as absent,
//!   which is what a blank name or path has always meant at these call sites.
//! - [`text_or_empty`] is a `String`: for a place where null has always meant
//!   "nothing to show" and an `Option` would only add a branch.
//! - [`translated`] / [`translated_dup`] are `gettext` messages, copied into Rust
//!   or into GLib's allocator for a `(transfer full)` return.
//!
//! What does *not* happen here is allocation in Rust's allocator: a string that
//! leaves the process is GLib's, because whoever receives it frees it with
//! `g_free` ([`crate::glib::strdup_string`] and `g_strdup_printf`).

use crate::glib::tr;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;

/// `pointer` as text, or `None` when it is null.
///
/// An empty string is a string: use [`non_empty_text`] when it is not.
///
/// # Safety
///
/// `pointer` must be null or NUL-terminated.
pub(crate) unsafe fn text(pointer: *const c_char) -> Option<String> {
    if pointer.is_null() {
        return None;
    }

    Some(CStr::from_ptr(pointer).to_string_lossy().into_owned())
}

/// `pointer` as text, or `None` when it is null or empty.
///
/// # Safety
///
/// `pointer` must be null or NUL-terminated.
pub(crate) unsafe fn non_empty_text(pointer: *const c_char) -> Option<String> {
    text(pointer).filter(|text| !text.is_empty())
}

/// `pointer` as text, or the empty string when it is null.
///
/// # Safety
///
/// `pointer` must be null or NUL-terminated.
pub(crate) unsafe fn text_or_empty(pointer: *const c_char) -> String {
    text(pointer).unwrap_or_default()
}

/// `pointer` as a borrowed `CStr`, or `None` when it is null: for a caller that
/// already has the text and only needs to read it.
///
/// # Safety
///
/// `pointer` must be null or NUL-terminated, and must outlive the answer.
pub(crate) unsafe fn borrowed<'a>(pointer: *const c_char) -> Option<&'a CStr> {
    pointer.as_ref().map(|pointer| CStr::from_ptr(pointer))
}

/// A `CString` of `text`, for the C calls that only read it.
///
/// An interior NUL truncates rather than fails: it is a log line or a file name,
/// not a protocol message, and `crate::glib::c_string` is the one place that
/// decides so.
pub(crate) fn cstring(text: &str) -> CString {
    crate::glib::c_string(text)
}

/// Translate `msgid`, whose literal stays at the call site, into Rust.
///
/// Safe although `gettext` is `unsafe` to declare: a `&CStr` is already a valid
/// string, and the result is either the static message table or memory gettext
/// owns -- never something the caller has to free.
pub(crate) fn translated(msgid: &CStr) -> String {
    // SAFETY: the message table answers with a static string, which is what
    // `CStr::from_ptr` reads here; nothing is written through that pointer.
    unsafe { CStr::from_ptr(tr(msgid)) }
        .to_string_lossy()
        .into_owned()
}

/// Translate `msgid` into a GLib-allocated string, for a `(transfer full)` return
/// the receiver frees with `g_free`.
///
/// Safe for the same reason as [`translated`]: the string is copied into GLib's
/// allocator here, and the receiver owns that copy.
pub(crate) fn translated_dup(msgid: &CStr) -> *mut c_char {
    let message = tr(msgid);
    if message.is_null() {
        return std::ptr::null_mut();
    }

    crate::glib::dup_c_str(unsafe { CStr::from_ptr(message) })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The three answers to "what is a null string?".
    #[test]
    fn null_empty_and_text_are_three_different_answers() {
        unsafe {
            assert_eq!(text(std::ptr::null()), None);
            assert_eq!(non_empty_text(std::ptr::null()), None);
            assert_eq!(text_or_empty(std::ptr::null()), "");

            // An empty string is a string, unless the caller asked for a value.
            assert_eq!(text(c"".as_ptr()).as_deref(), Some(""));
            assert_eq!(non_empty_text(c"".as_ptr()), None);
            assert_eq!(text_or_empty(c"".as_ptr()), "");

            assert_eq!(non_empty_text(c"Downloads".as_ptr()).as_deref(), Some("Downloads"));
            assert_eq!(text_or_empty(c"Downloads".as_ptr()), "Downloads");
        }
    }

    /// A translated literal degrades to itself when no catalogue is loaded, which
    /// is what makes these assertions work in a test binary.
    #[test]
    fn a_translation_of_a_known_literal_is_never_empty() {
        assert!(!translated(c"Back").is_empty());

        let owned = translated_dup(c"Back");
        assert!(!owned.is_null());
        unsafe {
            assert!(!CStr::from_ptr(owned).to_bytes().is_empty());
            crate::glib::g_free(owned.cast());
        }
    }

    /// A string with an interior NUL is truncated, not a failure: it is a log
    /// line or a file name, never a protocol message.
    #[test]
    fn an_interior_nul_truncates_the_c_string() {
        assert_eq!(cstring("two\0halves").to_bytes(), b"two");
    }
}
