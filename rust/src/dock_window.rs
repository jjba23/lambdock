// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! One dock's window: `src/dock_window.c` in Rust, starting with its layout half.
//!
//! `update_dock_dimensions()` used to interleave three jobs -- query the monitor,
//! decide how big everything should be, and push the numbers into a dozen
//! widgets. The decision half is `core::dock_dims`; this is the third job, plus
//! the input region that keeps the parts of the surface nobody can see from
//! swallowing clicks, and the slide's room for the fisheye.
//!
//! The rest of the C file -- the slide, the hover, the layer surface, the about
//! dialog and the teardown -- is still C, and calls into here through the header
//! the same way `main.c` does.

use gtk4::gdk;
use gtk4::glib;
use gtk4::glib::translate::{from_glib_borrow, from_glib_full, from_glib_none, Borrowed, IntoGlib};
use gtk4::prelude::*;
use std::cell::RefCell;
use std::os::raw::{c_int, c_uint, c_void};

use crate::cstr::{cstring, translated};
use crate::core::dock_dims::{
    lambdock_dock_dims_compute, DockDims, FISHEYE_MAX_SCALE, FISHEYE_RADIUS_PX,
};
use crate::core::dock_motion::{
    lambdock_dock_needs_full_input, lambdock_fisheye_scale, lambdock_hide_margin,
    lambdock_points_differ, lambdock_scaled_size, lambdock_settling, lambdock_size_step,
    lambdock_slide_position, lambdock_slide_progress,
};
use crate::core::render_stats::{
    lambdock_stats_add, lambdock_stats_bump, GEOMETRY_REFRESHES, HOVER_FRAMES, HOVER_SKIPS, HOVER_US,
    INPUT_REGION_SETS, LAYOUT_STEPS, SURFACE_RESIZES,
};
use crate::dock_item::{dock_items_refresh_polls, free_dock_item, DockItem};
use crate::state::LambdockState;
use crate::ui::icon_zoom::{
    lambdock_icon_zoom_is_zoom, lambdock_icon_zoom_set_scale, lambdock_icon_zoom_target_size,
};
use crate::ui::layer_shell::{self, EDGE_BOTTOM, EDGE_LEFT, EDGE_RIGHT, EDGE_TOP, KEYBOARD_MODE_ON_DEMAND, LAYER_TOP};
use crate::ui::shelf::{
    apply_shelf_layout, lambdock_shelf_active, lambdock_shelf_geometry_for,
    lambdock_shelf_params_effective, shelf_reflection_set_extent, shelf_reflection_set_icon_size,
};
use crate::ui::DEFAULT_ICON_SIZE;
use crate::x11_tracker::{lambdock_x11_dock_apply, lambdock_x11_dock_forget};

/// Drops every handler connected with `data`, which is what teardown does before
/// it frees the state those handlers carry.
///
/// `g_signal_handlers_disconnect_by_data` is a macro over this call in
/// gobject's own headers -- it is not an exported symbol -- so the mask is
/// spelled out here.
///
/// # Safety
///
/// `instance` must be a live `GObject`; `data` the pointer its handlers were
/// connected with.
unsafe fn disconnect_handlers_by_data(instance: *mut c_void, data: *mut c_void) {
    glib::gobject_ffi::g_signal_handlers_disconnect_matched(
        instance.cast(),
        glib::gobject_ffi::G_SIGNAL_MATCH_DATA,
        0,
        0,
        std::ptr::null_mut(),
        std::ptr::null_mut(),
        data,
    );
}

/// Whether the auto-hide trace is on.
///
/// A trace is a report about a sequence, not about a value, and the sequence is
/// not reproducible anywhere but on the machine it happened on -- so it is off
/// by default and costs one `getenv` per process.
fn trace_enabled() -> bool {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();

    *ENABLED.get_or_init(|| std::env::var_os("LAMBDOCK_DEBUG").is_some())
}

/// One line of that trace, stamped with the monotonic clock in microseconds so a
/// log can be lined up against a recording.
fn trace(message: &str) {
    if !trace_enabled() {
        return;
    }

    let now = unsafe { glib::ffi::g_get_monotonic_time() } / 1000;
    crate::log_debug!("auto-hide", "t={now} {message}");
}

/// A widget the state holds, borrowed for the length of one call.
///
/// Borrowed, not owned, even though a dock's widgets are parented: the test
/// fixtures build a dock out of floating boxes, and an owning wrapper would sink
/// one and then free it on the way out, leaving the caller with a dead pointer.
///
/// # Safety
///
/// `pointer` must be null or a live widget.
pub(crate) unsafe fn widget_of(pointer: *mut c_void) -> Option<Borrowed<gtk4::Widget>> {
    if pointer.is_null() {
        return None;
    }

    Some(crate::ui::widget_from_pointer(pointer.cast()))
}

/// The surface a widget is drawn on, or `None` before it is realized.
///
/// # Safety
///
/// `widget` must be a live widget.
unsafe fn surface_of(widget: &gtk4::Widget) -> Option<gdk::Surface> {
    let native = gtk4::ffi::gtk_widget_get_native(widget.as_ptr().cast());
    if native.is_null() {
        return None;
    }

    let surface = gtk4::ffi::gtk_native_get_surface(native);
    if surface.is_null() {
        return None;
    }

    Some(gtk4::glib::translate::from_glib_none(surface))
}

/// The dock's orientation, as the enum the widget calls take.
pub(crate) fn orientation_of(state: &LambdockState) -> gtk4::Orientation {
    if state.orientation == gtk4::Orientation::Vertical.into_glib() {
        gtk4::Orientation::Vertical
    } else {
        gtk4::Orientation::Horizontal
    }
}

/// Whether the dock is horizontal, which is the question the layout asks most.
fn is_horizontal(state: &LambdockState) -> bool {
    orientation_of(state) == gtk4::Orientation::Horizontal
}

// ---------------------------------------------------------------------------
// THE MONITOR
// ---------------------------------------------------------------------------

/// First monitor of the default display.
fn first_monitor() -> Option<gdk::Monitor> {
    let display = gdk::Display::default()?;
    let monitors = display.monitors();
    if monitors.n_items() == 0 {
        return None;
    }

    monitors.item(0)?.downcast::<gdk::Monitor>().ok()
}

/// The monitor the dock's surface is on, or `None` before it is realized.
///
/// # Safety
///
/// `state` must be a live dock.
unsafe fn surface_monitor(state: &LambdockState) -> Option<gdk::Monitor> {
    let window = state.window.as_ref()?;
    let surface = surface_of(window)?;

    gdk::Display::default()?.monitor_at_surface(&surface)
}

/// The dock's monitor size, falling back to the surface's and then to the first.
///
/// # Safety
///
/// `state` must be a live dock.
unsafe fn monitor_size(state: &LambdockState) -> (c_int, c_int) {
    // A monitor that has been unplugged is a dangling pointer: only trust one
    // the display still lists. A dock with no monitor of its own is placed by the
    // compositor, which may well be a monitor that is not the first one: measure
    // the surface's, then the first.
    let monitor = state
        .monitor
        .as_ref()
        .filter(|monitor| layer_shell::is_monitor_valid(monitor.as_ptr()) != 0)
        .cloned()
        .or_else(|| surface_monitor(state))
        .or_else(first_monitor);

    let Some(monitor) = monitor else {
        return (0, 0);
    };

    let geometry = monitor.geometry();

    (geometry.width(), geometry.height())
}

// ---------------------------------------------------------------------------
// APPLYING THE DIMENSIONS
// ---------------------------------------------------------------------------

/// Size-group mode follows the dock's orientation.
///
/// # Safety
///
/// `state` must be a live dock.
unsafe fn apply_size_group(state: &LambdockState) {
    let Some(group) = state.size_group.as_ref() else {
        return;
    };

    group.set_mode(match orientation_of(state) {
        gtk4::Orientation::Horizontal => gtk4::SizeGroupMode::Horizontal,
        _ => gtk4::SizeGroupMode::Vertical,
    });
}

/// The scrolled viewport only ever scrolls along the dock's main axis.
///
/// # Safety
///
/// `state` must be a live dock.
unsafe fn apply_scroll_policy(state: &LambdockState, dims: &DockDims) {
    let Some(scrolled) = state.scrolled_window.as_ref() else {
        return;
    };
    let Some(scrolled) = scrolled.downcast_ref::<gtk4::ScrolledWindow>() else {
        return;
    };

    if is_horizontal(state) {
        scrolled.set_policy(gtk4::PolicyType::External, gtk4::PolicyType::Never);
        scrolled.set_max_content_width(dims.max_main_h_px.max(0));
    } else {
        scrolled.set_policy(gtk4::PolicyType::Never, gtk4::PolicyType::External);
        scrolled.set_max_content_height(dims.max_main_v_px.max(0));
    }
}

/// Padding along the main axis, and the margin that keeps icons off the edge.
///
/// # Safety
///
/// `state` must be a live dock with its box built.
unsafe fn apply_box_margins(dock: &mut LambdockState, dims: &DockDims, horizontal: bool) {
    let Some(box_) = dock.box_.as_ref() else {
        return;
    };

    if horizontal {
        box_.set_margin_start(dims.padding_px);
        box_.set_margin_end(dims.padding_px);
        box_.set_margin_top(0);
        box_.set_margin_bottom(0);
    } else {
        box_.set_margin_top(dims.padding_px);
        box_.set_margin_bottom(dims.padding_px);
        box_.set_margin_start(0);
        box_.set_margin_end(0);
    }

    // Keep the icons off the outer edge by the front glass thickness, so the
    // mirrors (and any front face) have room in front of them. The reflection
    // widget already occupies the band inside that glass, hence the difference:
    // the icons' edge ends up exactly `front` pixels from the screen edge.
    if dims.outer_margin_px <= 0 {
        return;
    }

    let edge_is_start = if horizontal {
        dock.edge == EDGE_TOP
    } else {
        dock.edge == EDGE_LEFT
    };

    if horizontal {
        if edge_is_start {
            box_.set_margin_top(dims.outer_margin_px);
        } else {
            box_.set_margin_bottom(dims.outer_margin_px);
        }
    } else if edge_is_start {
        box_.set_margin_start(dims.outer_margin_px);
    } else {
        box_.set_margin_end(dims.outer_margin_px);
    }
}

/// The row's natural size along the dock's main axis, right now.
///
/// # Safety
///
/// The widget pointers in `state` must still be live.
unsafe fn content_main_px(state: &LambdockState, horizontal: bool) -> c_int {
    let Some(scrolled) = state.scrolled_window.as_ref() else {
        return 0;
    };

    let orientation = if horizontal {
        gtk4::Orientation::Horizontal
    } else {
        gtk4::Orientation::Vertical
    };

    // The second value is the natural size; the C asked for that one only.
    let (_, natural, _, _) = scrolled.measure(orientation, -1);

    natural
}

