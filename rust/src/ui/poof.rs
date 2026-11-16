// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The burst of smoke an icon leaves behind: `src/ui/poof.c` in Rust.
//!
//! A widget the size of the icon it replaces, animated by the frame clock: every
//! frame resolves the puffs in `core/poof.rs` for the current progress and paints
//! them as radial gradients. When the burst is over the widget takes itself out of
//! the dock's box, and the dock measures itself again so the remaining icons close
//! the gap.

use crate::core::poof::{
    lambdock_poof_core_at, lambdock_poof_particles_at, lambdock_poof_particles_init,
    lambdock_poof_progress_alpha, PoofParticle, PoofPuff,
};
use crate::dock_item::DockItem;
use crate::state::LambdockState;
use gtk4::gdk;
use gtk4::glib::{self, translate::from_glib_none};
use gtk4::graphene;
use gtk4::gsk;
use gtk4::prelude::*;
use gtk4::subclass::prelude::*;
use std::cell::{Cell, RefCell};
use std::os::raw::c_int;

/// The reference artwork is five frames of one icon; at the dock's frame clock
/// that is a little over a third of a second.
const PARTICLES: usize = 14;
const DURATION_MS: f64 = 320.0;

extern "C" {
    /// Re-derives the dock's layout from the finished row (`dock_window.h`).
    fn update_dock_dimensions(state: *mut LambdockState);
}

mod imp {
    use super::*;

    #[derive(Default)]
    pub struct Poof {
        /// The dock to re-measure when the burst is over; null once it is gone.
        pub state: Cell<*mut LambdockState>,
        pub particles: RefCell<Vec<PoofParticle>>,
        pub tick_id: RefCell<Option<gtk4::TickCallbackId>>,
        pub start_us: Cell<i64>,
        pub started: Cell<bool>,
        pub progress: Cell<f64>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for Poof {
        const NAME: &'static str = "LambdockPoof";
        type Type = super::Poof;
        type ParentType = gtk4::Widget;
    }

    impl ObjectImpl for Poof {
        fn constructed(&self) {
            self.parent_constructed();

            let widget = self.obj();
            widget.set_can_focus(false);
            widget.set_can_target(false);
            // What the dock's stylesheet and the tests key on.
            widget.add_css_class("dock-poof");
        }

        fn dispose(&self) {
            // The dock may be going away underneath the burst.
            self.state.set(std::ptr::null_mut());

            if let Some(tick) = self.tick_id.borrow_mut().take() {
                tick.remove();
            }
        }
    }

