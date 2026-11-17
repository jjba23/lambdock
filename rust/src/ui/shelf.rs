// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The glass shelf, measured: the first half of `src/ui/shelf.c` in Rust.
//!
//! What the dock *measures* moved first, because it is what everything else asks
//! the shelf for: which parameters are in force, whether the shelf look is on at
//! all, how thick the capsule is and how deep the reflection band runs. The two
//! widgets that paint the glass -- the frame and the reflections -- are still in
//! C, and will move in their own step.
//!
//! Geometry (all values in pixels, `icon` = icon size):
//!
//! ```text
//! band    = round(icon * reflection_ratio)
//! outer   = max(front_px, band)          // glass on the screen-edge side
//! capsule = icon + margin_px + outer
//! ```
//!
//! With the defaults (tilt 0, reflection 0, front 0, margin = capsule padding)
//! that is exactly the classic `icon + 24px` capsule, the frame is a pass-through
//! and the reflections stay hidden.

use crate::core::config_values::{
    lambdock_shelf_params_default, ShelfParams, EDGE_BOTTOM, EDGE_LEFT, EDGE_RIGHT, EDGE_TOP,
    SHELF_DEFAULT_PERSPECTIVE, SHELF_DEFAULT_REFLECTION, SHELF_DEFAULT_REFLECTION_BLUR,
    SHELF_DEFAULT_REFLECTION_INTENSITY,
};
use crate::core::dock_dims::ShelfGeometry;
use crate::dock_item::DockItem;
use crate::state::LambdockState;
use gtk4::glib::translate::{from_glib_borrow, from_glib_none, IntoGlib, IntoGlibPtr};
use gtk4::prelude::*;
use gtk4::subclass::prelude::*;
use gtk4::{ffi, gdk, glib, graphene, gsk};
use std::cell::Cell;
use std::os::raw::{c_double, c_int, c_void};

/// `DEFAULT_ICON_SIZE` in `lambdock.h`.
const DEFAULT_ICON_SIZE: c_int = 48;

/// The stops the reflection's fade is drawn with. Six is enough to bend the
/// straight line of a gradient into the wash the band stack approximated, and it
/// is one gradient either way.
const FADE_STOPS: i32 = 6;

/// The alpha a reflection fades to at `t`, where 0 is the icon's edge and 1 the
/// far edge of the band: `intensity` at the icon, nothing at the end, on the
/// power curve the band stack used.
pub fn reflection_fade_alpha(intensity: f64, t: f64) -> f64 {
    if !intensity.is_finite() || intensity <= 0.0 {
        return 0.0;
    }

    let t = t.clamp(0.0, 1.0);

    intensity.min(1.0) * (1.0 - t).powf(1.1)
}

/// The shelf parameters that should be used for `state`.
///
/// A zero-initialised state (a partially built dock, a unit test) carries no shelf
/// configuration at all; it is treated as the plain dock rather than as a capsule
/// with zero margin, which would collapse the layout.
pub fn lambdock_shelf_params_effective(state: &LambdockState) -> ShelfParams {
    let params = state.shelf;

    if params.margin_px == 0
        && params.front_px == 0
        && params.reflection_ratio == 0.0
        && params.tilt_deg == 0.0
        && params.flare_px == 0
    {
        return lambdock_shelf_params_default();
    }

    params
}

/// Whether the shelf look is in force for `state`.
pub fn lambdock_shelf_active(state: &LambdockState) -> c_int {
    let defaults = lambdock_shelf_params_default();
    let params = lambdock_shelf_params_effective(state);

    c_int::from(
        params.tilt_deg != defaults.tilt_deg
            || params.margin_px != defaults.margin_px
            || params.front_px != defaults.front_px
            || params.flare_px != defaults.flare_px
            || params.reflection_ratio > defaults.reflection_ratio,
    )
}

/// The shelf measured out for `params` at `icon_size`.
///
/// Pure: the same parameters always measure the same edge, which is what lets the
/// capsule be checked without a dock.
pub fn geometry(params: &ShelfParams, icon_size: c_int) -> ShelfGeometry {
    let icon = if icon_size > 0 {
        icon_size
    } else {
        DEFAULT_ICON_SIZE
    };

    let band = (f64::from(icon) * params.reflection_ratio).round() as c_int;
    let band = band.max(0);
    let outer = params.front_px.max(band);
    let capsule = (icon + params.margin_px + outer).max(0);

    ShelfGeometry {
        band_px: band,
        outer_px: outer,
        capsule_px: capsule,
        overhang_px: (-params.margin_px).max(0),
    }
}

/// The shelf measured out for `state` at `icon_size`.
pub fn lambdock_shelf_geometry_for(state: &LambdockState, icon_size: c_int) -> ShelfGeometry {
    geometry(&lambdock_shelf_params_effective(state), icon_size)
}

/// The keystone that gives the dock its shelf-look silhouette, as a graphene
/// matrix (row-major, applied to row vectors).
///
/// The inner edge keeps the dock's width and the edge lying on the screen is
/// drawn `flare` pixels wider per side, as if the slab were a shelf seen slightly
/// from above. That has to be a real projection -- a shear would slant both sides
/// the same way, while this widens symmetrically around the middle -- so the
/// matrix carries a `w` that depends on depth. It is written out directly instead
/// of being composed from translate/project/translate, because translations do not
/// commute with a projective row and the composed version drifts sideways.
///
/// For a bottom dock (depth = y, cross = x, cx = width/2):
///
/// ```text
/// w(y) = 1 - c * y                    with c = (1 - pinned_scale) / height
/// x'   = (x - cx) / w(y) + cx         ->  (x - cx*c*y, y) / w(y)
/// y'   = y / w(y)
/// ```
///
/// so the top edge (y = 0, w = 1) is untouched and the bottom edge is magnified by
/// `1 / pinned_scale` around the centre line.
///
/// `None` when there is nothing to project: no flare, no depth, or no slab left
/// after the reserved flare.
pub fn keystone(
    width: c_int,
    height: c_int,
    horizontal: bool,
    flare: c_int,
    pinned_at_end: bool,
) -> Option<[f32; 16]> {
    let depth = f64::from(if horizontal { height } else { width });
    // The frame reserves `2 * flare` on the cross axis for the widened edge, so
    // the slab itself spans the rest.
    let cross = f64::from(if horizontal { width } else { height }) - 2.0 * f64::from(flare);

    if flare <= 0 || depth <= 0.0 || cross <= 0.0 {
        return None;
    }

    // Keep the slab at most 1.5x as wide at the screen edge: past that the
    // projection folds in on itself.
    let amount = f64::from(flare).min(cross * 0.25);

    // w on the edge that touches the screen; dividing by it magnifies that edge.
    let pinned_scale = (cross - 2.0 * amount) / cross;
    let sweep = (1.0 - pinned_scale) / depth; // d(w)/d(depth)
    let centre = f64::from(if horizontal { width } else { height }) / 2.0;

    let (cross_from_depth, cross_constant, w_from_depth, w_constant) = if pinned_at_end {
        // w(depth) = 1 - sweep * depth, so the far edge (depth 0) is untouched.
        (-centre * sweep, 0.0, -sweep, 1.0)
    } else {
        // w(depth) = pinned_scale + sweep * depth, so the pinned edge (depth 0) is
        // the magnified one.
        (
            centre * sweep,
            centre * (pinned_scale - 1.0),
            sweep,
            pinned_scale,
        )
    };

    // graphene stores matrices row-major and applies them to row vectors, so
    // (x', y', w') = (x, y, 1) * M and the result is divided by w'. The `w' row is
    // row 3; the cross coordinate is row 0 for a horizontal dock and row 1 for a
    // vertical one, and it is the *column* of the depth axis that picks up the
    // projection.
    let mut values = [
        1.0f32, 0.0, 0.0, 0.0, // row 0: x
        0.0, 1.0, 0.0, 0.0, // row 1: y
        0.0, 0.0, 1.0, 0.0, // row 2: z
        0.0, 0.0, 0.0, 1.0, // row 3: w
    ];

    if horizontal {
        values[4] = cross_from_depth as f32; // m10: x picks up y
        values[12] = cross_constant as f32; // m30
        values[7] = w_from_depth as f32; // m13: w picks up y
        values[15] = w_constant as f32; // m33
    } else {
        values[1] = cross_from_depth as f32; // m01: y picks up x
        values[13] = cross_constant as f32; // m31
        values[3] = w_from_depth as f32; // m03: w picks up x
        values[15] = w_constant as f32; // m33
    }

    Some(values)
}