/// Reserves the room the row spreads into while the pointer runs over the dock.
///
/// Only the size requests change here, and only along the main axis, so this is
/// cheap enough to call on every hover (and it is called at rest, never
/// mid-spread).
///
/// Reserving the room *once*, before the icons start to grow, is what lets a
/// hover run without ever changing the surface's size. A layer surface that is
/// resized on every pointer frame makes the compositor reconfigure and
/// re-allocate its buffer per frame, which is exactly the flicker this prevents.
/// The transparent room is cut out of the input region (see
/// [`apply_input_region`]), so it never swallows a click meant for the windows
/// underneath.
///
/// # Safety
///
/// `dock` must be a live dock.
pub unsafe fn dock_reserve_hover_room(dock: &mut LambdockState) {
    let Some(overlay) = dock.overlay.as_ref() else {
        return;
    };

    let horizontal = is_horizontal(dock);

    // Only at rest: measuring while the row is spread would bake the magnified
    // width in.
    let headroom = if dock.hover_animate != 0 && dock.taskbar == 0 && dock.fisheye_active == 0 {
        dock.hover_headroom_px
    } else {
        0
    };
    let main_px = if headroom > 0 {
        content_main_px(dock, horizontal) + 2 * headroom
    } else {
        -1
    };

    let (width, height) = overlay.size_request();
    if horizontal {
        overlay.set_size_request(main_px, height);
    } else {
        overlay.set_size_request(width, main_px);
    }

    dock_schedule_input_region(dock);
}

/// Cross-axis size requests for the overlay and the capsule.
///
/// # Safety
///
/// `state` must be a live dock with its widgets built.
unsafe fn apply_size_requests(dock: &LambdockState, dims: &DockDims, horizontal: bool) {
    let (Some(overlay), Some(capsule)) = (dock.overlay.as_ref(), dock.bg_capsule.as_ref()) else {
        return;
    };

    if horizontal {
        overlay.set_size_request(-1, dims.overlay_cross_px);
        capsule.set_size_request(-1, dims.capsule_cross_px);
    } else {
        overlay.set_size_request(dims.overlay_cross_px, -1);
        capsule.set_size_request(dims.capsule_cross_px, -1);
    }
}

/// The scrolled viewport fills a taskbar and centres in a floating capsule.
///
/// # Safety
///
/// `state` must be a live dock.
unsafe fn apply_scroll_alignment(state: &LambdockState, horizontal: bool) {
    let Some(scrolled) = state.scrolled_window.as_ref() else {
        return;
    };
    let fill = if state.taskbar != 0 {
        gtk4::Align::Fill
    } else {
        gtk4::Align::Center
    };

    if horizontal {
        scrolled.set_valign(if state.edge == EDGE_TOP {
            gtk4::Align::Start
        } else {
            gtk4::Align::End
        });
        scrolled.set_halign(fill);
        // In taskbar mode the content viewport must expand to fill the bar so
        // icons grow from the leading edge.
        scrolled.set_hexpand(state.taskbar != 0);
    } else {
        scrolled.set_halign(if state.edge == EDGE_LEFT {
            gtk4::Align::Start
        } else {
            gtk4::Align::End
        });
        scrolled.set_valign(fill);
        scrolled.set_vexpand(state.taskbar != 0);
    }
}

/// A taskbar fills its cross axis; a floating capsule centres and hugs the edge.
///
/// # Safety
///
/// `state` must be a live dock with its capsule and box built.
unsafe fn apply_capsule_alignment(state: &LambdockState, horizontal: bool) {
    let (Some(capsule), Some(box_)) = (state.bg_capsule.as_ref(), state.box_.as_ref()) else {
        return;
    };

    let taskbar = state.taskbar != 0;
    let edge_is_start = if horizontal {
        state.edge == EDGE_TOP
    } else {
        state.edge == EDGE_LEFT
    };
    let hugging = |start: bool| if start { gtk4::Align::Start } else { gtk4::Align::End };

    if horizontal {
        capsule.set_halign(if taskbar {
            gtk4::Align::Fill
        } else {
            gtk4::Align::Center
        });
        box_.set_halign(if taskbar {
            gtk4::Align::Start
        } else {
            gtk4::Align::Center
        });
        capsule.set_valign(hugging(edge_is_start));
        box_.set_valign(hugging(edge_is_start));
    } else {
        capsule.set_valign(if taskbar {
            gtk4::Align::Fill
        } else {
            gtk4::Align::Center
        });
        box_.set_valign(if taskbar {
            gtk4::Align::Start
        } else {
            gtk4::Align::Center
        });
        capsule.set_halign(hugging(edge_is_start));
        box_.set_halign(hugging(edge_is_start));
    }
}

/// The shelf wrapper is what the overlay positions, so it mirrors the capsule.
///
/// # Safety
///
/// `state` must be a live dock.
unsafe fn mirror_shelf_alignment(state: &LambdockState) {
    let (Some(shelf), Some(capsule)) = (state.bg_shelf.as_ref(), state.bg_capsule.as_ref()) else {
        return;
    };

    shelf.set_halign(capsule.halign());
    shelf.set_valign(capsule.valign());
}

// ---------------------------------------------------------------------------
// THE INPUT REGION
// ---------------------------------------------------------------------------

/// The dock's clickable rectangle: `bounds`, rounded outwards with one pixel of
/// slack on each side so the rounded edge stays clickable.
fn slack_rect(x: f32, y: f32, width: f32, height: f32) -> (c_int, c_int, c_int, c_int) {
    (
        x.floor() as c_int - 1,
        y.floor() as c_int - 1,
        width.ceil() as c_int + 2,
        height.ceil() as c_int + 2,
    )
}

/// Whether the surface's input region is already the one asked for.
///
/// # Safety
///
/// `state` must be a live dock.
fn input_region_is_current(state: &LambdockState, whole: bool, rect: (c_int, c_int, c_int, c_int)) -> bool {
    if state.input_region_set == 0 || (state.input_region_whole != 0) != whole {
        return false;
    }

    if whole {
        return true;
    }

    state.input_region_x == rect.0
        && state.input_region_y == rect.1
        && state.input_region_width == rect.2
        && state.input_region_height == rect.3
}

/// Applies a region (`None` for the whole surface) unless it is already current.
///
/// # Safety
///
/// `state` must be a live dock with a window; `surface` must be that window's.
unsafe fn apply_input_region(
    state: &mut LambdockState,
    surface: &gdk::Surface,
    whole: bool,
    rect: (c_int, c_int, c_int, c_int),
) {
    if input_region_is_current(state, whole, rect) {
        return;
    }

    if whole {
        surface.set_input_region(None);
        trace(&format!(
            "input region: whole surface ({}x{}), margin {}, hidden {}",
            surface.width(),
            surface.height(),
            state.current_margin,
            state.is_hidden
        ));
    } else {
        let rectangle = gtk4::cairo::RectangleInt::new(rect.0, rect.1, rect.2, rect.3);
        let region = gtk4::cairo::Region::create_rectangle(&rectangle);
        surface.set_input_region(Some(&region));
        trace(&format!(
            "input region: dock only rect {},{}, {}x{} (surface {}x{}, margin {})",
            rect.0,
            rect.1,
            rect.2,
            rect.3,
            surface.width(),
            surface.height(),
            state.current_margin
        ));
    }

    state.input_region_set = 1;
    state.input_region_whole = c_int::from(whole);
    if !whole {
        state.input_region_x = rect.0;
        state.input_region_y = rect.1;
        state.input_region_width = rect.2;
        state.input_region_height = rect.3;
    }

    // One wl_region, one request, one commit: worth counting, because it happens
    // on the hover path and a region that changes every frame is churn.
    lambdock_stats_bump(INPUT_REGION_SETS);

    // Surface state in Wayland is double buffered: a new input region only
    // reaches the compositor with the surface's next commit. A dock that has
    // finished sliding, though, is drawing nothing -- so the region that would
    // let the pointer touch the strip can sit in the queue indefinitely, and the
    // strip stays dead exactly when it is the only part of the dock on screen.
    // Asking for a frame is what commits it; GTK has no partial invalidation,
    // but it does coalesce the request into the frame it is already drawing, so
    // this costs nothing during a hover, which is the only time it repeats.
    if let Some(window) = state.window.as_ref() {
        window.queue_draw();
    }
}

/// Cuts the dock surface's input region down to the dock itself.
///
/// The surface is deliberately bigger than the capsule: it carries the room the
/// icons magnify into, and the room the row spreads into while the pointer runs
/// over it. None of that is part of the dock, so a click there has to reach
/// whatever is underneath instead of being eaten by an invisible layer surface.
///
/// Called from a one-shot frame callback, because a size request only turns into
/// an allocation on the next frame: the bounds read before that are stale.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn apply_dock_input_region(dock: &mut LambdockState) -> bool {
    let (Some(window), Some(shelf)) = (dock.window.as_ref(), dock.bg_shelf.as_ref()) else {
        return false;
    };
    let Some(surface) = surface_of(window) else {
        return false;
    };
    if surface.width() <= 1 || surface.height() <= 1 {
        return false;
    }

    // Sample the surface size where the layout is known to have landed: a change
    // is a buffer re-allocation and a compositor reconfigure, so it is worth
    // counting rather than eyeballing.
    let (surface_width, surface_height) = (surface.width(), surface.height());
    if dock.last_surface_width != surface_width || dock.last_surface_height != surface_height {
        if dock.last_surface_width != 0 {
            lambdock_stats_bump(SURFACE_RESIZES);
        }
        dock.last_surface_width = surface_width;
        dock.last_surface_height = surface_height;
    }

    // A tucked-away dock is a strip at the screen edge, and that strip is the
    // surface's *headroom* -- the part the dock-shaped region below cuts away. It
    // has to stay clickable, or the pointer could never bring the dock back (a
    // slide moves the window every frame, so this is also the dock to be in for
    // the whole animation). While the dock is away the whole surface is the
    // strip, so the whole surface keeps its input.
    if lambdock_dock_needs_full_input(dock.is_hidden, dock.current_margin) != 0 {
        apply_input_region(dock, &surface, true, (0, 0, 0, 0));
        return true;
    }

    // The dock is the slab plus the row of icons that overhangs it, so the region
    // has to be the union of the two: the slab alone would leave the icons -- the
    // part people actually click -- outside the clickable area.
    let Some(bounds) = shelf.compute_bounds(window) else {
        return false;
    };
    if bounds.width() < 1.0 || bounds.height() < 1.0 {
        return false;
    }

    let mut bounds = bounds;
    if let Some(row) = dock.scrolled_window.as_ref() {
        if let Some(row) = row.compute_bounds(window) {
            if row.width() >= 1.0 && row.height() >= 1.0 {
                bounds = bounds.union(&row);
            }
        }
    }

    let rect = slack_rect(bounds.x(), bounds.y(), bounds.width(), bounds.height());
    apply_input_region(dock, &surface, false, rect);

    true
}

/// Frame callback that applies the input region once the window is really laid
/// out, retrying while it is not.
///
/// The first frames of a window have a 1x1 surface and no bounds to speak of, and
/// an input region computed then would be a handful of pixels somewhere: it would
/// make the dock ignore input entirely. So the tick keeps running until it has a
/// real answer, and gives up after a second or so rather than spinning forever on
/// a dock that never maps.
unsafe extern "C" fn on_input_region_tick(
    _widget: *mut gtk4::ffi::GtkWidget,
    _clock: *mut gdk::ffi::GdkFrameClock,
    user_data: *mut c_void,
) -> c_int {
    let state = user_data.cast::<LambdockState>();
    let Some(dock) = state.as_mut() else {
        return glib::ffi::GFALSE;
    };

    if apply_dock_input_region(dock) {
        dock.input_region_tick_id = 0;
        dock.input_region_attempts = 0;
        return glib::ffi::GFALSE;
    }

    dock.input_region_attempts += 1;
    if dock.input_region_attempts > 120 {
        dock.input_region_tick_id = 0;
        dock.input_region_attempts = 0;
        return glib::ffi::GFALSE;
    }

    glib::ffi::GTRUE
}

