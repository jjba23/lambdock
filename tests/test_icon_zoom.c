// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/ui/icon_zoom.h"
#include <gtk/gtk.h>

#define BASE_SIZE 48
#define MAX_SIZE 70 /* ceil(48 * 1.45), what the dock asks for */

/* ============================================================================
 * HELPERS
 * ============================================================================ */

static GtkWidget *make_zoom(void) {
  GtkWidget *image = gtk_image_new_from_icon_name("application-x-executable");
  gtk_image_set_pixel_size(GTK_IMAGE(image), BASE_SIZE);
  return lambdock_icon_zoom_new(image, BASE_SIZE, MAX_SIZE);
}

static int measure_side(GtkWidget *widget, GtkOrientation orientation) {
  int minimum = 0;
  int natural = 0;
  gtk_widget_measure(widget, orientation, -1, &minimum, &natural, NULL, NULL);
  return natural;
}

/* ============================================================================
 * SCALE -> SIZE
 * ============================================================================ */

static void test_scale_and_target(void) {
  GtkWidget *zoom = make_zoom();

  /* At rest the widget asks for exactly the icon size the dock configured. */
  g_assert_cmpfloat(lambdock_icon_zoom_get_scale(zoom), ==, 1.0);
  g_assert_cmpint(lambdock_icon_zoom_target_size(zoom), ==, BASE_SIZE);

  /* Magnification rounds like the layout does, and the scale itself is kept as
   * a double so the painting can be continuous. */
  lambdock_icon_zoom_set_scale(zoom, 1.45);
  g_assert_cmpint(lambdock_icon_zoom_target_size(zoom), ==, MAX_SIZE);
  g_assert_cmpfloat(lambdock_icon_zoom_get_scale(zoom), >, 1.449);
  g_assert_cmpfloat(lambdock_icon_zoom_get_scale(zoom), <, 1.451);

  lambdock_icon_zoom_set_scale(zoom, 1.2);
  g_assert_cmpint(lambdock_icon_zoom_target_size(zoom), ==, 58); /* 57.6 */

  /* The dock never shrinks an icon below its resting size. */
  lambdock_icon_zoom_set_scale(zoom, 0.5);
  g_assert_cmpfloat(lambdock_icon_zoom_get_scale(zoom), ==, 1.0);
  g_assert_cmpint(lambdock_icon_zoom_target_size(zoom), ==, BASE_SIZE);

  /* Nor can a nonsense value reach the layout. */
  lambdock_icon_zoom_set_scale(zoom, -3.0);
  g_assert_cmpint(lambdock_icon_zoom_target_size(zoom), ==, BASE_SIZE);
  lambdock_icon_zoom_set_scale(zoom, 0.0 / 0.0); /* NaN */
  g_assert_cmpint(lambdock_icon_zoom_target_size(zoom), ==, BASE_SIZE);

  g_object_ref_sink(zoom);
  g_object_unref(zoom);
}

static void test_measure_follows_scale(void) {
  GtkWidget *zoom = make_zoom();

  /* Both axes report the magnified size: the dock's buttons grow in every
   * direction, which is what spreads the row of icons apart. */
  g_assert_cmpint(measure_side(zoom, GTK_ORIENTATION_HORIZONTAL), ==, BASE_SIZE);
  g_assert_cmpint(measure_side(zoom, GTK_ORIENTATION_VERTICAL), ==, BASE_SIZE);

  lambdock_icon_zoom_set_scale(zoom, 1.45);
  g_assert_cmpint(measure_side(zoom, GTK_ORIENTATION_HORIZONTAL), ==, MAX_SIZE);
  g_assert_cmpint(measure_side(zoom, GTK_ORIENTATION_VERTICAL), ==, MAX_SIZE);

  lambdock_icon_zoom_set_scale(zoom, 1.0);
  g_assert_cmpint(measure_side(zoom, GTK_ORIENTATION_HORIZONTAL), ==, BASE_SIZE);

  g_object_ref_sink(zoom);
  g_object_unref(zoom);
}

/* ============================================================================
 * THE RASTER THE ARTWORK IS HELD AT
 * ============================================================================ */

static int child_pixel_size(GtkWidget *zoom) {
  GtkWidget *child = lambdock_icon_zoom_get_child(zoom);
  g_assert_true(GTK_IS_IMAGE(child));
  return gtk_image_get_pixel_size(GTK_IMAGE(child));
}

