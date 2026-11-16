// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! One dock icon, as C sees it: `DockItem` from `src/lambdock.h`.
//!
//! Rust reads this struct through a pointer C owns -- the widget modules that
//! are still to come read an item's fields and build the widget for it -- and
//! never allocates or frees one.
//!
//! The mirror matches the header field for field, in order. A wrong offset is
//! not a compile error, it is a corrupted field two modules away, so the test at
//! the bottom pins the size and the offsets of the fields the Rust modules read,
//! taken from `offsetof` in C. When `lambdock.h` grows a field, that test is what
//! says so.

use crate::app_window::GList;
use crate::state::LambdockState;
use std::os::raw::{c_char, c_int, c_uint, c_void};

/// `LambdockItemKind`: app, dynamic, separator, stack, launchpad, trash.
pub const ITEM_APP: c_int = 0;
pub const ITEM_DYNAMIC: c_int = 1;
pub const ITEM_SEPARATOR: c_int = 2;
pub const ITEM_STACK: c_int = 3;
pub const ITEM_LAUNCHPAD: c_int = 4;
pub const ITEM_TRASH: c_int = 5;

/// The record, field for field.
#[repr(C)]
pub struct DockItem {
    pub name: *mut c_char,
    pub icon: *mut c_char,
    pub exec: *mut c_char,
    pub app_id: *mut c_char,
    pub is_favorite: c_int,
    pub button: *mut c_void,
    pub cell: *mut c_void,
    pub container: *mut c_void,
    pub reflection: *mut c_void,
    pub separator_follow: *mut c_void,
    pub popover: *mut c_void,
    pub windows: *mut GList,
    pub state: *mut LambdockState,
    /// Opaque Guile handle; only the bridge dereferences it.
    pub poll_fn: *mut c_void,
    pub poll_timer_id: c_uint,
    pub label: *mut c_void,
    pub window_title_label: c_int,
    pub hover_animate: c_int,
    pub kind: c_int,
    pub stack_path: *mut c_char,
    pub stack_view: *mut c_char,
    pub dragging: c_int,
    pub meter: *mut c_void,
    pub clock: *mut c_void,
    pub meter_unit: *mut c_char,
    pub meter_max: f64,
    pub desktop_id: *mut c_char,
    pub poll_name: *mut c_char,
    pub poll_interval: c_uint,
    pub show_inspector: c_int,
    pub icon_image: *mut c_void,
    pub trash_timer_id: c_uint,
    pub trash_count: c_uint,
    pub trash_full: c_int,
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{offset_of, size_of};

    /// The numbers come from a `printf("%zu", sizeof/offsetof)` against
    /// `lambdock.h`, next to the `LambdockState` ones in `state.rs`.
    #[test]
    fn the_mirror_matches_the_header() {
        assert_eq!(size_of::<DockItem>(), 248);
        assert_eq!(offset_of!(DockItem, windows), 88);
        assert_eq!(offset_of!(DockItem, poll_fn), 104);
        assert_eq!(offset_of!(DockItem, kind), 136);
        assert_eq!(offset_of!(DockItem, meter_max), 192);
        assert_eq!(offset_of!(DockItem, show_inspector), 220);
        assert_eq!(offset_of!(DockItem, trash_full), 240);
    }

    /// `AppWindow` is mirrored in `app_window.rs`; its layout is pinned to the
    /// same measurement.
    #[test]
    fn the_window_record_matches_the_header() {
        assert_eq!(size_of::<crate::app_window::AppWindow>(), 24);
        assert_eq!(offset_of!(crate::app_window::AppWindow, activated), 20);
    }

    #[test]
    fn the_item_kinds_are_the_headers_enum() {
        assert_eq!(
            [ITEM_APP, ITEM_DYNAMIC, ITEM_SEPARATOR, ITEM_STACK, ITEM_LAUNCHPAD, ITEM_TRASH],
            [0, 1, 2, 3, 4, 5]
        );
    }
}