/// Asks for the input region to be recomputed after the next frame.
///
/// Cheap to call repeatedly: only one update is ever queued, so a pointer that
/// moves every frame adds no more than one region computation per frame.
///
/// # Safety
///
/// `dock` must be a live dock.
pub unsafe fn dock_schedule_input_region(dock: &mut LambdockState) {
    let pointer = dock as *mut LambdockState;
    if dock.input_region_tick_id != 0 {
        return;
    }

    let Some(window) = dock.window.as_ref() else {
        return;
    };

    dock.input_region_tick_id = gtk4::ffi::gtk_widget_add_tick_callback(
        window.as_ptr().cast(),
        Some(on_input_region_tick),
        pointer.cast(),
        None,
    );
}

// ---------------------------------------------------------------------------
// UPDATE
// ---------------------------------------------------------------------------

/// Recalculates and applies the dock's layout: the size requests of the overlay
/// surface and the background capsule, the row's margins and alignment, and the
/// input region that follows from them.
///
/// # Safety
///
/// `dock` must be a live dock.
pub unsafe fn update_dock_dimensions(dock: &mut LambdockState) {
    if dock.bg_capsule.is_none() || dock.overlay.is_none() {
        return;
    }

    let (monitor_width, monitor_height) = monitor_size(dock);
    let shelf = lambdock_shelf_geometry_for(dock, dock.icon_size);
    let dims = lambdock_dock_dims_compute(
        dock.icon_size,
        dock.capsule_padding,
        shelf,
        monitor_width,
        monitor_height,
        dock.hover_animate,
    );

    let horizontal = is_horizontal(dock);

    apply_size_group(dock);
    apply_scroll_policy(dock, &dims);
    apply_box_margins(dock, &dims, horizontal);
    dock.hover_headroom_px = dims.hover_headroom_px;
    apply_size_requests(dock, &dims, horizontal);
    dock_reserve_hover_room(dock);
    apply_scroll_alignment(dock, horizontal);
    apply_capsule_alignment(dock, horizontal);

    // Size, show or hide the glass-shelf reflections so they always match the
    // current icon size and shelf parameters.
    apply_shelf_layout(dock as *mut LambdockState);
    mirror_shelf_alignment(dock);

    // Icon size, spacing, padding, mode and shelf all move the items: the hover
    // path's cached geometry is stale.
    dock.centres_dirty = 1;

    // The surface may be bigger than the dock now: keep the input region on the
    // dock itself.
    dock_schedule_input_region(dock);

    // An X11 dock is moved and resized by hand, so a size change is also a
    // placement change. Idempotent: an unchanged rectangle is not re-applied.
    lambdock_x11_dock_apply(dock as *mut LambdockState);
}

/// The number of frames the input region may keep retrying before it gives up,
/// which is a second at 120 Hz.
const _: c_uint = 120;

// ===========================================================================
// THE SURFACE POLICY
// ===========================================================================
//
// Pure: a function of the state, and at most of the monotonic clock.

/// `AUTO_HIDE_SETTLE_MS` in `dock_window.c`: how long after a slide stops a
/// crossing is still an echo of that slide rather than the pointer moving.
const AUTO_HIDE_SETTLE_MS: u32 = 300;

/// `ANIMATION_DURATION_US` in `lambdock.h`: how long one slide takes.
const ANIMATION_DURATION_US: i64 = 250_000;

/// `CROSS_DIM_TOLERANCE_PX` in `lambdock.h`: how far off the cross dimension a
/// surface may measure and still count as the dock.
const CROSS_DIM_TOLERANCE_PX: c_int = 20;

/// `AUTOHIDE_TRIGGER_STRIP_PX` in `core/dock_motion.h`: the strip left on
/// screen, and clickable, while the dock is away.
const AUTOHIDE_TRIGGER_STRIP_PX: c_int = 4;

/// Everything the layer-shell surface needs to know, as a value.
///
/// The field order and types are the contract with `DockSurfacePolicy` in
/// `dock_window.c`: `gboolean` is an `int`, so the anchors are `c_int` here.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DockSurfacePolicy {
    pub anchor_top: c_int,
    pub anchor_bottom: c_int,
    pub anchor_left: c_int,
    pub anchor_right: c_int,
    pub exclusive_zone_px: c_int,
}

/// The icon size actually in use, with the default for partly built states.
pub fn dock_icon_size(state: &LambdockState) -> c_int {
    match state.icon_size {
        size if size > 0 => size,
        _ => DEFAULT_ICON_SIZE,
    }
}

/// The capsule's thickness for the current shelf parameters.
pub fn dock_capsule_cross_dim(state: &LambdockState) -> c_int {
    lambdock_shelf_geometry_for(state, dock_icon_size(state)).capsule_px
}

/// The cross-axis size the overlay asks for: fisheye headroom, the outer glass,
/// and any icon overhang past the inner edge.
pub fn dock_cross_axis_dim(state: &LambdockState) -> c_int {
    let icon_px = dock_icon_size(state);
    let geometry = lambdock_shelf_geometry_for(state, icon_px);
    let hover_magnify = state.hover_animate;

    // One formula, in the pure dimension maths: this is what the auto-hide
    // margin is measured from, so it has to be the number
    // `update_dock_dimensions` asks the surface for. The monitor size and the
    // main-axis padding only feed fields this does not read.
    lambdock_dock_dims_compute(icon_px, 0, geometry, 0, 0, hover_magnify).overlay_cross_px
}

/// Whether the dock is sliding, or has only just stopped.
///
/// Crossings reported in that moment are echoes of the dock's own movement
/// rather than the pointer moving, and auto-hide must not act on them: believing
/// one hides the dock, which puts the revealing strip back under a pointer that
/// never moved, which reveals it again -- the dock bounces at the screen edge.
pub fn dock_is_settling(state: &LambdockState) -> c_int {
    // Safety: `g_get_monotonic_time` only reads the clock.
    let now_us = unsafe { glib::ffi::g_get_monotonic_time() };
    lambdock_settling(
        c_int::from(state.tick_cb_id != 0),
        state.margin_anim_end_us,
        now_us,
        AUTO_HIDE_SETTLE_MS,
    )
}

/// CSS class naming the screen edge a shelf is docked to.
pub fn shelf_edge_class(edge: c_int) -> &'static str {
    match edge {
        EDGE_TOP => "shelf-top",
        EDGE_LEFT => "shelf-left",
        EDGE_RIGHT => "shelf-right",
        _ => "shelf-bottom",
    }
}

/// Sign of the shelf tilt: the edge touching the screen stays pinned while the
/// inner edge recedes away from the viewer, so top and left docks tilt backwards.
pub fn shelf_tilt_for_edge(edge: c_int, degrees: f64) -> f64 {
    match edge {
        EDGE_TOP | EDGE_LEFT => -degrees,
        _ => degrees,
    }
}

/// The axis running across the dock: vertical for a top or bottom dock.
fn cross_orientation(edge: c_int) -> gtk4::Orientation {
    if edge == EDGE_TOP || edge == EDGE_BOTTOM {
        gtk4::Orientation::Vertical
    } else {
        gtk4::Orientation::Horizontal
    }
}

/// The layer-shell surface the dock's configuration describes: which edges to
/// anchor, and how much desktop to reserve.
///
/// Two rules, both about not taking space the dock is not using:
///
/// - taskbar mode also anchors the perpendicular edges, so the surface is
///   stretched across the monitor while a floating dock keeps its natural,
///   content-hugging size;
/// - an auto-hiding dock reserves nothing, or the space would stay reserved
///   while the dock sits off screen.
pub fn dock_surface_policy_for(state: &LambdockState) -> DockSurfacePolicy {
    let mut policy = DockSurfacePolicy {
        anchor_top: c_int::from(state.edge == EDGE_TOP),
        anchor_bottom: c_int::from(state.edge == EDGE_BOTTOM),
        anchor_left: c_int::from(state.edge == EDGE_LEFT),
        anchor_right: c_int::from(state.edge == EDGE_RIGHT),
        exclusive_zone_px: if state.auto_hide != 0 {
            0
        } else {
            dock_capsule_cross_dim(state)
        },
    };

    if state.taskbar == 0 {
        return policy;
    }

    if state.edge == EDGE_TOP || state.edge == EDGE_BOTTOM {
        policy.anchor_left = 1;
        policy.anchor_right = 1;
    } else {
        policy.anchor_top = 1;
        policy.anchor_bottom = 1;
    }

    policy
}

// ===========================================================================
// THE SLIDE
// ===========================================================================

/// Sets the dock's layer-shell margin, which is where the slide actually moves.
///
/// `gtk_layer_set_margin` only means something on a layer surface: the
/// preferences window and the test fixtures build ordinary windows.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn set_window_margin(dock: &LambdockState, margin: c_int) {
    let Some(window) = window_of(dock) else {
        return;
    };
    let window = &*window;

    if layer_shell::is_layer_window(window) {
        layer_shell::set_margin(window, dock.edge, margin);
    }
}

/// Stops the slide frame callback, if one is running.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn stop_margin_animation(dock: &mut LambdockState) {
    if dock.tick_cb_id != 0 {
        if let Some(window) = dock.window.as_ref() {
            gtk4::ffi::gtk_widget_remove_tick_callback(window.as_ptr().cast(), dock.tick_cb_id);
        }
    }

    dock.tick_cb_id = 0;
}

/// Parks the dock at `margin` without animating there.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn snap_margin_to(dock: &mut LambdockState, margin: c_int) {
    dock.start_margin = margin;
    dock.current_margin = margin;
    dock.target_margin = margin;
    dock.anim_start_time = 0;
    // Crossings for a moment after this are echoes of the move, not the pointer
    // moving: see `dock_is_settling`.
    dock.margin_anim_end_us = glib::ffi::g_get_monotonic_time();

    set_window_margin(dock, margin);

    // Hidden and shown want different input regions: the whole surface while the
    // dock is a strip, the dock itself once it is out.
    dock_schedule_input_region(dock);
}

/// Drops a queued fisheye frame, so a stale position cannot be applied.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn stop_pointer_tick_inner(dock: &mut LambdockState) {
    if dock.pointer_tick_id != 0 {
        if let Some(window) = dock.window.as_ref() {
            gtk4::ffi::gtk_widget_remove_tick_callback(window.as_ptr().cast(), dock.pointer_tick_id);
        }
    }

    dock.pointer_tick_id = 0;
}

/// Drops a queued fisheye frame.
///
/// # Safety
///
/// `dock` must be a live dock.
pub unsafe fn stop_pointer_tick(dock: &mut LambdockState) {
    stop_pointer_tick_inner(dock);
}

/// Runs `visit` over the dock row from Rust, skipping empty slots.
///
/// # Safety
///
/// Every item in `items` must be null or live.
unsafe fn for_each_item(items: &[*mut DockItem], mut visit: impl FnMut(&DockItem)) {
    for item in items.iter().filter_map(|item| item.as_ref()) {
        visit(item);
    }
}

/// The fisheye zoom container of an item, the widget that scales the icon while
/// hovering, or null for items that have no icon (separators, meters).
///
/// # Safety
///
/// `item` must be null or a live item.
pub unsafe fn get_item_zoom_widget(item: *const DockItem) -> *mut gtk4::ffi::GtkWidget {
    let Some(item) = item.as_ref() else {
        return std::ptr::null_mut();
    };

    item_zoom_widget(item)
}

