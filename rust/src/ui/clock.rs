// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The analogue face: `src/ui/clock.c` in Rust.
//!
//! A squircle body with a brushed bezel, a paper dial, tick marks, three hands
//! and a sweep of glass across the top-left. Everything is proportional to the
//! face's size, so it stays crisp at every dock-icon-size and needs no artwork.
//!
//! The palette lives here rather than in the theme: a watch face is an object --
//! the same one on a light dock and a dark one -- which is also why it wears a
//! body of its own instead of hanging its numbers next to the icons.

use crate::core::clock_face::{
    lambdock_clock_turns_for_seconds, lambdock_squircle_point, seconds_in_day, ClockTurns,
};
use crate::core::config_values::DEFAULT_ICON_SIZE;
use gtk4::ffi;
use gtk4::gdk;
use gtk4::glib;
use gtk4::glib::translate::{from_glib_borrow, Borrowed, IntoGlibPtr};
use gtk4::graphene;
use gtk4::gsk;
use gtk4::prelude::*;
use gtk4::subclass::prelude::*;
use std::cell::Cell;
use std::os::raw::{c_int, c_void};

/// How square the body is: 2 is an ellipse, 4 a squircle, 8 nearly a rounded
/// rectangle. Four reads as "squircle" at 48 pixels without looking like a
/// button.
const BODY_SQUARENESS: f64 = 4.2;
const DIAL_SQUARENESS: f64 = 4.0;
/// Samples per turn for the body and the dial. Enough that the curve reads as
/// smooth at 96 pixels, cheap enough to rebuild on every redraw.
const CURVE_STEPS: i32 = 96;
/// Below this the face has no room for a dial, ticks and hands.
const MIN_DRAWABLE_PX: f64 = 8.0;

/// The two ends of a tick or a hand, as `((x, y), (x, y))`.
///
/// `turn` runs clockwise from 12 o'clock, which is the convention the face was
/// drawn with: at 0 the ray points up, at 0.25 right, at 0.5 down.
pub fn ray_endpoints(
    center_x: f64,
    center_y: f64,
    turn: f64,
    from_px: f64,
    to_px: f64,
) -> ((f32, f32), (f32, f32)) {
    let angle = turn * 2.0 * std::f64::consts::PI;
    let dx = angle.sin();
    let dy = -angle.cos(); // turns run clockwise from 12 o'clock

    let point = |distance: f64| {
        (
            (center_x + dx * distance) as f32,
            (center_y + dy * distance) as f32,
        )
    };

    (point(from_px), point(to_px))
}

/// The palette. A watch face is an object, the same one on a light dock and a
/// dark one, so it does not follow the theme -- the meter's tile is the same
/// decision.
const BEZEL_LIGHT: gdk::RGBA = gdk::RGBA::new(0.98, 0.97, 0.94, 1.0);
const BEZEL_MID: gdk::RGBA = gdk::RGBA::new(0.84, 0.82, 0.78, 1.0);
const BEZEL_DARK: gdk::RGBA = gdk::RGBA::new(0.60, 0.58, 0.54, 1.0);
const BEZEL_EDGE: gdk::RGBA = gdk::RGBA::new(0.30, 0.28, 0.26, 0.45);
const DIAL_LIGHT: gdk::RGBA = gdk::RGBA::new(0.999, 0.995, 0.980, 1.0);
const DIAL_SHADE: gdk::RGBA = gdk::RGBA::new(0.862, 0.828, 0.752, 1.0);
const DIAL_EDGE: gdk::RGBA = gdk::RGBA::new(0.26, 0.24, 0.21, 0.20);
const TICK_MAJOR: gdk::RGBA = gdk::RGBA::new(0.22, 0.20, 0.17, 0.95);
const TICK_MINOR: gdk::RGBA = gdk::RGBA::new(0.44, 0.41, 0.36, 0.60);
const HAND_INK: gdk::RGBA = gdk::RGBA::new(0.17, 0.15, 0.13, 1.0);
const SECOND_INK: gdk::RGBA = gdk::RGBA::new(0.74, 0.17, 0.13, 1.0);
const CAP_HIGHLIGHT: gdk::RGBA = gdk::RGBA::new(1.0, 1.0, 1.0, 0.60);

/// A closed squircle path centred on (`cx`, `cy`).
fn squircle_path(
    cx: f64,
    cy: f64,
    half_width: f64,
    half_height: f64,
    squareness: f64,
) -> gsk::Path {
    let builder = gsk::PathBuilder::new();

    for step in 0..=CURVE_STEPS {
        let (mut x, mut y) = (0.0, 0.0);
        unsafe {
            lambdock_squircle_point(
                f64::from(step) / f64::from(CURVE_STEPS),
                half_width,
                half_height,
                squareness,
                &mut x,
                &mut y,
            );
        }

        if step == 0 {
            builder.move_to((cx + x) as f32, (cy + y) as f32);
        } else {
            builder.line_to((cx + x) as f32, (cy + y) as f32);
        }
    }

    builder.to_path()
}