static void test_raster_follows_magnification(void) {
  GtkWidget *zoom = make_zoom();

  /* Resting: the icon is resolved at exactly the size it is drawn, so the dock
   * looks the same as it did before this widget existed. */
  g_assert_cmpint(child_pixel_size(zoom), ==, BASE_SIZE);

  /* Magnified far enough to matter: the artwork is re-resolved once at its
   * largest size, and drawn at that size rather than upscaled. */
  lambdock_icon_zoom_set_scale(zoom, 1.45);
  g_assert_cmpint(child_pixel_size(zoom), ==, MAX_SIZE);

  /* Back at rest, and the resting state is crisp again. */
  lambdock_icon_zoom_set_scale(zoom, 1.0);
  g_assert_cmpint(child_pixel_size(zoom), ==, BASE_SIZE);

  g_object_ref_sink(zoom);
  g_object_unref(zoom);
}

static void test_raster_hysteresis(void) {
  GtkWidget *zoom = make_zoom();

  /* Coming up from rest, the switch waits until the upscaled resting raster
   * would be visibly soft... */
  lambdock_icon_zoom_set_scale(zoom, 1.07);
  g_assert_cmpint(child_pixel_size(zoom), ==, BASE_SIZE);
  lambdock_icon_zoom_set_scale(zoom, 1.11);
  g_assert_cmpint(child_pixel_size(zoom), ==, MAX_SIZE);

  /* ...and once holding the large one, it keeps it a little longer. A pointer
   * hovering on the threshold must not re-resolve the artwork every frame. */
  lambdock_icon_zoom_set_scale(zoom, 1.07);
  g_assert_cmpint(child_pixel_size(zoom), ==, MAX_SIZE);
  lambdock_icon_zoom_set_scale(zoom, 1.04);
  g_assert_cmpint(child_pixel_size(zoom), ==, BASE_SIZE);

  g_object_ref_sink(zoom);
  g_object_unref(zoom);
}

/* ============================================================================
 * PAINTED GEOMETRY
 * ============================================================================ */

/* Draws WIDGET through a snapshot and returns the painted bounds, which is where
 * a wrong transform shows up: the icon must cover exactly `target` pixels and sit
 * centred in its allocation. */
/* Spins the main context until the widget's pending allocation has landed.
 *
 * Snapshotting a widget between a resize and its allocation is a GTK warning
 * ("without a current allocation"), and on a machine that is busy with other
 * work that warning arrives often enough to make this test flaky. Waiting for
 * two quiet frames costs nothing and makes the snapshot deterministic. */
static void settle(void) {
  for (int i = 0; i < 4; i++) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
  }
}

static graphene_rect_t snapshot_bounds(GtkWidget *parent, GtkWidget *child) {
  settle();

  GtkSnapshot *snapshot = gtk_snapshot_new();
  gtk_widget_snapshot_child(parent, child, snapshot);
  GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);

  graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, 0, 0);
  if (node) {
    gsk_render_node_get_bounds(node, &bounds);
    gsk_render_node_unref(node);
  }
  return bounds;
}

