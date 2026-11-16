// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The dock registry: `src/dock_registry.c` in Rust.
//!
//! Several `LambdockState` instances live at once -- one dock per monitor per
//! settings file -- and everything that has to act on "all the docks" used to
//! walk a process-wide `GList` directly, which meant any caller could mutate the
//! list mid-iteration. This module is the only owner of that list: callers get a
//! snapshot, so destroying a dock inside a loop cannot corrupt the walk.
//!
//! Like the rest of the GTK code it is touched from the main thread only, which
//! is why the state below is a plain `Vec` and not behind a lock.

use std::os::raw::{c_int, c_uint, c_void};

/// A dock instance, opaque here: the registry stores and returns pointers and
/// never looks inside.
#[repr(C)]
pub struct LambdockState {
    _private: [u8; 0],
}

/// GLib's array of pointers, which `dock_registry_snapshot` hands to C.
#[repr(C)]
pub struct GPtrArray {
    _private: [u8; 0],
}

extern "C" {
    fn g_ptr_array_sized_new(reserved_size: c_uint) -> *mut GPtrArray;
    fn g_ptr_array_add(array: *mut GPtrArray, data: *mut c_void);
}

/// The registry's whole state: the live docks, in registration order, borrowed.
static mut DOCKS: Vec<*mut LambdockState> = Vec::new();

/// The list itself.
///
/// `addr_of_mut!` rather than `&mut DOCKS`: taking a reference to a `static mut`
/// is what the `static_mut_refs` lint warns about, and the registry is
/// main-thread-only by contract, so there is no second caller to alias with.
fn docks() -> &'static mut Vec<*mut LambdockState> {
    unsafe { &mut *std::ptr::addr_of_mut!(DOCKS) }
}

/// Registers a dock; registering the same one twice is a no-op.
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn dock_registry_add(state: *mut LambdockState) {
    if state.is_null() || dock_registry_contains(state) != 0 {
        return;
    }

    docks().push(state);
}

/// Removes a dock if it is registered; removing an unknown one is a no-op, so
/// teardown paths can call it unconditionally.
///
/// # Safety
///
/// `state` must be null or a dock that was registered and is still alive.
#[no_mangle]
pub unsafe extern "C" fn dock_registry_remove(state: *mut LambdockState) {
    if state.is_null() {
        return;
    }

    let docks = docks();
    if let Some(index) = docks.iter().position(|registered| *registered == state) {
        docks.remove(index);
    }
}

/// Whether this dock is registered.
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn dock_registry_contains(state: *const LambdockState) -> c_int {
    if state.is_null() {
        return 0;
    }

    docks().iter().any(|registered| *registered as *const LambdockState == state) as c_int
}

/// How many docks are registered right now.
#[no_mangle]
pub extern "C" fn dock_registry_count() -> c_uint {
    docks().len() as c_uint
}

/// Whether the registry is empty.
#[no_mangle]
pub extern "C" fn dock_registry_is_empty() -> c_int {
    docks().is_empty() as c_int
}

/// A new array of borrowed docks, in registration order.
///
/// The caller owns the array (unref it) but not the docks.
#[no_mangle]
pub extern "C" fn dock_registry_snapshot() -> *mut GPtrArray {
    let docks = docks();
    let states = unsafe { g_ptr_array_sized_new(docks.len() as c_uint) };

    for state in docks.iter() {
        unsafe { g_ptr_array_add(states, (*state).cast()) };
    }

    states
}

/// Calls `visit` once per registered dock, in registration order.
///
/// The walk is over a snapshot, so a visitor may register or remove docks --
/// including its own -- without disturbing it.
///
/// # Safety
///
/// `visit` must be null or a valid C callback for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn dock_registry_foreach(
    visit: Option<unsafe extern "C" fn(*mut LambdockState, *mut c_void)>,
    user_data: *mut c_void,
) {
    let Some(visit) = visit else {
        return;
    };

    // A copy, not the list itself: that is the whole point of the snapshot.
    let snapshot: Vec<*mut LambdockState> = docks().clone();

    for state in snapshot {
        visit(state, user_data);
    }
}

/// Forgets every dock without touching the states themselves.
#[no_mangle]
pub extern "C" fn dock_registry_clear() {
    docks().clear();
}
