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
use gtk4::glib::translate::{from_glib_borrow, Borrowed, IntoGlib};
use gtk4::prelude::*;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_uint, c_void};

use crate::app_window::GList;
use crate::core::dock_dims::{lambdock_dock_dims_compute, DockDims};
use crate::core::dock_motion::{
    lambdock_dock_needs_full_input, lambdock_hide_margin, lambdock_settling, lambdock_slide_position,
    lambdock_slide_progress,
};
use crate::core::render_stats::{lambdock_stats_bump, INPUT_REGION_SETS, SURFACE_RESIZES};
use crate::dock_item::{dock_items_refresh_polls, DockItem};
use crate::state::LambdockState;
use crate::ui::icon_zoom::{lambdock_icon_zoom_is_zoom, lambdock_icon_zoom_set_scale};
use crate::ui::layer_shell::{self, EDGE_BOTTOM, EDGE_LEFT, EDGE_RIGHT, EDGE_TOP, KEYBOARD_MODE_ON_DEMAND, LAYER_TOP};
use crate::ui::shelf::{
    apply_shelf_layout, lambdock_shelf_active, lambdock_shelf_geometry_for, shelf_reflection_set_icon_size,
};
use crate::ui::DEFAULT_ICON_SIZE;
use crate::x11_tracker::lambdock_x11_dock_apply;

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
unsafe fn widget_of(pointer: *mut c_void) -> Option<Borrowed<gtk4::Widget>> {
    if pointer.is_null() {
        return None;
    }

    Some(crate::ui::widget_from_pointer(pointer.cast()))
}