/// The keystone as the C frame asks for it, into a caller-provided matrix.
///
/// # Safety
///
/// `out` must be null or point at a writable `graphene_matrix_t`.
pub unsafe fn lambdock_shelf_keystone_matrix(
    width: c_int,
    height: c_int,
    horizontal: c_int,
    flare: c_int,
    pinned_at_end: c_int,
    out: *mut graphene::ffi::graphene_matrix_t,
) -> c_int {
    if out.is_null() {
        return 0;
    }

    let Some(values) = keystone(width, height, horizontal != 0, flare, pinned_at_end != 0) else {
        return 0;
    };

    // graphene_matrix_t is sixteen floats and nothing else.
    std::ptr::copy_nonoverlapping(values.as_ptr(), out.cast::<f32>(), values.len());

    1
}

/// Whether a dock on this edge runs horizontally, i.e. whether its depth is the
/// vertical axis.
fn is_horizontal(edge: c_int) -> bool {
    edge == EDGE_TOP || edge == EDGE_BOTTOM
}

/// Whether the edge touching the screen is the far end of the depth axis.
fn is_pinned_at_end(edge: c_int) -> bool {
    edge == EDGE_BOTTOM || edge == EDGE_RIGHT
}

/// Whether a renderer of this type can draw a projective transform.
///
/// The cairo renderer cannot: it has no perspective at all, and instead of
/// skipping the node it paints the whole surface in its "unsupported node"
/// colour, which arrives as a flat hot-pink rectangle with square corners. That
/// is what a dock on a machine whose EGL display cannot be created shows, since
/// GTK then falls back to cairo -- and cairo is also why such a dock is slow in
/// the first place.
///
/// Named rather than typed so the rule holds without a display, which is what
/// makes it a test.
pub fn renderer_can_project(type_name: &str) -> bool {
    type_name != "GskCairoRenderer"
}

/// Whether the surface `widget` is drawn on can render a projective transform.
///
/// A widget with no native is not being painted on any surface: say yes, so a
/// frame built offscreen still measures and maps the way a real one does.
fn surface_can_project(widget: &gtk4::Widget) -> bool {
    let projectable = widget
        .native()
        .and_then(|native| native.renderer())
        .is_none_or(|renderer| renderer_can_project(renderer.type_().name()));

    if !projectable {
        warn_once_software_renderer();
    }

    projectable
}

/// Says once, in the log, why the shelf came out as a plain rectangle.
///
/// A dock on the software renderer is slow as well as square, and both answers
/// are the same one: GTK could not create an EGL display and fell back to cairo.
fn warn_once_software_renderer() {
    static WARNED: std::sync::OnceLock<()> = std::sync::OnceLock::new();

    if WARNED.set(()).is_ok() {
        crate::log_warn!(
            "UI",
            "The dock is drawn by the software (cairo) GSK renderer, which has no projective transform: the shelf's keystone and tilt are drawn as a plain rectangle, and every frame is composited on the CPU. Check that GTK can create an EGL display (GSK_RENDERER=gl) if the dock is slow."
        );
    }
}

/// The projection the frame applies to the slab: the keystone (which gives the
/// slab its trapezoid) followed by the tilt (which tips it away from the viewer).
///
/// `None` when the shelf is a plain rectangle that needs no projection, and when
/// the surface's renderer cannot draw one -- see [`renderer_can_project`].
pub fn frame_transform(
    projectable: bool,
    edge: c_int,
    tilt: f64,
    perspective: f64,
    flare: c_int,
    width: i32,
    height: i32,
) -> Option<gsk::Transform> {
    if !projectable || (tilt == 0.0 && flare == 0) {
        return None;
    }
    if width <= 0 || height <= 0 {
        return None;
    }

    let horizontal = is_horizontal(edge);

    let keystone = keystone(width, height, horizontal, flare, is_pinned_at_end(edge))
        .map(|values| gsk::Transform::new().matrix(&graphene::Matrix::from_float(values)));

    let tilt_transform = if tilt == 0.0 {
        None
    } else {
        // Tilt around the docked edge: that edge stays pinned on screen while the
        // inner edge recedes and is projected slightly narrower.
        let (width, height) = (width as f32, height as f32);
        let pivot = match edge {
            EDGE_TOP => graphene::Point::new(width / 2.0, 0.0),
            EDGE_LEFT => graphene::Point::new(0.0, height / 2.0),
            EDGE_RIGHT => graphene::Point::new(width, height / 2.0),
            _ => graphene::Point::new(width / 2.0, height),
        };

        let axis = if horizontal {
            graphene::Vec3::new(1.0, 0.0, 0.0)
        } else {
            graphene::Vec3::new(0.0, 1.0, 0.0)
        };

        let origin = graphene::Point::new(-pivot.x(), -pivot.y());

        Some(
            gsk::Transform::new()
                .translate(&origin)
                .rotate_3d(tilt as f32, &axis)
                .perspective(perspective as f32)
                .translate(&pivot),
        )
    };

    match (keystone, tilt_transform) {
        (None, None) => None,
        (Some(keystone), None) => Some(keystone),
        (None, Some(tilt)) => Some(tilt),
        // The keystone is applied to the tilted slab, as the frame has always drawn
        // it: `transform(next)` applies `self` first.
        (Some(keystone), Some(tilt)) => Some(keystone.transform(Some(&tilt))),
    }
}

mod imp {
    use super::*;

    #[derive(Default)]
    pub struct ShelfFrame {
        /// Docked edge: the tilt axis and the edge the slab stays pinned to.
        pub edge: Cell<c_int>,
        /// Tilt angle in degrees (0 = disabled).
        pub tilt: Cell<f64>,
        /// Projective distance in pixels.
        pub perspective: Cell<f64>,
        /// Keystone lean per side, in pixels.
        pub flare: Cell<c_int>,
        pub child: std::cell::RefCell<Option<gtk4::Widget>>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for ShelfFrame {
        const NAME: &'static str = "LambdockShelfFrame";
        type Type = super::ShelfFrame;
        type ParentType = gtk4::Widget;
    }

    impl ObjectImpl for ShelfFrame {
        fn constructed(&self) {
            self.parent_constructed();

            self.edge.set(EDGE_BOTTOM);
            self.perspective.set(SHELF_DEFAULT_PERSPECTIVE);

            let widget = self.obj();
            widget.set_can_focus(false);
            widget.set_can_target(false);
            widget.set_overflow(gtk4::Overflow::Visible);
            widget.add_css_class("shelf-frame");
        }

        fn dispose(&self) {
            if let Some(child) = self.child.borrow_mut().take() {
                child.unparent();
            }
        }
    }

    impl WidgetImpl for ShelfFrame {
        fn measure(&self, orientation: gtk4::Orientation, for_size: i32) -> (i32, i32, i32, i32) {
            let Some(child) = self
                .child
                .borrow()
                .clone()
                .filter(|child| child.should_layout())
            else {
                return (0, 0, -1, -1);
            };

            let (minimum, natural, min_baseline, nat_baseline) =
                child.measure(orientation, for_size);

            // The keystone widens the screen-side edge, so the frame has to reserve
            // that much room across the dock or the flare would be clipped by the
            // surface.
            let horizontal = is_horizontal(self.edge.get());
            let cross_axis = (horizontal && orientation == gtk4::Orientation::Horizontal)
                || (!horizontal && orientation == gtk4::Orientation::Vertical);
            let flare = self.flare.get().max(0);

            if cross_axis && flare > 0 {
                (
                    minimum + 2 * flare,
                    natural + 2 * flare,
                    min_baseline,
                    nat_baseline,
                )
            } else {
                (minimum, natural, min_baseline, nat_baseline)
            }
        }

        fn size_allocate(&self, width: i32, height: i32, baseline: i32) {
            let Some(child) = self
                .child
                .borrow()
                .clone()
                .filter(|child| child.should_layout())
            else {
                return;
            };

            let horizontal = is_horizontal(self.edge.get());

            // The child is the slab as the theme draws it: it keeps the dock's
            // width on the inner edge, and the projection spreads it out to fill the
            // room reserved for the flare.
            let inset = self.flare.get().max(0);
            let offset = graphene::Point::new(
                if horizontal { inset as f32 } else { 0.0 },
                if horizontal { 0.0 } else { inset as f32 },
            );
            let transform = if inset > 0 {
                Some(gsk::Transform::new().translate(&offset))
            } else {
                None
            };

            child.allocate(
                if horizontal { width - 2 * inset } else { width },
                if horizontal {
                    height
                } else {
                    height - 2 * inset
                },
                baseline,
                transform,
            );
        }