/// # Safety
///
/// `item` must be a live item.
unsafe fn item_zoom_widget(item: &DockItem) -> *mut gtk4::ffi::GtkWidget {
    let Some(button) = widget_of(item.button) else {
        return std::ptr::null_mut();
    };
    let Some(button) = button.downcast_ref::<gtk4::Button>() else {
        return std::ptr::null_mut();
    };

    let Some(child) = crate::ui::button_child(button) else {
        return std::ptr::null_mut();
    };

    if lambdock_icon_zoom_is_zoom(child.as_ptr().cast()) != 0 {
        return child.as_ptr();
    }

    if child.is::<gtk4::Box>() {
        let mut node = crate::ui::first_child_of(&child);
        while let Some(widget) = node {
            if lambdock_icon_zoom_is_zoom(widget.as_ptr().cast()) != 0 {
                return widget.as_ptr();
            }

            node = crate::ui::next_sibling_of(&widget);
        }
    }

    std::ptr::null_mut()
}

/// Puts one dock item back at its resting size, reflection included.
///
/// # Safety
///
/// `item` must be a live item whose widgets are built.
unsafe fn reset_item_scale(item: &DockItem, horizontal: bool, base_px: c_int) {
    let Some(button) = widget_of(item.button) else {
        return;
    };

    if horizontal {
        button.set_size_request(-1, base_px);
    } else {
        button.set_size_request(base_px, -1);
    }

    // The icon itself is never re-rasterised: it rests at its base scale.
    let zoom = item_zoom_widget(item);
    if !zoom.is_null() {
        lambdock_icon_zoom_set_scale(zoom, 1.0);
    }

    // Keep the glass-shelf reflection at the base icon size too.
    if !item.reflection.is_null() {
        shelf_reflection_set_icon_size(item.reflection.cast(), base_px);
    }
}

/// Resets every dock item to its base pixel size.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn reset_dock_items_scale_inner(dock: &mut LambdockState) {
    if dock.dock_items.is_empty() {
        return;
    }

    let horizontal = is_horizontal(dock);
    let base_px = dock_icon_size(dock);

    for_each_item(&dock.dock_items, |item| {
        reset_item_scale(item, horizontal, base_px);
    });

    // The row is back at rest, so the next frame has to apply the pointer again
    // even if it has not moved, and the surface's room can be re-measured.
    dock.pointer_applied = 0;
    dock.hover_axis_valid = 0;
    dock.fisheye_active = 0;

    // The row is back at rest: let the surface hug it again.
    dock_reserve_hover_room(dock);
}

/// Resets every dock item to its base pixel size.
///
/// # Safety
///
/// `dock` must be a live dock.
pub unsafe fn reset_dock_items_scale(dock: &mut LambdockState) {
    reset_dock_items_scale_inner(dock);
}

/// One frame of the slide: interpolates `current_margin` towards
/// `target_margin` from the frame clock's own timestamps.
unsafe extern "C" fn animate_margin_tick(
    _widget: *mut gtk4::ffi::GtkWidget,
    frame_clock: *mut gdk::ffi::GdkFrameClock,
    user_data: *mut c_void,
) -> c_int {
    let Some(dock) = user_data.cast::<LambdockState>().as_mut() else {
        return glib::ffi::GFALSE;
    };

    let clock: Borrowed<gdk::FrameClock> = from_glib_borrow(frame_clock);
    let now_us = clock.frame_time();

    // Anchor the clock on the first frame: the animation starts when the
    // compositor first ticked, not when it was requested.
    if dock.anim_start_time == 0 {
        dock.anim_start_time = now_us;
    }

    let progress = lambdock_slide_progress(dock.anim_start_time, now_us, ANIMATION_DURATION_US);

    if progress >= 1.0 {
        dock.current_margin = dock.target_margin;
        set_window_margin(dock, dock.current_margin);
        dock.tick_cb_id = 0;
        dock.margin_anim_end_us = glib::ffi::g_get_monotonic_time();
        trace(&format!("slide done at margin {}", dock.current_margin));

        // The dock has arrived, so the input region it needs may have changed:
        // out in the open it is the dock, mid-slide it was the whole surface.
        dock_schedule_input_region(dock);
        return glib::ffi::GFALSE;
    }

    dock.current_margin = lambdock_slide_position(dock.start_margin, dock.target_margin, progress);
    set_window_margin(dock, dock.current_margin);

    glib::ffi::GTRUE
}

/// Starts or updates a slide towards `target_margin`.
///
/// # Safety
///
/// `dock` must be a live dock with a window.
unsafe fn start_margin_animation(dock: &mut LambdockState, target_margin: c_int) {
    // Nothing to do if the dock is already where it is wanted -- and not on its
    // way somewhere else. `current_margin` is where the dock *is*, which is not
    // the same as where it is going: a hide that has been requested but has not
    // had its first frame yet still reads as margin 0 with a target below it,
    // and so does a hide that has only just started sliding. Returning early
    // then threw away the reveal that asked for this -- the dock went on sliding
    // away believing it was visible, and could not be brought back while the
    // pointer stayed on the strip.
    if dock.current_margin == target_margin
        && (dock.tick_cb_id == 0 || dock.target_margin == target_margin)
    {
        return;
    }

    // Reduced motion: the dock still has to get out of the way (or come back),
    // it just does it in one step instead of sliding.
    if crate::accessibility::lambdock_animations_enabled(dock) == 0 {
        stop_margin_animation(dock);
        snap_margin_to(dock, target_margin);
        return;
    }

    dock.start_margin = dock.current_margin;
    dock.target_margin = target_margin;
    dock.anim_start_time = 0;
    dock.margin_anim_end_us = 0;

    // A slide is about to move the dock somewhere the surface's input region was
    // not shaped for, and whether the whole surface has to take input depends on
    // the margin: ask for the region again on the frame the slide starts, not
    // only on the frames something else happens to reapply it.
    dock_schedule_input_region(dock);

    if dock.tick_cb_id == 0 {
        if let Some(window) = dock.window.as_ref() {
            dock.tick_cb_id = gtk4::ffi::gtk_widget_add_tick_callback(
                window.as_ptr().cast(),
                Some(animate_margin_tick),
                (dock as *mut LambdockState).cast(),
                None,
            );
        }
    }
}

/// Honours the dock's motion setting, and the desktop's.
///
/// Called when the dock reloads its settings and when the desktop's animation
/// setting changes. With motion reduced, a slide already in flight is finished
/// immediately, icons are put back at their resting size and the fisheye is
/// stopped; nothing is animated until motion is allowed again.
///
/// # Safety
///
/// `dock` must be a live dock.
pub unsafe fn dock_apply_motion_preference(dock: &mut LambdockState) {

    if dock.window.is_none() || crate::accessibility::lambdock_animations_enabled(dock) != 0 {
        return;
    }

    stop_margin_animation(dock);
    dock.current_margin = dock.target_margin;
    set_window_margin(dock, dock.current_margin);

    stop_pointer_tick_inner(dock);
    reset_dock_items_scale_inner(dock);
}

// ===========================================================================
// AUTO-HIDE AND REVEAL
// ===========================================================================

/// Adapter: a GLib timeout hands the state back as a plain pointer.
unsafe extern "C" fn on_hide_timeout(user_data: *mut c_void) -> c_int {
    let Some(dock) = user_data.cast::<LambdockState>().as_mut() else {
        return glib::ffi::GFALSE;
    };

    dock.hide_timer_id = 0;
    hide_dock(dock);

    glib::ffi::GFALSE
}

/// # Safety
///
/// `dock` must be a live dock.
unsafe fn cancel_hide_timer_inner(dock: &mut LambdockState) {
    if dock.hide_timer_id == 0 {
        return;
    }

    glib::ffi::g_source_remove(dock.hide_timer_id);
    dock.hide_timer_id = 0;
}

/// Drops any pending auto-hide timer. Safe to call at any time.
///
/// # Safety
///
/// `dock` must be a live dock.
pub unsafe fn cancel_hide_timer(dock: &mut LambdockState) {
    cancel_hide_timer_inner(dock);
}

/// (Re)schedules the auto-hide timeout, replacing a pending one.
///
/// # Safety
///
/// `dock` must be a live dock.
pub unsafe fn schedule_hide_timer(dock: &mut LambdockState, delay_ms: c_uint) {
    let pointer = dock as *mut LambdockState;

    cancel_hide_timer_inner(dock);
    dock.hide_timer_id = glib::ffi::g_timeout_add(delay_ms, Some(on_hide_timeout), pointer.cast());
}

/// The surface's current size along the dock's main axis, or 0 before GTK has
/// laid it out (which `lambdock_hide_margin` knows how to handle).
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn measured_main_axis_px(dock: &LambdockState) -> c_int {
    let Some(window) = dock.window.as_ref() else {
        return 0;
    };

    if cross_orientation(dock.edge) == gtk4::Orientation::Vertical {
        window.height()
    } else {
        window.width()
    }
}

/// Cancels pending hide timers and animates the dock back into full view.
///
/// # Safety
///
/// `dock` must be a live dock.
pub unsafe fn reveal_dock(dock: &mut LambdockState) {

    cancel_hide_timer_inner(dock);

    if dock.is_hidden != 0 || dock.target_margin != 0 {
        trace(&format!(
            "reveal: hidden {}, margin {} -> 0",
            dock.is_hidden, dock.current_margin
        ));
        start_margin_animation(dock, 0);
        dock.is_hidden = 0;

        // The pollers stood still while the dock was away; make every clock,
        // gauge and trash count current before the first frame is drawn.
        dock_items_refresh_polls(dock);
    } else {
        trace(&format!("reveal: already out (margin {})", dock.current_margin));
    }
}

/// Slides the dock off screen, keeping a trigger strip visible on the screen
/// edge so the pointer can bring it back.
///
/// # Safety
///
/// `dock` must be a live dock.
pub unsafe fn hide_dock(dock: &mut LambdockState) {

    // Auto-hide slides the surface off-screen with a layer-shell margin. Without
    // layer shell (X11, GNOME) there is no margin to move, so "hiding" would
    // only freeze the fisheye while the dock stays visible.
    let Some(window) = window_of(dock) else {
        return;
    };
    if !layer_shell::is_layer_window(&*window) {
        return;
    }

    // Respect the auto-hide setting, and never hide under an open menu.
    if dock.auto_hide == 0 || dock.active_popovers > 0 {
        return;
    }

    // `show-when-clear` means the dock belongs on screen while nothing runs.
    if dock.show_when_clear != 0 && dock.window_trackers.is_empty() {
        return;
    }

    // The row has to be at rest before its size is measured, or the measurement
    // is the magnified one.
    if dock.hover_animate != 0 {
        reset_dock_items_scale_inner(dock);
    }

    let measured_px = measured_main_axis_px(dock);
    let cross_px = dock_cross_axis_dim(dock);
    let hidden_margin = lambdock_hide_margin(
        measured_px,
        cross_px,
        CROSS_DIM_TOLERANCE_PX,
        AUTOHIDE_TRIGGER_STRIP_PX,
    );

    trace(&format!(
        "hide: window {}x{} (cross axis {cross_px}) -> margin {hidden_margin}, strip {AUTOHIDE_TRIGGER_STRIP_PX} px",
        window.width(),
        window.height()
    ));

    if dock.is_hidden == 0 || dock.target_margin != hidden_margin {
        start_margin_animation(dock, hidden_margin);
        dock.is_hidden = 1;
    }
}

// ===========================================================================
// DOCK MODE AND CSS CLASSES
// ===========================================================================

/// `AUTOHIDE_INITIAL_DELAY_MS` in `lambdock.h`: how long a fresh dock stays out
/// before it first slides away.
const AUTOHIDE_INITIAL_DELAY_MS: u32 = 1000;

