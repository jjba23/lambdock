// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

/* The dock's per-frame budget, asserted instead of eyeballed.
 *
 * A picture cannot answer "did hovering relayout the row?", "did the surface
 * keep its size?" or "how many draws is one reflection?", and those are the
 * questions that decide whether the dock is cheap to run. So this test builds a
 * real row of dock items in a real (offscreen) window, drives the hover path the
 * way the pointer does, and checks the counters the frame paths keep.
 *
 * The numbers are also printed, so a regression shows up as a number in the test
 * log even before it fails an assertion.
 */

#include "../src/core/render_stats.h"
#include "../src/dock_window.h"
#include "../src/lambdock.h"
#include "../src/ui/dock_item.h"
#include "../src/ui/shelf.h"
#include <gtk/gtk.h>

#define ITEMS 8
#define BASE_ICON 48

/* A row of app items, allocated by hand.
 *
 * No window on purpose: the hover path only needs the row's geometry, and a
 * test that maps a surface would be exercising the developer's compositor (and
 * flashing a window) instead of the dock's arithmetic. gtk_widget_allocate()
 * runs the real measure/allocate chain, so the bounds the fisheye reads are the
 * ones the dock would have.
 */
static GtkWidget *build_row(LambdockState *state, int width, int height) {
  *state = (LambdockState){0};
  state->orientation = GTK_ORIENTATION_HORIZONTAL;
  state->edge = GTK_LAYER_SHELL_EDGE_BOTTOM;
  state->icon_size = BASE_ICON;
  state->capsule_padding = 12;
  state->item_spacing = 4;
  state->hover_animate = TRUE;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, state->item_spacing);
  g_object_ref_sink(box);
  state->box = box;

  for (int i = 0; i < ITEMS; i++) {
    g_autofree char *name = g_strdup_printf("Item %d", i);
    create_dock_item(state, name, "utilities-terminal", "true", name, TRUE,
                     TRUE);
  }

  gtk_widget_allocate(box, width, height, -1, NULL);

  return box;
}

static void settle(int frames) {
  for (int i = 0; i < frames; i++) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(2000);
  }
}

/* ============================================================================
 * THE HOVER PATH
 * ============================================================================ */

static void test_hover_keeps_the_surface_and_reuses_geometry(void) {
  /* The fish-eye only runs with animations enabled; headless sessions default
   * the setting off. */
  GtkSettings *settings = gtk_settings_get_default();
  gboolean previous = TRUE;
  if (settings) {
    g_object_get(settings, "gtk-enable-animations", &previous, NULL);
    g_object_set(settings, "gtk-enable-animations", TRUE, NULL);
  }

  LambdockState state;
  GtkWidget *box = build_row(&state, 900, 120);
  settle(4);

  g_assert_cmpint(g_list_length(state.dock_items), ==, ITEMS);

  int width = gtk_widget_get_width(box);
  g_assert_cmpint(width, >, 0);

  lambdock_stats_reset();

  /* Sweep the pointer across the row the way a hand does: 40 frames, moving. */
  const int frames = 40;
  for (int i = 0; i < frames; i++) {
    double x = (double)width * (double)i / (double)(frames - 1);
    dock_apply_hover_at(&state, x, gtk_widget_get_height(box) / 2.0);
    settle(2);
  }

  g_autofree char *numbers = lambdock_stats_describe();
  g_test_message("hover sweep over %d items: %s", ITEMS, numbers);

  guint64 frames_measured = lambdock_stats_get(LAMBDOCK_STAT_HOVER_FRAMES);
  guint64 ours_us = lambdock_stats_get(LAMBDOCK_STAT_HOVER_US);
  g_test_message("our own work per hover frame: %.1f us (%.0f%% of a 16.7 ms "
                 "frame)",
                 (double)ours_us / (double)MAX(frames_measured, 1),
                 100.0 * (double)ours_us / (double)MAX(frames_measured, 1) /
                     16666.0);

  /* The dock's own arithmetic has to be a rounding error next to the frame it
   * happens in: the row's layout and repaint are GTK's, and if our share is
   * small then that -- not this -- is where the time goes. */
  g_assert_cmpuint(ours_us / MAX(frames_measured, 1), <, 2000);

  /* Something did happen: the icons were magnified. */
  g_assert_cmpuint(lambdock_stats_get(LAMBDOCK_STAT_HOVER_FRAMES), ==, frames);
  g_assert_cmpuint(lambdock_stats_get(LAMBDOCK_STAT_LAYOUT_STEPS), >, 0);

  /* ... but the *surface* never changed size: growing it per frame is a buffer
   * re-allocation and a compositor reconfigure per frame. */
  g_assert_cmpuint(lambdock_stats_get(LAMBDOCK_STAT_SURFACE_RESIZES), ==, 0);
  g_assert_cmpint(gtk_widget_get_width(box), ==, width);

  /* The geometry is rebuilt only when a layout step moved an item, not on every
   * pointer frame: this is what the cache is for. */
  guint64 refreshes = lambdock_stats_get(LAMBDOCK_STAT_GEOMETRY_REFRESHES);
  guint64 steps = lambdock_stats_get(LAMBDOCK_STAT_LAYOUT_STEPS);
  g_assert_cmpuint(refreshes, <=, steps + 1);

  /* And it is not rebuilt more often than there are frames, ever. */
  g_assert_cmpuint(refreshes, <=, frames);

  clear_favorite_dock_items(&state);
  g_object_unref(box);
  if (settings)
    g_object_set(settings, "gtk-enable-animations", previous, NULL);
}

