// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The fish-eye container: `src/ui/icon_zoom.c` in Rust.
//!
//! Hovering used to ask the icon theme for the artwork again at every pixel size
//! the pointer passed through, which is milliseconds per icon per frame. This
//! widget holds the artwork at a raster and paints it through a scale transform
//! instead, so magnification costs a matrix on the GPU and follows the pointer
//! continuously.
//!
//! It is the second widget *subclass* on the Rust side, after `ui/poof.rs`, and
//! the first that overrides measure, size allocation and snapshot at once: the
//! size it reports is what spreads the row, the allocation centres the child, and
//! the snapshot is where the transform happens.

use gtk4::glib;
use gtk4::glib::translate::{from_glib_borrow, from_glib_none, Borrowed, IntoGlibPtr, ToGlibPtr};
use gtk4::graphene;
use gtk4::gsk::Transform;
use gtk4::prelude::*;
use gtk4::subclass::prelude::*;
use gtk4::{ffi, ImageType};
use std::cell::{Cell, RefCell};
use std::os::raw::{c_int, c_void};

/// Magnification at which the artwork is re-resolved at its largest size, and the
/// lower one at which it goes back to the resting size. The gap between them is
/// hysteresis: without it an icon hovering exactly on the threshold would
/// re-resolve its artwork every frame, which is the cost this widget exists to
/// avoid.
const HIRES_IN: f64 = 1.10;
const HIRES_OUT: f64 = 1.05;

/// The size the widget hands to its parent: the icon drawn at the current scale,
/// in whole pixels, or the resting size if that rounds away.
fn allocated_size(base_size: i32, scale: f64) -> i32 {
    let size = (f64::from(base_size) * scale).round() as i32;

    if size > 0 {
        size
    } else {
        base_size
    }
}

/// A scale the layout can use: at least 1.0 (the dock never draws icons smaller
/// than their resting size), and 1.0 for anything nonsense that reaches here.
fn clamped_scale(scale: f64) -> f64 {
    if scale.is_finite() && scale > 1.0 {
        scale
    } else {
        1.0
    }
}

/// Whether the artwork should be held at its largest size, with the hysteresis
/// band between `HIRES_IN` and `HIRES_OUT`.
fn wants_hi_res(hi_res: bool, scale: f64, base_size: i32, max_size: i32) -> bool {
    // A dock that does not magnify has nothing to switch to.
    if max_size <= base_size {
        return hi_res;
    }

    if hi_res {
        scale > HIRES_OUT
    } else {
        scale >= HIRES_IN
    }
}

/// Whether the child resamples itself at whatever size it is given, and so is
/// allocated the target size and painted straight through.
///
/// An icon theme answers each pixel size with a fresh lookup and resample, so
/// icons resolved by name or gicon are held as a raster; a paintable or a texture
/// scales on the GPU for free.
fn paints_at_target(is_image: bool, storage: ImageType) -> bool {
    is_image && !matches!(storage, ImageType::IconName | ImageType::Gicon)
}

mod imp {
    use super::*;

    #[derive(Default)]
    pub struct IconZoom {
        /// The icon, kept as a raster of `raster` px. Held owned rather than
        /// borrowed so dispose can take it back out and unparent it.
        pub child: RefCell<Option<gtk4::Widget>>,
        /// Size the dock lays out at rest.
        pub base_size: Cell<i32>,
        /// Largest size the icon is ever drawn at.
        pub max_size: Cell<i32>,
        /// Size the child is currently resolved at.
        pub raster: Cell<i32>,
        /// 1.0 = `base_size`.
        pub scale: Cell<f64>,
        /// Currently holding the large raster.
        pub hi_res: Cell<bool>,
        /// The child resamples itself: draw it at the size it is allocated.
        pub paint_at_target: Cell<bool>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for IconZoom {
        const NAME: &'static str = "LambdockIconZoom";
        type Type = super::IconZoom;
        type ParentType = gtk4::Widget;
    }

    impl ObjectImpl for IconZoom {
        fn constructed(&self) {
            self.parent_constructed();

            self.scale.set(1.0);

            // A magnified icon is drawn outside the box it was allocated, and
            // every ancestor the dock builds is built the same way.
            self.obj().set_overflow(gtk4::Overflow::Visible);
        }

