// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The circular gauge: `src/ui/meter.c` in Rust.
//!
//! A ring, a value arc and the reading in the middle, sized like a dock icon so
//! it lines up with the launchers around it. Colour carries the meaning -- green
//! while there is room, amber when it gets tight, red when it is nearly full --
//! which is the one thing a plain number cannot say at a glance from across the
//! room.
//!
//! Every part of the drawing is a value ([`geometry`]) painted by the snapshot,
//! and the arc is a polyline fine enough that the round caps hide the seams.

use crate::core::config_values::{lambdock_sanitize_double, DEFAULT_ICON_SIZE};
use gtk4::glib;
use gtk4::glib::translate::{from_glib_borrow, Borrowed, IntoGlibPtr};
use gtk4::graphene;
use gtk4::gsk::{self};
use gtk4::prelude::*;
use gtk4::subclass::prelude::*;
use gtk4::{ffi, gdk, pango};
use std::cell::Cell;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_void};

/// Steps in a full turn of the value arc. `GskPathBuilder` has no arc primitive,
/// so the curve is a polyline; this is how fine it is.
const ARC_STEPS_PER_TURN: f64 = 180.0;

/// The geometry of one gauge, as a value: computed from the size and the
/// reading, painted by the snapshot, and asserted in the tests without a
/// widget.
///
/// Where every part of a gauge goes. The snapshot only draws what this describes,
/// which is what lets the proportions be checked without painting anything.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct MeterGeometry {
    /// The rounded tile behind the ring, in widget coordinates.
    pub tile: graphene::Rect,
    /// Corner radius of that tile.
    pub tile_radius: f32,
    /// Ring centre, x.
    pub center_x: f32,
    /// Ring centre, y.
    pub center_y: f32,
    /// Radius of the ring's centre line.
    pub radius: f32,
    /// Stroke width of the ring and of the value arc.
    pub line_width: f32,
    /// Square available for the reading in the middle.
    pub text_box: f32,
}

/// The geometry for a gauge in a `width` x `height` box.
///
/// Pure: the same box always produces the same proportions. A box with no area
/// yields the zero geometry, and the snapshot returns before drawing anything.
pub fn geometry(width: i32, height: i32) -> MeterGeometry {
    if width <= 0 || height <= 0 {
        return MeterGeometry::default();
    }

    let size = f64::from(width.min(height));
    let mut geometry = MeterGeometry {
        tile_radius: (size * 0.22) as f32,
        center_x: (f64::from(width) / 2.0) as f32,
        center_y: (f64::from(height) / 2.0) as f32,
        ..MeterGeometry::default()
    };

    // The gauge wears a tile of its own: a bare ring would be drawn half over the
    // glass and half over whatever is behind the dock (a dock whose icons
    // overhang its slab puts the icon row above the glass centre), and a reading
    // that changes colour has to stay legible on both.
    let padding = size * 0.04;
    let inner = (size - 2.0 * padding) as f32;
    geometry.tile = graphene::Rect::new(padding as f32, padding as f32, inner, inner);

    // Thick enough to read at a glance, thin enough to leave room for the number
    // in the middle.
    geometry.line_width = (size * 0.10).max(2.0) as f32;
    geometry.radius = ((size - f64::from(geometry.line_width)) / 2.0 - size * 0.16) as f32;

    // The reading has to fit inside the ring: the snapshot starts at a
    // comfortable size and shrinks until it fits into this box.
    geometry.text_box = (size * 0.66) as f32;

    geometry
}

/// The colour of the value arc: green while there is room, amber when it gets
/// tight, red when it is nearly full.
pub fn tone_for_fraction(fraction: f64) -> gdk::RGBA {
    // NaN compares false against everything, so it lands on red: a reading the
    // dock cannot understand should not look comfortable.
    if fraction < 0.60 {
        gdk::RGBA::new(0.22, 0.76, 0.36, 1.0)
    } else if fraction < 0.85 {
        gdk::RGBA::new(0.97, 0.68, 0.20, 1.0)
    } else {
        gdk::RGBA::new(0.90, 0.26, 0.26, 1.0)
    }
}