/// Sets or clears one CSS class on a widget the state holds.
fn set_class(widget: Option<&gtk4::Widget>, class: &str, enabled: bool) {
    if let Some(widget) = widget {
        crate::ui::widget_utils::set_css_class(widget, class, enabled);
    }
}

/// The dock's window, borrowed, when it has one.
///
/// # Safety
///
/// `dock` must be a live dock.
pub(crate) unsafe fn window_of(dock: &LambdockState) -> Option<Borrowed<gtk4::Window>> {
    let widget = dock.window.as_ref()?;
    let pointer = widget.as_ptr();

    let window: Borrowed<gtk4::Window> = from_glib_borrow(pointer.cast());

    Some(window)
}

/// Tags the capsule and the window with the shelf's state classes, plus the
/// class naming the edge the shelf is docked to.
///
/// Both widgets are tagged on purpose: user CSS can then target the whole dock
/// (the window) or the shelf itself (the capsule, which carries the theme).
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn apply_shelf_css_classes(dock: &LambdockState, shelf_active: bool, mirroring: bool) {
    const EDGE_CLASSES: [&str; 4] = ["shelf-bottom", "shelf-top", "shelf-left", "shelf-right"];

    let tagged = [dock.bg_capsule.as_ref(), dock.window.as_ref()];

    // Edges are mutually exclusive: clear them all, then set the current one.
    for widget in tagged {
        for class in EDGE_CLASSES {
            set_class(widget, class, false);
        }
    }

    for widget in tagged {
        set_class(widget, "shelf-mode", shelf_active);
        // `shelf-mirror` means the glass actually shows icon reflections: the
        // reflection widget then draws the running indicator itself, so the CSS
        // one must stay quiet.
        set_class(widget, "shelf-mirror", mirroring);
    }

    if !shelf_active {
        return;
    }

    let edge_class = shelf_edge_class(dock.edge);
    for widget in tagged {
        set_class(widget, edge_class, true);
    }
}

/// Projects the styled capsule onto the shape the shelf parameters describe: the
/// tilt, the keystone (flare) or both. With neither, the frame passes through.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn apply_shelf_shape(dock: &LambdockState) {
    if dock.bg_shelf.is_none() {
        return;
    }

    crate::ui::shelf::shelf_frame_set_shape(
        dock.bg_shelf
            .as_ref()
            .map_or(std::ptr::null_mut(), |frame| frame.as_ptr().cast()),
        dock.edge,
        shelf_tilt_for_edge(dock.edge, dock.shelf.tilt_deg),
        dock.shelf.perspective,
        dock.shelf.flare_px,
    );
}

/// Toggles the `taskbar` class on the capsule so themes can give taskbar mode
/// its own rectangular, edge-to-edge styling, and the `shelf-*` classes that let
/// any theme restyle the glass shelf.
///
/// # Safety
///
/// `dock` must be a live dock.
pub unsafe fn apply_dock_mode_classes(dock: &LambdockState) {
    if dock.bg_capsule.is_none() {
        return;
    }

    let shelf_active = lambdock_shelf_active(dock) != 0;
    let mirroring = shelf_active && lambdock_shelf_geometry_for(dock, dock.icon_size).band_px > 0;

    set_class(dock.bg_capsule.as_ref(), "taskbar", dock.taskbar != 0);
    apply_shelf_css_classes(dock, shelf_active, mirroring);
    apply_shelf_shape(dock);
}

// ===========================================================================
// THE LAYER-SHELL SURFACE
// ===========================================================================

/// Puts `policy`'s anchors onto the surface.
///
/// # Safety
///
/// `window` must already be a layer surface.
unsafe fn apply_surface_anchors(window: &gtk4::Window, policy: &DockSurfacePolicy) {
    for (edge, anchored) in [
        (EDGE_TOP, policy.anchor_top),
        (EDGE_BOTTOM, policy.anchor_bottom),
        (EDGE_LEFT, policy.anchor_left),
        (EDGE_RIGHT, policy.anchor_right),
    ] {
        layer_shell::set_anchor(window, edge, anchored != 0);
    }
}

/// Reserves desktop space for the dock, or nothing at all while it auto-hides.
///
/// The rule lives in [`dock_surface_policy_for`], and this is the one way to
/// apply it: the surface is created with it, and every configuration reload
/// re-applies it (auto-hide and the capsule thickness can both change).
///
/// # Safety
///
/// `dock` must be a live dock.
pub unsafe fn dock_apply_exclusive_zone(dock: &LambdockState) {
    let Some(window) = window_of(dock) else {
        return;
    };
    if !layer_shell::is_layer_window(&*window) {
        return;
    }

    let policy = dock_surface_policy_for(dock);
    layer_shell::set_exclusive_zone(&window, policy.exclusive_zone_px);
}

/// Converts a standard GTK window to a layer-shell surface anchored to the
/// docked edge.
///
/// Layer shell is a Wayland protocol that not every compositor implements (and
/// that X11 does not have at all). Where it is missing the dock is still a
/// perfectly good normal window, so this says so once and leaves it alone rather
/// than letting every later `gtk_layer_*` call log its own critical.
///
/// # Safety
///
/// `dock` must be a live dock with its window built.
pub unsafe fn setup_layer_shell_window(dock: &LambdockState) {
    let Some(widget) = dock.window.as_ref() else {
        return;
    };
    let Some(window) = window_of(dock) else {
        return;
    };

    if layer_shell::lambdock_layer_shell_available() == 0 {
        // Two different reasons with two different consequences, so they get two
        // different messages: on X11 the dock still claims its edge through EWMH
        // and only loses the slide; on a Wayland compositor without the protocol
        // there is nothing to claim it with.
        if crate::x11_tracker::lambdock_x11_session() != 0 {
            crate::log_info!(
                "Dock",
                "X11 session: docking through EWMH instead of layer shell. The dock reserves its edge and tracks windows; auto-hide needs a layer surface and stays off."
            );
        } else {
            crate::log_warn!(
                "Dock",
                "This compositor has no wlr-layer-shell support (GNOME on Wayland is the common one): showing the dock as an ordinary window. It launches and lists nothing running; place it, or use a compositor that implements the protocol."
            );
        }

        // An X11 dock has to say what it is *before* the window manager sees the
        // window: by the time it is mapped it has already been managed, and a
        // window managed as an application is decorated, tiled and listed in
        // taskbars. Layer shell has the same requirement in the other direction
        // -- `gtk_layer_init_for_window` must come before the surface exists --
        // which is why this only happens on the branch not using it.
        widget.realize();
        lambdock_x11_dock_apply(dock as *const LambdockState as *mut LambdockState);
        return;
    }

    layer_shell::init_for_window(&window);
    layer_shell::set_namespace(&window, c"lambdock");

    // Layer surfaces get no keyboard by default, which would leave the dock
    // unreachable for keyboard-only users: icons could not be tabbed to, and
    // Escape would never reach an open stack. On-demand keeps the compositor in
    // charge while letting a click (or a compositor shortcut) hand the dock the
    // keyboard. Protocol versions below 4 have no on-demand mode and fall back
    // to "none"; the dock stays usable by pointer there.
    layer_shell::set_keyboard_mode(&window, KEYBOARD_MODE_ON_DEMAND);

    // A monitor that has already been unplugged must not be handed to the
    // compositor; with none set the surface follows the default output.
    let monitor = dock
        .monitor
        .as_ref()
        .map_or(std::ptr::null_mut(), |monitor| monitor.as_ptr());
    if layer_shell::is_monitor_valid(monitor) != 0 {
        layer_shell::set_monitor(&window, monitor);
    }

    // The dock renders on the top layer, above normal application windows.
    layer_shell::set_layer(&window, LAYER_TOP);

    let policy = dock_surface_policy_for(dock);
    apply_surface_anchors(&window, &policy);

    // The exclusive zone reserves desktop space so windows do not overlap the
    // dock.
    layer_shell::set_exclusive_zone(&window, policy.exclusive_zone_px);
}

/// Re-applies the anchors, the mode classes and the dimensions after a live
/// configuration reload toggles dock <-> taskbar mode.
///
/// # Safety
///
/// `dock` must be a live dock.
pub unsafe fn refresh_dock_mode(dock: &mut LambdockState) {
    let Some(window) = window_of(dock) else {
        return;
    };

    // Anchors only exist on a layer surface (no layer shell on X11 or GNOME).
    if layer_shell::is_layer_window(&*window) {
        let policy = dock_surface_policy_for(dock);
        apply_surface_anchors(&window, &policy);
    }

    apply_dock_mode_classes(dock);
    // Dimension updates end by re-placing the X11 window, which is where a mode
    // switch (dock <-> taskbar) changes the dock's size.
    update_dock_dimensions(dock);
}

/// Schedules the first hide, on a dock that auto-hides.
///
/// # Safety
///
/// `dock` must be a live dock.
pub unsafe fn schedule_initial_autohide(dock: &mut LambdockState) {
    let Some(window) = window_of(dock) else {
        return;
    };

    // Without a layer surface there is no slide, so there is nothing to
    // schedule.
    if dock.auto_hide != 0 && layer_shell::is_layer_window(&*window) {
        schedule_hide_timer(dock, AUTOHIDE_INITIAL_DELAY_MS);
    }
}

// ===========================================================================
// THE BACKGROUND MENU
// ===========================================================================

/// `AUTOHIDE_POPOVER_DELAY_MS` in `lambdock.h`: how long the dock waits after
/// its last menu closes before the hide countdown restarts.
const AUTOHIDE_POPOVER_DELAY_MS: u32 = 400;

/// Opens the backdrop menu at the click, so it grows away from the pointer.
///
/// # Safety
///
/// `user_data` must be null or a live dock; the signal arguments are ignored.
pub unsafe fn on_right_click(
    _gesture: *mut gtk4::ffi::GtkGestureClick,
    _n_press: c_int,
    x: f64,
    y: f64,
    user_data: *mut c_void,
) {
    let Some(dock) = user_data.cast::<LambdockState>().as_ref() else {
        return;
    };
    let Some(popover) = dock.popover.as_ref() else {
        return;
    };
    let Some(popover) = popover.downcast_ref::<gtk4::Popover>() else {
        return;
    };

    let point = gdk::Rectangle::new(x as c_int, y as c_int, 1, 1);
    popover.set_pointing_to(Some(&point));

    // `is_shown`, not `is_visible`: a popover is nobody's child, so the
    // ancestor-asking getter answers "no" for one that is open.
    if !crate::ui::is_shown(popover.upcast_ref()) {
        // The state is the counter's owner, so a menu that is already open is
        // not counted twice.
        if let Some(dock) = user_data.cast::<LambdockState>().as_mut() {
            dock.active_popovers += 1;
        }
    }

    popover.popup();
}

/// Counts a menu out, and lets auto-hide resume once the last one is gone.
///
/// # Safety
///
/// `user_data` must be null or a live dock.
pub unsafe fn on_popover_closed(
    _popover: *mut gtk4::ffi::GtkPopover,
    user_data: *mut c_void,
) {
    let Some(dock) = user_data.cast::<LambdockState>().as_mut() else {
        return;
    };

    if dock.active_popovers > 0 {
        dock.active_popovers -= 1;
    }

    if dock.auto_hide != 0 && dock.active_popovers == 0 {
        schedule_hide_timer(dock, AUTOHIDE_POPOVER_DELAY_MS);
    }
}

// ===========================================================================
// FISHEYE HOVER MAGNIFICATION
// ===========================================================================
//
// The bell curve itself is in `core::dock_motion`; this half reads widget bounds
// and pushes the resulting sizes into the row.

