// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "poof.h"

#include <math.h>

/* ============================================================================
 * DETERMINISTIC RANDOMNESS
 * ============================================================================
 * The cloud only has to look random, and a test has to be able to predict it, so
 * this is a plain linear congruential generator rather than the GLib one. The
 * multiplier and increment are the Numerical Recipes constants; taking the top
 * eight bits of the state avoids the weak low bits of an LCG.
 */
static double poof_next_unit(guint32 *state) {
  *state = (*state * 1664525u) + 1013904223u;
  return (double)(*state >> 8) / 16777216.0;
}

void lambdock_poof_particles_init(LambdockPoofParticle *particles, gsize n,
                                  guint32 seed) {
  if (!particles || n == 0)
    return;

  guint32 state = seed ? seed : 1u;

  for (gsize i = 0; i < n; i++) {
    LambdockPoofParticle *particle = &particles[i];

    /* Spread the directions evenly and jitter them, so the cloud has no gap in
     * it however few puffs there are. */
    double slice = (2.0 * G_PI) / (double)n;
    particle->angle =
        slice * (double)i + slice * 0.8 * poof_next_unit(&state);

    /* Puffs start on the icon and travel 0.12 to 0.28 icon outwards. */
    particle->start = 0.04 * poof_next_unit(&state);
    particle->distance = 0.12 + 0.16 * poof_next_unit(&state);

    /* Each ends 0.06 to 0.18 icon wide. Together the reach and the radius are at
     * most half an icon, which is what keeps the cloud inside the icon it
     * replaced -- the same box the five reference frames use. */
    particle->radius = 0.06 + 0.12 * poof_next_unit(&state);

    /* Everything is under way in the first third of the animation. */
    particle->delay = 0.12 * poof_next_unit(&state);

    /* The smoke thins from the outside in -- that is what makes the visible
     * cloud shrink from the full frame to ~76x86 in the reference -- so the
     * puffs that travel furthest die first. */
    double reach = (particle->distance - 0.12) / 0.16;
    particle->life = 0.95 - 0.45 * reach;
  }
}

void lambdock_poof_particles_at(const LambdockPoofParticle *particles, gsize n,
                                double t, LambdockPoofPuff *out) {
  if (!out)
    return;

  for (gsize i = 0; i < n; i++) {
    LambdockPoofPuff *puff = &out[i];
    puff->x = 0.5;
    puff->y = 0.5;
    puff->radius = 0.0;
    puff->alpha = 0.0;

    if (!particles)
      continue;

    const LambdockPoofParticle *particle = &particles[i];
    if (particle->life <= 0.0)
      continue;

    double local = (t - particle->delay) / particle->life;
    if (local <= 0.0 || local >= 1.0)
      continue;

    /* Fast out of the middle, then slowing down: the smoke leaves quickly and
     * drifts at the end. */
    double grow = 1.0 - pow(1.0 - local, 3.0);
    /* Two thirds of the way out on the first frame, then a slow drift: the
     * reference cloud is already full-size when the burst starts. */
    double reach = particle->start + particle->distance * (0.62 + 0.38 * grow);

    puff->x = 0.5 + cos(particle->angle) * reach;
    puff->y = 0.5 + sin(particle->angle) * reach;

    /* Puffs swell as they thin out. */
    puff->radius = 0.06 + particle->radius * sqrt(local);

    /* ... and they are thickest early, which is what the reference frames show:
     * the cloud loses density over the whole animation. */
    puff->alpha = pow(1.0 - local, 1.7);
  }
}

void lambdock_poof_core_at(double t, LambdockPoofPuff *out) {
  if (!out)
    return;

  if (t < 0.0)
    t = 0.0;
  if (t > 1.0)
    t = 1.0;

  out->x = 0.5;
  out->y = 0.5;
  /* Already full-sized on the first frame, as in the artwork, then opening up a
   * little more as the smoke leaves. */
  out->radius = 0.16 + 0.30 * (1.0 - pow(1.0 - t, 2.0));
  out->alpha = 0.85 * pow(1.0 - t, 1.4);
}

double lambdock_poof_progress_alpha(double t) {
  if (t >= 1.0)
    return 0.0;
  if (t <= 0.0)
    return 1.0;

  return 1.0 - t * t;
}
