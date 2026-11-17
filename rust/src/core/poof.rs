// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The poof as arithmetic: `src/core/poof.c` in Rust.
//!
//! Dragging an icon out of a shelf dock made it burst into a puff of smoke: five
//! frames whose alpha falls while the visible cloud shrinks, because the outer
//! smoke thins out first. This module is that burst as values, so the animation
//! is testable without a display; `src/ui/poof.c` only draws what these return.

use std::f64::consts::PI;

/// One puff of smoke, in units of the icon the poof replaces.
///
/// The field order is the contract: `LambdockPoofParticle` in the header.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct PoofParticle {
    pub angle: f64,
    pub start: f64,
    pub distance: f64,
    pub radius: f64,
    pub delay: f64,
    pub life: f64,
}

/// One puff, resolved for a moment in time.
///
/// The field order is the contract: `LambdockPoofPuff` in the header.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct PoofPuff {
    pub x: f64,
    pub y: f64,
    pub radius: f64,
    pub alpha: f64,
}

/// The next value in `[0, 1)` from the particle LCG.
///
/// The multiplier and increment are the Numerical Recipes constants; taking the
/// top eight bits of the state avoids the weak low bits of an LCG.
fn next_unit(state: &mut u32) -> f64 {
    *state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
    f64::from(*state >> 8) / 16_777_216.0
}

/// Fills `particles` with a cloud that starts dense in the middle and spreads
/// outwards. The same seed always produces the same cloud, so a test can assert
/// what the animation does.
///
/// # Safety
///
/// `particles` must point to `n` writable particles, or be null.
pub unsafe fn lambdock_poof_particles_init(
    particles: *mut PoofParticle,
    n: usize,
    seed: u32,
) {
    if particles.is_null() || n == 0 {
        return;
    }

    let particles = std::slice::from_raw_parts_mut(particles, n);
    // An LCG rather than a real random source: the cloud only has to look
    // random, and the test has to be able to predict it.
    let mut state = if seed == 0 { 1 } else { seed };

    // Directions are spread evenly and then jittered, so the cloud has no gap in
    // it however few puffs there are.
    let slice = 2.0 * PI / n as f64;

    for (i, particle) in particles.iter_mut().enumerate() {
        particle.angle = slice * i as f64 + slice * 0.8 * next_unit(&mut state);

        // Puffs start on the icon and travel 0.12 to 0.28 icon outwards.
        particle.start = 0.04 * next_unit(&mut state);
        particle.distance = 0.12 + 0.16 * next_unit(&mut state);

        // Each ends 0.06 to 0.18 icon wide: reach plus radius stays inside half
        // an icon, the box the reference frames use.
        particle.radius = 0.06 + 0.12 * next_unit(&mut state);

        // Everything is under way in the first third of the animation.
        particle.delay = 0.12 * next_unit(&mut state);

        // The smoke thins from the outside in, so the puffs that travel
        // furthest die first -- that is what shrinks the visible cloud.
        let reach = (particle.distance - 0.12) / 0.16;
        particle.life = 0.95 - 0.45 * reach;
    }
}

/// Resolves every particle for `t`. A particle that has not started yet, or
/// whose life is over, comes back with zero alpha.
///
/// # Safety
///
/// `out` must point to `n` writable puffs, or be null; `particles` must point to
/// `n` readable particles, or be null.
pub unsafe fn lambdock_poof_particles_at(
    particles: *const PoofParticle,
    n: usize,
    t: f64,
    out: *mut PoofPuff,
) {
    if out.is_null() {
        return;
    }

    let out = std::slice::from_raw_parts_mut(out, n);

    for (i, puff) in out.iter_mut().enumerate() {
        *puff = PoofPuff {
            x: 0.5,
            y: 0.5,
            radius: 0.0,
            alpha: 0.0,
        };

        if particles.is_null() {
            continue;
        }

        let particle = &*particles.add(i);
        if particle.life <= 0.0 {
            continue;
        }

        let local = (t - particle.delay) / particle.life;
        if local <= 0.0 || local >= 1.0 {
            continue;
        }

        // Fast out of the middle, then slowing down: the smoke leaves quickly
        // and drifts at the end.
        let grow = 1.0 - (1.0 - local).powf(3.0);
        // Two thirds of the way out on the first frame, then a slow drift: the
        // reference cloud is already full-size when the burst starts.
        let reach = particle.start + particle.distance * (0.62 + 0.38 * grow);

        puff.x = 0.5 + particle.angle.cos() * reach;
        puff.y = 0.5 + particle.angle.sin() * reach;

        // Puffs swell as they thin out, and are thickest early.
        puff.radius = 0.06 + particle.radius * local.sqrt();
        puff.alpha = (1.0 - local).powf(1.7);
    }
}