/// `FISHEYE_POINTER_STEP_PX` in `lambdock.h`: how far the pointer has to move
/// along the dock before a hover frame is worth applying.
const FISHEYE_POINTER_STEP_PX: f64 = 0.5;

/// `FISHEYE_LAYOUT_STEP_PX` in `lambdock.h`: the magnified sizes are coarsened
/// to this, so the row re-measures about half as often; the resting size stays
/// exact.
const FISHEYE_LAYOUT_STEP_PX: c_int = 2;

/// What one hover frame needs, computed once per pointer position.
///
/// It holds the values it reads rather than the state, so the row walk can take
/// the state mutably at the end of the frame.
struct FisheyeFrame<'a> {
    /// The pointer along the dock's main axis, relative to the row.
    pointer_axis: f64,
    base_size_px: c_int,
    radius_px: f64,
    max_scale: f64,
    centres: &'a [c_int],
    horizontal: bool,
    shelf_active: bool,
    /// The reflection band, as a fraction of the icon size.
    band_ratio: f64,
    /// Set when a size request moved an item, which is what makes the cached
    /// centres stale.
    geometry_changed: bool,
}

impl FisheyeFrame<'_> {
    /// Grows the glass reflection with a magnified icon, keeping the band fixed.
    ///
    /// # Safety
    ///
    /// `item` must be a live item.
    unsafe fn apply_reflection_extent(&self, item: &DockItem, icon_px: c_int) {
        if item.reflection.is_null() || !self.shelf_active {
            return;
        }

        let band_px = (f64::from(self.base_size_px) * self.band_ratio).round() as c_int;
        if band_px > 0 {
            shelf_reflection_set_extent(item.reflection.cast(), icon_px, band_px);
        }
    }

    /// Sizes one dock item for this frame.
    ///
    /// # Safety
    ///
    /// `item` must be a live item whose widgets are built.
    unsafe fn apply(&mut self, index: usize, item: &DockItem) {
        // Separators and meters do not magnify, and a tile without a zoom
        // container has nothing to scale.
        if item.hover_animate == 0 || item.button.is_null() {
            return;
        }
        let Some(button) = widget_of(item.button) else {
            return;
        };
        if !button.is::<gtk4::Button>() {
            return;
        }

        let zoom = item_zoom_widget(item);
        if zoom.is_null() {
            return;
        }

        // The centres are per item, in row order, and a frame that ran past them
        // has nothing to place.
        let Some(centre) = self.centres.get(index).copied() else {
            return;
        };
        let centre = f64::from(centre);
        let scale = lambdock_fisheye_scale(self.pointer_axis - centre, self.radius_px, self.max_scale);
        let target_px = lambdock_scaled_size(self.base_size_px, scale);

        // The paint follows the pointer continuously (below); only the layout
        // steps. Coarsening the magnified sizes cuts the row's re-measures by
        // about half, and the resting size stays exact.
        let layout_px = if target_px > self.base_size_px {
            lambdock_size_step(target_px, FISHEYE_LAYOUT_STEP_PX)
        } else {
            target_px
        };

        if lambdock_icon_zoom_target_size(zoom) != layout_px {
            lambdock_stats_bump(LAYOUT_STEPS);

            // Expand along the dock axis; preserve the base size across it.
            if self.horizontal {
                button.set_size_request(layout_px, self.base_size_px);
            } else {
                button.set_size_request(self.base_size_px, layout_px);
            }

            // Mirror the magnified icon in its reflection: the copy grows along
            // the dock axis but keeps the band thickness, so the row of icons
            // does not shift while hovering.
            self.apply_reflection_extent(item, layout_px);

            self.geometry_changed = true;
        }

        // The layout steps in whole pixels, the icon does not: the zoom paints
        // the artwork through a transform, so magnification follows the pointer
        // continuously and costs no rasterisation at all.
        lambdock_icon_zoom_set_scale(zoom, scale);
    }
}

/// Rebuilds the cache of item centres along the dock's main axis.
///
/// One walk of the row (a bounds query per item) instead of one per pointer
/// frame: the fisheye needs to know where each item *is*, and that only changes
/// when something resizes the row -- an icon size, the item list, a mode change,
/// or the magnification itself stepping to a new size.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn refresh_item_centres(dock: &mut LambdockState) {
    let horizontal = is_horizontal(dock);

    // The row is the reference, not the window: it is the frame the items live
    // in, and measuring against it is a shorter walk than measuring against the
    // surface. The pointer arrives in window coordinates, so the row's own
    // origin is what has to be subtracted from it.
    let reference = if dock.box_.is_none() {
        dock.window.as_ref()
    } else {
        dock.box_.as_ref()
    };

    dock.box_origin_px = 0.0;
    if let (Some(window), Some(reference)) = (dock.window.as_ref(), reference) {
        if reference.as_ptr() != window.as_ptr() {
            if let Some(origin) = reference.compute_bounds(window) {
                dock.box_origin_px = if horizontal {
                    f64::from(origin.x())
                } else {
                    f64::from(origin.y())
                };
            }
        }
    }

    dock.item_centres = dock
        .dock_items
        .iter()
        .map(|item| {
            let (Some(item), Some(reference)) = (item.as_ref(), reference) else {
                return 0;
            };
            let Some(button) = widget_of(item.button) else {
                return 0;
            };
            let Some(bounds) = button.compute_bounds(reference) else {
                return 0;
            };

            if horizontal {
                (f64::from(bounds.x()) + f64::from(bounds.width()) / 2.0).round() as c_int
            } else {
                (f64::from(bounds.y()) + f64::from(bounds.height()) / 2.0).round() as c_int
            }
        })
        .collect();

    dock.centres_dirty = 0;
    lambdock_stats_bump(GEOMETRY_REFRESHES);
}

/// Applies the fisheye for a pointer at (`x`, `y`) in window coordinates.
///
/// This is the whole hover path: a pointer frame costs one cached geometry
/// lookup and some arithmetic per item, plus -- only when an item's layout step
/// actually changed -- one size request. The magnification itself is painted
/// through a transform, so the row does not relayout to look magnified.
///
/// It only applies to a settled, visible dock: while it slides (or is hidden) the
/// widget bounds are stale and the pointer position is not the user's.
///
/// # Safety
///
/// `dock` must be a live dock.
pub unsafe fn dock_apply_hover_at(dock: &mut LambdockState, x: f64, y: f64) {

    if dock.dock_items.is_empty()
        || dock.hover_animate == 0
        || crate::accessibility::lambdock_animations_enabled(dock) == 0
        || dock.is_hidden != 0
        || dock.tick_cb_id != 0
        || dock.current_margin != 0
    {
        return;
    }

    lambdock_stats_bump(HOVER_FRAMES);

    let horizontal = is_horizontal(dock);
    let axis = if horizontal { x } else { y };

    // A trackpad reports sub-pixel movement continuously, and half a pixel
    // changes nothing anyone can see: the icons would be repainted at the same
    // layout step and a magnification nobody can perceive. The frame is dropped
    // before it touches a widget.
    if dock.hover_axis_valid != 0 && (axis - dock.applied_hover_axis).abs() < FISHEYE_POINTER_STEP_PX
    {
        lambdock_stats_bump(HOVER_SKIPS);
        return;
    }

    dock.hover_axis_valid = 1;
    dock.applied_hover_axis = axis;

    // What one hover frame costs *us*, as opposed to what GTK spends laying the
    // row out and painting it afterwards: the two are worth telling apart before
    // optimising either.
    let started_us = glib::ffi::g_get_monotonic_time();

    if dock.centres_dirty != 0 || dock.item_centres.len() != dock.dock_items.len() {
        refresh_item_centres(dock);
    }

    let shelf_active = lambdock_shelf_active(dock) != 0;
    let band_ratio = if shelf_active {
        lambdock_shelf_params_effective(dock).reflection_ratio
    } else {
        0.0
    };
    let mut frame = FisheyeFrame {
        // The centres are relative to the row, the pointer to the window.
        pointer_axis: axis - dock.box_origin_px,
        base_size_px: dock_icon_size(dock),
        radius_px: FISHEYE_RADIUS_PX,
        max_scale: FISHEYE_MAX_SCALE,
        centres: &dock.item_centres,
        horizontal,
        shelf_active,
        band_ratio,
        geometry_changed: false,
    };

    for (index, item) in dock.dock_items.iter().enumerate() {
        if let Some(item) = item.as_ref() {
            frame.apply(index, item);
        }
    }

    if frame.geometry_changed {
        dock.centres_dirty = 1;
    }

    // The capsule grows under the pointer, so the clickable region has to follow
    // it; the tick that sizes the surface is the same one that cuts it out.
    dock.fisheye_active = 1;
    dock_schedule_input_region(dock);

    lambdock_stats_add(
        HOVER_US,
        (glib::ffi::g_get_monotonic_time() - started_us) as u64,
    );
}

// ===========================================================================
// APPLICATION ACTIONS (QUIT AND ABOUT)
// ===========================================================================

/// Where the live About dialog hangs off the application.
///
/// The dialog is created from an app-level action and must be singular. The live
/// instance lives on the `GtkApplication` -- the object that outlives every
/// window here -- instead of a file-scope pointer, so there is no hidden global
/// and its lifetime is the application's.
const ABOUT_DIALOG_KEY: &str = "lambdock-about-dialog";

/// The bundled logo, and the icon name to fall back on.
const ABOUT_LOGO_RESOURCE: &str = "/org/jointhefreeworld/lambdock/img/agnus-dei.png";

/// Terminates the GTK application loop.
///
/// # Safety
///
/// `user_data` must be the live `GtkApplication`.
pub unsafe fn on_quit_action(
    _action: *mut gtk4::gio::ffi::GSimpleAction,
    _parameter: *mut glib::ffi::GVariant,
    user_data: *mut c_void,
) {
    if user_data.is_null() {
        return;
    }

    let app: gtk4::Application = from_glib_none(user_data.cast());
    app.quit();
}

/// The open About dialog, if there is one.
fn about_dialog_open(app: &gtk4::Application) -> Option<gtk4::Window> {
    let dialog = unsafe { app.data::<gtk4::Window>(ABOUT_DIALOG_KEY) }?;
    let dialog = unsafe { dialog.as_ref() }.clone();

    // A dialog that is being destroyed still answers its data key for a moment:
    // the type check the C did is the `is` below.
    dialog.is::<gtk4::Window>().then_some(dialog)
}

/// Fills the identity fields: name, version, description, copyright.
fn about_dialog_set_identity(about: &gtk4::AboutDialog) {
    about.set_program_name(Some("lambdock"));

    // The version meson stamped into the C half: `option_env!` keeps a build
    // without meson (a bare `cargo test`) from claiming one.
    if let Some(version) = option_env!("LAMBDOCK_PACKAGE_VERSION") {
        about.set_version(Some(version));
    }

    about.set_comments(Some(&translated(
        c"A modern Wayland-native hackable desktop dock bar.\n\nEngineered with the performance of C and GTK4 Layer Shell for smooth auto-hide and animations, featuring dynamic Wayland toplevel window tracking, live CSS hot-reloading, and dynamic configuration powered by GNU Guile Scheme.",
    )));

    about.set_copyright(Some("Copyright © Josep Bigorra <jjbigorra@gmail.com>"));
}