static void test_hover_is_skipped_while_hidden(void) {
  GtkSettings *settings = gtk_settings_get_default();
  gboolean previous = TRUE;
  if (settings) {
    g_object_get(settings, "gtk-enable-animations", &previous, NULL);
    g_object_set(settings, "gtk-enable-animations", TRUE, NULL);
  }

  LambdockState state;
  GtkWidget *box = build_row(&state, 900, 120);
  settle(4);

  lambdock_stats_reset();

  /* A dock that is tucked away must not magnify: its geometry is off screen. */
  state.is_hidden = TRUE;
  dock_apply_hover_at(&state, 100, 10);
  g_assert_cmpuint(lambdock_stats_get(LAMBDOCK_STAT_HOVER_FRAMES), ==, 0);
  g_assert_cmpuint(lambdock_stats_get(LAMBDOCK_STAT_LAYOUT_STEPS), ==, 0);

  /* Nor while it slides. */
  state.is_hidden = FALSE;
  state.current_margin = -60;
  dock_apply_hover_at(&state, 100, 10);
  g_assert_cmpuint(lambdock_stats_get(LAMBDOCK_STAT_HOVER_FRAMES), ==, 0);

  clear_favorite_dock_items(&state);
  g_object_unref(box);
  if (settings)
    g_object_set(settings, "gtk-enable-animations", previous, NULL);
}

static void test_subpixel_jitter_is_dropped(void) {
  GtkSettings *settings = gtk_settings_get_default();
  gboolean previous = TRUE;
  if (settings) {
    g_object_get(settings, "gtk-enable-animations", &previous, NULL);
    g_object_set(settings, "gtk-enable-animations", TRUE, NULL);
  }

  LambdockState state;
  GtkWidget *box = build_row(&state, 900, 120);
  settle(4);

  /* Land one frame to set the reference position. */
  dock_apply_hover_at(&state, 450.0, 60.0);
  lambdock_stats_reset();

  /* A trackpad then reports sub-pixel jitter: 20 frames that cannot change what
   * is painted must not touch a widget. */
  for (int i = 0; i < 20; i++)
    dock_apply_hover_at(&state, 450.0 + (i % 2 ? 0.3 : -0.3), 60.0);

  g_autofree char *numbers = lambdock_stats_describe();
  g_test_message("20 sub-pixel frames: %s", numbers);

  g_assert_cmpuint(lambdock_stats_get(LAMBDOCK_STAT_HOVER_FRAMES), ==, 20);
  g_assert_cmpuint(lambdock_stats_get(LAMBDOCK_STAT_HOVER_SKIPS), ==, 20);
  g_assert_cmpuint(lambdock_stats_get(LAMBDOCK_STAT_LAYOUT_STEPS), ==, 0);
  g_assert_cmpuint(lambdock_stats_get(LAMBDOCK_STAT_GEOMETRY_REFRESHES), ==, 0);

  /* A real move is applied again. */
  dock_apply_hover_at(&state, 500.0, 60.0);
  g_assert_cmpuint(lambdock_stats_get(LAMBDOCK_STAT_HOVER_SKIPS), ==, 20);

  clear_favorite_dock_items(&state);
  g_object_unref(box);
  if (settings)
    g_object_set(settings, "gtk-enable-animations", previous, NULL);
}

/* ============================================================================
 * THE HOVER PATH: A SWEEP HAS TO BE MONOTONE
 * ============================================================================ */

/* An icon's magnification is a function of where the pointer is, and the pointer
 * only moves one way during a sweep -- so an icon near the start of the sweep
 * may only grow as the pointer comes towards it and shrink once the pointer has
 * passed. If it grows, shrinks and grows again the row is chasing its own tail:
 * the sizes come from item positions that the sizes themselves moved, which
 * with a real pointer reads as a dock that wobbles under it.
 *
 * Each frame is one call into the same path the frame clock uses, so this
 * measures the response and not an approximation of it.
 */
