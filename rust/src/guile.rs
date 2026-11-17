// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The libguile surface the ported bridge uses, in one place.
//!
//! Guile owns its own garbage-collected heap, so nothing here is `Drop`ped by
//! Rust: an `Scm` is a word the interpreter manages, and holding one across a
//! collection is only safe while something on the Scheme side still references
//! it. Every function in `bridge.rs` converts an `Scm` to a C value immediately,
//! which is why no `Scm` is ever stored in a Rust struct.
//!
//! Two allocation domains meet here as well: `scm_to_utf8_string` and
//! `scm_to_locale_string` return `malloc`ed memory (freed with `free`), while
//! everything the bridge hands to C is `g_strdup`ed (freed with `g_free`). The
//! conversion stays local to the bridge so no caller has to remember which.

use std::os::raw::{c_char, c_int, c_void};
use std::sync::atomic::{AtomicPtr, Ordering};

/// Guile's `SCM`: an immediate value or a pointer to a heap object, opaque here.
#[repr(C)]
pub struct ScmOpaque {
    _private: [u8; 0],
}

/// A Scheme object, as C code passes it around.
pub type Scm = *mut ScmOpaque;

extern "C" {
    pub fn scm_c_public_ref(module: *const c_char, name: *const c_char) -> Scm;
    pub fn scm_c_eval_string(expression: *const c_char) -> Scm;

    pub fn scm_call_0(procedure: Scm) -> Scm;
    pub fn scm_call_1(procedure: Scm, argument: Scm) -> Scm;
    pub fn scm_call_2(procedure: Scm, first: Scm, second: Scm) -> Scm;
    pub fn scm_call_3(procedure: Scm, first: Scm, second: Scm, third: Scm) -> Scm;

    /// Calls `procedure` with a list of arguments, which is how the layout
    /// overrides are handed over.
    pub fn scm_apply_0(procedure: Scm, arguments: Scm) -> Scm;

    pub fn scm_cons(head: Scm, tail: Scm) -> Scm;
    pub fn scm_list_5(e1: Scm, e2: Scm, e3: Scm, e4: Scm, e5: Scm) -> Scm;
    pub fn scm_reverse_x(list: Scm, tail: Scm) -> Scm;

    pub fn scm_from_int64(value: i64) -> Scm;
    pub fn scm_from_double(value: f64) -> Scm;
    pub fn scm_is_number(value: Scm) -> c_int;

    pub fn scm_string_to_symbol(text: Scm) -> Scm;

    /// Keeps a procedure alive across a collection, and lets it go again. The
    /// handles are the values themselves: a protected `Scm` does not move.
    pub fn scm_gc_protect_object(value: Scm) -> Scm;
    pub fn scm_gc_unprotect_object(value: Scm) -> Scm;

    /// Runs `body` with `body_data` and, if Scheme raises, `handler` with
    /// `handler_data` -- the one way to call a procedure the dock did not write
    /// without a Scheme error unwinding through C.
    pub fn scm_internal_catch(
        tag: Scm,
        body: Option<unsafe extern "C" fn(*mut c_void) -> Scm>,
        body_data: *mut c_void,
        handler: Option<unsafe extern "C" fn(*mut c_void, Scm, Scm) -> Scm>,
        handler_data: *mut c_void,
    ) -> Scm;

    pub fn scm_from_utf8_string(text: *const c_char) -> Scm;
    pub fn scm_to_utf8_string(value: Scm) -> *mut c_char;

    pub fn scm_to_bool(value: Scm) -> c_int;

    pub fn scm_symbol_p(value: Scm) -> Scm;
    pub fn scm_symbol_to_string(symbol: Scm) -> Scm;
    pub fn scm_is_string(value: Scm) -> c_int;

    pub fn scm_is_real(value: Scm) -> c_int;
    pub fn scm_nan_p(value: Scm) -> Scm;
    pub fn scm_to_double(value: Scm) -> f64;

    pub fn scm_procedure_p(value: Scm) -> Scm;

    pub fn scm_is_pair(value: Scm) -> c_int;
    pub fn scm_car(pair: Scm) -> Scm;
    pub fn scm_cdr(pair: Scm) -> Scm;

    /// Frees what `scm_to_utf8_string` hands back.
    pub fn free(memory: *mut c_void);
}

/// `#f`, the only false value.
///
/// Guile spells truthiness as `scm_is_true`, a macro over `SCM_BOOL_F`, and
/// neither the macro nor that constant crosses the FFI: the value is asked of the
/// interpreter once and then compared word for word. It is an immediate, so it
/// does not move and the collector has nothing to trace.
pub fn false_value() -> Scm {
    static FALSE: AtomicPtr<ScmOpaque> = AtomicPtr::new(std::ptr::null_mut());

    let cached = FALSE.load(Ordering::Relaxed);
    if !cached.is_null() {
        return cached;
    }

    let value = unsafe { scm_c_eval_string(c"#f".as_ptr()) };
    FALSE.store(value, Ordering::Relaxed);
    value
}

/// Whether a Scheme value counts as true: everything except `#f`.
///
/// # Safety
///
/// Guile must be initialized, as it must be for every other call here.
pub fn is_true(value: Scm) -> bool {
    value != false_value()
}

/// `#t`, asked of the interpreter once for the same reason as `#f`.
pub fn true_value() -> Scm {
    static TRUE: AtomicPtr<ScmOpaque> = AtomicPtr::new(std::ptr::null_mut());

    let cached = TRUE.load(Ordering::Relaxed);
    if !cached.is_null() {
        return cached;
    }

    let value = unsafe { scm_c_eval_string(c"#t".as_ptr()) };
    TRUE.store(value, Ordering::Relaxed);
    value
}

/// Whether a Scheme value is a symbol.
///
/// # Safety
///
/// Guile must be initialized.
pub unsafe fn is_symbol(value: Scm) -> bool {
    is_true(scm_symbol_p(value))
}