/// The dense centre of the burst.
///
/// # Safety
///
/// `out` must be null or point to a writable puff.
pub unsafe fn lambdock_poof_core_at(t: f64, out: *mut PoofPuff) {
    if out.is_null() {
        return;
    }

    let t = t.clamp(0.0, 1.0);

    *out = PoofPuff {
        x: 0.5,
        y: 0.5,
        // Already full-sized on the first frame, as in the artwork, then opening
        // up a little more as the smoke leaves.
        radius: 0.16 + 0.30 * (1.0 - (1.0 - t).powf(2.0)),
        alpha: 0.85 * (1.0 - t).powf(1.4),
    };
}

/// The global fade, multiplied into every puff, so the animation is guaranteed
/// to end invisible whatever the individual lifetimes are.
pub fn lambdock_poof_progress_alpha(t: f64) -> f64 {
    if t >= 1.0 {
        return 0.0;
    }

    if t <= 0.0 {
        return 1.0;
    }

    1.0 - t * t
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Mirrors `LAMBDOCK_POOF_PARTICLES` in the header: the cloud the widget
    /// draws. The functions take the count, so only the tests need the number.
    const PARTICLES: usize = 14;

    fn fill(seed: u32) -> Vec<PoofParticle> {
        let mut particles = vec![PoofParticle::default(); PARTICLES];
        unsafe { lambdock_poof_particles_init(particles.as_mut_ptr(), PARTICLES, seed) };
        particles
    }

    fn resolve(particles: &[PoofParticle], t: f64) -> Vec<PoofPuff> {
        let mut puffs = vec![PoofPuff::default(); PARTICLES];
        unsafe {
            lambdock_poof_particles_at(
                particles.as_ptr(),
                PARTICLES,
                t,
                puffs.as_mut_ptr(),
            )
        };
        puffs
    }

    fn core_at(t: f64) -> PoofPuff {
        let mut puff = PoofPuff::default();
        unsafe { lambdock_poof_core_at(t, &mut puff) };
        puff
    }

    fn alpha_sum(puffs: &[PoofPuff]) -> f64 {
        puffs.iter().map(|puff| puff.alpha).sum()
    }

    fn mean_radius(puffs: &[PoofPuff]) -> f64 {
        let visible: Vec<&PoofPuff> = puffs.iter().filter(|p| p.alpha > 0.0).collect();
        if visible.is_empty() {
            return 0.0;
        }
        visible.iter().map(|p| p.radius).sum::<f64>() / visible.len() as f64
    }

    fn mean_distance(puffs: &[PoofPuff]) -> f64 {
        let visible: Vec<&PoofPuff> = puffs.iter().filter(|p| p.alpha > 0.0).collect();
        if visible.is_empty() {
            return 0.0;
        }
        let total: f64 = visible
            .iter()
            .map(|p| ((p.x - 0.5).powi(2) + (p.y - 0.5).powi(2)).sqrt())
            .sum();
        total / visible.len() as f64
    }

    // ======================================================================
    // The particles
    // ======================================================================

    #[test]
    fn the_same_seed_makes_the_same_cloud() {
        let a = fill(12345);
        let b = fill(12345);

        for (a, b) in a.iter().zip(b.iter()) {
            assert_eq!(a.angle, b.angle);
            assert_eq!(a.distance, b.distance);
            assert_eq!(a.radius, b.radius);
            assert_eq!(a.delay, b.delay);
            assert_eq!(a.life, b.life);
        }
    }

    #[test]
    fn a_different_seed_makes_another_cloud() {
        let a = fill(12345);
        let c = fill(999);
        assert!(a
            .iter()
            .zip(c.iter())
            .any(|(a, c)| (a.angle - c.angle).abs() > 1e-9));
    }

    #[test]
    fn every_particle_stays_inside_the_icon_it_replaces() {
        for p in fill(7) {
            assert!((0.0..2.0 * PI * 2.0).contains(&p.angle));
            assert!((0.0..0.05).contains(&p.start));
            assert!((0.12..=0.28).contains(&p.distance));
            assert!((0.06..=0.18).contains(&p.radius));
            // Reach plus radius fits in half an icon.
            assert!(p.start + p.distance + p.radius <= 0.51);
            assert!((0.0..=0.12).contains(&p.delay));
            assert!((0.5..=0.95).contains(&p.life));
        }
    }

    #[test]
    fn the_directions_cover_the_whole_circle() {
        let particles = fill(2024);
        let mut quadrants = [false; 4];

        for p in particles {
            let mut angle = p.angle % (2.0 * PI);
            if angle < 0.0 {
                angle += 2.0 * PI;
            }
            quadrants[(angle / (PI / 2.0)) as usize % 4] = true;
        }

        assert!(quadrants.iter().all(|used| *used));
    }

    // ======================================================================
    // The cloud
    // ======================================================================

    #[test]
    fn nothing_is_drawn_before_the_burst_starts() {
        assert_eq!(alpha_sum(&resolve(&fill(31), 0.0)), 0.0);
    }

    #[test]
    fn the_cloud_is_thick_early_and_gone_when_the_animation_ends() {
        let particles = fill(31);

        let early = alpha_sum(&resolve(&particles, 0.25));
        assert!(early > 1.0);

        let late = alpha_sum(&resolve(&particles, 0.75));
        assert!(late < early);

        assert_eq!(alpha_sum(&resolve(&particles, 1.0)), 0.0);
        // Past the end nothing comes back to life either.
        assert_eq!(alpha_sum(&resolve(&particles, 1.5)), 0.0);
    }

    #[test]
    fn the_smoke_travels_outwards_and_swells() {
        let particles = fill(4242);

        let early = resolve(&particles, 0.2);
        let (near, small) = (mean_distance(&early), mean_radius(&early));

        let later = resolve(&particles, 0.6);
        let (far, big) = (mean_distance(&later), mean_radius(&later));

        assert!(far > near);
        assert!(big > small);
    }

    #[test]
    fn the_cloud_stays_on_the_icon_it_replaced() {
        for puff in resolve(&fill(4242), 0.6) {
            assert!(puff.x - puff.radius < 1.02);
            assert!(puff.x + puff.radius > -0.02);
            assert!(puff.y - puff.radius < 1.02);
            assert!(puff.y + puff.radius > -0.02);
        }
    }

    #[test]
    fn a_null_particle_array_resolves_to_an_empty_cloud() {
        let mut puffs = vec![PoofPuff::default(); PARTICLES];
        unsafe {
            lambdock_poof_particles_at(
                std::ptr::null(),
                PARTICLES,
                0.5,
                puffs.as_mut_ptr(),
            )
        };

        assert!(puffs.iter().all(|p| p.alpha == 0.0 && p.x == 0.5));
    }

    // ======================================================================
    // The core and the fade
    // ======================================================================

    #[test]
    fn the_core_starts_dense_and_fades_out() {
        let (first, middle, last) = (core_at(0.0), core_at(0.5), core_at(1.0));

        assert!(first.alpha > 0.5);
        assert!(first.radius > 0.0);
        assert!(middle.radius > first.radius);
        assert!(middle.alpha < first.alpha);
        assert_eq!(last.alpha, 0.0);

        // It is centred on the icon.
        assert!((first.x - 0.5).abs() < 1e-9);
        assert!((first.y - 0.5).abs() < 1e-9);
    }

    #[test]
    fn the_global_fade_never_goes_back_up() {
        assert_eq!(lambdock_poof_progress_alpha(0.0), 1.0);
        assert_eq!(lambdock_poof_progress_alpha(1.0), 0.0);
        assert_eq!(lambdock_poof_progress_alpha(-0.5), 1.0);
        assert_eq!(lambdock_poof_progress_alpha(1.5), 0.0);

        let mut previous = lambdock_poof_progress_alpha(0.0);
        for step in 1..=20 {
            let alpha = lambdock_poof_progress_alpha(f64::from(step) / 20.0);
            assert!(alpha <= previous);
            previous = alpha;
        }
    }
}
