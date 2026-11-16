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
    SHELF_DEFAULT_PERSPECTIVE,
};
use crate::core::dock_dims::ShelfGeometry;
use crate::state::LambdockState;
use gtk4::glib::translate::{from_glib_borrow, IntoGlib, IntoGlibPtr};
use gtk4::prelude::*;
use gtk4::subclass::prelude::*;
use gtk4::{ffi, glib, graphene, gsk};
use std::cell::Cell;
use std::os::raw::{c_double, c_int, c_void};

/// `DEFAULT_ICON_SIZE` in `lambdock.h`.
const DEFAULT_ICON_SIZE: c_int = 48;

/// `SHELF_REFLECTION_BANDS` in `lambdock.h`: the most bands one reflection
/// repaint may cost.
const REFLECTION_BANDS: c_int = 16;

/// The fewest bands a reflection is drawn with, however thin the band is.
const MIN_BANDS: c_int = 6;

/// The shelf parameters that should be used for `state`.
///
/// A zero-initialised state (a partially built dock, a unit test) carries no shelf
/// configuration at all; it is treated as the plain dock rather than as a capsule
/// with zero margin, which would collapse the layout.
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn lambdock_shelf_params_effective(
    state: *const LambdockState,
) -> ShelfParams {
    if state.is_null() {
        return lambdock_shelf_params_default();
    }

    let params = (*state).shelf;

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
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn lambdock_shelf_active(state: *const LambdockState) -> c_int {
    if state.is_null() {
        return 0;
    }

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
///
/// # Safety
///
/// `state` must be null or a live dock.
#[no_mangle]
pub unsafe extern "C" fn lambdock_shelf_geometry_for(
    state: *const LambdockState,
    icon_size: c_int,
) -> ShelfGeometry {
    geometry(&lambdock_shelf_params_effective(state), icon_size)
}

/// How many clipped draws one reflection repaint costs.
///
/// The fade is painted as a stack of bands, so this is the per-repaint budget of
/// the whole widget: a pure function, and tested, because "a few bands" is not a
/// budget.
#[no_mangle]
pub extern "C" fn lambdock_reflection_band_count(axis_len: f64) -> c_int {
    if !(axis_len > 0.0) {
        return REFLECTION_BANDS;
    }

    (axis_len.round() as c_int).clamp(MIN_BANDS, REFLECTION_BANDS)
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
#[no_mangle]
pub unsafe extern "C" fn lambdock_shelf_keystone_matrix(
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

/// The projection the frame applies to the slab: the keystone (which gives the
/// slab its trapezoid) followed by the tilt (which tips it away from the viewer).
///
/// `None` when the shelf is a plain rectangle that needs no projection.
pub fn frame_transform(
    edge: c_int,
    tilt: f64,
    perspective: f64,
    flare: c_int,
    width: i32,
    height: i32,
) -> Option<gsk::Transform> {
    if tilt == 0.0 && flare == 0 {
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

            let transform = frame_transform(
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
#[no_mangle]
pub unsafe extern "C" fn shelf_frame_new(child: *mut ffi::GtkWidget) -> *mut ffi::GtkWidget {
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
#[no_mangle]
pub unsafe extern "C" fn shelf_frame_set_tilt(
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
#[no_mangle]
pub unsafe extern "C" fn shelf_frame_set_shape(
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
#[no_mangle]
pub unsafe extern "C" fn shelf_frame_is_frame(widget: *mut c_void) -> c_int {
    c_int::from(as_frame(widget.cast()).is_some())
}

/// Whether the slab is drawn as a shape rather than a rectangle.
///
/// # Safety
///
/// `frame` must be null or a live `GtkWidget`.
#[no_mangle]
pub unsafe extern "C" fn shelf_frame_is_projected(frame: *mut ffi::GtkWidget) -> c_int {
    match as_frame(frame) {
        Some(frame) => c_int::from(frame.imp().tilt.get() != 0.0 || frame.imp().flare.get() != 0),
        None => 0,
    }
}

/// The axis across the dock: vertical for a dock on the top or bottom edge,
/// horizontal for one on the left or right.
///
/// # Safety
///
/// `frame` must be null or a live `GtkWidget`.
#[no_mangle]
pub unsafe extern "C" fn shelf_frame_cross_orientation(frame: *mut ffi::GtkWidget) -> c_int {
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
#[no_mangle]
pub unsafe extern "C" fn shelf_frame_project_point(
    frame: *mut ffi::GtkWidget,
    point: *mut graphene::ffi::graphene_point_t,
) -> c_int {
    let Some(frame) = as_frame(frame) else {
        return 0;
    };
    if point.is_null() {
        return 0;
    }

    let Some(transform) = frame_transform(
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

#[cfg(test)]
mod tests {
    use super::*;

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

    /// The band has a floor and a ceiling: a hairline band is still drawn in
    /// enough strips to fade, and a deep one does not cost an unbounded number of
    /// draws.
    #[test]
    fn the_band_count_is_a_budget() {
        assert_eq!(lambdock_reflection_band_count(0.0), REFLECTION_BANDS);
        assert_eq!(lambdock_reflection_band_count(-4.0), REFLECTION_BANDS);
        assert_eq!(lambdock_reflection_band_count(f64::NAN), REFLECTION_BANDS);

        assert_eq!(lambdock_reflection_band_count(1.0), MIN_BANDS);
        assert_eq!(lambdock_reflection_band_count(10.4), 10);
        assert_eq!(lambdock_reflection_band_count(99.0), REFLECTION_BANDS);
    }

    /// The geometry the C asks for is the same one these tests measure, from a
    /// state whose shelf is all zeroes and from one that configures it.
    #[test]
    fn a_zeroed_state_measures_the_plain_dock() {
        let mut state: LambdockState = unsafe { std::mem::zeroed() };

        unsafe {
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
        unsafe {
            let defaults = lambdock_shelf_params_default();
            let mut state: LambdockState = std::mem::zeroed();

            let effective = lambdock_shelf_params_effective(&state);
            assert_eq!(effective.margin_px, defaults.margin_px);
            assert_eq!(effective.front_px, defaults.front_px);

            state.shelf.front_px = 9;
            assert_eq!(lambdock_shelf_params_effective(&state).front_px, 9);

            // A null state is the defaults too.
            assert_eq!(
                lambdock_shelf_params_effective(std::ptr::null()).front_px,
                defaults.front_px
            );
        }
    }
    /// A frame with no tilt and no flare hands the widget straight through: the
    /// flat dock must stay pixel-identical to the unwrapped capsule.
    #[test]
    fn a_flat_shelf_has_no_transform() {
        assert!(frame_transform(EDGE_BOTTOM, 0.0, 400.0, 0, 100, 40).is_none());
        // Nothing allocated yet, nothing to project.
        assert!(frame_transform(EDGE_BOTTOM, 8.0, 400.0, 10, 0, 0).is_none());
    }

    /// Either projection on its own is enough to need a transform.
    #[test]
    fn a_tilt_or_a_flare_is_a_transform() {
        assert!(frame_transform(EDGE_BOTTOM, 8.0, 400.0, 0, 100, 40).is_some());
        assert!(frame_transform(EDGE_BOTTOM, 0.0, 400.0, 10, 100, 40).is_some());
        assert!(frame_transform(EDGE_BOTTOM, 8.0, 400.0, 10, 100, 40).is_some());
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