/// How many segments the value arc is drawn with: fine enough that the round
/// caps and joins hide the seams, and never fewer than two.
///
/// The fraction is clamped here, so a reading of 200% is a closed ring rather
/// than an arc that keeps going round.
pub fn arc_steps(fraction: f64) -> i32 {
    let fraction = if fraction.is_finite() {
        fraction.clamp(0.0, 1.0)
    } else {
        0.0
    };

    ((fraction * ARC_STEPS_PER_TURN).ceil() as i32).max(2)
}

/// The fraction clamped into 0..1, with anything not a number at rest.
///
/// `set_value` already sanitises, but the arc helpers are `pub` and a closed
/// ring is the only sane answer to 200%.
fn sanitized_fraction(fraction: f64) -> f64 {
    if fraction.is_finite() {
        fraction.clamp(0.0, 1.0)
    } else {
        0.0
    }
}

/// An arc from the top of the circle, clockwise, covering `fraction` of a full
/// turn. `GskPathBuilder` has no arc primitive, so it is a polyline.
fn arc_path(center_x: f32, center_y: f32, radius: f32, fraction: f64) -> gsk::Path {
    let builder = gsk::PathBuilder::new();
    let steps = arc_steps(fraction);
    let fraction = sanitized_fraction(fraction);

    for step in 0..=steps {
        let turn = fraction * (f64::from(step) / f64::from(steps));
        let angle = -std::f64::consts::FRAC_PI_2 + turn * 2.0 * std::f64::consts::PI;
        let x = (f64::from(center_x) + f64::from(radius) * angle.cos()) as f32;
        let y = (f64::from(center_y) + f64::from(radius) * angle.sin()) as f32;

        if step == 0 {
            builder.move_to(x, y);
        } else {
            builder.line_to(x, y);
        }
    }

    builder.to_path()
}

/// The empty track the value arc is drawn on.
fn ring_path(center_x: f32, center_y: f32, radius: f32) -> gsk::Path {
    let builder = gsk::PathBuilder::new();
    builder.add_circle(&graphene::Point::new(center_x, center_y), radius);
    builder.to_path()
}

mod imp {
    use super::*;

    #[derive(Default)]
    pub struct Meter {
        /// 0.0 .. 1.0; sanitised on the way in.
        pub fraction: Cell<f64>,
        /// The reading written in the middle ("42%"), absent on a fresh gauge.
        pub text: std::cell::RefCell<Option<String>>,
        /// Square size the dock lays the gauge out at.
        pub icon_size: Cell<i32>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for Meter {
        const NAME: &'static str = "LambdockMeter";
        type Type = super::Meter;
        type ParentType = gtk4::Widget;

        fn class_init(klass: &mut Self::Class) {
            // What the dock's stylesheet keys on, as the C class had it.
            klass.set_css_name("meter");
        }
    }

    impl ObjectImpl for Meter {
        fn constructed(&self) {
            self.parent_constructed();

            self.icon_size.set(DEFAULT_ICON_SIZE);

            self.obj().set_overflow(gtk4::Overflow::Visible);
        }
    }

    impl WidgetImpl for Meter {
        fn measure(&self, _orientation: gtk4::Orientation, _for_size: i32) -> (i32, i32, i32, i32) {
            // A meter is measured like an icon: a square of the requested size.
            let size = if self.icon_size.get() > 0 {
                self.icon_size.get()
            } else {
                DEFAULT_ICON_SIZE
            };

            (size, size, -1, -1)
        }