/// A straight run from one distance out of the centre to another.
fn ray_path(cx: f64, cy: f64, turn: f64, from_px: f64, to_px: f64) -> gsk::Path {
    let (from, to) = ray_endpoints(cx, cy, turn, from_px, to_px);
    let builder = gsk::PathBuilder::new();

    builder.move_to(from.0, from.1);
    builder.line_to(to.0, to.1);

    builder.to_path()
}

/// A stroked run with round ends, the shape every tick and hand is made of.
///
/// Eight numbers is what a line segment from a centre is: the callers are the
/// dial, the hour, the minute and the second hand, and naming the arguments is
/// cheaper than a struct four helpers have to build.
#[allow(clippy::too_many_arguments)]
fn paint_ray(
    snapshot: &gtk4::Snapshot,
    cx: f64,
    cy: f64,
    turn: f64,
    from_px: f64,
    to_px: f64,
    thickness: f64,
    colour: &gdk::RGBA,
) {
    let ray = ray_path(cx, cy, turn, from_px, to_px);

    let stroke = gsk::Stroke::new(thickness as f32);
    stroke.set_line_cap(gsk::LineCap::Round);

    snapshot.append_stroke(&ray, &stroke, colour);
}

fn paint_circle(snapshot: &gtk4::Snapshot, cx: f64, cy: f64, radius: f64, colour: &gdk::RGBA) {
    let builder = gsk::PathBuilder::new();
    builder.add_circle(&graphene::Point::new(cx as f32, cy as f32), radius as f32);
    let path = builder.to_path();

    snapshot.append_fill(&path, gsk::FillRule::Winding, colour);
}

/// The body's brushed bezel: light where the window is, darker towards the far
/// corner.
fn paint_bezel(snapshot: &gtk4::Snapshot, body: &gsk::Path, size: f64) {
    let bounds = bounds_of(body, size);

    let stops = [
        gsk::ColorStop::new(0.0, BEZEL_LIGHT),
        gsk::ColorStop::new(0.45, BEZEL_MID),
        gsk::ColorStop::new(1.0, BEZEL_DARK),
    ];
    let start = graphene::Point::new(0.0, 0.0);
    let end = graphene::Point::new(size as f32, size as f32);

    snapshot.append_linear_gradient(&bounds, &start, &end, &stops);

    // A crisp outline instead of a blurred one: at 48 pixels a soft edge reads as
    // a smudge.
    let edge = gsk::Stroke::new(size.mul_add(0.015, 0.0).max(1.0) as f32);
    snapshot.append_stroke(body, &edge, &BEZEL_EDGE);
}

/// The paper dial, lit from the top-left so it has a warm side and a shaded one.
fn paint_dial(snapshot: &gtk4::Snapshot, size: f64, half: f64) {
    let dial_half = half * 0.82;
    let dial = squircle_path(half, half, dial_half, dial_half, DIAL_SQUARENESS);

    let bounds = bounds_of(&dial, size);
    let stops = [
        gsk::ColorStop::new(0.0, DIAL_LIGHT),
        gsk::ColorStop::new(1.0, DIAL_SHADE),
    ];
    let centre = graphene::Point::new(
        (half - dial_half * 0.35) as f32,
        (half - dial_half * 0.45) as f32,
    );

    snapshot.append_radial_gradient(
        &bounds,
        &centre,
        (dial_half * 1.05) as f32,
        (dial_half * 1.05) as f32,
        (dial_half * 0.1) as f32,
        (dial_half * 1.5) as f32,
        &stops,
    );

    // The rim the bezel casts on the dial.
    let rim = gsk::Stroke::new(size.mul_add(0.02, 0.0).max(0.8) as f32);
    snapshot.append_stroke(&dial, &rim, &DIAL_EDGE);
}

fn paint_ticks(snapshot: &gtk4::Snapshot, half: f64) {
    // Minute ticks are the first thing to go mushy at 48 pixels, so they only
    // appear once the face has room to draw them apart.
    if half >= 20.0 {
        let minor_thickness = (half * 0.022).max(0.5);

        for minute in 0..60 {
            if minute % 5 == 0 {
                continue;
            }

            paint_ray(
                snapshot,
                half,
                half,
                f64::from(minute) / 60.0,
                half * 0.55,
                half * 0.62,
                minor_thickness,
                &TICK_MINOR,
            );
        }
    }

    let major_thickness = (half * 0.055).max(1.0);

    for hour in 0..12 {
        paint_ray(
            snapshot,
            half,
            half,
            f64::from(hour) / 12.0,
            half * 0.50,
            half * 0.63,
            major_thickness,
            &TICK_MAJOR,
        );
    }
}

