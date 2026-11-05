// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAMBDOCK_CORE_POOF_H
#define LAMBDOCK_CORE_POOF_H

#include <glib.h>

/*
 * =========================================================================
 * THE POOF, AS ARITHMETIC
 * =========================================================================
 * Dragging an icon out of the shelf look dock made it burst into a puff of smoke.
 * the reference dock's `poof.png` is that animation: five 128x128 frames -- exactly the
 * size of the icon they replace -- whose measured alpha falls from 139 (mean
 * over the frame) to 11, while the visible cloud shrinks from the full frame to
 * about 76x86 as the outer smoke thins out first.
 *
 * This module is that burst as values: a fixed set of puffs, each with a
 * direction, a reach, a size, a start delay and a lifetime, plus the dense core
 * they come out of. `src/ui/poof.c` only draws what these functions return,
 * which is what lets the animation be tested without a display.
 */

/* The reference artwork is five frames; at the dock's frame clock that reads as
 * a little over a third of a second. */
#define LAMBDOCK_POOF_FRAMES 5
#define LAMBDOCK_POOF_DURATION_MS 320.0

/* Enough smoke to read as a cloud rather than as dots, few enough to stay one
 * cheap radial gradient each on every frame. */
#define LAMBDOCK_POOF_PARTICLES 14

/**
 * LambdockPoofParticle:
 * One puff of smoke, in units of the icon the poof replaces.
 *
 * @angle: Direction it travels, in radians.
 * @start: Where it starts, as a fraction of the icon from its centre.
 * @distance: How much further out it travels before it dies (at most 0.28).
 * @radius: Its radius at the end of its life (at most 0.18, so the whole cloud
 *          stays inside the icon the poof replaced).
 * @delay: Fraction of the animation to wait before it starts growing.
 * @life: Fraction of the animation it lives for, counted from @delay.
 */
typedef struct LambdockPoofParticle {
  double angle;
  double start;
  double distance;
  double radius;
  double delay;
  double life;
} LambdockPoofParticle;

/**
 * LambdockPoofPuff:
 * One puff, resolved for a moment in time.
 *
 * @x: Centre across the icon, 0 = left edge, 1 = right edge.
 * @y: Centre down the icon, 0 = top edge, 1 = bottom edge.
 * @radius: Radius as a fraction of the icon.
 * @alpha: Opacity, 0..1.
 */
typedef struct LambdockPoofPuff {
  double x;
  double y;
  double radius;
  double alpha;
} LambdockPoofPuff;

/**
 * lambdock_poof_particles_init:
 * @particles: (out caller-allocates): array of @n particles.
 * @n: How many to fill.
 * @seed: Seed for the deterministic pseudo-random placement.
 *
 * Fills @particles with a smoke cloud that starts dense in the middle and
 * spreads outwards. The same seed always produces the same cloud, so a test can
 * assert what the animation does.
 */
void lambdock_poof_particles_init(LambdockPoofParticle *particles, gsize n,
                                  guint32 seed);

/**
 * lambdock_poof_particles_at:
 * @particles: Particles from lambdock_poof_particles_init().
 * @n: How many there are.
 * @t: Progress through the animation, 0..1.
 * @out: (out caller-allocates): array of @n resolved puffs.
 *
 * Resolves every particle for @t. A particle that has not started yet, or whose
 * life is over, comes back with zero alpha.
 */
void lambdock_poof_particles_at(const LambdockPoofParticle *particles, gsize n,
                                double t, LambdockPoofPuff *out);

/**
 * lambdock_poof_core_at:
 * @t: Progress through the animation, 0..1.
 * @out: (out caller-allocates): the dense centre of the burst.
 *
 * The cloud the puffs come out of: it is already there on the first frame (the
 * reference artwork starts at full density), grows, and thins to nothing.
 */
void lambdock_poof_core_at(double t, LambdockPoofPuff *out);

/**
 * lambdock_poof_progress_alpha:
 * @t: Progress through the animation, 0..1.
 *
 * Global fade, multiplied into every puff, so the animation is guaranteed to
 * end invisible whatever the individual lifetimes are.
 *
 * Returns: 1 at the start, 0 at the end.
 */
double lambdock_poof_progress_alpha(double t);

#endif // LAMBDOCK_CORE_POOF_H