    impl WidgetImpl for Poof {
        fn snapshot(&self, snapshot: &gtk4::Snapshot) {
            let widget = self.obj();
            let (width, height) = (f64::from(widget.width()), f64::from(widget.height()));
            let size = width.min(height);
            let progress = self.progress.get();

            if size <= 0.0 || progress <= 0.0 {
                return;
            }

            let t = progress.min(1.0);
            let fade = lambdock_poof_progress_alpha(t);
            if fade <= 0.0 {
                return;
            }

            // The puffs are laid out in the icon's own square, centred in whatever
            // the widget was allocated: the smoke must never stretch into a band
            // just because the dock's box is taller than the icon.
            let (ox, oy) = ((width - size) / 2.0, (height - size) / 2.0);

            // The dense middle first, then the puffs that leave it.
            let mut core = PoofPuff::default();
            unsafe { lambdock_poof_core_at(t, &mut core) };
            paint_puff(
                snapshot,
                ox + size / 2.0,
                oy + size / 2.0,
                core.radius * size,
                core.alpha * fade,
            );

            let particles = self.particles.borrow();
            let mut puffs = vec![PoofPuff::default(); particles.len()];
            unsafe {
                lambdock_poof_particles_at(
                    particles.as_ptr(),
                    particles.len(),
                    t,
                    puffs.as_mut_ptr(),
                )
            };

            for puff in puffs {
                if puff.alpha <= 0.004 {
                    continue;
                }

                paint_puff(
                    snapshot,
                    ox + puff.x * size,
                    oy + puff.y * size,
                    puff.radius * size,
                    puff.alpha * fade,
                );
            }
        }
    }
}

glib::wrapper! {
    /// The burst widget itself.
    pub struct Poof(ObjectSubclass<imp::Poof>)
        @extends gtk4::Widget,
        @implements gtk4::Accessible, gtk4::Buildable, gtk4::ConstraintTarget;
}

/// Draws one puff: light in the middle, greyer at its edge, fading to nothing.
///
/// The two-tone fill is what lets a single puff read on a pale platinum dock and
/// on a smoked one; a flat grey circle would disappear into one or the other.
fn paint_puff(snapshot: &gtk4::Snapshot, cx: f64, cy: f64, radius: f64, alpha: f64) {
    if radius <= 0.0 || alpha <= 0.0 {
        return;
    }

    let bounds = graphene::Rect::new(
        (cx - radius) as f32,
        (cy - radius) as f32,
        (radius * 2.0) as f32,
        (radius * 2.0) as f32,
    );
    let center = graphene::Point::new(cx as f32, cy as f32);

    let stops = [
        gsk::ColorStop::new(0.0, gdk::RGBA::new(0.97, 0.97, 0.99, alpha as f32)),
        gsk::ColorStop::new(0.5, gdk::RGBA::new(0.80, 0.81, 0.85, (0.72 * alpha) as f32)),
        gsk::ColorStop::new(1.0, gdk::RGBA::new(0.32, 0.33, 0.36, 0.0)),
    ];

    snapshot.append_radial_gradient(
        &bounds,
        &center,
        radius as f32,
        radius as f32,
        0.0,
        1.0,
        &stops,
    );
}

/// Takes the finished widget out of the dock on an idle: a widget must not
/// disappear from inside its own tick callback while GTK is still iterating.
fn schedule_removal(poof: &Poof) {
    let poof = poof.clone();

    glib::idle_add_local_full(glib::Priority::DEFAULT_IDLE, move || {
        let state = poof.imp().state.replace(std::ptr::null_mut());

        if let Some(parent) = poof.parent().and_then(|parent| parent.downcast::<gtk4::Box>().ok()) {
            parent.remove(&poof);
        }

        // The gap closes now, not while the smoke was still there.
        if !state.is_null() && unsafe { !(*state).window.is_null() } {
            unsafe { update_dock_dimensions(state) };
        }

        glib::ControlFlow::Break
    });
}

/// Starts the frame clock for this burst.
fn start_tick(poof: &Poof) {
    let tick = poof.add_tick_callback(|poof, clock| {
        let imp = poof.imp();
        let now = clock.frame_time();

        if !imp.started.get() {
            imp.started.set(true);
            imp.start_us.set(now);
        }

        let elapsed_ms = (now - imp.start_us.get()) as f64 / 1000.0;
        imp.progress.set(elapsed_ms / DURATION_MS);

        if imp.progress.get() >= 1.0 {
            // The source is ending; forget the handle rather than removing it.
            imp.tick_id.borrow_mut().take();
            schedule_removal(poof);
            return glib::ControlFlow::Break;
        }

        poof.queue_draw();
        glib::ControlFlow::Continue
    });

    *poof.imp().tick_id.borrow_mut() = Some(tick);
}

/// Puts a poof widget in the item's place and starts the burst.
///
/// Call it immediately before removing the item: the puff stays where the icon
/// was, keeps the dock's width for the animation, removes itself when it is done
/// and re-measures the dock so the remaining icons close the gap.
///
/// # Safety
///
/// `item` must be null or a live `DockItem` whose widget is in its dock's box.
#[no_mangle]
pub unsafe extern "C" fn lambdock_poof_take_slot(item: *mut DockItem) -> c_int {
    // gtk-rs asserts its own initialization flag inside the subclass machinery,
    // and C's gtk_init does not set it.
    crate::ui::ensure_gtk_initialized();

    if item.is_null() {
        return 0;
    }

    let item = &*item;
    let state = item.state;

    if state.is_null()
        || (*state).window.is_null()
        || (*state).box_.is_null()
        || (*state).box_ == std::ptr::null_mut()
    {
        return 0;
    }

    let dock_box: gtk4::Box = from_glib_none((*state).box_.cast::<gtk4::ffi::GtkBox>());

    // The slot the icon occupies: the shelf wrapper when there is one.
    let slot = [item.container, item.cell, item.button]
        .into_iter()
        .find(|pointer| !pointer.is_null());

    let Some(slot) = slot else {
        return 0;
    };
    let slot: gtk4::Widget = from_glib_none(slot.cast::<gtk4::ffi::GtkWidget>());

    if slot.parent().as_ref() != Some(dock_box.upcast_ref()) {
        return 0;
    }

    // An unsized slot means the dock is not on screen; there is nothing to see,
    // so the item is removed without an animation.
    let (width, height) = (slot.width(), slot.height());
    if width <= 0 || height <= 0 {
        return 0;
    }

    let poof: Poof = glib::Object::builder().build();
    poof.imp().state.set(state);

    let seed = (glib::monotonic_time() as u32) ^ ((width as u32) << 16) ^ (height as u32);
    let mut particles = vec![PoofParticle::default(); PARTICLES];
    lambdock_poof_particles_init(particles.as_mut_ptr(), PARTICLES, seed);
    *poof.imp().particles.borrow_mut() = particles;

    // The same footprint as the icon, so the dock does not reflow under the
    // smoke, and the same alignment, so it sits on the shelf like its neighbours.
    poof.set_size_request(width, height);
    let choose = |align: gtk4::Align| {
        if align == gtk4::Align::Fill {
            gtk4::Align::Center
        } else {
            align
        }
    };
    poof.set_valign(choose(slot.valign()));
    poof.set_halign(choose(slot.halign()));

    // Right after the slot, which the caller removes next: the puff ends up at
    // exactly the index the icon had, and the dock's layout does not jump.
    dock_box.insert_child_after(&poof, Some(&slot));

    start_tick(&poof);

    1
}