        fn dispose(&self) {
            if let Some(child) = self.child.replace(None) {
                child.unparent();
            }
        }
    }

    impl WidgetImpl for IconZoom {
        fn measure(&self, orientation: gtk4::Orientation, for_size: i32) -> (i32, i32, i32, i32) {
            // GTK requires a container to have measured a child before it
            // allocates it; the answer itself is unused, because the child is
            // allocated at the size of the raster it holds.
            if let Some(child) = self.child.borrow().as_ref() {
                let _ = child.measure(orientation, for_size);
            }

            // Both axes: the dock's buttons grow with the icon in every direction,
            // which is how the row of icons spreads as the pointer passes.
            let size = allocated_size(self.base_size.get(), self.scale.get());

            (size, size, -1, -1)
        }

        fn size_allocate(&self, width: i32, height: i32, baseline: i32) {
            let Some(child) = self.child.borrow().clone() else {
                return;
            };

            // The child is allocated at the size of the raster it holds, centred
            // in the (usually smaller) allocation; the snapshot scales it down to
            // the size on screen, so it is never re-rasterised. A paintable child
            // resamples itself and is allocated the target size instead.
            let size = if self.paint_at_target.get() {
                width.max(height)
            } else {
                self.raster.get()
            };

            let offset =
                graphene::Point::new(((width - size) / 2) as f32, ((height - size) / 2) as f32);

            child.allocate(
                size,
                size,
                baseline,
                Some(Transform::new().translate(&offset)),
            );
        }

