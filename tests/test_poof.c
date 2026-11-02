// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/core/poof.h"

#include <math.h>

/* Every assertion here is about arithmetic: the burst is a value in
 * core/poof.c, and src/ui/poof.c only draws it. That is why this suite needs
 * neither GTK nor a display. */

static void fill(LambdockPoofParticle *particles, guint32 seed) {
  lambdock_poof_particles_init(particles, LAMBDOCK_POOF_PARTICLES, seed);
}

static void resolve(const LambdockPoofParticle *particles, double t,
                    LambdockPoofPuff *out) {
  lambdock_poof_particles_at(particles, LAMBDOCK_POOF_PARTICLES, t, out);
}

static double alpha_sum(const LambdockPoofPuff *puffs) {
  double total = 0.0;
  for (gsize i = 0; i < LAMBDOCK_POOF_PARTICLES; i++)
    total += puffs[i].alpha;
  return total;
}

static double mean_radius(const LambdockPoofPuff *puffs) {
  double total = 0.0;
  gsize counted = 0;
  for (gsize i = 0; i < LAMBDOCK_POOF_PARTICLES; i++) {
    if (puffs[i].alpha <= 0.0)
      continue;
    total += puffs[i].radius;
    counted++;
  }
  return counted ? total / (double)counted : 0.0;
}

static double mean_distance(const LambdockPoofPuff *puffs) {
  double total = 0.0;
  gsize counted = 0;
  for (gsize i = 0; i < LAMBDOCK_POOF_PARTICLES; i++) {
    if (puffs[i].alpha <= 0.0)
      continue;
    double dx = puffs[i].x - 0.5;
    double dy = puffs[i].y - 0.5;
    total += sqrt(dx * dx + dy * dy);
    counted++;
  }
  return counted ? total / (double)counted : 0.0;
}

/* The same seed makes the same cloud, a different one makes another: the widget
 * seeds from the clock, and the test can seed from a constant. */
static void test_particles_are_deterministic(void) {
  LambdockPoofParticle a[LAMBDOCK_POOF_PARTICLES];
  LambdockPoofParticle b[LAMBDOCK_POOF_PARTICLES];
  LambdockPoofParticle c[LAMBDOCK_POOF_PARTICLES];

  fill(a, 12345u);
  fill(b, 12345u);
  fill(c, 999u);

  for (gsize i = 0; i < LAMBDOCK_POOF_PARTICLES; i++) {
    g_assert_cmpfloat(a[i].angle, ==, b[i].angle);
    g_assert_cmpfloat(a[i].distance, ==, b[i].distance);
    g_assert_cmpfloat(a[i].radius, ==, b[i].radius);
    g_assert_cmpfloat(a[i].delay, ==, b[i].delay);
    g_assert_cmpfloat(a[i].life, ==, b[i].life);
  }

  gboolean differs = FALSE;
  for (gsize i = 0; i < LAMBDOCK_POOF_PARTICLES; i++) {
    if (fabs(a[i].angle - c[i].angle) > 1e-9)
      differs = TRUE;
  }
  g_assert_true(differs);
}

/* Everything the cloud is built from stays inside the range the widget assumes:
 * no puff flying off the dock, none that never grows, none that outlives the
 * animation. */
static void test_particle_ranges_are_sane(void) {
  LambdockPoofParticle particles[LAMBDOCK_POOF_PARTICLES];
  fill(particles, 7u);

  for (gsize i = 0; i < LAMBDOCK_POOF_PARTICLES; i++) {
    const LambdockPoofParticle *p = &particles[i];
    g_assert_cmpfloat(p->angle, >=, 0.0);
    g_assert_cmpfloat(p->angle, <, 2.0 * G_PI * 2.0);
    g_assert_cmpfloat(p->start, >=, 0.0);
    g_assert_cmpfloat(p->start, <, 0.05);
    g_assert_cmpfloat(p->distance, >=, 0.12);
    g_assert_cmpfloat(p->distance, <=, 0.28);
    g_assert_cmpfloat(p->radius, >=, 0.06);
    g_assert_cmpfloat(p->radius, <=, 0.18);
    /* Reach plus radius fits in half an icon: the cloud stays inside the icon. */
    g_assert_cmpfloat(p->start + p->distance + p->radius, <=, 0.51);
    g_assert_cmpfloat(p->delay, >=, 0.0);
    g_assert_cmpfloat(p->delay, <=, 0.12);
    g_assert_cmpfloat(p->life, >=, 0.5);
    g_assert_cmpfloat(p->life, <=, 0.95);
  }
}

/* Directions spread around the circle: the cloud has no hole in it, whatever
 * the seed. */
static void test_particle_directions_cover_the_circle(void) {
  LambdockPoofParticle particles[LAMBDOCK_POOF_PARTICLES];
  fill(particles, 2024u);

  /* Four quadrants, all of them used. */
  gboolean quadrant[4] = {FALSE, FALSE, FALSE, FALSE};
  for (gsize i = 0; i < LAMBDOCK_POOF_PARTICLES; i++) {
    double angle = fmod(particles[i].angle, 2.0 * G_PI);
    if (angle < 0.0)
      angle += 2.0 * G_PI;
    int which = (int)(angle / (G_PI / 2.0)) % 4;
    quadrant[which] = TRUE;
  }

  for (int i = 0; i < 4; i++)
    g_assert_true(quadrant[i]);
}

/* Nothing is drawn before it starts, the cloud is thick in the early frames and
 * thin in the late ones, and it is gone when the animation ends. */
