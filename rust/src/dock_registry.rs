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
//! is why the state below is a thread-local `Vec` and not behind a lock.

use std::cell::RefCell;
use std::os::raw::{c_int, c_uint, c_void};

pub use crate::state::LambdockState;

/// GLib's array of pointers, which `dock_registry_snapshot` hands to C.
#[repr(C)]
pub struct GPtrArray {
    _private: [u8; 0],
}

extern "C" {
    fn g_ptr_array_sized_new(reserved_size: c_uint) -> *mut GPtrArray;
    fn g_ptr_array_add(array: *mut GPtrArray, data: *mut c_void);
}

// The registry's whole state: the live docks, in registration order.
//
// A `static mut` was the C-shaped answer, and it is the one thing in this file
// that had to go: two `&mut` handed out from a `static mut` are aliasing the
// compiler is allowed to assume never happens. A thread-local `RefCell` says
// what the contract always said -- one thread, one owner at a time -- and a
// visitor that re-enters the registry panics instead of corrupting it.
thread_local! {
    static DOCKS: RefCell<Vec<*mut LambdockState>> = const { RefCell::new(Vec::new()) };
}

/// Registers a dock; registering the same one twice is a no-op.
///
/// # Safety
///
/// `state` must be null or a live dock.
pub unsafe fn dock_registry_add(state: *mut LambdockState) {
    if state.is_null() || dock_registry_contains(state) != 0 {
        return;
    }

    DOCKS.with_borrow_mut(|docks| docks.push(state));
}

/// Removes a dock if it is registered; removing an unknown one is a no-op, so
/// teardown paths can call it unconditionally.
///
/// # Safety
///
/// `state` must be null or a dock that was registered and is still alive.
pub unsafe fn dock_registry_remove(state: *mut LambdockState) {
    if state.is_null() {
        return;
    }

    DOCKS.with_borrow_mut(|docks| {
        if let Some(index) = docks.iter().position(|registered| *registered == state) {
            docks.remove(index);
        }
    });
}

/// Whether this dock is registered.
///
/// # Safety
///
/// `state` must be null or a live dock.
pub unsafe fn dock_registry_contains(state: *const LambdockState) -> c_int {
    if state.is_null() {
        return 0;
    }

    DOCKS.with_borrow(|docks| {
        docks
            .iter()
            .any(|registered| std::ptr::eq(*registered as *const LambdockState, state))
            as c_int
    })
}

/// How many docks are registered right now.
pub fn dock_registry_count() -> c_uint {
    DOCKS.with_borrow(|docks| docks.len() as c_uint)
}

/// Whether the registry is empty.
pub fn dock_registry_is_empty() -> c_int {
    DOCKS.with_borrow(|docks| docks.is_empty() as c_int)
}

/// A new array of borrowed docks, in registration order.
///
/// The caller owns the array (unref it) but not the docks.
pub fn dock_registry_snapshot() -> *mut GPtrArray {
    DOCKS.with_borrow(|docks| {
        let states = unsafe { g_ptr_array_sized_new(docks.len() as c_uint) };

        for state in docks.iter() {
            unsafe { g_ptr_array_add(states, (*state).cast()) };
        }

        states
    })
}

/// Calls `visit` once per registered dock, in registration order.
///
/// The walk is over a snapshot, so a visitor may register or remove docks --
/// including its own -- without disturbing it. Taking the copy also ends the
/// borrow before the first visitor runs, which is what keeps re-entry a normal
/// call rather than a panic.
///
/// # Safety
///
/// `visit` must be null or a valid C callback for the duration of the call.
pub unsafe fn dock_registry_foreach(
    visit: Option<unsafe fn(*mut LambdockState, *mut c_void)>,
    user_data: *mut c_void,
) {
    let Some(visit) = visit else {
        return;
    };

    let snapshot: Vec<*mut LambdockState> = DOCKS.with_borrow(std::clone::Clone::clone);

    for state in snapshot {
        visit(state, user_data);
    }
}

/// Forgets every dock without touching the states themselves.
pub fn dock_registry_clear() {
    DOCKS.with_borrow_mut(Vec::clear);
}