        fn snapshot(&self, snapshot: &gtk4::Snapshot) {
            let widget = self.obj();

            let Some(child) = self.child.borrow().clone() else {
                return;
            };

            if self.paint_at_target.get() {
                widget.snapshot_child(&child, snapshot);
                return;
            }

            let target = f64::from(allocated_size(self.base_size.get(), self.scale.get()));
            let factor = target / f64::from(self.raster.get());

            if (factor - 1.0).abs() < 0.001 {
                widget.snapshot_child(&child, snapshot);
                return;
            }

            // Scale about the widget's centre. GskTransform applies the new
            // operation after the one it wraps, so translating first and scaling
            // after means the translation is in the widget's own coordinates.
            let centre_x = f64::from(widget.width()) / 2.0;
            let centre_y = f64::from(widget.height()) / 2.0;
            let (sx, sy) = (factor as f32, factor as f32);

            let offset = graphene::Point::new(
                (centre_x * (1.0 - f64::from(sx)) / f64::from(sx)) as f32,
                (centre_y * (1.0 - f64::from(sy)) / f64::from(sy)) as f32,
            );
            let transform = Transform::new().translate(&offset).scale(sx, sy);

            snapshot.save();
            snapshot.transform(Some(&transform));
            widget.snapshot_child(&child, snapshot);
            snapshot.restore();
        }
    }
}

glib::wrapper! {
    /// A dock icon that can be drawn larger without being rasterised again.
    pub struct IconZoom(ObjectSubclass<imp::IconZoom>)
        @extends gtk4::Widget,
        @implements gtk4::Accessible, gtk4::Buildable, gtk4::ConstraintTarget;
}

/// Switches the artwork between its resting and its largest size.
///
/// The switch happens twice per hover at most, and only for icons whose artwork
/// is resolved by size (icon names and gicons); a paintable resamples itself, so
/// it keeps its one copy.
fn sync_raster(zoom: &IconZoom) {
    let imp = zoom.imp();

    if imp.paint_at_target.get() {
        return;
    }

    let want_hi = wants_hi_res(
        imp.hi_res.get(),
        imp.scale.get(),
        imp.base_size.get(),
        imp.max_size.get(),
    );

    if want_hi == imp.hi_res.get() {
        return;
    }

    imp.hi_res.set(want_hi);
    imp.raster.set(if want_hi {
        imp.max_size.get()
    } else {
        imp.base_size.get()
    });

    if let Some(image) = imp
        .child
        .borrow()
        .as_ref()
        .and_then(|child| child.downcast_ref::<gtk4::Image>())
    {
        image.set_pixel_size(imp.raster.get());
    }

    zoom.queue_resize();
}

/// The zoom behind a widget pointer, or `None` when it is not one.
///
/// # Safety
///
/// `pointer` must be null or a live `GtkWidget`.
unsafe fn as_zoom(pointer: *mut ffi::GtkWidget) -> Option<Borrowed<IconZoom>> {
    if pointer.is_null() {
        return None;
    }

    let widget: Borrowed<gtk4::Widget> = from_glib_borrow(pointer);

    // The C callers pass whatever widget they have (`is_zoom` is how they ask),
    // so the type test comes before the cast.
    if !widget.is::<IconZoom>() {
        return None;
    }

    // `into_inner` hands the wrapper back without touching the reference count,
    // and `unsafe_cast` is sound because the test above just ran. Wrapping the
    // result borrowed keeps the transfer `(transfer none)`, as the header says.
    let widget: gtk4::Widget = widget.into_inner();
    Some(Borrowed::new(widget.unsafe_cast()))
}

/// Builds a zoom around `child`.
///
/// # Safety
///
/// `child` must be null or a live, unparented `GtkWidget`.
pub unsafe fn lambdock_icon_zoom_new(
    child: *mut ffi::GtkWidget,
    base_size: c_int,
    max_size: c_int,
) -> *mut ffi::GtkWidget {
    // gtk-rs asserts its own initialization flag inside the subclass machinery,
    // and C's gtk_init does not set it.
    crate::ui::ensure_gtk_initialized();

    let base_size = if base_size <= 0 { 48 } else { base_size };
    let max_size = if max_size < base_size {
        base_size
    } else {
        max_size
    };

    let zoom: IconZoom = glib::Object::builder().build();
    {
        let imp = zoom.imp();
        imp.base_size.set(base_size);
        imp.max_size.set(max_size);
        imp.raster.set(base_size);
    }

    if !child.is_null() {
        let child: gtk4::Widget = from_glib_none(child);

        // Icons resolved by name are re-rasterised for every size they are asked
        // for, so they are held as a raster; anything else (a texture loaded from
        // a file) already resamples on the GPU at whatever size it is given.
        let image = child.downcast_ref::<gtk4::Image>();
        let storage = image.map_or(ImageType::Empty, |image| image.storage_type());
        zoom.imp()
            .paint_at_target
            .set(paints_at_target(image.is_some(), storage));

        child.set_overflow(gtk4::Overflow::Visible);
        if let Some(image) = image {
            image.set_pixel_size(base_size);
        }

        child.set_parent(&zoom);
        *zoom.imp().child.borrow_mut() = Some(child);
    }

    // The contract `src/ui/icon_zoom.h` declares is `(transfer full)`, and what
    // the callers expect is what `g_object_new` used to hand them: a *floating*
    // reference that the box they append it to sinks. A gtk4-rs `build()` sinks
    // the object already, so it is floated again and the wrapper lets go without
    // unreffing.
    glib::gobject_ffi::g_object_force_floating(zoom.as_ptr().cast());

    zoom.upcast::<gtk4::Widget>().into_glib_ptr()
}

/// Sets the painted size, and in whole pixels the size reported to the parent.
///
/// # Safety
///
/// `pointer` must be null or a live `GtkWidget`.
pub unsafe fn lambdock_icon_zoom_set_scale(pointer: *mut ffi::GtkWidget, scale: f64) {
    let Some(zoom) = as_zoom(pointer) else {
        return;
    };

    let scale = clamped_scale(scale);

    // A pointer moving by a thousandth of a pixel is not a repaint.
    if (scale - zoom.imp().scale.get()).abs() < 0.002 {
        return;
    }

    let previous_size = allocated_size(zoom.imp().base_size.get(), zoom.imp().scale.get());
    zoom.imp().scale.set(scale);
    sync_raster(&zoom);

    // Only the layout steps in whole pixels; the painting is continuous.
    if allocated_size(zoom.imp().base_size.get(), zoom.imp().scale.get()) != previous_size {
        zoom.queue_resize();
    } else {
        zoom.queue_draw();
    }
}

/// The current magnification factor, or 1.0 for anything that is not a zoom.
///
/// # Safety
///
/// `pointer` must be null or a live `GtkWidget`.
pub unsafe fn lambdock_icon_zoom_get_scale(pointer: *mut ffi::GtkWidget) -> f64 {
    match as_zoom(pointer) {
        Some(zoom) => zoom.imp().scale.get(),
        None => 1.0,
    }
}

/// The size the icon is drawn at, rounded the way the layout sees it.
///
/// # Safety
///
/// `pointer` must be null or a live `GtkWidget`.
pub unsafe fn lambdock_icon_zoom_target_size(pointer: *mut ffi::GtkWidget) -> c_int {
    match as_zoom(pointer) {
        Some(zoom) => allocated_size(zoom.imp().base_size.get(), zoom.imp().scale.get()),
        None => 0,
    }
}

/// The icon the zoom holds, borrowed.
///
/// # Safety
///
/// `pointer` must be null or a live `GtkWidget`.
pub unsafe fn lambdock_icon_zoom_get_child(
    pointer: *mut ffi::GtkWidget,
) -> *mut ffi::GtkWidget {
    let Some(zoom) = as_zoom(pointer) else {
        return std::ptr::null_mut();
    };

    let child = zoom.imp().child.borrow();
    let pointer = match child.as_ref() {
        Some(child) => child.to_glib_none().0,
        None => std::ptr::null_mut(),
    };

    pointer
}

/// Whether `widget` is a zoom, which is how the dock finds one among its items.
///
/// # Safety
///
/// `widget` must be null or a live `GtkWidget`.
pub unsafe fn lambdock_icon_zoom_is_zoom(widget: *mut c_void) -> c_int {
    i32::from(as_zoom(widget.cast()).is_some())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The dock compares this against its own stepped size, so it has to round
    /// the way `lambdock_scaled_size()` does: to the nearest whole pixel.
    #[test]
    fn the_layout_size_rounds_like_the_dock_does() {
        assert_eq!(allocated_size(48, 1.0), 48);
        assert_eq!(allocated_size(48, 1.45), 70); // 69.6
        assert_eq!(allocated_size(48, 1.2), 58); // 57.6
        assert_eq!(allocated_size(48, 1.01), 48); // 48.48
    }

    /// A scale nobody meant must not reach the layout: 0.5 and NaN both rest.
    #[test]
    fn a_nonsense_scale_rests_at_one() {
        assert_eq!(clamped_scale(1.45), 1.45);
        assert_eq!(clamped_scale(1.0), 1.0);
        assert_eq!(clamped_scale(0.5), 1.0);
        assert_eq!(clamped_scale(-3.0), 1.0);
        assert_eq!(clamped_scale(f64::NAN), 1.0);
        assert_eq!(clamped_scale(f64::INFINITY), 1.0);
    }

    /// The two thresholds differ, and that difference is the point: a pointer
    /// parked exactly on one size must not re-resolve the artwork every frame.
    #[test]
    fn the_raster_switches_with_hysteresis() {
        // Coming up from rest, wait until the upscaled resting raster would be
        // visibly soft.
        assert!(!wants_hi_res(false, 1.07, 48, 70));
        assert!(wants_hi_res(false, HIRES_IN, 48, 70));
        assert!(wants_hi_res(false, 1.45, 48, 70));

        // Holding the large one, keep it a little longer.
        assert!(wants_hi_res(true, 1.07, 48, 70));
        assert!(!wants_hi_res(true, HIRES_OUT, 48, 70));
        assert!(!wants_hi_res(true, 1.0, 48, 70));

        // A dock that does not magnify has nothing to switch to.
        assert!(!wants_hi_res(false, 2.0, 48, 48));
        assert!(wants_hi_res(true, 2.0, 48, 48));
    }

    /// Which artwork is held as a raster, and which paints straight through.
    #[test]
    fn artwork_that_resamples_itself_paints_at_the_target_size() {
        assert!(!paints_at_target(true, ImageType::IconName));
        assert!(!paints_at_target(true, ImageType::Gicon));

        assert!(paints_at_target(true, ImageType::Paintable));

        // Not an image at all: there is no artwork to re-resolve.
        assert!(!paints_at_target(false, ImageType::Empty));
    }
}