static void test_painted_size(void) {
  GtkWidget *window = gtk_window_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *zoom = make_zoom();
  gtk_box_append(GTK_BOX(box), zoom);
  gtk_window_set_child(GTK_WINDOW(window), box);
  gtk_window_set_default_size(GTK_WINDOW(window), 200, 200);
  gtk_window_present(GTK_WINDOW(window));

  /* Let GTK lay the window out; a couple of frames is enough. */
  for (int i = 0; i < 400 && gtk_widget_get_width(zoom) <= 0; i++) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
  }

  if (gtk_widget_get_width(zoom) <= 0) {
    g_test_skip("window was never allocated");
    gtk_window_destroy(GTK_WINDOW(window));
    return;
  }

  g_assert_cmpint(gtk_widget_get_width(zoom), ==, BASE_SIZE);

  graphene_rect_t at_rest = snapshot_bounds(box, zoom);
  g_assert_cmpfloat(fabs(at_rest.size.width - BASE_SIZE), <, 1.5);
  g_assert_cmpfloat(fabs(at_rest.size.height - BASE_SIZE), <, 1.5);

  lambdock_icon_zoom_set_scale(zoom, 1.45);
  for (int i = 0; i < 400 && gtk_widget_get_width(zoom) < MAX_SIZE; i++) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
  }

  g_assert_cmpint(gtk_widget_get_width(zoom), ==, MAX_SIZE);

  graphene_rect_t magnified = snapshot_bounds(box, zoom);
  g_assert_cmpfloat(fabs(magnified.size.width - MAX_SIZE), <, 1.5);
  g_assert_cmpfloat(fabs(magnified.size.height - MAX_SIZE), <, 1.5);

  /* It fills its own allocation and grows around its centre, so an item keeps
   * its place in the row rather than sliding as it magnifies. */
  graphene_rect_t allocation = GRAPHENE_RECT_INIT(0, 0, 0, 0);
  g_assert_true(gtk_widget_compute_bounds(zoom, box, &allocation));
  g_assert_cmpfloat(fabs((magnified.origin.x + magnified.size.width / 2.0) -
                         (allocation.origin.x + allocation.size.width / 2.0)),
                    <, 1.5);
  g_assert_cmpfloat(fabs((magnified.origin.y + magnified.size.height / 2.0) -
                         (allocation.origin.y + allocation.size.height / 2.0)),
                    <, 1.5);

  /* At rest too. */
  lambdock_icon_zoom_set_scale(zoom, 1.0);
  for (int i = 0; i < 400 && gtk_widget_get_width(zoom) > BASE_SIZE; i++) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
  }

  graphene_rect_t back = snapshot_bounds(box, zoom);
  g_assert_cmpfloat(fabs(back.size.width - BASE_SIZE), <, 1.5);
  g_assert_true(gtk_widget_compute_bounds(zoom, box, &allocation));
  g_assert_cmpfloat(fabs((back.origin.x + back.size.width / 2.0) -
                         (allocation.origin.x + allocation.size.width / 2.0)),
                    <, 1.5);

  gtk_window_destroy(GTK_WINDOW(window));
}

/* ============================================================================
 * MISUSE
 * ============================================================================ */

static void test_misuse(void) {
  GtkWidget *zoom = make_zoom();

  g_assert_true(lambdock_icon_zoom_is_zoom(zoom));
  g_assert_false(lambdock_icon_zoom_is_zoom(NULL));

  GtkWidget *label = gtk_label_new("not an icon");
  g_assert_false(lambdock_icon_zoom_is_zoom(label));
  g_assert_null(lambdock_icon_zoom_get_child(label));
  g_assert_cmpint(lambdock_icon_zoom_target_size(label), ==, 0);
  g_assert_cmpfloat(lambdock_icon_zoom_get_scale(label), ==, 1.0);
  lambdock_icon_zoom_set_scale(label, 1.4); /* must not crash */
  g_assert_cmpfloat(lambdock_icon_zoom_get_scale(label), ==, 1.0);

  /* A zoom with no icon is useless but must not crash. */
  GtkWidget *empty = lambdock_icon_zoom_new(NULL, BASE_SIZE, MAX_SIZE);
  g_assert_null(lambdock_icon_zoom_get_child(empty));
  lambdock_icon_zoom_set_scale(empty, 1.45);
  g_assert_cmpint(lambdock_icon_zoom_target_size(empty), ==, MAX_SIZE);

  g_object_ref_sink(zoom);
  g_object_unref(zoom);
  g_object_ref_sink(label);
  g_object_unref(label);
  g_object_ref_sink(empty);
  g_object_unref(empty);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  /* Every test here builds widgets, so there is nothing to test without a
   * display: report the Meson "skipped" status like the other UI tests, which is
   * what a package build in a sandbox (no Wayland socket) relies on. */
  if (!gtk_init_check()) {
    g_print("Skipping icon zoom tests: No display server available\n");
    return 77;
  }

  /* The dock runs on the GL renderer (main.c sets it the same way); the
   * geometry test below opens a real window, so it has to render exactly like
   * the dock and not depend on whichever renderer this environment would pick. */
  g_setenv("GSK_RENDERER", "gl", FALSE);

  g_test_add_func("/ui/icon_zoom/scale_and_target", test_scale_and_target);
  g_test_add_func("/ui/icon_zoom/measure_follows_scale",
                  test_measure_follows_scale);
  g_test_add_func("/ui/icon_zoom/raster_follows_magnification",
                  test_raster_follows_magnification);
  g_test_add_func("/ui/icon_zoom/raster_hysteresis", test_raster_hysteresis);
  g_test_add_func("/ui/icon_zoom/painted_size", test_painted_size);
  g_test_add_func("/ui/icon_zoom/misuse", test_misuse);

  return g_test_run();
}