/// The dock's size group, which is not a widget: a plain `GObject` that only
/// ever groups widgets.
///
/// # Safety
///
/// `pointer` must be null or a live `GtkSizeGroup`.
unsafe fn size_group_of(pointer: *mut c_void) -> Option<Borrowed<gtk4::SizeGroup>> {
    if pointer.is_null() {
        return None;
    }

    Some(gtk4::glib::translate::from_glib_borrow(pointer.cast()))
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
fn orientation_of(state: &LambdockState) -> gtk4::Orientation {
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
    let window = widget_of(state.window)?;
    let surface = surface_of(&window)?;

    gdk::Display::default()?.monitor_at_surface(&surface)
}

/// The dock's monitor size, falling back to the surface's and then to the first.
///
/// # Safety
///
/// `state` must be a live dock.
unsafe fn monitor_size(state: &LambdockState) -> (c_int, c_int) {
    let monitor = state.monitor.cast::<gdk::ffi::GdkMonitor>();

    // A monitor that has been unplugged is a dangling pointer: only trust one
    // the display still lists.
    let monitor = if !monitor.is_null() && layer_shell::is_monitor_valid(monitor) != 0 {
        gtk4::glib::translate::from_glib_none::<_, gdk::Monitor>(monitor)
    } else {
        // A dock with no monitor of its own is placed by the compositor, which
        // may well be a monitor that is not the first one: measure the surface's.
        match surface_monitor(state).or_else(first_monitor) {
            Some(monitor) => monitor,
            None => return (0, 0),
        }
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
    let Some(group) = size_group_of(state.size_group) else {
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
    let Some(scrolled) = widget_of(state.scrolled_window) else {
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
unsafe fn apply_box_margins(state: &LambdockState, dims: &DockDims, horizontal: bool) {
    let Some(box_) = widget_of(state.box_) else {
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
        state.edge == EDGE_TOP
    } else {
        state.edge == EDGE_LEFT
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
/// `state` must be null or a live dock.
unsafe fn content_main_px(state: *const LambdockState, horizontal: bool) -> c_int {
    let Some(state) = state.as_ref() else {
        return 0;
    };
    let Some(scrolled) = widget_of(state.scrolled_window) else {
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
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn dock_reserve_hover_room(state: *mut LambdockState) {
    let Some(dock) = state.as_ref() else {
        return;
    };
    let Some(overlay) = widget_of(dock.overlay) else {
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
        content_main_px(state, horizontal) + 2 * headroom
    } else {
        -1
    };

    let (width, height) = overlay.size_request();
    if horizontal {
        overlay.set_size_request(main_px, height);
    } else {
        overlay.set_size_request(width, main_px);
    }

    dock_schedule_input_region(state);
}

/// Cross-axis size requests for the overlay and the capsule.
///
/// # Safety
///
/// `state` must be a live dock with its widgets built.
unsafe fn apply_size_requests(state: &LambdockState, dims: &DockDims, horizontal: bool) {
    let (Some(overlay), Some(capsule)) = (widget_of(state.overlay), widget_of(state.bg_capsule)) else {
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
    let Some(scrolled) = widget_of(state.scrolled_window) else {
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
    let (Some(capsule), Some(box_)) = (widget_of(state.bg_capsule), widget_of(state.box_)) else {
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
    let (Some(shelf), Some(capsule)) = (widget_of(state.bg_shelf), widget_of(state.bg_capsule)) else {
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
unsafe fn input_region_is_current(state: &LambdockState, whole: bool, rect: (c_int, c_int, c_int, c_int)) -> bool {
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
    if let Some(window) = widget_of(state.window) {
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
/// `state` must be null or a live dock.
unsafe fn apply_dock_input_region(state: *mut LambdockState) -> bool {
    let Some(dock) = state.as_mut() else {
        return false;
    };
    let (Some(window), Some(shelf)) = (widget_of(dock.window), widget_of(dock.bg_shelf)) else {
        return false;
    };
    let Some(surface) = surface_of(&window) else {
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
    // slide moves the window every frame, so this is also the state to be in for
    // the whole animation). While the dock is away the whole surface is the
    // strip, so the whole surface keeps its input.
    if lambdock_dock_needs_full_input(dock.is_hidden, dock.current_margin) != 0 {
        apply_input_region(dock, &surface, true, (0, 0, 0, 0));
        return true;
    }

    // The dock is the slab plus the row of icons that overhangs it, so the region
    // has to be the union of the two: the slab alone would leave the icons -- the
    // part people actually click -- outside the clickable area.
    let Some(bounds) = shelf.compute_bounds(&*window) else {
        return false;
    };
    if bounds.width() < 1.0 || bounds.height() < 1.0 {
        return false;
    }

    let mut bounds = bounds;
    if let Some(row) = widget_of(dock.scrolled_window) {
        if let Some(row) = row.compute_bounds(&*window) {
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

    if apply_dock_input_region(state) {
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
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn dock_schedule_input_region(state: *mut LambdockState) {
    let Some(dock) = state.as_mut() else {
        return;
    };
    if dock.input_region_tick_id != 0 {
        return;
    }

    let Some(window) = widget_of(dock.window) else {
        return;
    };

    dock.input_region_tick_id = gtk4::ffi::gtk_widget_add_tick_callback(
        window.as_ptr().cast(),
        Some(on_input_region_tick),
        state.cast(),
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
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn update_dock_dimensions(state: *mut LambdockState) {
    let Some(dock) = state.as_mut() else {
        return;
    };
    if dock.bg_capsule.is_null() || dock.overlay.is_null() {
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
    dock_reserve_hover_room(state);
    apply_scroll_alignment(dock, horizontal);
    apply_capsule_alignment(dock, horizontal);

    // Size, show or hide the glass-shelf reflections so they always match the
    // current icon size and shelf parameters.
    apply_shelf_layout(state);
    mirror_shelf_alignment(dock);

    // Icon size, spacing, padding, mode and shelf all move the items: the hover
    // path's cached geometry is stale.
    dock.centres_dirty = 1;

    // The surface may be bigger than the dock now: keep the input region on the
    // dock itself.
    dock_schedule_input_region(state);

    // An X11 dock is moved and resized by hand, so a size change is also a
    // placement change. Idempotent: an unchanged rectangle is not re-applied.
    lambdock_x11_dock_apply(state);
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
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn dock_icon_size(state: *const LambdockState) -> c_int {
    match state.as_ref() {
        Some(state) if state.icon_size > 0 => state.icon_size,
        _ => DEFAULT_ICON_SIZE,
    }
}

/// The capsule's thickness for the current shelf parameters.
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn dock_capsule_cross_dim(state: *const LambdockState) -> c_int {
    lambdock_shelf_geometry_for(state, dock_icon_size(state)).capsule_px
}

/// The cross-axis size the overlay asks for: fisheye headroom, the outer glass,
/// and any icon overhang past the inner edge.
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn dock_cross_axis_dim(state: *const LambdockState) -> c_int {
    let icon_px = dock_icon_size(state);
    let geometry = lambdock_shelf_geometry_for(state, icon_px);
    let hover_magnify = state.as_ref().map_or(0, |state| state.hover_animate);

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
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn dock_is_settling(state: *const LambdockState) -> c_int {
    let Some(state) = state.as_ref() else {
        return 0;
    };

    lambdock_settling(
        c_int::from(state.tick_cb_id != 0),
        state.margin_anim_end_us,
        glib::ffi::g_get_monotonic_time(),
        AUTO_HIDE_SETTLE_MS,
    )
}

/// CSS class naming the screen edge a shelf is docked to.
#[no_mangle]
pub extern "C" fn shelf_edge_class(edge: c_int) -> *const c_char {
    match edge {
        EDGE_TOP => c"shelf-top".as_ptr(),
        EDGE_LEFT => c"shelf-left".as_ptr(),
        EDGE_RIGHT => c"shelf-right".as_ptr(),
        _ => c"shelf-bottom".as_ptr(),
    }
}

/// Sign of the shelf tilt: the edge touching the screen stays pinned while the
/// inner edge recedes away from the viewer, so top and left docks tilt backwards.
#[no_mangle]
pub extern "C" fn shelf_tilt_for_edge(edge: c_int, degrees: f64) -> f64 {
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
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn dock_surface_policy_for(state: *const LambdockState) -> DockSurfacePolicy {
    let Some(state) = state.as_ref() else {
        return DockSurfacePolicy::default();
    };

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
        if let Some(window) = widget_of(dock.window) {
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
    dock_schedule_input_region(dock as *mut LambdockState);
}

/// Drops a queued fisheye frame, so a stale position cannot be applied.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn stop_pointer_tick_inner(dock: &mut LambdockState) {
    if dock.pointer_tick_id != 0 {
        if let Some(window) = widget_of(dock.window) {
            gtk4::ffi::gtk_widget_remove_tick_callback(window.as_ptr().cast(), dock.pointer_tick_id);
        }
    }

    dock.pointer_tick_id = 0;
}

/// Drops a queued fisheye frame.
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn stop_pointer_tick(state: *mut LambdockState) {
    if let Some(dock) = state.as_mut() {
        stop_pointer_tick_inner(dock);
    }
}

/// Runs `visit` over the dock row, skipping null items.
///
/// `g_list_foreach` would do the walk, but it takes a `GFunc` over `gpointer`,
/// and casting dock-item callbacks to it gives up the type checking that keeps
/// these visitors honest. The loop is four lines; the signature is worth it.
///
/// # Safety
///
/// `items` must be null or a live `GList` of `DockItem`s; `visit` must be able
/// to take each of them.
#[no_mangle]
pub unsafe extern "C" fn for_each_dock_item(
    items: *mut GList,
    visit: Option<unsafe extern "C" fn(*mut DockItem, *mut c_void)>,
    data: *mut c_void,
) {
    let Some(visit) = visit else {
        return;
    };

    let mut node = items;
    while !node.is_null() {
        let item = (*node).data.cast::<DockItem>();
        if !item.is_null() {
            visit(item, data);
        }

        node = (*node).next;
    }
}

/// Runs `visit` over the dock row from Rust, skipping null items.
///
/// # Safety
///
/// `items` must be null or a live `GList` of `DockItem`s.
unsafe fn for_each_item(items: *mut GList, mut visit: impl FnMut(&DockItem)) {
    let mut node = items;

    while !node.is_null() {
        if let Some(item) = (*node).data.cast::<DockItem>().as_ref() {
            visit(item);
        }

        node = (*node).next;
    }
}

/// The fisheye zoom container of an item, the widget that scales the icon while
/// hovering, or null for items that have no icon (separators, meters).
///
/// # Safety
///
/// `item` must be null or a live item.
#[no_mangle]
pub unsafe extern "C" fn get_item_zoom_widget(item: *const DockItem) -> *mut gtk4::ffi::GtkWidget {
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
    if dock.dock_items.is_null() {
        return;
    }

    let horizontal = is_horizontal(dock);
    let base_px = dock_icon_size(dock as *const LambdockState);
    let items = dock.dock_items;

    for_each_item(items, |item| reset_item_scale(item, horizontal, base_px));

    // The row is back at rest, so the next frame has to apply the pointer again
    // even if it has not moved, and the surface's room can be re-measured.
    dock.pointer_applied = 0;
    dock.hover_axis_valid = 0;
    dock.fisheye_active = 0;

    // The row is back at rest: let the surface hug it again.
    dock_reserve_hover_room(dock as *mut LambdockState);
}

/// Resets every dock item to its base pixel size.
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn reset_dock_items_scale(state: *mut LambdockState) {
    if let Some(dock) = state.as_mut() {
        reset_dock_items_scale_inner(dock);
    }
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
        dock_schedule_input_region(user_data.cast());
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
    if crate::accessibility::lambdock_animations_enabled(dock as *const LambdockState) == 0 {
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
    dock_schedule_input_region(dock as *mut LambdockState);

    if dock.tick_cb_id == 0 {
        if let Some(window) = widget_of(dock.window) {
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
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn dock_apply_motion_preference(state: *mut LambdockState) {
    let Some(dock) = state.as_mut() else {
        return;
    };
    let pointer = dock as *const LambdockState;

    if dock.window.is_null() || crate::accessibility::lambdock_animations_enabled(pointer) != 0 {
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
    if let Some(dock) = user_data.cast::<LambdockState>().as_mut() {
        dock.hide_timer_id = 0;
    }

    hide_dock(user_data.cast());

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
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn cancel_hide_timer(state: *mut LambdockState) {
    if let Some(dock) = state.as_mut() {
        cancel_hide_timer_inner(dock);
    }
}

/// (Re)schedules the auto-hide timeout, replacing a pending one.
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn schedule_hide_timer(state: *mut LambdockState, delay_ms: c_uint) {
    let Some(dock) = state.as_mut() else {
        return;
    };

    cancel_hide_timer_inner(dock);
    dock.hide_timer_id = glib::ffi::g_timeout_add(delay_ms, Some(on_hide_timeout), state.cast());
}

/// The surface's current size along the dock's main axis, or 0 before GTK has
/// laid it out (which `lambdock_hide_margin` knows how to handle).
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn measured_main_axis_px(dock: &LambdockState) -> c_int {
    let Some(window) = widget_of(dock.window) else {
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
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn reveal_dock(state: *mut LambdockState) {
    let Some(dock) = state.as_mut() else {
        return;
    };

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
        dock_items_refresh_polls(state);
    } else {
        trace(&format!("reveal: already out (margin {})", dock.current_margin));
    }
}

/// Slides the dock off screen, keeping a trigger strip visible on the screen
/// edge so the pointer can bring it back.
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn hide_dock(state: *mut LambdockState) {
    let Some(dock) = state.as_mut() else {
        return;
    };

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
    if dock.show_when_clear != 0 && dock.window_trackers.is_null() {
        return;
    }

    // The row has to be at rest before its size is measured, or the measurement
    // is the magnified one.
    if dock.hover_animate != 0 {
        reset_dock_items_scale_inner(dock);
    }

    let measured_px = measured_main_axis_px(dock);
    let cross_px = dock_cross_axis_dim(state as *const LambdockState);
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
///
/// # Safety
///
/// `widget` must be null or a live widget.
unsafe fn set_class(widget: *mut c_void, class: &CStr, enabled: bool) {
    crate::ui::widget_utils::lambdock_set_css_class(
        widget.cast(),
        class.as_ptr(),
        c_int::from(enabled),
    );
}

/// The dock's window, borrowed, when it has one.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn window_of(dock: &LambdockState) -> Option<Borrowed<gtk4::Window>> {
    let widget = widget_of(dock.window)?;
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
    const EDGE_CLASSES: [&CStr; 4] = [c"shelf-bottom", c"shelf-top", c"shelf-left", c"shelf-right"];

    // Edges are mutually exclusive: clear them all, then set the current one.
    for widget in [dock.bg_capsule, dock.window] {
        for class in EDGE_CLASSES {
            set_class(widget, class, false);
        }
    }

    for widget in [dock.bg_capsule, dock.window] {
        set_class(widget, c"shelf-mode", shelf_active);
        // `shelf-mirror` means the glass actually shows icon reflections: the
        // reflection widget then draws the running indicator itself, so the CSS
        // one must stay quiet.
        set_class(widget, c"shelf-mirror", mirroring);
    }

    if !shelf_active {
        return;
    }

    // `shelf_edge_class` answers a static string, so it is read, not taken.
    let edge_class = CStr::from_ptr(shelf_edge_class(dock.edge));
    for widget in [dock.bg_capsule, dock.window] {
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
    if dock.bg_shelf.is_null() {
        return;
    }

    crate::ui::shelf::shelf_frame_set_shape(
        dock.bg_shelf.cast(),
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
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn apply_dock_mode_classes(state: *mut LambdockState) {
    let Some(dock) = state.as_ref() else {
        return;
    };
    if dock.bg_capsule.is_null() {
        return;
    }

    let shelf_active = lambdock_shelf_active(state) != 0;
    let mirroring = shelf_active && lambdock_shelf_geometry_for(state, dock.icon_size).band_px > 0;

    set_class(dock.bg_capsule, c"taskbar", dock.taskbar != 0);
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
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn dock_apply_exclusive_zone(state: *mut LambdockState) {
    let Some(dock) = state.as_ref() else {
        return;
    };
    let Some(window) = window_of(dock) else {
        return;
    };
    if !layer_shell::is_layer_window(&*window) {
        return;
    }

    let policy = dock_surface_policy_for(state);
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
/// `state` must be null or a live dock with its window built.
#[no_mangle]
pub unsafe extern "C" fn setup_layer_shell_window(state: *mut LambdockState) {
    let Some(dock) = state.as_ref() else {
        return;
    };
    let Some(widget) = widget_of(dock.window) else {
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
        lambdock_x11_dock_apply(state);
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
    let monitor = dock.monitor.cast::<gdk::ffi::GdkMonitor>();
    if layer_shell::is_monitor_valid(monitor) != 0 {
        layer_shell::set_monitor(&window, monitor);
    }

    // The dock renders on the top layer, above normal application windows.
    layer_shell::set_layer(&window, LAYER_TOP);

    let policy = dock_surface_policy_for(state);
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
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn refresh_dock_mode(state: *mut LambdockState) {
    let Some(dock) = state.as_ref() else {
        return;
    };
    let Some(window) = window_of(dock) else {
        return;
    };

    // Anchors only exist on a layer surface (no layer shell on X11 or GNOME).
    if layer_shell::is_layer_window(&*window) {
        let policy = dock_surface_policy_for(state);
        apply_surface_anchors(&window, &policy);
    }

    apply_dock_mode_classes(state);
    // Dimension updates end by re-placing the X11 window, which is where a mode
    // switch (dock <-> taskbar) changes the dock's size.
    update_dock_dimensions(state);
}

/// Schedules the first hide, on a dock that auto-hides.
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn schedule_initial_autohide(state: *mut LambdockState) {
    let Some(dock) = state.as_ref() else {
        return;
    };
    let Some(window) = window_of(dock) else {
        return;
    };

    // Without a layer surface there is no slide, so there is nothing to
    // schedule.
    if dock.auto_hide != 0 && layer_shell::is_layer_window(&*window) {
        schedule_hide_timer(state, AUTOHIDE_INITIAL_DELAY_MS);
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
#[no_mangle]
pub unsafe extern "C" fn on_right_click(
    _gesture: *mut gtk4::ffi::GtkGestureClick,
    _n_press: c_int,
    x: f64,
    y: f64,
    user_data: *mut c_void,
) {
    let Some(dock) = user_data.cast::<LambdockState>().as_ref() else {
        return;
    };
    let Some(popover) = widget_of(dock.popover) else {
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
#[no_mangle]
pub unsafe extern "C" fn on_popover_closed(
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
        schedule_hide_timer(user_data.cast(), AUTOHIDE_POPOVER_DELAY_MS);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;

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
        let mut state: LambdockState = unsafe { std::mem::zeroed() };

        state.edge = EDGE_BOTTOM;
        let policy = unsafe { dock_surface_policy_for(&state) };
        assert_eq!(policy.anchor_bottom, 1);
        assert_eq!(
            (policy.anchor_top, policy.anchor_left, policy.anchor_right),
            (0, 0, 0)
        );

        // A taskbar fills the perpendicular axis, so it anchors both of those
        // edges too -- and a floating dock does not.
        state.taskbar = 1;
        let policy = unsafe { dock_surface_policy_for(&state) };
        assert_eq!((policy.anchor_left, policy.anchor_right), (1, 1));
        assert_eq!(policy.anchor_top, 0);

        // The same rule turned sideways: a left-hand dock wakes up its top and
        // bottom anchors instead.
        state.edge = EDGE_LEFT;
        let policy = unsafe { dock_surface_policy_for(&state) };
        assert_eq!((policy.anchor_top, policy.anchor_bottom), (1, 1));
        assert_eq!(policy.anchor_right, 0);
    }

    /// An auto-hiding dock reserves no desktop space; a resting one reserves its
    /// own capsule thickness.
    #[test]
    fn an_auto_hiding_dock_reserves_nothing() {
        let mut state: LambdockState = unsafe { std::mem::zeroed() };
        state.edge = EDGE_BOTTOM;
        state.icon_size = 48;

        state.auto_hide = 0;
        let resting = unsafe { dock_surface_policy_for(&state) };
        assert!(resting.exclusive_zone_px > 0);

        state.auto_hide = 1;
        let hiding = unsafe { dock_surface_policy_for(&state) };
        assert_eq!(hiding.exclusive_zone_px, 0);
    }

    /// The shelf names and leans its edge: the edge on the screen stays pinned,
    /// so top and left tilt backwards.
    #[test]
    fn the_shelf_knows_which_edge_it_is_on() {
        assert_eq!(
            unsafe { CStr::from_ptr(shelf_edge_class(EDGE_TOP)) }.to_str(),
            Ok("shelf-top")
        );
        assert_eq!(
            unsafe { CStr::from_ptr(shelf_edge_class(EDGE_LEFT)) }.to_str(),
            Ok("shelf-left")
        );
        assert_eq!(
            unsafe { CStr::from_ptr(shelf_edge_class(EDGE_RIGHT)) }.to_str(),
            Ok("shelf-right")
        );
        assert_eq!(
            unsafe { CStr::from_ptr(shelf_edge_class(EDGE_BOTTOM)) }.to_str(),
            Ok("shelf-bottom")
        );

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
        let mut state: LambdockState = unsafe { std::mem::zeroed() };
        assert!(is_horizontal(&state));

        state.orientation = gtk4::Orientation::Vertical.into_glib();
        assert!(!is_horizontal(&state));
    }
}
