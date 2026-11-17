// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! Separators that stand on the glass: `src/ui/shelf_follow.c` in Rust.
//!
//! A separator is drawn flat, but the shelf it sits on is projected: a vertical
//! line on the slab leans further the further it is from the middle of the dock,
//! and stretches as it comes forward. None of that is affine, but a widget is only
//! a few dozen pixels tall, so the Jacobian of the projection at its centre
//! describes it to within a fraction of a pixel -- and unlike the full projection
//! it can be applied as a transform.
//!
//! The widget also hands its child the slab's depth to cover, so a separator runs
//! from the line the icons stand on to the front edge of the glass instead of
//! stopping at the height the dock laid out for it.

use gtk4::ffi;
use gtk4::glib;
use gtk4::glib::translate::{from_glib_borrow, from_glib_none, FromGlib, IntoGlib, IntoGlibPtr};
use gtk4::graphene;
use gtk4::gsk;
use gtk4::prelude::*;
use gtk4::subclass::prelude::*;
use std::cell::RefCell;
use std::os::raw::{c_int, c_void};
use crate::ui::shelf::{shelf_frame_cross_orientation, shelf_frame_is_frame, shelf_frame_is_projected, shelf_frame_project_point};

/// The separator's lean is that of the slab under it, exaggerated. The shelf's
/// keystone is gentle; doubling the shear gives a separator that reads as standing
/// on a tilted surface at a glance instead of one that merely looks a little off
/// vertical.
const LEAN_EMPHASIS: f32 = 2.0;

/// A couple of pixels short of either edge: the separator fades out before it
/// reaches them, and a line drawn right up to the lip reads as a scratch.
const SLAB_INSET: f64 = 2.0;

/// A point as the shelf frame's projector reads it, so the call does not depend on
/// which crate re-exports graphene's C type.
#[repr(C)]
struct ShelfPoint {
    x: f32,
    y: f32,
}

// Where this module reaches back into the C shelf.

/// The transform that leans the child with the slab under it, from the projection's
/// Jacobian at the child's centre.
///
/// `j00`/`j01` are where one pixel right and one pixel down end up, in the frame's
/// own coordinates. `None` when the projection does nothing -- there is no reason
/// to pay for a render node then -- or when it produced something that is not a
/// number.
pub fn follow_matrix(
    j00: f32,
    j10: f32,
    j01: f32,
    j11: f32,
    width: f64,
    height: f64,
) -> Option<[f32; 16]> {
    // The shear is the lean; the diagonal is the slab's own stretch, which stays as
    // it is.
    let j10 = j10 * LEAN_EMPHASIS;
    let j01 = j01 * LEAN_EMPHASIS;

    if !j00.is_finite() || !j10.is_finite() || !j01.is_finite() || !j11.is_finite() {
        return None;
    }

    // A projection that does nothing is not worth a render node.
    if (j00 - 1.0).abs() < 0.0005
        && (j11 - 1.0).abs() < 0.0005
        && j01.abs() < 0.0005
        && j10.abs() < 0.0005
    {
        return None;
    }

    // Applied to row vectors, and pivoting on the child's own centre so it stays
    // where the layout put it: p' = J (p - c) + c.
    let cx = (width / 2.0) as f32;
    let cy = (height / 2.0) as f32;

    Some([
        j00,
        j10,
        0.0,
        0.0,
        j01,
        j11,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        cx - (j00 * cx + j01 * cy),
        cy - (j10 * cx + j11 * cy),
        0.0,
        1.0,
    ])
}

/// Where the glass runs, relative to the widget: from its inner edge (the line the
/// icons stand on) to its front edge, as offsets from the widget's own top edge.
///
/// `None` when there is nothing to cover -- a slab or a widget with no height, or a
/// span too thin to be worth drawing.
pub fn slab_span(slab: graphene::Rect, mine: graphene::Rect) -> Option<(f64, f64)> {
    if slab.height() <= 0.0 || mine.height() <= 0.0 {
        return None;
    }

    let top = f64::from(slab.y() - mine.y()) + SLAB_INSET;
    let bottom = f64::from(slab.y() + slab.height() - mine.y()) - SLAB_INSET;

    if bottom > top + 1.0 {
        Some((top, bottom))
    } else {
        None
    }
}