static void test_hover_sweep_is_monotone_per_item(void) {
  GtkSettings *settings = gtk_settings_get_default();
  gboolean previous = TRUE;
  if (settings) {
    g_object_get(settings, "gtk-enable-animations", &previous, NULL);
    g_object_set(settings, "gtk-enable-animations", TRUE, NULL);
  }

  LambdockState state;
  GtkWidget *box = build_row(&state, 1400, 120);
  settle(4);

  const int frames = 60;
  int width = gtk_widget_get_width(box);
  GArray *sizes[ITEMS];
  for (int i = 0; i < ITEMS; i++)
    sizes[i] = g_array_new(FALSE, FALSE, sizeof(int));

  for (int f = 0; f < frames; f++) {
    double x = (double)width * (double)f / (double)(frames - 1);
    dock_apply_hover_at(&state, x, gtk_widget_get_height(box) / 2.0);
    settle(1);

    GList *node = state.dock_items;
    for (int i = 0; i < ITEMS && node; i++, node = node->next) {
      DockItem *item = node->data;
      int req = -1;
      gtk_widget_get_size_request(item->button, &req, NULL);
      g_array_append_val(sizes[i], req);
    }
  }

  /* A unimodal sequence has at most one change of direction. Reading the sizes
   * as the y of a curve, count sign changes between consecutive steps, ignoring
   * repeats and steps smaller than the layout granularity (which are not
   * painted differently anyway). */
  int worst_changes = 0;
  for (int i = 0; i < ITEMS; i++) {
    int changes = 0;
    int last_dir = 0;
    for (guint k = 1; k < sizes[i]->len; k++) {
      int a = g_array_index(sizes[i], int, k - 1);
      int b = g_array_index(sizes[i], int, k);
      if (abs(b - a) < FISHEYE_LAYOUT_STEP_PX)
        continue;
      int dir = (b > a) ? 1 : -1;
      if (last_dir != 0 && dir != last_dir)
        changes++;
      last_dir = dir;
    }
    if (changes > worst_changes)
      worst_changes = changes;
    g_test_message("item %d sizes: %d..%d, direction changes: %d", i,
                   g_array_index(sizes[i], int, 0),
                   g_array_index(sizes[i], int, sizes[i]->len - 1), changes);
  }

  for (int i = 0; i < ITEMS; i++)
    g_array_free(sizes[i], TRUE);
  clear_favorite_dock_items(&state);
  g_object_unref(box);
  if (settings)
    g_object_set(settings, "gtk-enable-animations", previous, NULL);

  /* One reversal is the magnification passing over the icon. More than that is
   * the row oscillating. */
  g_assert_cmpint(worst_changes, <=, 1);
}

/* ============================================================================
 * THE PAINT PATH
 * ============================================================================ */

static void test_reflection_draw_budget(void) {
  /* One reflection repaint costs one clipped draw per fade band, so the band
   * count *is* the repaint budget. A 14 px band (the macOS Mavericks shelf at
   * 48 px icons) is 14 draws; the floor keeps a hairline band from banding
   * visibly, and the ceiling is the widget's worst case. */
  g_assert_cmpint(lambdock_reflection_band_count(14), ==, 14);
  g_assert_cmpint(lambdock_reflection_band_count(30), ==,
                  SHELF_REFLECTION_BANDS);
  g_assert_cmpint(lambdock_reflection_band_count(1), ==, 6);
  g_assert_cmpint(lambdock_reflection_band_count(0), ==,
                  SHELF_REFLECTION_BANDS);
  g_assert_cmpint(lambdock_reflection_band_count(-5), ==,
                  SHELF_REFLECTION_BANDS);
  g_assert_cmpint(lambdock_reflection_band_count(1000), ==,
                  SHELF_REFLECTION_BANDS);

  g_test_message("reflection repaint budget: %d draws at a 14 px band, %d at a "
                 "30 px one (SHELF_REFLECTION_BANDS = %d)",
                 lambdock_reflection_band_count(14),
                 lambdock_reflection_band_count(30), SHELF_REFLECTION_BANDS);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);

  /* The counters are plain integers, but the row is built from real dock items,
   * and those resolve icons through the display's icon theme: without a display
   * there is no row to measure. Report the Meson "skipped" status, which is what
   * a package build in a sandbox (no Wayland socket) relies on. */
  if (!gtk_init_check()) {
    g_print("Skipping render budget tests: No display server available\n");
    return 77;
  }

  g_test_add_func("/perf/hover/keeps_surface_and_caches_geometry",
                  test_hover_keeps_the_surface_and_reuses_geometry);
  g_test_add_func("/perf/hover/skips_hidden", test_hover_is_skipped_while_hidden);
  g_test_add_func("/perf/hover/subpixel_jitter", test_subpixel_jitter_is_dropped);
  g_test_add_func("/perf/hover/sweep_is_monotone",
                  test_hover_sweep_is_monotone_per_item);
  g_test_add_func("/perf/paint/reflection_budget",
                  test_reflection_draw_budget);

  return g_test_run();
}
