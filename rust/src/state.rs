// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! One dock instance, as C sees it: `LambdockState` from `src/lambdock.h`.
//!
//! Rust reads this struct through a pointer C owns; it never allocates one and
//! never frees it. The mirror has to match the header field for field, in order,
//! because a wrong offset is not a compile error -- it is a corrupted field two
//! modules away. The test at the bottom pins the size and the offsets the Rust
//! modules read, taken from `offsetof` in C; when `lambdock.h` grows a field,
//! that test is what says so.
//!
//! Widgets and other GObject pointers are `*mut c_void` here: which type each one
//! is belongs to the module that uses it, and a module that needs a widget
//! borrows it at the call site.

use crate::core::config_values::ShelfParams;
use std::os::raw::{c_char, c_int, c_uint, c_ulong, c_void};

/// The record, field for field.
#[repr(C)]
pub struct LambdockState {
    pub window: *mut c_void,
    pub overlay: *mut c_void,
    pub bg_shelf: *mut c_void,
    pub bg_capsule: *mut c_void,
    pub scrolled_window: *mut c_void,
    pub box_: *mut c_void,
    pub popover: *mut c_void,
    pub reconnect_label: *mut c_void,
    pub tracker_connected: c_int,
    pub size_group: *mut c_void,
    pub auto_hide: c_int,
    pub show_when_clear: c_int,
    pub track_windows: c_int,
    pub hover_animate: c_int,
    pub reduce_motion: c_int,
    pub taskbar: c_int,
    pub show_window_titles: c_int,
    pub shelf: ShelfParams,
    pub icon_size: c_int,
    pub item_spacing: c_int,
    pub capsule_padding: c_int,
    pub hide_timer_id: c_uint,
    pub is_hidden: c_int,
    pub theme: *mut c_char,
    pub active_theme_class: *mut c_char,
    pub config_path: *mut c_char,
    pub is_destroying: c_int,
    pub monitor: *mut c_void,
    pub edge: c_int,
    pub orientation: c_int,
    pub tick_cb_id: c_uint,
    pub anim_start_time: i64,
    pub current_margin: c_int,
    pub start_margin: c_int,
    pub target_margin: c_int,
    pub margin_anim_end_us: i64,
    pub active_popovers: c_int,
    pub pointer_tick_id: c_uint,
    pub pointer_x: f64,
    pub pointer_y: f64,
    pub applied_pointer_x: f64,
    pub applied_pointer_y: f64,
    pub pointer_applied: c_int,
    pub input_region_tick_id: c_uint,
    pub input_region_attempts: c_int,
    pub input_region_set: c_int,
    pub input_region_whole: c_int,
    pub input_region_x: c_int,
    pub input_region_y: c_int,
    pub input_region_width: c_int,
    pub input_region_height: c_int,
    pub fisheye_active: c_int,
    pub hover_headroom_px: c_int,
    pub item_centres: *mut c_void,
    pub centres_dirty: c_int,
    pub applied_hover_axis: f64,
    pub hover_axis_valid: c_int,
    pub box_origin_px: f64,
    pub last_surface_width: c_int,
    pub last_surface_height: c_int,
    pub x11_window: c_ulong,
    pub x11_placed: c_int,
    pub x11_surface: *mut c_void,
    pub x11_rect_x: c_int,
    pub x11_rect_y: c_int,
    pub x11_rect_width: c_int,
    pub x11_rect_height: c_int,
    /// The row's items: a `GList` of `DockItem *`, owned by the dock.
    pub dock_items: *mut crate::app_window::GList,
    /// The windows this dock is tracking: a `GList` of borrowed
    /// `LambdockToplevel *`, owned by the tracker.
    pub window_trackers: *mut crate::app_window::GList,
    pub base_css_provider: *mut c_void,
    pub taskbar_css_provider: *mut c_void,
    pub shelf_css_provider: *mut c_void,
    pub indicator_guard_css_provider: *mut c_void,
    pub user_css_provider: *mut c_void,
}

    /// The C offsets, measured with `offsetof` against `lambdock.h`. A wrong one is
    /// not a compile error: it is a write into the neighbouring field.
    #[test]
    fn the_state_mirror_matches_the_header() {
        use std::mem::{offset_of, size_of};

        assert_eq!(size_of::<LambdockState>(), 512);
        assert_eq!(offset_of!(LambdockState, box_), 40);
        assert_eq!(offset_of!(LambdockState, centres_dirty), 376);
        assert_eq!(offset_of!(LambdockState, orientation), 236);
        assert_eq!(offset_of!(LambdockState, is_hidden), 184);
        assert_eq!(offset_of!(LambdockState, is_destroying), 216);
        assert_eq!(offset_of!(LambdockState, dock_items), 456);
        assert_eq!(offset_of!(LambdockState, active_popovers), 280);
        assert_eq!(offset_of!(LambdockState, config_path), 208);
        assert_eq!(offset_of!(LambdockState, icon_size), 168);
    }

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{offset_of, size_of};

    /// The numbers come from a `printf("%zu", sizeof/offsetof)` against
    /// `lambdock.h`. A failure here means the header moved and this mirror has to
    /// move with it -- before a module reads a field at the wrong address.
    #[test]
    fn the_mirror_matches_the_header() {
        assert_eq!(size_of::<LambdockState>(), 512);
        assert_eq!(offset_of!(LambdockState, reduce_motion), 96);
        assert_eq!(offset_of!(LambdockState, shelf), 112);
        assert_eq!(offset_of!(LambdockState, x11_surface), 432);
        assert_eq!(offset_of!(LambdockState, user_css_provider), 504);
    }
}