        fn snapshot(&self, snapshot: &gtk4::Snapshot) {
            let widget = self.obj();
            let Some(child) = self.child.borrow().clone() else {
                return;
            };

            let as_widget: &gtk4::Widget = widget.upcast_ref();
            let transform = frame_transform(
                surface_can_project(as_widget),
                self.edge.get(),
                self.tilt.get(),
                self.perspective.get(),
                self.flare.get(),
                widget.width(),
                widget.height(),
            );

            let Some(transform) = transform else {
                widget.snapshot_child(&child, snapshot);
                return;
            };

            snapshot.save();
            snapshot.transform(Some(&transform));
            widget.snapshot_child(&child, snapshot);
            snapshot.restore();
        }
    }
}

glib::wrapper! {
    /// A slab re-rendered through a projective tilt and keystone.
    pub struct ShelfFrame(ObjectSubclass<imp::ShelfFrame>)
        @extends gtk4::Widget,
        @implements gtk4::Accessible, gtk4::Buildable, gtk4::ConstraintTarget;
}

/// The frame behind a widget pointer, or `None` when it is not one.
///
/// # Safety
///
/// `pointer` must be null or a live `GtkWidget`.
unsafe fn as_frame(pointer: *mut ffi::GtkWidget) -> Option<glib::translate::Borrowed<ShelfFrame>> {
    if pointer.is_null() {
        return None;
    }

    let widget: glib::translate::Borrowed<gtk4::Widget> = from_glib_borrow(pointer);
    if !widget.is::<ShelfFrame>() {
        return None;
    }

    let widget: gtk4::Widget = widget.into_inner();
    Some(glib::translate::Borrowed::new(widget.unsafe_cast()))
}

/// Wraps `child` in a frame.
///
/// # Safety
///
/// `child` must be null or an unparented widget; the result is `(transfer full)`
/// and the C callers append it, like the `g_object_new` this replaces.
pub unsafe fn shelf_frame_new(child: *mut ffi::GtkWidget) -> *mut ffi::GtkWidget {
    crate::ui::ensure_gtk_initialized();

    let frame: ShelfFrame = glib::Object::builder().build();

    if !child.is_null() {
        let child: gtk4::Widget = gtk4::glib::translate::from_glib_none(child);
        child.set_parent(&frame);
        *frame.imp().child.borrow_mut() = Some(child);
    }

    glib::gobject_ffi::g_object_force_floating(frame.as_ptr().cast());

    frame.upcast::<gtk4::Widget>().into_glib_ptr()
}

/// Sets the tilt, keeping whatever flare the frame already has.
///
/// # Safety
///
/// `frame` must be null or a live frame.
pub unsafe fn shelf_frame_set_tilt(
    frame: *mut ffi::GtkWidget,
    edge: c_int,
    degrees: c_double,
    perspective: c_double,
) {
    let flare = match as_frame(frame) {
        Some(frame) => frame.imp().flare.get(),
        None => 0,
    };

    shelf_frame_set_shape(frame, edge, degrees, perspective, flare);
}

/// Sets the docked edge, the tilt and the keystone in one go.
///
/// # Safety
///
/// `frame` must be null or a live frame.
pub unsafe fn shelf_frame_set_shape(
    frame: *mut ffi::GtkWidget,
    edge: c_int,
    degrees: c_double,
    perspective: c_double,
    flare_px: c_int,
) {
    let Some(frame) = as_frame(frame) else {
        return;
    };

    let imp = frame.imp();
    let geometry_changed = imp.edge.get() != edge || imp.flare.get() != flare_px;
    let paint_changed = geometry_changed || imp.tilt.get() != degrees;

    imp.edge.set(edge);
    imp.tilt.set(degrees);
    imp.flare.set(flare_px);
    if perspective > 0.0 {
        imp.perspective.set(perspective);
    }

    // The flare reserves room in the layout, so a change in it has to re-measure;
    // the tilt only changes how the same slab is painted.
    if geometry_changed {
        frame.queue_resize();
    } else if paint_changed {
        frame.queue_draw();
    }
}

/// Whether `widget` is a shelf frame.
///
/// # Safety
///
/// `widget` must be null or a live `GtkWidget`.
pub unsafe fn shelf_frame_is_frame(widget: *mut c_void) -> c_int {
    c_int::from(as_frame(widget.cast()).is_some())
}

/// Whether the slab is drawn as a shape rather than a rectangle.
///
/// It is a rectangle when the surface's renderer cannot draw the projection, so
/// this answers what is painted rather than what was asked for: the pointer
/// mapping that reads it has to agree with the pixels.
///
/// # Safety
///
/// `frame` must be null or a live `GtkWidget`.
pub unsafe fn shelf_frame_is_projected(frame: *mut ffi::GtkWidget) -> c_int {
    let Some(frame) = as_frame(frame) else {
        return 0;
    };
    let widget: &gtk4::Widget = frame.upcast_ref();
    let projected = frame.imp().tilt.get() != 0.0 || frame.imp().flare.get() != 0;

    c_int::from(projected && surface_can_project(widget))
}

/// The axis across the dock: vertical for a dock on the top or bottom edge,
/// horizontal for one on the left or right.
///
/// # Safety
///
/// `frame` must be null or a live `GtkWidget`.
pub unsafe fn shelf_frame_cross_orientation(frame: *mut ffi::GtkWidget) -> c_int {
    let Some(frame) = as_frame(frame) else {
        return gtk4::Orientation::Vertical.into_glib();
    };

    let cross = if is_horizontal(frame.imp().edge.get()) {
        gtk4::Orientation::Vertical
    } else {
        gtk4::Orientation::Horizontal
    };

    cross.into_glib()
}

/// Projects `point` through the frame's transform, in place.
///
/// # Safety
///
/// `frame` must be null or a live frame; `point` must be null or point at a
/// writable `graphene_point_t`.
pub unsafe fn shelf_frame_project_point(
    frame: *mut ffi::GtkWidget,
    point: *mut graphene::ffi::graphene_point_t,
) -> c_int {
    let Some(frame) = as_frame(frame) else {
        return 0;
    };
    if point.is_null() {
        return 0;
    }

    let widget: &gtk4::Widget = frame.upcast_ref();
    let Some(transform) = frame_transform(
        surface_can_project(widget),
        frame.imp().edge.get(),
        frame.imp().tilt.get(),
        frame.imp().perspective.get(),
        frame.imp().flare.get(),
        frame.width(),
        frame.height(),
    ) else {
        return 0;
    };

    let target = graphene::Point::new((*point).x, (*point).y);
    let projected = transform.transform_point(&target);

    (*point).x = projected.x();
    (*point).y = projected.y();

    1
}

// ============================================================================
// LambdockReflection -- mirrored, blurred, fading icon reflection
// ============================================================================
//
// The widget paints, on the glass in front of one icon:
//
//   1. a soft contact shadow where the icon meets the shelf,
//   2. the icon itself, mirrored across that contact line, squashed into the
//      reflection band and blurred with a gaussian blur,
//   3. an alpha falloff (band by band, power curve) so it fades as it recedes,
//   4. the running-app glow dot near the outer edge of the glass.
//
// The fade is drawn as REFLECTION_BANDS stacked semi-transparent bands; the blur
// pass smooths their boundaries back into a continuous gradient.

/// Whether the band is stacked vertically, which a dock on the top or bottom edge
/// asks for.
fn is_vertical_band(edge: c_int) -> bool {
    edge == EDGE_TOP || edge == EDGE_BOTTOM
}

/// Height of the reflection band for a given icon size.
fn band_size(band_ratio: f64, icon_size: i32) -> i32 {
    let ratio = if band_ratio > 0.0 {
        band_ratio
    } else {
        SHELF_DEFAULT_REFLECTION
    };

    (f64::from(icon_size) * ratio).round() as i32
}

/// The artwork a reflection mirrors: the icon itself rather than the widget that
/// shows it. A widget paintable of a widget that is not mapped and sized yet
/// latches a zero intrinsic size and then never paints anything.
fn artwork_for(source: &gtk4::Widget) -> Option<gdk::Paintable> {
    let image = source.downcast_ref::<gtk4::Image>()?;

    let size = match image.pixel_size() {
        size if size > 0 => size,
        _ => DEFAULT_ICON_SIZE,
    };
    let scale = source.scale_factor().max(1);
    let theme = gtk4::IconTheme::for_display(&gdk::Display::default()?);

    match image.storage_type() {
        gtk4::ImageType::IconName => {
            let name = image.icon_name().filter(|name| !name.is_empty())?;
            let paintable = theme.lookup_icon(
                &name,
                &[],
                size,
                scale,
                gtk4::TextDirection::None,
                gtk4::IconLookupFlags::empty(),
            );

            Some(paintable.upcast())
        }
        gtk4::ImageType::Gicon => {
            let icon = image.gicon()?;
            let paintable = theme.lookup_by_gicon(
                &icon,
                size,
                scale,
                gtk4::TextDirection::None,
                gtk4::IconLookupFlags::empty(),
            );

            Some(paintable.upcast())
        }
        gtk4::ImageType::Paintable => image.paintable(),
        _ => None,
    }
}

/// Appends one elliptical radial gradient: centre and radii in pixels, stops as
/// fractions of the radius.
///
/// Every decoration the band paints goes through here, because Gsk wants the
/// bounds rectangle and both radii and it is easy to get the axes the wrong way
/// round by hand.
fn paint_ellipse(
    snapshot: &gtk4::Snapshot,
    cx: f64,
    cy: f64,
    rx: f64,
    ry: f64,
    stops: &[gsk::ColorStop],
) {
    if rx <= 0.0 || ry <= 0.0 || stops.is_empty() {
        return;
    }

    let bounds = graphene::Rect::new(
        (cx - rx) as f32,
        (cy - ry) as f32,
        (rx * 2.0) as f32,
        (ry * 2.0) as f32,
    );
    let centre = graphene::Point::new(cx as f32, cy as f32);

    snapshot.append_radial_gradient(&bounds, &centre, rx as f32, ry as f32, 0.0, 1.0, stops);
}

/// Soft dark pool where the icon meets the glass, so the icon reads as resting on
/// the shelf instead of floating above it.
///
/// The shape is the reference dock's `wviconshadow.png`: a Gaussian pool that
/// measures 0.40 alpha at the icon's centre, 0.28 at a third of an icon out, 0.06
/// at three quarters, and nothing past 0.61 icon. Drawing it as a pool centred on
/// the mirror axis -- rather than a linear band running down the whole band -- is
/// what keeps it under the icon: it fades out at the icon's sides too instead of
/// turning the band into the flat grey strip that made this decoration worth
/// switching off.
///
/// The pool follows `reflection-intensity`, since it is part of the same material
/// as the mirror: at the default 0.55 it peaks at 0.22, and at zero it is gone.
fn paint_contact_shadow(snapshot: &gtk4::Snapshot, edge: c_int, w: f64, h: f64, intensity: f64) {
    let vertical = is_vertical_band(edge);
    let cross = if vertical { w } else { h };
    let axis_len = if vertical { h } else { w };
    let peak = 0.45f64.min(0.40 * intensity.max(0.0));

    if cross <= 0.0 || axis_len <= 0.0 || peak <= 0.0 {
        return;
    }

    // The icon-side edge of the band is the mirror axis: the top of the band for a
    // bottom dock, the left for a left dock. The pool is centred there, so the
    // half the glass can show is the half that fades away from the icon.
    let mut cx = w / 2.0;
    let mut cy = h / 2.0;

    if vertical {
        cy = if edge == EDGE_BOTTOM { 0.0 } else { h };
    } else {
        cx = if edge == EDGE_LEFT { 0.0 } else { w };
    }

    // Across the mirror axis the measured pool spans 0.61 icon; along it the band's
    // own depth is the extent, so the shadow dies out before the outer edge.
    let radius_cross = cross * 0.625;
    let radius_along = axis_len;

    let stops = [
        gsk::ColorStop::new(0.00, gdk::RGBA::new(0.0, 0.0, 0.0, peak as f32)),
        gsk::ColorStop::new(0.20, gdk::RGBA::new(0.0, 0.0, 0.0, (peak * 0.79) as f32)),
        gsk::ColorStop::new(0.40, gdk::RGBA::new(0.0, 0.0, 0.0, (peak * 0.43) as f32)),
        gsk::ColorStop::new(0.60, gdk::RGBA::new(0.0, 0.0, 0.0, (peak * 0.17) as f32)),
        gsk::ColorStop::new(0.80, gdk::RGBA::new(0.0, 0.0, 0.0, (peak * 0.04) as f32)),
        gsk::ColorStop::new(1.00, gdk::RGBA::new(0.0, 0.0, 0.0, 0.0)),
    ];

    let (rx, ry) = if vertical {
        (radius_cross, radius_along)
    } else {
        (radius_along, radius_cross)
    };

    paint_ellipse(snapshot, cx, cy, rx, ry, &stops);
}

/// The small glowing dot the shelf-look dock showed for apps with open windows: a
/// white bead sitting on a cyan glow.
///
/// Two images describe it, and they are not the same shape rotated: the bottom
/// dock's glow is 54x12 with a 12x3 bead, the side one's 6x16 with a 2x11 bead. In
/// both cases the bead runs along the row of icons and the glow is stretched that
/// way too, but the side dock's glow is the tighter of the two.
///
/// Three things are drawn on purpose rather than by the reference's proportions
/// alone, because a 12x3px bead on a 64px tile is 9x2.2px at 48px icons and
/// antialiases into the glass: the bead is a little larger (and never under 7x3px),
/// it is painted as a bar with crisp top and bottom edges instead of a radial blob,
/// and the dark pool under it is tight -- about 2.4 bead widths -- because a wide
/// pool turns the reflection band into a grey smear that the white bead then has to
/// be seen against.
///
/// The bead is white, and a white bead disappears into a pale slab, so it is drawn
/// over that pool: on light material the pool reads as the icon's shadow and the
/// bead stays visible; on dark glass the pool blends in and only the glow shows.
fn paint_running_dot(snapshot: &gtk4::Snapshot, edge: c_int, w: f64, h: f64) {
    let vertical = is_vertical_band(edge);
    let cross = if vertical { w } else { h };

    let bead_cross_ratio = if vertical { 0.20 } else { 0.18 };
    let bead_along_ratio = if vertical { 0.07 } else { 0.04 };
    let glow_cross_scale = if vertical { 3.2 } else { 1.6 };
    let glow_along_scale = 2.6;
    let pool_cross_scale = if vertical { 2.4 } else { 2.0 };
    let pool_along_scale = if vertical { 1.9 } else { 3.2 };
    let edge_lift = 2.5;

    let bead_cross = 7.0f64.max(cross * bead_cross_ratio);
    let bead_along = 3.0f64.max(cross * bead_along_ratio);

    // Distance from the outer edge to the middle of the bead.
    let lift = bead_along / 2.0 + edge_lift;

    let mut cx = w / 2.0;
    let mut cy = h / 2.0;

    if vertical {
        cy = if edge == EDGE_BOTTOM { h - lift } else { lift };
    } else {
        cx = if edge == EDGE_LEFT { w - lift } else { lift };
    }

    // Which of x and y is the cross axis depends on the band's orientation; the
    // gradients below take explicit radii, so this is the only place the swap has
    // to happen.
    let bead_rx = if vertical {
        bead_cross / 2.0
    } else {
        bead_along / 2.0
    };
    let bead_ry = if vertical {
        bead_along / 2.0
    } else {
        bead_cross / 2.0
    };

    // Three layers, back to front: the pool the bead sits in, the cyan glow, and the
    // bead.
    let pool_stops = [
        gsk::ColorStop::new(0.0, gdk::RGBA::new(0.0, 0.0, 0.0, 0.34)),
        gsk::ColorStop::new(0.45, gdk::RGBA::new(0.0, 0.0, 0.0, 0.16)),
        gsk::ColorStop::new(1.0, gdk::RGBA::new(0.0, 0.0, 0.0, 0.0)),
    ];
    let (pool_rx, pool_ry) = if vertical {
        (bead_rx * pool_cross_scale, bead_ry * pool_along_scale)
    } else {
        (bead_ry * pool_along_scale, bead_rx * pool_cross_scale)
    };
    paint_ellipse(snapshot, cx, cy, pool_rx, pool_ry, &pool_stops);

    // The glow colour is the one the reference image for this orientation uses:
    // 139/207/255 across, 0/158/250 down the side.
    let (glow_r, glow_g) = if vertical { (0.55, 0.81) } else { (0.0, 0.62) };
    let glow_stops = [
        gsk::ColorStop::new(0.0, gdk::RGBA::new(glow_r, glow_g, 1.0, 0.45)),
        gsk::ColorStop::new(0.45, gdk::RGBA::new(glow_r, glow_g, 1.0, 0.18)),
        gsk::ColorStop::new(1.0, gdk::RGBA::new(glow_r, glow_g, 1.0, 0.0)),
    ];
    let (glow_rx, glow_ry) = if vertical {
        (bead_rx * glow_cross_scale, bead_ry * glow_along_scale)
    } else {
        (bead_ry * glow_along_scale, bead_rx * glow_cross_scale)
    };
    paint_ellipse(snapshot, cx, cy, glow_rx, glow_ry, &glow_stops);

    // The bead itself is a bar, not a radial blob: a gradient that fades along the
    // row gives it soft ends, and drawing it as a rectangle keeps its two or three
    // pixels of height crisp instead of smearing them into the glass. That crisp
    // edge is most of what makes the dot legible.
    let bead = graphene::Rect::new(
        (cx - bead_rx) as f32,
        (cy - bead_ry) as f32,
        (bead_rx * 2.0) as f32,
        (bead_ry * 2.0) as f32,
    );
    let (from, to) = if vertical {
        (
            graphene::Point::new((cx - bead_rx) as f32, cy as f32),
            graphene::Point::new((cx + bead_rx) as f32, cy as f32),
        )
    } else {
        (
            graphene::Point::new(cx as f32, (cy - bead_ry) as f32),
            graphene::Point::new(cx as f32, (cy + bead_ry) as f32),
        )
    };

    let white = gdk::RGBA::new(1.0, 1.0, 1.0, 0.99);
    let bead_stops = [
        gsk::ColorStop::new(0.00, gdk::RGBA::new(1.0, 1.0, 1.0, 0.0)),
        gsk::ColorStop::new(0.22, white),
        gsk::ColorStop::new(0.78, white),
        gsk::ColorStop::new(1.00, gdk::RGBA::new(1.0, 1.0, 1.0, 0.0)),
    ];

    snapshot.append_linear_gradient(&bead, &from, &to, &bead_stops);
}

/// The pixel size of the reflected source, falling back to the configured icon size
/// while the source widget has not been allocated yet.
fn source_size(paintable: Option<&gdk::Paintable>, icon_size: i32) -> (f64, f64) {
    let mut width = paintable.map_or(0.0, |paintable| f64::from(paintable.intrinsic_width()));
    let mut height = paintable.map_or(0.0, |paintable| f64::from(paintable.intrinsic_height()));

    if width <= 0.0 {
        width = f64::from(if icon_size > 0 {
            icon_size
        } else {
            DEFAULT_ICON_SIZE
        });
    }
    if height <= 0.0 {
        height = width;
    }

    (width, height)
}

mod reflection_imp {
    use super::*;
    use std::cell::RefCell;

