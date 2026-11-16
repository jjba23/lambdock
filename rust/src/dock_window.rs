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
use gtk4::glib::translate::{Borrowed, IntoGlib};
use gtk4::prelude::*;
use std::os::raw::{c_int, c_uint, c_void};

use crate::core::dock_dims::{lambdock_dock_dims_compute, DockDims};
use crate::core::dock_motion::lambdock_dock_needs_full_input;
use crate::core::render_stats::{lambdock_stats_bump, INPUT_REGION_SETS, SURFACE_RESIZES};
use crate::state::LambdockState;
use crate::ui::layer_shell::{self, EDGE_LEFT, EDGE_TOP};
use crate::ui::shelf::{apply_shelf_layout, lambdock_shelf_geometry_for};
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