/// Where the child goes once it has to cover the slab.
///
/// A dock on the top or bottom edge stacks its items vertically, so the slab's
/// depth is the vertical extent; a side dock's is the horizontal one. Across the
/// dock the child keeps its own size, centred where the layout put it.
pub fn child_allocation(
    vertical: bool,
    width: i32,
    height: i32,
    top: f64,
    bottom: f64,
    min_span: i32,
    natural_along: i32,
) -> (i32, i32, i32, i32) {
    let mut span = bottom - top;
    if span < f64::from(min_span) {
        span = f64::from(min_span);
    }

    let across = span.round() as i32;
    let along_start = top.round() as i32;

    if vertical {
        (
            (width - natural_along) / 2,
            along_start,
            natural_along,
            across,
        )
    } else {
        (
            along_start,
            (height - natural_along) / 2,
            across,
            natural_along,
        )
    }
}

/// The frame's projection at `widget`'s place, as a matrix.
///
/// # Safety
///
/// `frame` must be null or a live shelf frame; `widget` a live widget.
unsafe fn local_matrix(
    frame: *mut ffi::GtkWidget,
    widget: &gtk4::Widget,
) -> Option<graphene::Matrix> {
    if frame.is_null() || shelf_frame_is_projected(frame) == 0 {
        return None;
    }

    let (width, height) = (widget.width(), widget.height());
    if width <= 0 || height <= 0 {
        return None;
    }

    let frame_widget: gtk4::Widget = from_glib_none(frame);
    let bounds = widget.compute_bounds(&frame_widget)?;

    let centre = (
        bounds.x() + bounds.width() / 2.0,
        bounds.y() + bounds.height() / 2.0,
    );
    let mut points = [
        ShelfPoint {
            x: centre.0,
            y: centre.1,
        },
        ShelfPoint {
            x: centre.0 + 1.0,
            y: centre.1,
        },
        ShelfPoint {
            x: centre.0,
            y: centre.1 + 1.0,
        },
    ];

    for point in &mut points {
        if shelf_frame_project_point(frame, point as *mut ShelfPoint as *mut graphene::ffi::graphene_point_t) == 0 {
            return None;
        }
    }

    let matrix = follow_matrix(
        points[1].x - points[0].x,
        points[1].y - points[0].y,
        points[2].x - points[0].x,
        points[2].y - points[0].y,
        f64::from(width),
        f64::from(height),
    )?;

    Some(graphene::Matrix::from_float(matrix))
}

mod imp {
    use super::*;

    #[derive(Default)]
    pub struct ShelfFollow {
        pub child: RefCell<Option<gtk4::Widget>>,
        /// Weak: the shelf frame whose surface to follow.
        pub frame: RefCell<Option<glib::WeakRef<gtk4::Widget>>>,
        /// Weak: the capsule, i.e. how deep the glass is.
        pub slab: RefCell<Option<glib::WeakRef<gtk4::Widget>>>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for ShelfFollow {
        const NAME: &'static str = "LambdockShelfFollow";
        type Type = super::ShelfFollow;
        type ParentType = gtk4::Widget;
    }

    impl ObjectImpl for ShelfFollow {
        fn constructed(&self) {
            self.parent_constructed();

            // The projected child leans past the box the layout gave it.
            self.obj().set_overflow(gtk4::Overflow::Visible);
        }

        fn dispose(&self) {
            if let Some(child) = self.child.replace(None) {
                child.unparent();
            }
        }
    }

    impl WidgetImpl for ShelfFollow {
        fn measure(&self, orientation: gtk4::Orientation, for_size: i32) -> (i32, i32, i32, i32) {
            if let Some(child) = self
                .child
                .borrow()
                .as_ref()
                .filter(|child| child.should_layout())
            {
                return child.measure(orientation, for_size);
            }

            (0, 0, -1, -1)
        }

        fn size_allocate(&self, width: i32, height: i32, baseline: i32) {
            let widget = self.obj();
            let Some(child) = self.child.borrow().clone() else {
                return;
            };

            if !child.should_layout() {
                return;
            }

            let frame = upgrade(&self.frame);

            // Stylesheets get to know that this widget sits on a projected slab:
            // the tilted separators have their own look, and the flat dock keeps
            // the theme's.
            let projected = is_projected(frame.as_ref());

            crate::ui::widget_utils::set_css_class(
                widget.upcast_ref::<gtk4::Widget>(),
                "shelf-projected",
                projected,
            );

            let mut allocation = (0, 0, width, height);

            if projected {
                let slab = upgrade(&self.slab);
                if let (Some(slab), Some(frame)) = (slab, frame.clone()) {
                    let bounds = widget.compute_bounds(&frame);
                    let slab_bounds = slab.compute_bounds(&frame);

                    if let (Some(mine), Some(slab_rect)) = (bounds, slab_bounds) {
                        if let Some((top, bottom)) = slab_span(slab_rect, mine) {
                            let cross =
                                unsafe { shelf_frame_cross_orientation(frame.as_ptr().cast()) };
                            let vertical = cross == gtk4::Orientation::Vertical.into_glib();
                            let against = if vertical {
                                gtk4::Orientation::Horizontal
                            } else {
                                gtk4::Orientation::Vertical
                            };

                            let (min_span, _, _, _) =
                                child.measure(unsafe { gtk4::Orientation::from_glib(cross) }, -1);
                            let (_, natural_along, _, _) = child.measure(against, -1);

                            allocation = child_allocation(
                                vertical,
                                width,
                                height,
                                top,
                                bottom,
                                min_span,
                                natural_along,
                            );
                        }
                    }
                }
            }

            child.allocate(allocation.2, allocation.3, baseline, {
                let offset = graphene::Point::new(allocation.0 as f32, allocation.1 as f32);
                Some(gsk::Transform::new().translate(&offset))
            });
        }