    #[derive(Default)]
    pub struct Reflection {
        /// The mirrored widget, held weakly: it owns the reflection, not the other
        /// way round.
        pub source: RefCell<Option<glib::WeakRef<gtk4::Widget>>>,
        /// What gets mirrored.
        pub paintable: RefCell<Option<gdk::Paintable>>,
        /// False until a lazy (widget) paintable is bound.
        pub paintable_ready: Cell<bool>,
        /// Shelf edge: decides the mirror and the fade axis.
        pub edge: Cell<c_int>,
        /// Fallback size while the source is unsized.
        pub icon_size: Cell<i32>,
        /// Reflection band / icon size.
        pub band_ratio: Cell<f64>,
        /// Blur radius / icon size.
        pub blur_ratio: Cell<f64>,
        /// Alpha at the icon-side edge of the band.
        pub intensity: Cell<f64>,
        /// Draw the running-app glow dot.
        pub running: Cell<bool>,
        /// Draw the contact shadow under the icon.
        pub has_shadow: Cell<bool>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for Reflection {
        const NAME: &'static str = "LambdockReflection";
        type Type = super::Reflection;
        type ParentType = gtk4::Widget;
    }

    impl ObjectImpl for Reflection {
        fn constructed(&self) {
            self.parent_constructed();

            self.edge.set(EDGE_BOTTOM);
            self.icon_size.set(DEFAULT_ICON_SIZE);
            self.band_ratio.set(SHELF_DEFAULT_REFLECTION);
            self.blur_ratio.set(SHELF_DEFAULT_REFLECTION_BLUR);
            self.intensity.set(SHELF_DEFAULT_REFLECTION_INTENSITY);

            let widget = self.obj();
            widget.set_can_focus(false);
            widget.set_can_target(false);
            widget.set_halign(gtk4::Align::Center);
            widget.set_valign(gtk4::Align::Center);
            widget.add_css_class("shelf-reflection");
        }
    }