fn paint_hands(snapshot: &gtk4::Snapshot, half: f64, turns: ClockTurns) {
    // The minute hand over the hour hand, the second hand over both: the order
    // they are stacked in on a real watch.
    paint_ray(
        snapshot,
        half,
        half,
        turns.hour,
        -half * 0.05,
        half * 0.40,
        (half * 0.115).max(1.4),
        &HAND_INK,
    );
    paint_ray(
        snapshot,
        half,
        half,
        turns.minute,
        -half * 0.05,
        half * 0.60,
        (half * 0.075).max(1.0),
        &HAND_INK,
    );
    paint_ray(
        snapshot,
        half,
        half,
        turns.second,
        -half * 0.18,
        half * 0.66,
        (half * 0.028).max(0.6),
        &SECOND_INK,
    );

    // The hub, with one specular dot so it reads as a pin and not a blob.
    paint_circle(snapshot, half, half, (half * 0.065).max(1.4), &HAND_INK);
    paint_circle(
        snapshot,
        half - half * 0.018,
        half - half * 0.022,
        (half * 0.024).max(0.6),
        &CAP_HIGHLIGHT,
    );
}

/// The bounds of a path, or the whole face when the path has none yet.
fn bounds_of(path: &gsk::Path, size: f64) -> graphene::Rect {
    path.bounds()
        .unwrap_or_else(|| graphene::Rect::new(0.0, 0.0, size as f32, size as f32))
}

mod imp {
    use super::*;

    #[derive(Default)]
    pub struct Clock {
        /// Seconds since local midnight, always a whole day's worth.
        pub seconds: Cell<i32>,
        /// Square size the dock lays the face out at.
        pub icon_size: Cell<i32>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for Clock {
        const NAME: &'static str = "LambdockClock";
        type Type = super::Clock;
        type ParentType = gtk4::Widget;

        fn class_init(klass: &mut Self::Class) {
            // What the dock's stylesheet keys on, as the C class had it.
            klass.set_css_name("clock");
        }
    }

    impl ObjectImpl for Clock {
        fn constructed(&self) {
            self.parent_constructed();

            self.icon_size.set(DEFAULT_ICON_SIZE);

            self.obj().set_overflow(gtk4::Overflow::Visible);
        }
    }

    impl WidgetImpl for Clock {
        fn measure(&self, _orientation: gtk4::Orientation, _for_size: i32) -> (i32, i32, i32, i32) {
            // A face is measured like an icon: a square of the requested size.
            let size = if self.icon_size.get() > 0 {
                self.icon_size.get()
            } else {
                DEFAULT_ICON_SIZE
            };

            (size, size, -1, -1)
        }