/// Fills the credits: authors, licence and project links.
fn about_dialog_set_credits(about: &gtk4::AboutDialog) {
    about.set_authors(&["Josep Bigorra (jjbigorra@gmail.com)"]);
    about.set_license_type(gtk4::License::MitX11);
    about.set_website(Some("https://github.com/jjba23/lambdock"));
    about.set_website_label(&translated(c"Find the project's source code here"));
}

/// Uses the bundled logo when it loads, and the icon theme otherwise.
fn about_dialog_set_logo(about: &gtk4::AboutDialog) {
    // The gtk-rs wrapper of `gdk_texture_new_from_resource` assumes the
    // resource exists; the C checked, so this does too.
    let resource = cstring(ABOUT_LOGO_RESOURCE);
    let texture = unsafe { gdk::ffi::gdk_texture_new_from_resource(resource.as_ptr()) };

    if texture.is_null() {
        crate::log_warn!(
            "About",
            "Failed to load logo from GResource: {ABOUT_LOGO_RESOURCE}"
        );
        about.set_logo_icon_name(Some("lambdock"));
        return;
    }

    let texture: gdk::Texture = unsafe { from_glib_full(texture) };
    about.set_logo(Some(&texture));
}

/// Builds the dialog, registers it as the live instance, and returns it.
fn about_dialog_build(app: &gtk4::Application) -> gtk4::Window {
    let about = gtk4::AboutDialog::new();

    about.add_css_class("about-dialog");
    about_dialog_set_identity(&about);
    about_dialog_set_credits(&about);
    about_dialog_set_logo(&about);
    about.set_modal(true);

    // The dialog forgets itself before it goes away, so the next invocation of
    // the action builds a fresh one.
    let app_for_close = app.clone();
    about.connect_close_request(move |about| {
        unsafe { app_for_close.set_data(ABOUT_DIALOG_KEY, Option::<gtk4::Window>::None) };
        about.destroy();

        glib::Propagation::Stop
    });

    unsafe { app.set_data(ABOUT_DIALOG_KEY, about.clone().upcast::<gtk4::Window>()) };

    about.upcast()
}

/// Shows the About dialog, reusing the one already open if there is one.
///
/// # Safety
///
/// `user_data` must be the live `GtkApplication`.
pub unsafe fn on_about_action(
    _action: *mut gtk4::gio::ffi::GSimpleAction,
    _parameter: *mut glib::ffi::GVariant,
    user_data: *mut c_void,
) {
    if user_data.is_null() {
        return;
    }

    let app: gtk4::Application = from_glib_none(user_data.cast());

    let about = match about_dialog_open(&app) {
        Some(about) => about,
        None => about_dialog_build(&app),
    };

    about.present();
}

// ===========================================================================
// MEMORY AND DESTRUCTORS
// ===========================================================================
//
// `destroy_dock_instance` is a sequence of named steps, one per kind of resource
// a dock owns. Each step checks and clears its own pointers, so the body reads as
// an inventory and no step can run twice by accident.

/// Removes and drops this dock's CSS providers.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn destroy_css_providers(dock: &mut LambdockState) {
    let Some(display) = gdk::Display::default() else {
        return;
    };

    for provider in [
        &mut dock.base_css_provider,
        &mut dock.taskbar_css_provider,
        &mut dock.shelf_css_provider,
        &mut dock.user_css_provider,
    ] {
        if let Some(css) = provider.take() {
            gtk4::style_context_remove_provider_for_display(&display, &css);
        }
    }
}

/// Stops the auto-hide timeout and the frame callbacks this dock owns.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn destroy_timers(dock: &mut LambdockState) {
    cancel_hide_timer_inner(dock);

    if dock.tick_cb_id != 0 {
        if let Some(window) = dock.window.as_ref() {
            gtk4::ffi::gtk_widget_remove_tick_callback(window.as_ptr().cast(), dock.tick_cb_id);
        }
    }
    dock.tick_cb_id = 0;

    stop_pointer_tick_inner(dock);
}

/// Detaches every handler carrying `state`, so teardown cannot re-enter.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn detach_window_handlers(dock: &mut LambdockState) {
    if let Some(window) = dock.window.as_ref() {
        disconnect_handlers_by_data(
            window.as_ptr().cast(),
            (dock as *mut LambdockState).cast(),
        );
    }

    // The surface outlives this state: it is GTK's, it is destroyed after the
    // last signal has been emitted for it, and a `layout` or a resize during
    // that teardown reaches the handler with a dangling state. Dropping the
    // handlers here is why the X11 placement does not need a liveness check.
    let handlers = take_surface_handlers(dock as *const LambdockState);
    if !dock.x11_surface.is_null() {
        let surface: gdk::Surface = from_glib_none(dock.x11_surface.cast::<gdk::ffi::GdkSurface>());
        for handler in handlers {
            surface.disconnect(handler);
        }

        dock.x11_surface = std::ptr::null_mut();
    }
}

/// Unparents the background context menu without letting its handlers run.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn destroy_background_popover(dock: &mut LambdockState) {
    let Some(popover) = dock.popover.clone() else {
        return;
    };

    // The popover's "closed" handler carries `state` and would schedule a hide
    // timer on this soon-to-be-freed state; disconnect it before unparenting so
    // no callback runs against freed memory.
    disconnect_handlers_by_data(
        popover.as_ptr().cast(),
        (dock as *mut LambdockState).cast(),
    );

    if crate::ui::parent_of(&popover).is_some() {
        popover.unparent();
    }

    dock.popover = None;
}

/// Frees the dock items, and their individual polling timers and popovers.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn destroy_dock_items(dock: &mut LambdockState) {
    // Taken first: freeing an item may call back.
    let items = std::mem::take(&mut dock.dock_items);

    for item in items {
        free_dock_item(item);
    }
}

/// Frees the Wayland toplevel window trackers.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn destroy_window_trackers(dock: &mut LambdockState) {
    // Taken first: freeing a record may call back.
    for tracker in std::mem::take(&mut dock.window_trackers) {
        crate::toplevel_tracker::free_window_tracker(tracker.cast());
    }
}

/// Destroys the GTK window, safing the widget pointers before GTK can emit
/// "destroy".
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn destroy_window(dock: &mut LambdockState) {
    // Cloned so the borrow of `dock` ends before the fields are cleared.
    let Some(window) = dock
        .window
        .as_ref()
        .and_then(|widget| widget.downcast_ref::<gtk4::Window>())
        .cloned()
    else {
        return;
    };

    dock.window = None;
    dock.bg_shelf = None;
    dock.bg_capsule = None;

    if layer_shell::is_layer_window(&window) {
        layer_shell::set_monitor(&window, std::ptr::null_mut());
    }

    window.destroy();
}

/// Drops the borrowed monitor reference, and its handlers, together.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn destroy_monitor(dock: &mut LambdockState) {
    let Some(monitor) = dock.monitor.take() else {
        return;
    };

    disconnect_handlers_by_data(
        monitor.as_ptr().cast(),
        (dock as *mut LambdockState).cast(),
    );
}

/// Unparents popovers and widgets, cleans up window trackers and dock items, and
/// destroys the GTK window safely without disposal warnings.
///
/// Takes the state with it: the pointer is invalid on return.
///
/// # Safety
///
/// `state` must be null or a live dock this module created.
pub unsafe fn destroy_dock_instance(state: *mut LambdockState) {
    let Some(dock) = state.as_mut() else {
        return;
    };

    // Re-entrancy guard: a "destroy" signal emitted while this runs must not
    // free the state a second time.
    if dock.is_destroying != 0 {
        return;
    }
    dock.is_destroying = 1;

    destroy_css_providers(dock);
    if !dock.active_theme_class.is_null() {
        crate::glib::g_free(dock.active_theme_class.cast());
        dock.active_theme_class = std::ptr::null_mut();
    }

    // The launchpad is a global overlay that belongs to no single dock: take it
    // down with the dock it was opened from (it is recreated lazily).
    crate::ui::launchpad::lambdock_launchpad_shutdown();

    crate::dock_registry::dock_registry_remove(state);

    destroy_timers(dock);

    // Window handlers go before the widgets: nothing they carry may fire while
    // the tree below is being taken apart.
    detach_window_handlers(dock);
    destroy_background_popover(dock);
    destroy_dock_items(dock);
    destroy_window_trackers(dock);
    destroy_window(dock);

    dock.size_group = None;
    dock.item_centres = Vec::new();
    destroy_monitor(dock);

    for text in [&mut dock.config_path, &mut dock.theme] {
        if !text.is_null() {
            let pointer = std::mem::replace(text, std::ptr::null_mut());
            crate::glib::g_free(pointer.cast());
        }
    }

    // The window id and the rectangle it had are the X11 placement's memory: a
    // dock that is rebuilt gets a new window, and it has to be placed again.
    lambdock_x11_dock_forget(state);

    // Back to the box the dock was built in: dropping it releases the widget and
    // provider references, in field order.
    drop(Box::from_raw(state));
}

// ===========================================================================
// THE RECONNECTING STATE
// ===========================================================================
//
// Window tracking is a listener: when the compositor restarts, its
// foreign-toplevel proxies disappear with it and the tracker binds the new
// manager when it is announced. The dock's items are configuration, not tracking
// state, so they are left where they are; the label is the only visible
// difference, and it says why nothing is being tracked right now.

/// Shows or hides the "reconnecting" label, and the class that dims the dock.
///
/// # Safety
///
/// `dock` must be a live dock.
pub unsafe fn dock_window_set_tracker_connected(
    dock: &mut LambdockState,
    connected: bool,
) {

    dock.tracker_connected = c_int::from(connected);

    if let Some(label) = dock.reconnect_label.as_ref() {
        label.set_visible(!connected);
    }

    if let Some(window) = window_of(dock) {
        if connected {
            window.remove_css_class("tracker-disconnected");
        } else {
            window.add_css_class("tracker-disconnected");
        }
    }
}

// ===========================================================================
// THE POINTER
// ===========================================================================
//
// The callbacks here are adapters: read a pointer position, call one helper,
// return. The decisions they encode are documented where they are made.

/// `AUTOHIDE_LEAVE_DELAY_MS` in `lambdock.h`: how long the pointer has to be
/// away before the hide countdown starts.
const AUTOHIDE_LEAVE_DELAY_MS: u32 = 600;

thread_local! {
    /// The `GdkSurface` handlers each dock connected, keyed by the dock's
    /// address.
    ///
    /// The state mirror has no room for signal ids, and the surface outlives the
    /// state it reports to -- GTK destroys it after the last signal has been
    /// emitted for it -- so teardown has to be able to drop these by name.
    static SURFACE_HANDLERS: RefCell<Vec<(usize, Vec<glib::SignalHandlerId>)>> =
        const { RefCell::new(Vec::new()) };
}

/// Remembers the handlers connected to a dock's surface.
fn store_surface_handlers(state: *const LambdockState, ids: Vec<glib::SignalHandlerId>) {
    SURFACE_HANDLERS.with_borrow_mut(|handlers| handlers.push((state as usize, ids)));
}

/// Takes a dock's surface handlers back, so teardown can disconnect them.
fn take_surface_handlers(state: *const LambdockState) -> Vec<glib::SignalHandlerId> {
    SURFACE_HANDLERS.with_borrow_mut(|handlers| {
        handlers
            .iter()
            .position(|(key, _)| *key == state as usize)
            .map(|index| handlers.remove(index).1)
            .unwrap_or_default()
    })
}