        fn snapshot(&self, snapshot: &gtk4::Snapshot) {
            let widget = self.obj();
            let Some(child) = self.child.borrow().clone() else {
                return;
            };

            let frame = upgrade(&self.frame);
            let Some(frame) = frame else {
                widget.snapshot_child(&child, snapshot);
                return;
            };

            let matrix = unsafe { local_matrix(frame.as_ptr().cast(), widget.upcast_ref()) };

            let Some(matrix) = matrix else {
                widget.snapshot_child(&child, snapshot);
                return;
            };

            snapshot.save();
            snapshot.transform(Some(&gsk::Transform::new().matrix(&matrix)));
            widget.snapshot_child(&child, snapshot);
            snapshot.restore();
        }
    }
}

/// The widget a weak reference still points at, if any.
fn upgrade(reference: &RefCell<Option<glib::WeakRef<gtk4::Widget>>>) -> Option<gtk4::Widget> {
    reference.borrow().as_ref().and_then(|weak| weak.upgrade())
}

/// Whether the frame is projected, which is what decides if anything leans.
fn is_projected(frame: Option<&gtk4::Widget>) -> bool {
    match frame {
        Some(frame) => unsafe { shelf_frame_is_projected(frame.as_ptr().cast()) != 0 },
        None => false,
    }
}

glib::wrapper! {
    /// A child that leans with the shelf's projection and covers the glass.
    pub struct ShelfFollow(ObjectSubclass<imp::ShelfFollow>)
        @extends gtk4::Widget,
        @implements gtk4::Accessible, gtk4::Buildable, gtk4::ConstraintTarget;
}

/// The follow behind a widget pointer, or `None` when it is not one.
///
/// # Safety
///
/// `pointer` must be null or a live `GtkWidget`.
unsafe fn as_follow(
    pointer: *mut ffi::GtkWidget,
) -> Option<gtk4::glib::translate::Borrowed<ShelfFollow>> {
    if pointer.is_null() {
        return None;
    }

    let widget: gtk4::glib::translate::Borrowed<gtk4::Widget> = from_glib_borrow(pointer);
    if !widget.is::<ShelfFollow>() {
        return None;
    }

    let widget: gtk4::Widget = widget.into_inner();
    Some(gtk4::glib::translate::Borrowed::new(widget.unsafe_cast()))
}

/// Wraps `child` so it leans with `frame`'s projection and covers `slab`'s depth.
///
/// # Safety
///
/// `child`, `frame` and `slab` must be null or live widgets; the result is
/// `(transfer full)` and the callers append it (which sinks it), like the
/// `g_object_new` this replaces.
pub unsafe fn shelf_follow_new(
    child: *mut ffi::GtkWidget,
    frame: *mut ffi::GtkWidget,
    slab: *mut ffi::GtkWidget,
) -> *mut ffi::GtkWidget {
    crate::ui::ensure_gtk_initialized();

    let follow: ShelfFollow = glib::Object::builder().build();

    if !frame.is_null() && shelf_frame_is_frame(frame.cast()) != 0 {
        let frame: gtk4::Widget = from_glib_none(frame);
        *follow.imp().frame.borrow_mut() = Some(frame.downgrade());
    }

    if !slab.is_null() {
        let slab: gtk4::Widget = from_glib_none(slab);
        *follow.imp().slab.borrow_mut() = Some(slab.downgrade());
    }

    if !child.is_null() {
        let child: gtk4::Widget = from_glib_none(child);
        child.set_parent(&follow);
        *follow.imp().child.borrow_mut() = Some(child);
    }

    // The C contract is a floating reference that the caller's container sinks.
    glib::gobject_ffi::g_object_force_floating(follow.as_ptr().cast());

    follow.upcast::<gtk4::Widget>().into_glib_ptr()
}

/// Whether `widget` is a follow.
///
/// # Safety
///
/// `widget` must be null or a live `GtkWidget`.
pub unsafe fn shelf_follow_is_follow(widget: *mut c_void) -> c_int {
    c_int::from(as_follow(widget.cast()).is_some())
}

/// Whether the follow's slab is projected, i.e. whether its child leans.
///
/// # Safety
///
/// `follow` must be null or a live `GtkWidget`.
pub unsafe fn shelf_follow_is_projected(follow: *mut ffi::GtkWidget) -> c_int {
    let Some(follow) = as_follow(follow) else {
        return 0;
    };

    c_int::from(is_projected(upgrade(&follow.imp().frame).as_ref()))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A projection that does nothing is not worth a render node.
    #[test]
    fn a_flat_projection_has_no_matrix() {
        assert_eq!(follow_matrix(1.0, 0.0, 0.0, 1.0, 40.0, 20.0), None);
        // A hair of noise is still nothing.
        assert_eq!(follow_matrix(1.0001, 0.0, 0.0, 0.9999, 40.0, 20.0), None);
        // A projection the compositor could not compute is not applied.
        assert_eq!(follow_matrix(f32::NAN, 0.0, 0.0, 1.0, 40.0, 20.0), None);
        assert_eq!(
            follow_matrix(1.0, f32::INFINITY, 0.0, 1.0, 40.0, 20.0),
            None
        );
    }

    /// The shear is the lean, doubled; the diagonal is the slab's stretch; and the
    /// pivot keeps the child where the layout put it.
    #[test]
    fn the_matrix_leans_and_pivots_on_the_centre() {
        // One pixel right lands at (1, 0.1); one pixel down lands at (0.05, 1).
        let matrix = follow_matrix(1.0, 0.1, 0.05, 1.0, 40.0, 20.0).expect("a projected slab");

        assert_eq!(matrix[0], 1.0);
        assert_eq!(matrix[1], 0.2); // j10, doubled
        assert_eq!(matrix[4], 0.1); // j01, doubled
        assert_eq!(matrix[5], 1.0);

        // p' = J (p - c) + c, with c = (20, 10).
        assert_eq!(matrix[12], 20.0 - (1.0 * 20.0 + 0.1 * 10.0));
        assert_eq!(matrix[13], 10.0 - (0.2 * 20.0 + 1.0 * 10.0));
        assert_eq!(
            [matrix[3], matrix[7], matrix[11], matrix[15]],
            [0.0, 0.0, 0.0, 1.0]
        );
    }

    /// A slab stretched without a lean still needs the transform: its child has to
    /// grow with it.
    #[test]
    fn a_stretch_is_still_a_projection() {
        let matrix = follow_matrix(2.0, 0.0, 0.0, 2.0, 40.0, 20.0).expect("a stretched slab");

        assert_eq!(matrix[0], 2.0);
        assert_eq!(matrix[5], 2.0);
        assert_eq!(matrix[12], -20.0);
        assert_eq!(matrix[13], -10.0);
    }

    /// The separator covers the glass, two pixels short of either lip, or it does
    /// not move at all.
    #[test]
    fn the_child_covers_the_slab_span() {
        let slab = graphene::Rect::new(0.0, 100.0, 40.0, 30.0);
        let mine = graphene::Rect::new(0.0, 120.0, 10.0, 48.0);

        assert_eq!(slab_span(slab, mine), Some((-18.0, 8.0)));

        // A slab thinner than the two insets, and one with no height: nothing to
        // cover.
        assert_eq!(
            slab_span(
                graphene::Rect::new(0.0, 0.0, 40.0, 3.0),
                graphene::Rect::new(0.0, 0.0, 10.0, 48.0)
            ),
            None
        );
        assert_eq!(
            slab_span(
                graphene::Rect::new(0.0, 0.0, 40.0, 0.0),
                graphene::Rect::new(0.0, 0.0, 10.0, 48.0)
            ),
            None
        );
    }

    /// Across the dock the child keeps its size; along it, the span wins unless the
    /// child needs more.
    #[test]
    fn the_allocation_follows_the_dock_edge() {
        // A top/bottom dock: the span is the vertical extent, and the child is
        // centred across.
        assert_eq!(
            child_allocation(true, 100, 48, 10.0, 30.0, 8, 12),
            (44, 10, 12, 20)
        );

        // A side dock: the span is the horizontal extent.
        assert_eq!(
            child_allocation(false, 48, 100, 10.0, 30.0, 8, 12),
            (10, 44, 20, 12)
        );

        // A child that needs more than the glass gives gets its minimum.
        assert_eq!(
            child_allocation(true, 100, 48, 10.0, 12.0, 20, 12),
            (44, 10, 12, 20)
        );
    }
}