        fn snapshot(&self, snapshot: &gtk4::Snapshot) {
            let widget = self.obj();
            let size = f64::from(widget.width().min(widget.height()));

            // Below this there is no room for a dial, ticks and hands.
            if size < MIN_DRAWABLE_PX {
                return;
            }

            let half = size / 2.0;

            let body = squircle_path(half, half, half, half, BODY_SQUARENESS);
            paint_bezel(snapshot, &body, size);

            paint_dial(snapshot, size, half);
            paint_ticks(snapshot, half);
            paint_hands(
                snapshot,
                half,
                lambdock_clock_turns_for_seconds(self.seconds.get()),
            );

            // The crystal: one sweep of light across the body, clipped to it
            // rather than to the dial -- a highlight that stops at the dial's
            // edge leaves a visible seam inside the bezel, which at 64 pixels
            // reads as two stacked faces.
            snapshot.push_fill(&body, gsk::FillRule::Winding);
            {
                let bounds = bounds_of(&body, size);
                let stops = [
                    gsk::ColorStop::new(0.0, gdk::RGBA::new(1.0, 1.0, 1.0, 0.40)),
                    gsk::ColorStop::new(0.5, gdk::RGBA::new(1.0, 1.0, 1.0, 0.10)),
                    gsk::ColorStop::new(1.0, gdk::RGBA::new(1.0, 1.0, 1.0, 0.0)),
                ];
                let start = graphene::Point::new(0.0, 0.0);
                let end = graphene::Point::new(size as f32, size as f32);

                snapshot.append_linear_gradient(&bounds, &start, &end, &stops);
            }
            snapshot.pop();
        }
    }
}

glib::wrapper! {
    /// An analogue clock that fits in one dock icon slot.
    pub struct Clock(ObjectSubclass<imp::Clock>)
        @extends gtk4::Widget,
        @implements gtk4::Accessible, gtk4::Buildable, gtk4::ConstraintTarget;
}

/// The clock behind a widget pointer, or `None` when it is not one.
///
/// # Safety
///
/// `pointer` must be null or a live `GtkWidget`.
unsafe fn as_clock(pointer: *mut ffi::GtkWidget) -> Option<Borrowed<Clock>> {
    if pointer.is_null() {
        return None;
    }

    let widget: Borrowed<gtk4::Widget> = from_glib_borrow(pointer);

    // The C callers pass whatever widget they have (`is_clock` is how they ask),
    // so the type test comes before the cast.
    if !widget.is::<Clock>() {
        return None;
    }

    // `into_inner` hands the wrapper back without touching the reference count,
    // and `unsafe_cast` is sound because the test above just ran. Wrapping the
    // result borrowed keeps the transfer `(transfer none)`, as the header says.
    let widget: gtk4::Widget = widget.into_inner();
    Some(Borrowed::new(widget.unsafe_cast()))
}

/// Creates a face showing midnight.
///
/// # Safety
///
/// Returns a floating reference, like the `g_object_new` this replaces: the
/// callers hand it to a box, and the box sinks it.
#[no_mangle]
pub unsafe extern "C" fn lambdock_clock_new() -> *mut ffi::GtkWidget {
    // gtk-rs asserts its own initialization flag inside the subclass machinery,
    // and C's gtk_init does not set it.
    crate::ui::ensure_gtk_initialized();

    let clock: Clock = glib::Object::builder().build();

    // `build()` sinks the object already; the C callers rely on the sink their
    // container does, so the widget is floated again and the wrapper lets go
    // without unreffing.
    glib::gobject_ffi::g_object_force_floating(clock.as_ptr().cast());

    clock.upcast::<gtk4::Widget>().into_glib_ptr()
}

/// Points the hands at that time. Cheap enough to call from a poll timer: it
/// only stores the reading and queues a redraw.
///
/// # Safety
///
/// `pointer` must be null or a live `GtkWidget`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_clock_set_time(
    pointer: *mut ffi::GtkWidget,
    seconds_of_day: c_int,
) {
    let Some(clock) = as_clock(pointer) else {
        return;
    };

    // The wrap happens here rather than at the caller: a poll that reads the
    // clock across midnight can hand over 86400, and the face must not draw that
    // as a full extra turn.
    let seconds = seconds_in_day(seconds_of_day);

    if seconds == clock.imp().seconds.get() {
        return;
    }

    clock.imp().seconds.set(seconds);
    clock.queue_draw();
}

/// The reading the face was last given, in seconds since midnight.
///
/// # Safety
///
/// `pointer` must be null or a live `GtkWidget`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_clock_get_time(pointer: *mut ffi::GtkWidget) -> c_int {
    match as_clock(pointer) {
        Some(clock) => clock.imp().seconds.get(),
        None => 0,
    }
}

/// Resizes the face to one icon slot.
///
/// # Safety
///
/// `pointer` must be null or a live `GtkWidget`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_clock_set_icon_size(
    pointer: *mut ffi::GtkWidget,
    icon_size: c_int,
) {
    let Some(clock) = as_clock(pointer) else {
        return;
    };

    if icon_size <= 0 || clock.imp().icon_size.get() == icon_size {
        return;
    }

    clock.imp().icon_size.set(icon_size);
    clock.queue_resize();
}

/// Whether `widget` is a clock face.
///
/// # Safety
///
/// `widget` must be null or a live `GtkWidget`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_clock_is_clock(widget: *mut c_void) -> c_int {
    i32::from(as_clock(widget.cast()).is_some())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The hands and ticks all hang off this: a sign flipped here would turn the
    /// clock into a mirror image, which is the kind of wrong nobody notices until
    /// it is 3 o'clock and the face says 9.
    #[test]
    fn a_ray_runs_clockwise_from_twelve_oclock() {
        let (from, to) = ray_endpoints(50.0, 50.0, 0.0, 0.0, 10.0);
        assert_eq!(from, (50.0, 50.0));
        assert_eq!(to, (50.0, 40.0)); // up

        let (_, down) = ray_endpoints(50.0, 50.0, 0.5, 0.0, 10.0);
        assert_eq!(down, (50.0, 60.0));

        let (_, right) = ray_endpoints(50.0, 50.0, 0.25, 0.0, 10.0);
        assert_eq!(right, (60.0, 50.0));

        let (_, left) = ray_endpoints(50.0, 50.0, 0.75, 0.0, 10.0);
        assert_eq!(left, (40.0, 50.0));
    }

    /// A hand reaches back past the hub, and that end is a distance, not a
    /// fraction of the other one.
    #[test]
    fn a_ray_can_start_behind_the_centre() {
        let (from, to) = ray_endpoints(50.0, 50.0, 0.25, -5.0, 20.0);

        assert_eq!(from, (45.0, 50.0));
        assert_eq!(to, (70.0, 50.0));
    }
}