/// The pointer entered the dock: whatever was queued is stale, and the dock has
/// to be revealed.
unsafe fn on_motion_enter(state: *mut LambdockState, x: f64, y: f64) {
    let Some(dock) = state.as_mut() else {
        return;
    };

    trace(&format!(
        "enter at {x:.1},{y:.1} (hidden {}, margin {})",
        dock.is_hidden, dock.current_margin
    ));

    dock.pointer_applied = 0;

    // Make room for the spread before it starts: sizing the surface now, with
    // the row still at rest, is what keeps the hover itself free of resizes.
    dock_reserve_hover_room(dock);
    reveal_dock(dock);
}

/// The newest pointer position, applied to the fisheye once per frame.
///
/// Motion events can arrive several times per frame (high-rate mice, libinput
/// batching, a compositor that coalesces late), and every one of them used to
/// resize the whole row of icons and repaint the dock. Applying only the latest
/// position keeps the work proportional to the frame rate instead of to the
/// event rate, and it does so right before the frame is laid out, so the icons
/// are never a frame behind the pointer.
unsafe fn on_hover_tick(state: *mut LambdockState) {
    let Some(dock) = state.as_mut() else {
        return;
    };

    // One-shot: the next motion schedules the next one.
    dock.pointer_tick_id = 0;

    // Nothing moved since the last frame (the pointer can repeat a position),
    // so the icons are already where they belong.
    if dock.pointer_applied != 0
        && lambdock_points_differ(
            dock.pointer_x,
            dock.pointer_y,
            dock.applied_pointer_x,
            dock.applied_pointer_y,
            1.0,
        ) == 0
    {
        return;
    }

    dock.applied_pointer_x = dock.pointer_x;
    dock.applied_pointer_y = dock.pointer_y;
    dock.pointer_applied = 1;

    let (x, y) = (dock.pointer_x, dock.pointer_y);
    dock_apply_hover_at(dock, x, y);
}

/// Adapter: GLib wants a frame-clock callback, this module wants a dock.
unsafe extern "C" fn hover_tick(
    _widget: *mut gtk4::ffi::GtkWidget,
    _clock: *mut gdk::ffi::GdkFrameClock,
    user_data: *mut c_void,
) -> c_int {
    on_hover_tick(user_data.cast());

    glib::ffi::GFALSE
}

/// The pointer left the dock: stop magnifying, and start the hide countdown.
unsafe fn on_motion_leave(state: *mut LambdockState) {
    let Some(dock) = state.as_mut() else {
        return;
    };

    // Not every leave means the pointer went anywhere. The dock slides out from
    // under a pointer that is not moving -- the window moves, the pointer does
    // not -- and compositors report a crossing that follows from that, which is
    // what used to bounce the dock at its screen edge: a leave hides it, the
    // revealing strip comes back under the pointer, an enter reveals it, and
    // round it goes. Those echoes all arrive while the dock is moving, or within
    // a frame or two of it stopping, so the settle window tells them apart.
    //
    // It used to also require that the pointer had moved since it entered, on
    // the reasoning that a pointer that has not moved cannot have left. It can:
    // a pointer that touches the edge to reveal the dock and then moves away has
    // been still for the whole reveal, and its leave was thrown away with it, so
    // the dock stayed out until the pointer came back and did the whole thing
    // again -- auto-hide that quietly stops hiding.
    if dock_is_settling(dock) != 0 {
        trace("leave ignored: the dock is sliding, or just stopped");
        return;
    }

    trace(&format!("leave: hiding in {AUTOHIDE_LEAVE_DELAY_MS} ms"));

    stop_pointer_tick_inner(dock);
    dock.pointer_applied = 0;

    if dock.hover_animate != 0 {
        reset_dock_items_scale_inner(dock);
    }

    if dock.auto_hide != 0 {
        schedule_hide_timer(dock, AUTOHIDE_LEAVE_DELAY_MS);
    }
}

/// Records where the pointer is; the frame clock does the magnifying.
///
/// It is also the dock's *reveal* trigger, which is the one thing about it that
/// is not obvious. A pointer only has to cross into the dock once to be inside
/// it, and a hidden dock is on screen only as the trigger strip: a pointer that
/// rests on the strip while the surface slides away underneath it never leaves
/// -- the compositor keeps sending it motion, not a crossing -- so it can never
/// cross back in. Motion inside the surface while the dock is away means the
/// same thing an enter would: the pointer is on the strip, and the dock belongs
/// on screen.
unsafe fn on_motion_notify(state: *mut LambdockState, x: f64, y: f64) {
    let Some(dock) = state.as_mut() else {
        return;
    };

    if dock.is_hidden != 0 {
        trace(&format!("motion at {x:.0},{y:.0} on the strip: revealing"));
        reveal_dock(dock);
        return;
    }

    if dock.hover_animate == 0 || dock.tick_cb_id != 0 || dock.current_margin != 0 {
        return;
    }

    dock.pointer_x = x;
    dock.pointer_y = y;

    if dock.pointer_tick_id == 0 {
        if let Some(window) = dock.window.as_ref() {
            dock.pointer_tick_id = gtk4::ffi::gtk_widget_add_tick_callback(
                window.as_ptr().cast(),
                Some(hover_tick),
                (dock as *mut LambdockState).cast(),
                None,
            );
        }
    }
}

/// Applies the input region whenever the dock window (re)appears, and wires the
/// surface's own size signals the first time it does.
unsafe fn on_window_map(state: *mut LambdockState) {
    let Some(dock) = state.as_mut() else {
        return;
    };
    dock_schedule_input_region(dock);

    // The window id only exists once the surface is realised, so this is the
    // first moment an X11 dock can be typed, moved and given its strut. On
    // Wayland the placement is the compositor's, and this changes nothing.
    let surface = state
        .as_ref()
        .filter(|dock| dock.x11_surface.is_null())
        .and_then(|dock| dock.window.as_ref())
        .and_then(|window| surface_of(window));

    if let Some(surface) = surface {
        // `layout` is the surface telling us it was laid out; the size
        // notifications cover a resize that arrives without a layout.
        let pointer = state;
        let layout = surface.connect_layout(move |_, _, _| lambdock_x11_dock_apply(pointer));

        let pointer = state;
        let width = surface.connect_width_notify(move |_| lambdock_x11_dock_apply(pointer));

        let pointer = state;
        let height = surface.connect_height_notify(move |_| lambdock_x11_dock_apply(pointer));

        store_surface_handlers(state, vec![layout, width, height]);

        if let Some(dock) = state.as_mut() {
            dock.x11_surface = surface.as_ptr().cast();
        }
    }

    lambdock_x11_dock_apply(state);
}

/// Attaches a motion controller to the dock's window: entry, leave and motion.
///
/// # Safety
///
/// `dock` must be a live dock with its window built.
pub unsafe fn setup_dock_motion_controller(dock: &mut LambdockState) {
    // The handlers outlive this call, so they carry the pointer rather than the
    // borrow it came from.
    let pointer = dock as *mut LambdockState;

    let Some(window) = dock.window.as_ref() else {
        return;
    };

    let motion = gtk4::EventControllerMotion::new();
    motion.connect_enter(move |_, x, y| on_motion_enter(pointer, x, y));
    motion.connect_leave(move |_| on_motion_leave(pointer));
    motion.connect_motion(move |_, x, y| on_motion_notify(pointer, x, y));
    window.add_controller(motion);

    // The surface is bigger than the dock, so its clickable area has to be cut
    // down to the dock every time the window appears.
    window.connect_map(move |_| on_window_map(pointer));

    dock_schedule_input_region(dock);
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The clickable rectangle is the bounds rounded outwards, one pixel of
    /// slack per side so a rounded corner still takes a click.
    #[test]
    fn the_input_region_has_a_pixel_of_slack() {
        assert_eq!(slack_rect(10.4, 20.6, 100.0, 48.0), (9, 19, 102, 50));
        assert_eq!(slack_rect(0.0, 0.0, 1.0, 1.0), (-1, -1, 3, 3));
        // A fractional size rounds up: half a pixel of the dock must stay
        // clickable.
        assert_eq!(slack_rect(3.2, 4.8, 12.1, 7.5), (2, 3, 15, 10));
    }

    /// A dock anchored to one edge anchors to exactly that edge, and a taskbar
    /// stretches across the other two.
    #[test]
    fn the_surface_policy_follows_the_edge_and_the_mode() {
        let mut state = LambdockState {
            edge: EDGE_BOTTOM,
            ..LambdockState::default()
        };
        let policy = dock_surface_policy_for(&state);
        assert_eq!(policy.anchor_bottom, 1);
        assert_eq!(
            (policy.anchor_top, policy.anchor_left, policy.anchor_right),
            (0, 0, 0)
        );

        // A taskbar fills the perpendicular axis, so it anchors both of those
        // edges too -- and a floating dock does not.
        state.taskbar = 1;
        let policy = dock_surface_policy_for(&state);
        assert_eq!((policy.anchor_left, policy.anchor_right), (1, 1));
        assert_eq!(policy.anchor_top, 0);

        // The same rule turned sideways: a left-hand dock wakes up its top and
        // bottom anchors instead.
        state.edge = EDGE_LEFT;
        let policy = dock_surface_policy_for(&state);
        assert_eq!((policy.anchor_top, policy.anchor_bottom), (1, 1));
        assert_eq!(policy.anchor_right, 0);
    }

    /// An auto-hiding dock reserves no desktop space; a resting one reserves its
    /// own capsule thickness.
    #[test]
    fn an_auto_hiding_dock_reserves_nothing() {
        let mut state = LambdockState {
            edge: EDGE_BOTTOM,
            icon_size: 48,
            ..LambdockState::default()
        };

        state.auto_hide = 0;
        let resting = dock_surface_policy_for(&state);
        assert!(resting.exclusive_zone_px > 0);

        state.auto_hide = 1;
        let hiding = dock_surface_policy_for(&state);
        assert_eq!(hiding.exclusive_zone_px, 0);
    }

    /// The shelf names and leans its edge: the edge on the screen stays pinned,
    /// so top and left tilt backwards.
    #[test]
    fn the_shelf_knows_which_edge_it_is_on() {
        assert_eq!(shelf_edge_class(EDGE_TOP), "shelf-top");
        assert_eq!(shelf_edge_class(EDGE_LEFT), "shelf-left");
        assert_eq!(shelf_edge_class(EDGE_RIGHT), "shelf-right");
        assert_eq!(shelf_edge_class(EDGE_BOTTOM), "shelf-bottom");

        assert_eq!(shelf_tilt_for_edge(EDGE_TOP, 12.0), -12.0);
        assert_eq!(shelf_tilt_for_edge(EDGE_LEFT, 12.0), -12.0);
        assert_eq!(shelf_tilt_for_edge(EDGE_RIGHT, 12.0), 12.0);
        assert_eq!(shelf_tilt_for_edge(EDGE_BOTTOM, 12.0), 12.0);

        assert_eq!(cross_orientation(EDGE_TOP), gtk4::Orientation::Vertical);
        assert_eq!(cross_orientation(EDGE_BOTTOM), gtk4::Orientation::Vertical);
        assert_eq!(cross_orientation(EDGE_LEFT), gtk4::Orientation::Horizontal);
        assert_eq!(cross_orientation(EDGE_RIGHT), gtk4::Orientation::Horizontal);
    }

    /// The vertical dock is the one whose orientation says so; everything else
    /// is horizontal, which is how a partly built state already behaved.
    #[test]
    fn the_orientation_is_horizontal_unless_it_says_vertical() {
        let mut state = LambdockState::default();
        assert!(is_horizontal(&state));

        state.orientation = gtk4::Orientation::Vertical.into_glib();
        assert!(!is_horizontal(&state));
    }
}