        fn snapshot(&self, snapshot: &gtk4::Snapshot) {
            let widget = self.obj();
            let (width, height) = (widget.width(), widget.height());

            if width <= 0 || height <= 0 {
                return;
            }

            let geometry = geometry(width, height);
            let size = f64::from(width.min(height));

            let tile_outline = gsk::RoundedRect::from_rect(geometry.tile, geometry.tile_radius);

            snapshot.push_rounded_clip(&tile_outline);
            {
                let stops = [
                    gsk::ColorStop::new(0.0, gdk::RGBA::new(0.24, 0.26, 0.31, 0.94)),
                    gsk::ColorStop::new(1.0, gdk::RGBA::new(0.10, 0.11, 0.14, 0.96)),
                ];
                let start = graphene::Point::new(geometry.tile.x(), geometry.tile.y());
                let end = graphene::Point::new(
                    geometry.tile.x() + geometry.tile.width(),
                    geometry.tile.y() + geometry.tile.height(),
                );

                snapshot.append_linear_gradient(&geometry.tile, &start, &end, &stops);
            }
            snapshot.pop();

            {
                let edge = gdk::RGBA::new(1.0, 1.0, 1.0, 0.22);
                let widths = [1.0; 4];
                let edges = [edge; 4];

                snapshot.append_border(&tile_outline, &widths, &edges);
            }

            // The tile is dark, so the track and the reading are drawn in light
            // ink, whatever theme the dock wears.
            let foreground = gdk::RGBA::new(0.93, 0.95, 0.98, 1.0);

            let stroke = gsk::Stroke::new(geometry.line_width);
            stroke.set_line_cap(gsk::LineCap::Round);
            stroke.set_line_join(gsk::LineJoin::Round);

            // The empty track first, so the filled arc sits on top of it.
            let track = ring_path(geometry.center_x, geometry.center_y, geometry.radius);
            let mut track_colour = foreground;
            track_colour.set_alpha(0.18);
            snapshot.append_stroke(&track, &stroke, &track_colour);

            let fraction = self.fraction.get();
            if fraction > 0.0 {
                let arc = arc_path(
                    geometry.center_x,
                    geometry.center_y,
                    geometry.radius,
                    fraction,
                );

                snapshot.append_stroke(&arc, &stroke, &tone_for_fraction(fraction));
            }

            let Some(text) = self.text.borrow().clone() else {
                return;
            };
            if text.is_empty() {
                return;
            }

            // The reading has to fit inside the ring, whatever it says ("42%",
            // "54°C"): start at a comfortable size and shrink until it does. Two
            // or three passes are enough; a floor keeps it from vanishing on tiny
            // icons.
            let available = f64::from(geometry.text_box);
            let mut font_size = (size * 0.40) as i32;
            let layout = widget.create_pango_layout(Some(&text));
            let mut text_width = 0;
            let mut text_height = 0;

            for _ in 0..6 {
                if font_size < 7 {
                    font_size = 7;
                }

                let attributes = pango::AttrList::new();
                attributes.insert(pango::AttrSize::new(font_size * pango::SCALE));
                attributes.insert(pango::AttrInt::new_weight(pango::Weight::Semibold));
                layout.set_attributes(Some(&attributes));

                (text_width, text_height) = layout.pixel_size();
                if f64::from(text_width) <= available && f64::from(text_height) <= available {
                    break;
                }

                font_size = (f64::from(font_size) * 0.85) as i32;
            }

            let origin = graphene::Point::new(
                (f64::from(geometry.center_x) - f64::from(text_width) / 2.0) as f32,
                (f64::from(geometry.center_y) - f64::from(text_height) / 2.0) as f32,
            );

            snapshot.save();
            snapshot.translate(&origin);
            snapshot.append_layout(&layout, &foreground);
            snapshot.restore();
        }
    }
}

glib::wrapper! {
    /// A circular gauge that fits in one dock icon slot.
    pub struct Meter(ObjectSubclass<imp::Meter>)
        @extends gtk4::Widget,
        @implements gtk4::Accessible, gtk4::Buildable, gtk4::ConstraintTarget;
}

/// The meter behind a widget pointer, or `None` when it is not one.
///
/// # Safety
///
/// `pointer` must be null or a live `GtkWidget`.
unsafe fn as_meter(pointer: *mut ffi::GtkWidget) -> Option<Borrowed<Meter>> {
    if pointer.is_null() {
        return None;
    }

    let widget: Borrowed<gtk4::Widget> = from_glib_borrow(pointer);

    // The C callers pass whatever widget they have (`is_meter` is how they ask),
    // so the type test comes before the cast.
    if !widget.is::<Meter>() {
        return None;
    }

    // `into_inner` hands the wrapper back without touching the reference count,
    // and `unsafe_cast` is sound because the test above just ran. Wrapping the
    // result borrowed keeps the transfer `(transfer none)`, as the header says.
    let widget: gtk4::Widget = widget.into_inner();
    Some(Borrowed::new(widget.unsafe_cast()))
}

/// Creates an empty gauge (0%, no reading).
///
/// # Safety
///
/// Returns a floating reference, like the `g_object_new` this replaces: the
/// callers hand it to a box, and the box sinks it.
pub unsafe fn lambdock_meter_new() -> *mut ffi::GtkWidget {
    // gtk-rs asserts its own initialization flag inside the subclass machinery,
    // and C's gtk_init does not set it.
    crate::ui::ensure_gtk_initialized();

    let meter: Meter = glib::Object::builder().build();

    // `build()` sinks the object already; the C callers rely on the sink their
    // container does, so the widget is floated again and the wrapper lets go
    // without unreffing.
    glib::gobject_ffi::g_object_force_floating(meter.as_ptr().cast());

    meter.upcast::<gtk4::Widget>().into_glib_ptr()
}

/// Updates the gauge. Cheap enough to call from a poll timer: it only stores the
/// values and queues a redraw.
///
/// # Safety
///
/// `meter` must be null or a live `GtkWidget`; `text` must be null or a
/// NUL-terminated string.
pub unsafe fn lambdock_meter_set_value(
    pointer: *mut ffi::GtkWidget,
    fraction: f64,
    text: *const c_char,
) {
    let Some(meter) = as_meter(pointer) else {
        return;
    };

    // `clamp` lets NaN through (every comparison against NaN is false), which
    // would reach the paint code; the sanitiser maps it to a safe value.
    meter
        .imp()
        .fraction
        .set(lambdock_sanitize_double(fraction, 0.0, 0.0, 1.0));

    // A NULL text keeps the previous reading.
    if !text.is_null() {
        let text = CStr::from_ptr(text).to_string_lossy().into_owned();
        *meter.imp().text.borrow_mut() = Some(text);
    }

    meter.queue_draw();
}

/// Sizes the gauge like a dock icon, so it lines up with the launchers next to
/// it.
///
/// # Safety
///
/// `meter` must be null or a live `GtkWidget`.
pub unsafe fn lambdock_meter_set_icon_size(
    pointer: *mut ffi::GtkWidget,
    icon_size: c_int,
) {
    let Some(meter) = as_meter(pointer) else {
        return;
    };

    // A broken dock-icon-size must not collapse the gauge.
    if icon_size <= 0 || meter.imp().icon_size.get() == icon_size {
        return;
    }

    meter.imp().icon_size.set(icon_size);
    meter.queue_resize();
}

/// Whether `widget` is a meter.
///
/// # Safety
///
/// `widget` must be null or a live `GtkWidget`.
pub unsafe fn lambdock_meter_is_meter(widget: *mut c_void) -> c_int {
    i32::from(as_meter(widget.cast()).is_some())
}

/// The stored fraction, 0.0 when `pointer` is not a meter.
///
/// # Safety
///
/// `pointer` must be null or a live `GtkWidget`.
pub unsafe fn lambdock_meter_fraction(pointer: *mut ffi::GtkWidget) -> f64 {
    match as_meter(pointer) {
        Some(meter) => meter.imp().fraction.get(),
        None => 0.0,
    }
}

/// Where every part of a gauge in a `width` x `height` box goes.
pub fn lambdock_meter_geometry_compute(width: c_int, height: c_int) -> MeterGeometry {
    geometry(width, height)
}

/// The colour of the value arc for a reading.
pub fn lambdock_meter_tone_for_fraction(fraction: f64) -> gdk::RGBA {
    tone_for_fraction(fraction)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The C test reads this struct by value and by field, so a mismatch here is
    /// a corrupted drawing rather than a compile error: 4 floats for the tile and
    /// 6 more for the rest.
    #[test]
    fn the_geometry_mirror_matches_the_c_struct() {
        assert_eq!(std::mem::size_of::<MeterGeometry>(), 40);
        assert_eq!(std::mem::align_of::<MeterGeometry>(), 4);
    }

    /// The tile stays inside the box, the ring inside the tile, and everything
    /// scales with the icon size.
    #[test]
    fn the_geometry_is_proportional_to_the_box() {
        let g = geometry(48, 48);

        // The tile is the box minus 4% of padding on each side, with 22% corners.
        assert!((g.tile.x() - 48.0 * 0.04).abs() < 1e-4);
        assert!((g.tile.y() - 48.0 * 0.04).abs() < 1e-4);
        assert!((g.tile.width() - (48.0 - 2.0 * 48.0 * 0.04)).abs() < 1e-4);
        assert!((g.tile_radius - 48.0 * 0.22).abs() < 1e-4);

        // Centred, with a ring that fits inside the tile.
        assert!((g.center_x - 24.0).abs() < 1e-4);
        assert!((g.center_y - 24.0).abs() < 1e-4);
        assert!(g.line_width >= 2.0);
        assert!(g.radius > 0.0);
        assert!(g.radius + g.line_width / 2.0 < 48.0 * 0.5);

        // The reading gets a square inside the ring.
        assert!((g.text_box - 48.0 * 0.66).abs() < 1e-4);

        // Bigger icons are the same drawing, scaled.
        let big = geometry(96, 96);
        assert!(big.radius > g.radius);
        assert!(big.tile.width() > g.tile.width());
        assert!((big.radius / g.radius - 2.0).abs() < 0.05);
    }

    /// A gauge in a rectangular box uses the short side, so it never overflows.
    #[test]
    fn a_wide_box_uses_its_short_side() {
        let wide = geometry(120, 40);

        assert!((wide.center_x - 60.0).abs() < 1e-4);
        assert!((wide.center_y - 20.0).abs() < 1e-4);
        assert!(wide.radius + wide.line_width / 2.0 < 20.0);
    }

    /// No area, no geometry: the snapshot returns before drawing.
    #[test]
    fn an_empty_box_has_no_geometry() {
        for (width, height) in [(0, 48), (48, 0), (-4, 48)] {
            let none = geometry(width, height);
            assert_eq!(none.radius, 0.0);
            assert_eq!(none.tile.width(), 0.0);
        }
    }

    /// Green while there is room, amber when it gets tight, red when it is
    /// nearly full -- with the thresholds the dock documents.
    #[test]
    fn the_tone_follows_the_fraction() {
        let green = tone_for_fraction(0.10);
        let green_edge = tone_for_fraction(0.59);
        let amber = tone_for_fraction(0.60);
        let amber_edge = tone_for_fraction(0.84);
        let red = tone_for_fraction(0.85);
        let full = tone_for_fraction(1.0);

        assert_eq!(green.green(), green_edge.green());
        assert_ne!(green_edge.green(), amber.green());
        assert_eq!(amber.green(), amber_edge.green());
        assert_ne!(amber_edge.green(), red.green());
        assert_eq!(red.green(), full.green());

        // Green, then amber (less green, more red), then red dominates.
        assert!(green.green() > amber.green());
        assert!(amber.red() > green.red());
        assert!(red.red() > red.green());

        // Out-of-range readings still get a colour, and so does a NaN: a reading
        // the dock cannot understand must not look comfortable.
        assert_eq!(tone_for_fraction(-1.0), green);
        assert_eq!(tone_for_fraction(2.0), red);
        assert_eq!(tone_for_fraction(f64::NAN), red);
    }

    /// A hair of an arc still needs two points to be a line, and a full ring is
    /// not finer than the step count allows.
    #[test]
    fn the_arc_is_a_polyline_of_whole_steps() {
        assert_eq!(arc_steps(0.0), 2);
        assert_eq!(arc_steps(0.001), 2);
        assert_eq!(arc_steps(0.25), 45);
        assert_eq!(arc_steps(0.5), 90);
        assert_eq!(arc_steps(1.0), 180);
        assert_eq!(arc_steps(2.0), 180);
    }
}