    impl WidgetImpl for Reflection {
        fn measure(&self, orientation: gtk4::Orientation, _for_size: i32) -> (i32, i32, i32, i32) {
            let (sw, sh) = source_size(self.paintable.borrow().as_ref(), self.icon_size.get());

            // The band keeps the source's cross size and is band_ratio of it along
            // the mirror axis.
            let vertical = is_vertical_band(self.edge.get());
            let cross = if vertical { sw } else { sh };
            let band = (if vertical { sh } else { sw }) * self.band_ratio.get();

            let cross_px = cross.round().max(0.0) as i32;
            let band_px = band.round().max(0.0) as i32;

            let natural = if orientation == gtk4::Orientation::Horizontal {
                cross_px
            } else {
                band_px
            };

            (0, natural, -1, -1)
        }

        fn snapshot(&self, snapshot: &gtk4::Snapshot) {
            let widget = self.obj();

            crate::core::render_stats::lambdock_stats_bump(
                crate::core::render_stats::REFLECTION_PAINTS,
            );
            sync_paintable(self);

            let (w, h) = (f64::from(widget.width()), f64::from(widget.height()));
            if w <= 0.0 || h <= 0.0 {
                return;
            }

            let edge = self.edge.get();
            let vertical = is_vertical_band(edge);

            if self.has_shadow.get() {
                paint_contact_shadow(snapshot, edge, w, h, self.intensity.get());
            }

            let paintable = self.paintable.borrow().clone();
            let (sw, sh) = source_size(self.paintable.borrow().as_ref(), self.icon_size.get());

            if let Some(paintable) = paintable.filter(|_| sw > 0.0 && sh > 0.0) {
                // Mirror the artwork at full scale across the edge that touches the
                // icon and let the glass crop it: a reflection does not shrink, it
                // just disappears into the surface it fades on. Only the part of the
                // icon within the band's depth is ever visible, which is what makes
                // the mirror read as a reflection instead of a squashed copy.
                let mut sx = (w / sw) as f32;
                let mut sy = 1.0f32;
                let mut tx = 0.0f32;
                let mut ty = 0.0f32;

                if vertical {
                    if edge == EDGE_BOTTOM {
                        // Mirror about the icon's bottom edge: the shelf reflects
                        // downwards.
                        sy = -1.0;
                        ty = sh as f32;
                    }
                } else if edge == EDGE_LEFT {
                    // Mirror about the icon's right edge: the shelf reflects
                    // sideways.
                    sx = -sx;
                    tx = sw as f32;
                }

                let blur_unit = if vertical { w } else { h };
                // A blur of its own is a render target per reflection, per
                // repaint: a stylesheet that wants the mirror crisp says so with a
                // radius of zero instead of paying for a blur nobody sees.
                let blur = blur_unit * self.blur_ratio.get();

                let offset = graphene::Point::new(tx, ty);
                let mirror = gsk::Transform::new().translate(&offset).scale(sx, sy);

                // One masked draw, under a blur when the stylesheet asks for one.
                // It replaces one clipped draw per band (fourteen at the depth the
                // Platinum shelf uses) inside the same blur: the blur is what
                // softens the mirror, not the banding, so it stays.
                let blurred = blur > 0.0;
                if blurred {
                    snapshot.push_blur(blur);
                }

                // The fade is the mask: a linear gradient from `intensity` at the
                // icon's edge to transparent at the far edge. A band stack
                // approximated that with hard steps the blur had to smooth; a
                // gradient has no steps, and costs one draw instead of fourteen.
                let axis_len = if vertical { h } else { w };
                let (from, to) = if vertical {
                    (
                        graphene::Point::new(0.0, 0.0),
                        graphene::Point::new(0.0, axis_len as f32),
                    )
                } else {
                    (
                        graphene::Point::new(0.0, 0.0),
                        graphene::Point::new(axis_len as f32, 0.0),
                    )
                };

                let intensity = self.intensity.get();
                let stops: Vec<gsk::ColorStop> = (0..FADE_STOPS)
                    .map(|index| {
                        let t = f64::from(index) / f64::from(FADE_STOPS - 1);
                        let alpha = reflection_fade_alpha(intensity, t) as f32;
                        gsk::ColorStop::new(t as f32, gdk::RGBA::new(1.0, 1.0, 1.0, alpha))
                    })
                    .collect();

                let bounds = graphene::Rect::new(0.0, 0.0, w as f32, h as f32);

                snapshot.push_mask(gsk::MaskMode::Alpha);
                snapshot.append_linear_gradient(&bounds, &from, &to, &stops);
                // Ends the mask; what follows is drawn through it.
                snapshot.pop();

                snapshot.save();
                snapshot.transform(Some(&mirror));
                paintable.snapshot(snapshot, sw, sh);
                snapshot.restore();

                crate::core::render_stats::lambdock_stats_bump(
                    crate::core::render_stats::REFLECTION_DRAWS,
                );

                // Ends the masked group, then the blur when there is one.
                snapshot.pop();
                if blurred {
                    snapshot.pop();
                }
            }

            if self.running.get() {
                paint_running_dot(snapshot, edge, w, h);
            }
        }
    }
}

/// (Re)binds the widget paintable used as the mirror source.
///
/// A widget paintable created before its widget is mapped and sized can latch a zero
/// intrinsic size and then never paint anything, so the binding happens from the first
/// snapshot, when the icon certainly has an allocation.
fn sync_paintable(imp: &reflection_imp::Reflection) {
    if imp.paintable_ready.get() {
        return;
    }

    let Some(source) = imp.source.borrow().as_ref().and_then(|weak| weak.upgrade()) else {
        return;
    };

    if !source.is_mapped() || source.width() <= 0 || source.height() <= 0 {
        return;
    }

    // Second chance for a widget paintable: once the source is mapped and sized, a
    // fresh one does paint.
    *imp.paintable.borrow_mut() = Some(gtk4::WidgetPaintable::new(Some(&source)).upcast());
    imp.paintable_ready.set(true);
    imp.obj().queue_resize();
}

glib::wrapper! {
    /// A mirrored, blurred, fading copy of an icon, meant for the dock's glass.
    pub struct Reflection(ObjectSubclass<reflection_imp::Reflection>)
        @extends gtk4::Widget,
        @implements gtk4::Accessible, gtk4::Buildable, gtk4::ConstraintTarget;
}

/// The reflection behind a widget pointer, or `None` when it is not one.
///
/// # Safety
///
/// `pointer` must be null or a live `GtkWidget`.
unsafe fn as_reflection(
    pointer: *mut ffi::GtkWidget,
) -> Option<glib::translate::Borrowed<Reflection>> {
    if pointer.is_null() {
        return None;
    }

    let widget: glib::translate::Borrowed<gtk4::Widget> = from_glib_borrow(pointer);
    if !widget.is::<Reflection>() {
        return None;
    }

    let widget: gtk4::Widget = widget.into_inner();
    Some(glib::translate::Borrowed::new(widget.unsafe_cast()))
}

/// Creates a reflection of `source`, or an empty slot when it is null.
///
/// # Safety
///
/// `source` must be null or a live widget; the result is `(transfer full)` and the C
/// callers append it, like the `g_object_new` this replaces.
pub unsafe fn shelf_reflection_new(source: *mut ffi::GtkWidget) -> *mut ffi::GtkWidget {
    crate::ui::ensure_gtk_initialized();

    let reflection: Reflection = glib::Object::builder().build();

    if !source.is_null() {
        let source: gtk4::Widget = gtk4::glib::translate::from_glib_none(source);
        *reflection.imp().source.borrow_mut() = Some(source.downgrade());

        // Mirror the artwork itself, not the widget that shows it: see
        // artwork_for().
        match artwork_for(&source) {
            Some(paintable) => {
                *reflection.imp().paintable.borrow_mut() = Some(paintable);
                reflection.imp().paintable_ready.set(true);
            }
            None => {
                *reflection.imp().paintable.borrow_mut() =
                    Some(gtk4::WidgetPaintable::new(Some(&source)).upcast());
                reflection.imp().paintable_ready.set(false);
            }
        }

        // The contact shadow belongs to this artwork: it is the pool the icon casts on
        // the glass. An item with nothing to mirror -- a separator, a gauge, a clock --
        // reserves the band so it lines up with its neighbours, but it must not paint
        // a shadow of an icon it does not have.
        reflection.imp().has_shadow.set(true);
    }

    glib::gobject_ffi::g_object_force_floating(reflection.as_ptr().cast());

    reflection.upcast::<gtk4::Widget>().into_glib_ptr()
}

/// Sets the shelf edge, the icon size and the mirror's three ratios.
///
/// # Safety
///
/// `reflection` must be null or a live reflection.
pub unsafe fn shelf_reflection_configure(
    reflection: *mut ffi::GtkWidget,
    edge: c_int,
    icon_size: c_int,
    band_ratio: c_double,
    blur_ratio: c_double,
    intensity: c_double,
) {
    let Some(reflection) = as_reflection(reflection) else {
        return;
    };

    let imp = reflection.imp();
    let mut changed = imp.edge.get() != edge;

    imp.edge.set(edge);
    if icon_size > 0 && icon_size != imp.icon_size.get() {
        imp.icon_size.set(icon_size);
        changed = true;
    }
    if imp.band_ratio.get() != band_ratio {
        imp.band_ratio.set(band_ratio);
        changed = true;
    }
    imp.blur_ratio.set(blur_ratio);
    imp.intensity.set(intensity);

    if changed {
        reflection.queue_resize();
    }
    reflection.queue_draw();
}

/// Stops mirroring: the widget becomes a plain slot of the band's height.
///
/// # Safety
///
/// `reflection` must be null or a live reflection.
pub unsafe fn shelf_reflection_clear_source(reflection: *mut ffi::GtkWidget) {
    let Some(reflection) = as_reflection(reflection) else {
        return;
    };

    *reflection.imp().source.borrow_mut() = None;
    *reflection.imp().paintable.borrow_mut() = None;

    // Nothing left to bind, and nothing that could ever appear again.
    reflection.imp().paintable_ready.set(true);
    reflection.queue_draw();
}

/// Asks for a band of `band_size` across `icon_size`.
///
/// # Safety
///
/// `reflection` must be null or a live reflection.
pub unsafe fn shelf_reflection_set_extent(
    reflection: *mut ffi::GtkWidget,
    icon_size: c_int,
    band_size: c_int,
) {
    let Some(reflection) = as_reflection(reflection) else {
        return;
    };
    if icon_size <= 0 || band_size <= 0 {
        return;
    }

    if is_vertical_band(reflection.imp().edge.get()) {
        reflection.set_size_request(icon_size, band_size);
    } else {
        reflection.set_size_request(band_size, icon_size);
    }
}

/// Resizes the band for a new icon size, keeping the configured ratio.
///
/// # Safety
///
/// `reflection` must be null or a live reflection.
pub unsafe fn shelf_reflection_set_icon_size(
    reflection: *mut ffi::GtkWidget,
    icon_size: c_int,
) {
    let Some(reflection) = as_reflection(reflection) else {
        return;
    };
    if icon_size <= 0 {
        return;
    }

    let band = band_size(reflection.imp().band_ratio.get(), icon_size);

    if is_vertical_band(reflection.imp().edge.get()) {
        reflection.set_size_request(icon_size, band);
    } else {
        reflection.set_size_request(band, icon_size);
    }
}

/// Toggles the running-app glow dot.
///
/// # Safety
///
/// `reflection` must be null or a live reflection.
pub unsafe fn shelf_reflection_set_running(
    reflection: *mut ffi::GtkWidget,
    running: c_int,
) {
    let Some(reflection) = as_reflection(reflection) else {
        return;
    };

    if reflection.imp().running.get() == (running != 0) {
        return;
    }

    reflection.imp().running.set(running != 0);
    reflection.queue_draw();
}

/// Whether `widget` is a reflection.
///
/// # Safety
///
/// `widget` must be null or a live `GtkWidget`.
pub unsafe fn shelf_reflection_is_reflection(widget: *mut c_void) -> c_int {
    c_int::from(as_reflection(widget.cast()).is_some())
}

/// The size an item's icon is drawn at right now: the magnified size while the
/// fisheye is scaling it, the resting size otherwise.
///
/// # Safety
///
/// `item` must be null or a live dock item.
unsafe fn item_icon_size(item: *const DockItem) -> i32 {
    if item.is_null() || (*item).button.is_null() {
        return 0;
    }

    let button: gtk4::Button =
        match from_glib_none::<_, gtk4::Widget>((*item).button.cast::<ffi::GtkWidget>())
            .downcast::<gtk4::Button>()
        {
            Ok(button) => button,
            Err(_) => return 0,
        };

    let Some(child) = button.child() else {
        return 0;
    };

    // App and dynamic items hold their icon inside a zoom container; stacks, the
    // launchpad and meter items put it straight in the content box.
    let zoomed = |widget: &gtk4::Widget| -> Option<i32> {
        if crate::ui::icon_zoom::lambdock_icon_zoom_is_zoom(widget.as_ptr().cast()) != 0 {
            let size = crate::ui::icon_zoom::lambdock_icon_zoom_target_size(widget.as_ptr().cast());
            return Some(size);
        }

        if let Some(image) = widget.downcast_ref::<gtk4::Image>() {
            if image.pixel_size() > 0 {
                return Some(image.pixel_size());
            }
        }

        None
    };

    if let Ok(dock_box) = child.clone().downcast::<gtk4::Box>() {
        let mut each = dock_box.first_child();
        while let Some(widget) = each {
            if let Some(size) = zoomed(&widget) {
                return size;
            }
            each = widget.next_sibling();
        }

        return 0;
    }

    zoomed(&child).unwrap_or(0)
}

/// Wraps an item's button and its reflection in the shelf's stack.
///
/// The reflection always goes on the outward side of the shelf.
///
/// # Safety
///
/// `button` must be null or a live widget; `reflection` null or a live widget;
/// `state` null or a live dock.
pub unsafe fn shelf_wrap_item(
    state: *const LambdockState,
    button: *mut ffi::GtkWidget,
    reflection: *mut ffi::GtkWidget,
) -> *mut ffi::GtkWidget {
    if button.is_null() {
        return std::ptr::null_mut();
    }

    crate::ui::ensure_gtk_initialized();

    // Stack along the dock's cross axis: a bottom dock stacks icons vertically with
    // the reflection underneath, a side dock stacks them horizontally.
    let vertical_dock =
        !state.is_null() && (*state).orientation == gtk4::Orientation::Vertical.into_glib();
    let stack_orientation = if vertical_dock {
        gtk4::Orientation::Horizontal
    } else {
        gtk4::Orientation::Vertical
    };

    let container = gtk4::Box::new(stack_orientation, 0);
    container.add_css_class("shelf-item");
    container.set_overflow(gtk4::Overflow::Visible);
    container.set_valign(gtk4::Align::Center);
    container.set_halign(gtk4::Align::Center);

    let edge = if state.is_null() {
        EDGE_BOTTOM
    } else {
        (*state).edge
    };
    let reflection_first = edge == EDGE_TOP || edge == EDGE_RIGHT;

    let button: gtk4::Widget = from_glib_none(button);
    let reflection: Option<gtk4::Widget> = if reflection.is_null() {
        None
    } else {
        Some(from_glib_none(reflection))
    };

    if let Some(reflection) = reflection.as_ref().filter(|_| reflection_first) {
        container.append(reflection);
    }
    container.append(&button);
    if let Some(reflection) = reflection.as_ref().filter(|_| !reflection_first) {
        container.append(reflection);
    }

    glib::gobject_ffi::g_object_force_floating(container.as_ptr().cast());

    container.upcast::<gtk4::Widget>().into_glib_ptr()
}

/// Lays the reflections of every item out for the dock's current shelf.
///
/// The band follows the current icon size so fisheye magnification is mirrored
/// too, but its thickness is tied to the *base* icon size: growing the band would
/// push the whole row of icons up while hovering.
///
/// # Safety
///
/// `state` must be a live dock.
pub unsafe fn apply_shelf_layout(state: &mut LambdockState) {
    if state.dock_items.is_empty() {
        return;
    }

    let params = lambdock_shelf_params_effective(&*state);
    let default_icon = if state.icon_size > 0 {
        state.icon_size
    } else {
        DEFAULT_ICON_SIZE
    };
    let geometry = lambdock_shelf_geometry_for(&*state, default_icon);

    for item in state.dock_items.iter() {
        let item = *item;

        if !item.is_null() && !(*item).reflection.is_null() {
            let reflection = (*item).reflection.cast::<ffi::GtkWidget>();

            if geometry.band_px <= 0 {
                let widget: gtk4::Widget = from_glib_none(reflection);
                widget.set_visible(false);
            } else {
                let mut icon_px = item_icon_size(item);
                if icon_px <= 0 {
                    icon_px = default_icon;
                }

                shelf_reflection_configure(
                    reflection,
                    state.edge,
                    icon_px,
                    params.reflection_ratio,
                    params.reflection_blur_ratio,
                    params.reflection_intensity,
                );
                shelf_reflection_set_extent(reflection, icon_px, geometry.band_px);
                shelf_reflection_set_running(reflection, c_int::from(!(*item).windows.is_null()));

                let widget: gtk4::Widget = from_glib_none(reflection);
                widget.set_visible(true);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The software renderer has no projective transform, and asking it for one
    /// does not draw a slanted slab: it fills the whole surface with its
    /// "unsupported node" colour, which is a flat hot-pink rectangle. So a frame
    /// on cairo has to stay a rectangle.
    #[test]
    fn only_a_renderer_with_a_projective_transform_may_project() {
        assert!(!renderer_can_project("GskCairoRenderer"));
        assert!(renderer_can_project("GskGLRenderer"));
        assert!(renderer_can_project("GskVulkanRenderer"));
        // Anything this does not know is given the benefit of the doubt: a
        // newer renderer is more likely to have perspective than not to.
        assert!(renderer_can_project("GskSomeFutureRenderer"));
    }

    /// The capsule is the icon plus the margin plus whatever the glass takes on
    /// the outer side, and a reflection deeper than the front is what decides
    /// `outer`.
    #[test]
    fn the_geometry_is_the_capsule_and_its_band() {
        let params = ShelfParams {
            margin_px: 12,
            front_px: 8,
            reflection_ratio: 0.25,
            ..lambdock_shelf_params_default()
        };

        let geometry = crate::ui::shelf::geometry(&params, 48);

        // 48 * 0.25 = 12 of band, which is deeper than the 8px front.
        assert_eq!(geometry.band_px, 12);
        assert_eq!(geometry.outer_px, 12);
        assert_eq!(geometry.capsule_px, 48 + 12 + 12);
        assert_eq!(
            geometry.overhang_px, 0,
            "a positive margin does not overhang"
        );
    }

    /// A negative margin puts the icons past the capsule's inner edge.
    #[test]
    fn a_negative_margin_is_an_overhang() {
        let params = ShelfParams {
            margin_px: -18,
            front_px: 14,
            reflection_ratio: 0.0,
            ..lambdock_shelf_params_default()
        };

        let geometry = geometry(&params, 48);

        assert_eq!(geometry.band_px, 0);
        assert_eq!(geometry.overhang_px, 18);
        assert_eq!(geometry.capsule_px, 48 - 18 + 14);
    }

    /// A nonsense icon size falls back to the dock's default, and nothing comes
    /// out negative.
    #[test]
    fn a_missing_icon_size_uses_the_default() {
        let params = ShelfParams {
            margin_px: 0,
            front_px: 0,
            reflection_ratio: 0.0,
            ..lambdock_shelf_params_default()
        };

        assert_eq!(geometry(&params, 0).capsule_px, 48);
        assert_eq!(geometry(&params, -5).capsule_px, 48);
    }

    /// The reflection fades from its intensity at the icon's edge to nothing at
    /// the far edge, on a curve that stays above the straight line.
    #[test]
    fn the_reflection_fades_from_its_intensity_to_nothing() {
        assert!((reflection_fade_alpha(0.6, 0.0) - 0.6).abs() < 1e-9);
        assert_eq!(reflection_fade_alpha(0.6, 1.0), 0.0);

        // Monotonic towards the far edge, and below the straight ramp it bends
        // away from: the exponent keeps the fade close to the icon.
        let quarter = reflection_fade_alpha(0.6, 0.25);
        let half = reflection_fade_alpha(0.6, 0.5);
        assert!(quarter > half && half > 0.0 && quarter < 0.6);
        assert!(half < 0.6 * 0.5);

        // Out-of-range fractions are the ends, not a panic or a negative alpha.
        assert!((reflection_fade_alpha(0.6, -3.0) - 0.6).abs() < 1e-9);
        assert_eq!(reflection_fade_alpha(0.6, 9.0), 0.0);

        // An intensity nobody meant paints nothing, and one past 1 is clamped.
        assert_eq!(reflection_fade_alpha(f64::NAN, 0.5), 0.0);
        assert_eq!(reflection_fade_alpha(-1.0, 0.0), 0.0);
        assert_eq!(reflection_fade_alpha(0.0, 0.0), 0.0);
        assert!((reflection_fade_alpha(4.0, 0.0) - 1.0).abs() < 1e-9);
    }

    /// The geometry the C asks for is the same one these tests measure, from a
    /// state whose shelf is all zeroes and from one that configures it.
    #[test]
    fn a_zeroed_state_measures_the_plain_dock() {
        let mut state = LambdockState::default();

        assert_eq!(
            lambdock_shelf_active(&state),
            0,
            "all zeroes is not a shelf"
        );

        // A zeroed shelf is treated as the defaults, not as a zero-margin
        // capsule that would collapse the layout.
        let geometry = lambdock_shelf_geometry_for(&state, 48);
        assert_eq!(
            geometry.capsule_px,
            48 + lambdock_shelf_params_default().margin_px
        );

        // A reflection and a tilt turn the shelf look on.
        state.shelf.reflection_ratio = 0.3;
        assert_ne!(lambdock_shelf_active(&state), 0);
        assert!(lambdock_shelf_geometry_for(&state, 48).band_px > 0);

        state.shelf.reflection_ratio = 0.0;
        state.shelf.tilt_deg = 8.0;
        assert_ne!(lambdock_shelf_active(&state), 0);
    }

    /// The keystone leaves the far edge alone and magnifies the edge on the
    /// screen, symmetrically around the middle.
    #[test]
    fn the_keystone_magnifies_the_pinned_edge() {
        // A bottom dock: 100 wide, 40 deep, 10px of flare per side.
        let matrix = keystone(100, 40, true, 10, true).expect("a flared shelf");

        // Mapped through the matrix, as graphene does: row vectors, divided by w'.
        let project = |x: f32, y: f32| {
            let x_out = x * matrix[0] + y * matrix[4] + matrix[12];
            let y_out = x * matrix[1] + y * matrix[5] + matrix[13];
            let w_out = y * matrix[7] + matrix[15];
            (x_out / w_out, y_out / w_out)
        };

        // The far edge (y = 0) is untouched.
        assert!((project(0.0, 0.0).0 - 0.0).abs() < 1e-4);
        assert!((project(100.0, 0.0).0 - 100.0).abs() < 1e-4);

        // The pinned edge is magnified around the centre line: 100px of dock
        // becoming 100 / 0.75 = 133.33px, so 16.67px past each side.
        let (left, _) = project(0.0, 40.0);
        let (right, _) = project(100.0, 40.0);
        assert!((left - -16.667).abs() < 0.01, "{left}");
        assert!((right - 116.667).abs() < 0.01, "{right}");

        // The centre line does not move: the widening is symmetric.
        assert!((project(50.0, 40.0).0 - 50.0).abs() < 1e-4);
    }

    /// A top or left dock pins the other edge, so the projection runs the other
    /// way.
    #[test]
    fn the_keystone_pins_whichever_edge_touches_the_screen() {
        let matrix = keystone(100, 40, true, 10, false).expect("a flared shelf");

        let project = |x: f32, y: f32| {
            let x_out = x * matrix[0] + y * matrix[4] + matrix[12];
            let w_out = y * matrix[7] + matrix[15];
            x_out / w_out
        };

        // Now the inner edge (y = 0) is the widened one and the far edge is not.
        let inner = project(0.0, 0.0);
        assert!((inner - -16.667).abs() < 0.01, "{inner}");
        assert!((project(0.0, 40.0) - 0.0).abs() < 1e-4);
    }

    /// A side dock projects along the other axis.
    #[test]
    fn a_side_dock_projects_on_the_other_axis() {
        let matrix = keystone(40, 100, false, 10, true).expect("a flared shelf");

        // y picks up x, and w picks up x.
        assert_ne!(matrix[1], 0.0);
        assert_ne!(matrix[3], 0.0);
        assert_eq!(matrix[4], 0.0);
        assert_eq!(matrix[7], 0.0);
    }

    /// Nothing to project, and a slab that the flare would eat entirely, are not
    /// projections at all.
    #[test]
    fn a_shelf_without_a_flare_is_not_projected() {
        assert_eq!(keystone(100, 40, true, 0, true), None);
        assert_eq!(keystone(100, 40, true, -4, true), None);
        assert_eq!(keystone(100, 0, true, 10, true), None);
        // 20px of flare on a 20px slab leaves nothing to widen.
        assert_eq!(keystone(20, 40, true, 10, true), None);
    }

    /// The parameters in force are the dock's, unless it configured none.
    #[test]
    fn an_unconfigured_dock_gets_the_defaults() {
        let defaults = lambdock_shelf_params_default();
        let mut state = LambdockState::default();

        let effective = lambdock_shelf_params_effective(&state);
        assert_eq!(effective.margin_px, defaults.margin_px);
        assert_eq!(effective.front_px, defaults.front_px);

        state.shelf.front_px = 9;
        assert_eq!(lambdock_shelf_params_effective(&state).front_px, 9);
    }
    /// A frame with no tilt and no flare hands the widget straight through: the
    /// flat dock must stay pixel-identical to the unwrapped capsule.
    #[test]
    fn a_flat_shelf_has_no_transform() {
        assert!(frame_transform(true, EDGE_BOTTOM, 0.0, 400.0, 0, 100, 40).is_none());
        // Nothing allocated yet, nothing to project.
        assert!(frame_transform(true, EDGE_BOTTOM, 8.0, 400.0, 10, 0, 0).is_none());
    }

    /// Either projection on its own is enough to need a transform -- on a
    /// renderer that has one. Without, the same settings have to stay flat, or
    /// the surface comes out as the renderer's unsupported-node colour.
    #[test]
    fn a_tilt_or_a_flare_is_a_transform() {
        assert!(frame_transform(true, EDGE_BOTTOM, 8.0, 400.0, 0, 100, 40).is_some());
        assert!(frame_transform(true, EDGE_BOTTOM, 0.0, 400.0, 10, 100, 40).is_some());
        assert!(frame_transform(true, EDGE_BOTTOM, 8.0, 400.0, 10, 100, 40).is_some());

        assert!(frame_transform(false, EDGE_BOTTOM, 8.0, 400.0, 10, 100, 40).is_none());
    }

    /// The dock's edges decide both axes: which way the depth runs and which edge
    /// the slab stays pinned to.
    #[test]
    fn the_edges_decide_the_axes() {
        assert!(is_horizontal(EDGE_TOP));
        assert!(is_horizontal(EDGE_BOTTOM));
        assert!(!is_horizontal(EDGE_LEFT));
        assert!(!is_horizontal(EDGE_RIGHT));

        assert!(is_pinned_at_end(EDGE_BOTTOM));
        assert!(is_pinned_at_end(EDGE_RIGHT));
        assert!(!is_pinned_at_end(EDGE_TOP));
        assert!(!is_pinned_at_end(EDGE_LEFT));
    }
}
