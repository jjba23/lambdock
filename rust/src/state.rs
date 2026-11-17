// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! One dock instance: its widgets, its settings, and the state the frame loop
//! carries.
//!
//! The widgets and GObjects this dock builds are held here as owned `Option`s, so
//! the state keeps its tree alive rather than remembering addresses: dropping the
//! struct drops the references, and a field set back to `None` is the one way to
//! let go of a widget that is being destroyed. Nothing in here is a C mirror; the
//! layout is the compiler's business.
//!
//! The C strings (`theme`, `active_theme_class`, `config_path`) and the two
//! `GList`s are still allocated by GLib: the settings readers hand out `g_strdup`ed
//! text and the item list is a `GList` the dock mutates in place.

use crate::core::config_values::ShelfParams;
use gtk4::gdk;
use std::os::raw::{c_char, c_int, c_uint, c_ulong, c_void};

/// The record.
#[derive(Default)]
pub struct LambdockState {
    pub window: Option<gtk4::Widget>,
    pub overlay: Option<gtk4::Widget>,
    pub bg_shelf: Option<gtk4::Widget>,
    pub bg_capsule: Option<gtk4::Widget>,
    pub scrolled_window: Option<gtk4::Widget>,
    pub box_: Option<gtk4::Widget>,
    pub popover: Option<gtk4::Widget>,
    pub reconnect_label: Option<gtk4::Widget>,
    pub tracker_connected: c_int,
    pub size_group: Option<gtk4::SizeGroup>,
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
    pub monitor: Option<gdk::Monitor>,
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
    pub base_css_provider: Option<gtk4::CssProvider>,
    pub taskbar_css_provider: Option<gtk4::CssProvider>,
    pub shelf_css_provider: Option<gtk4::CssProvider>,
    pub indicator_guard_css_provider: Option<gtk4::CssProvider>,
    pub user_css_provider: Option<gtk4::CssProvider>,
}