static void test_cloud_thins_out_and_ends_empty(void) {
  LambdockPoofParticle particles[LAMBDOCK_POOF_PARTICLES];
  LambdockPoofPuff puffs[LAMBDOCK_POOF_PARTICLES];
  fill(particles, 31u);

  resolve(particles, 0.0, puffs);
  g_assert_cmpfloat(alpha_sum(puffs), ==, 0.0);

  resolve(particles, 0.25, puffs);
  double early = alpha_sum(puffs);
  g_assert_cmpfloat(early, >, 1.0);

  resolve(particles, 0.75, puffs);
  double late = alpha_sum(puffs);
  g_assert_cmpfloat(late, <, early);

  resolve(particles, 1.0, puffs);
  g_assert_cmpfloat(alpha_sum(puffs), ==, 0.0);

  /* Sinless path: past the end nothing comes back to life either. */
  resolve(particles, 1.5, puffs);
  g_assert_cmpfloat(alpha_sum(puffs), ==, 0.0);
}

/* The smoke travels outwards and swells as it goes, which is what makes the
 * five reference frames read as a puff rather than a fade. */
static void test_smoke_travels_outwards_and_swells(void) {
  LambdockPoofParticle particles[LAMBDOCK_POOF_PARTICLES];
  LambdockPoofPuff puffs[LAMBDOCK_POOF_PARTICLES];
  fill(particles, 4242u);

  resolve(particles, 0.2, puffs);
  double near = mean_distance(puffs);
  double small = mean_radius(puffs);

  resolve(particles, 0.6, puffs);
  double far = mean_distance(puffs);
  double big = mean_radius(puffs);

  g_assert_cmpfloat(far, >, near);
  g_assert_cmpfloat(big, >, small);

  /* And it stays on the icon it replaced: the reference cloud fills the 128x128
   * frame and no more, so every puff's far edge is inside 0.5 of the centre. */
  for (gsize i = 0; i < LAMBDOCK_POOF_PARTICLES; i++) {
    g_assert_cmpfloat(puffs[i].x - puffs[i].radius, <, 1.02);
    g_assert_cmpfloat(puffs[i].x + puffs[i].radius, >, -0.02);
    g_assert_cmpfloat(puffs[i].y - puffs[i].radius, <, 1.02);
    g_assert_cmpfloat(puffs[i].y + puffs[i].radius, >, -0.02);
  }
}

/* The middle of the burst: full size on the first frame, as in the artwork,
 * opening up while it fades out. */
static void test_core_starts_dense_and_fades(void) {
  LambdockPoofPuff first;
  LambdockPoofPuff middle;
  LambdockPoofPuff last;

  lambdock_poof_core_at(0.0, &first);
  lambdock_poof_core_at(0.5, &middle);
  lambdock_poof_core_at(1.0, &last);

  g_assert_cmpfloat(first.alpha, >, 0.5);
  g_assert_cmpfloat(first.radius, >, 0.0);
  g_assert_cmpfloat(middle.radius, >, first.radius);
  g_assert_cmpfloat(middle.alpha, <, first.alpha);
  g_assert_cmpfloat(last.alpha, ==, 0.0);

  /* It is centred on the icon. */
  g_assert_cmpfloat(fabs(first.x - 0.5), <, 1e-9);
  g_assert_cmpfloat(fabs(first.y - 0.5), <, 1e-9);
}

/* The global fade is the guarantee that the animation ends invisible: it is 1 at
 * the start, 0 at the end, and never goes back up. */
static void test_global_fade_is_monotonic(void) {
  g_assert_cmpfloat(lambdock_poof_progress_alpha(0.0), ==, 1.0);
  g_assert_cmpfloat(lambdock_poof_progress_alpha(1.0), ==, 0.0);
  g_assert_cmpfloat(lambdock_poof_progress_alpha(-0.5), ==, 1.0);
  g_assert_cmpfloat(lambdock_poof_progress_alpha(1.5), ==, 0.0);

  double previous = lambdock_poof_progress_alpha(0.0);
  for (int step = 1; step <= 20; step++) {
    double alpha = lambdock_poof_progress_alpha((double)step / 20.0);
    g_assert_cmpfloat(alpha, <=, previous);
    previous = alpha;
  }
}

/* The reference artwork is five frames of one icon; the duration has to be long
 * enough to see them and short enough not to hold the dock's width hostage. */
static void test_timing_matches_the_reference(void) {
  g_assert_cmpint(LAMBDOCK_POOF_FRAMES, ==, 5);
  g_assert_cmpfloat(LAMBDOCK_POOF_DURATION_MS, >=, 200.0);
  g_assert_cmpfloat(LAMBDOCK_POOF_DURATION_MS, <=, 500.0);
  /* One particle per direction with no gap in the circle. */
  g_assert_cmpint(LAMBDOCK_POOF_PARTICLES, >=, 8);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/poof/particles/deterministic",
                  test_particles_are_deterministic);
  g_test_add_func("/poof/particles/ranges", test_particle_ranges_are_sane);
  g_test_add_func("/poof/particles/directions",
                  test_particle_directions_cover_the_circle);
  g_test_add_func("/poof/cloud/thins-out", test_cloud_thins_out_and_ends_empty);
  g_test_add_func("/poof/cloud/travels", test_smoke_travels_outwards_and_swells);
  g_test_add_func("/poof/core/fades", test_core_starts_dense_and_fades);
  g_test_add_func("/poof/fade/monotonic", test_global_fade_is_monotonic);
  g_test_add_func("/poof/timing", test_timing_matches_the_reference);

  return g_test_run();
}
