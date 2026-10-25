// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY ; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock. If not, see <https://www.gnu.org/licenses/>.

#include "../src/dock_window.h"
#include "../src/lambdock.h"
#include "../src/ui/dock_item.h"
#include "../src/ui/shelf.h"
#include <gtk/gtk.h>

#define BAND(ratio, size) ((int)round((size) * (ratio)))

/* The parameter set of the 'big-cat special mode (see scheme presets). */
static LambdockShelfParams big_cat_params(void) {
  LambdockShelfParams p = lambdock_shelf_params_default();
  p.tilt_deg = 14.0;
  p.perspective = 400.0;
  p.cover = 0.58;
  p.floor_px = 8;
  p.padding_px = 0;
  p.reflection_ratio = 0.52;
  p.reflection_intensity = 0.85;
  p.reflection_blur_ratio = 0.035;
  return p;
}

/* ============================================================================
 * DEFAULTS & ACTIVATION
 * ============================================================================ */

static void test_default_params(void) {
  LambdockShelfParams p = lambdock_shelf_params_default();

  g_assert_cmpfloat(p.tilt_deg, ==, 0.0);
  g_assert_cmpfloat(p.perspective, ==, 400.0);
  g_assert_cmpfloat(p.cover, ==, 1.0);
  g_assert_cmpint(p.floor_px, ==, 0);
  g_assert_cmpint(p.padding_px, ==, CAPSULE_PADDING_PX);
  g_assert_cmpfloat(p.reflection_ratio, ==, 0.0);
  g_assert_cmpfloat(p.reflection_intensity, ==, 0.85);
  g_assert_cmpfloat(p.reflection_blur_ratio, ==, 0.035);
}

static void test_shelf_active(void) {
  LambdockState state = {0};

  /* Defaults describe the plain capsule: the shelf is not active. */
  state.shelf = lambdock_shelf_params_default();
  g_assert_false(lambdock_shelf_active(&state));
  g_assert_false(lambdock_shelf_active(NULL));

  /* Any deviation switches the shelf implementation on. */
  state.shelf.tilt_deg = 5.0;
  g_assert_true(lambdock_shelf_active(&state));
  state.shelf = lambdock_shelf_params_default();

  state.shelf.reflection_ratio = 0.1;
  g_assert_true(lambdock_shelf_active(&state));
  state.shelf = lambdock_shelf_params_default();

  state.shelf.floor_px = 4;
  g_assert_true(lambdock_shelf_active(&state));
  state.shelf = lambdock_shelf_params_default();

  state.shelf.cover = 0.9;
  g_assert_true(lambdock_shelf_active(&state));
  state.shelf = lambdock_shelf_params_default();

  state.shelf.padding_px = 20;
  g_assert_true(lambdock_shelf_active(&state));
}

/* ============================================================================
 * TYPE GUARDS
 * ============================================================================ */

static void test_type_guards(void) {
  g_assert_false(shelf_reflection_is_reflection(NULL));
  g_assert_false(shelf_frame_is_frame(NULL));

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  g_assert_false(shelf_reflection_is_reflection(box));
  g_assert_false(shelf_frame_is_frame(box));

  GtkWidget *reflection = shelf_reflection_new(NULL);
  g_assert_true(shelf_reflection_is_reflection(reflection));
  g_assert_false(shelf_frame_is_frame(reflection));

  GtkWidget *frame = shelf_frame_new(box);
  g_assert_true(shelf_frame_is_frame(frame));
  g_assert_false(shelf_reflection_is_reflection(frame));

  /* Setters must ignore NULL and foreign widgets. */
  shelf_reflection_configure(NULL, GTK_LAYER_SHELL_EDGE_BOTTOM, 48, 0.5, 0.03,
                             0.8);
  shelf_reflection_configure(box, GTK_LAYER_SHELL_EDGE_BOTTOM, 48, 0.5, 0.03,
                             0.8);
  shelf_reflection_set_icon_size(NULL, 48);
  shelf_reflection_set_icon_size(box, 48);
  shelf_reflection_set_extent(NULL, 48, 24);
  shelf_reflection_set_extent(box, 48, 24);
  shelf_frame_set_tilt(NULL, GTK_LAYER_SHELL_EDGE_BOTTOM, 12.0, 400.0);
  shelf_frame_set_tilt(box, GTK_LAYER_SHELL_EDGE_BOTTOM, 12.0, 400.0);

  g_object_ref_sink(reflection);
  g_object_unref(reflection);
  /* `frame` owns `box`; dropping the frame frees both. */
  g_object_ref_sink(frame);
  g_object_unref(frame);
}

/* ============================================================================
 * REFLECTION GEOMETRY
 * ============================================================================ */

static void test_reflection_measure(void) {
  GtkWidget *reflection = shelf_reflection_new(NULL);

  /* Nothing configured: the default (disabled) band ratio yields no band. */
  big_cat_params();
  shelf_reflection_configure(reflection, GTK_LAYER_SHELL_EDGE_BOTTOM, 48, 0.52,
                             0.035, 0.85);

  int minimum = -1;
  int natural = -1;
  gtk_widget_measure(reflection, GTK_ORIENTATION_HORIZONTAL, -1, &minimum,
                     &natural, NULL, NULL);
  g_assert_cmpint(minimum, ==, 0);
  g_assert_cmpint(natural, ==, 48);

  gtk_widget_measure(reflection, GTK_ORIENTATION_VERTICAL, -1, &minimum,
                     &natural, NULL, NULL);
  g_assert_cmpint(minimum, ==, 0);
  g_assert_cmpint(natural, ==, BAND(0.52, 48));

  /* A different icon size rescales the band. */
  shelf_reflection_configure(reflection, GTK_LAYER_SHELL_EDGE_BOTTOM, 64, 0.52,
                             0.035, 0.85);
  gtk_widget_measure(reflection, GTK_ORIENTATION_VERTICAL, -1, &minimum,
                     &natural, NULL, NULL);
  g_assert_cmpint(natural, ==, BAND(0.52, 64));

  /* A different band ratio too. */
  shelf_reflection_configure(reflection, GTK_LAYER_SHELL_EDGE_BOTTOM, 64, 0.25,
                             0.035, 0.85);
  gtk_widget_measure(reflection, GTK_ORIENTATION_VERTICAL, -1, &minimum,
                     &natural, NULL, NULL);
  g_assert_cmpint(natural, ==, BAND(0.25, 64));

  g_object_ref_sink(reflection);
  g_object_unref(reflection);
}

static void test_reflection_size_request_per_edge(void) {
  GtkWidget *reflection = shelf_reflection_new(NULL);

  /* A bottom/top dock stacks the reflection vertically: icon-wide, band-tall. */
  shelf_reflection_configure(reflection, GTK_LAYER_SHELL_EDGE_BOTTOM, 48, 0.52,
                             0.035, 0.85);
  shelf_reflection_set_icon_size(reflection, 48);

  int width = 0;
  int height = 0;
  gtk_widget_get_size_request(reflection, &width, &height);
  g_assert_cmpint(width, ==, 48);
  g_assert_cmpint(height, ==, BAND(0.52, 48));

  /* A side dock mirrors sideways: band-wide, icon-tall. */
  shelf_reflection_configure(reflection, GTK_LAYER_SHELL_EDGE_RIGHT, 48, 0.52,
                             0.035, 0.85);
  shelf_reflection_set_icon_size(reflection, 48);

  gtk_widget_get_size_request(reflection, &width, &height);
  g_assert_cmpint(width, ==, BAND(0.52, 48));
  g_assert_cmpint(height, ==, 48);

  /* Invalid sizes are ignored rather than clearing the request. */
  shelf_reflection_set_icon_size(reflection, 0);
  gtk_widget_get_size_request(reflection, &width, &height);
  g_assert_cmpint(width, ==, BAND(0.52, 48));

  /* A fixed band thickness keeps the row stable while the icon magnifies. */
  shelf_reflection_configure(reflection, GTK_LAYER_SHELL_EDGE_BOTTOM, 48, 0.52,
                             0.035, 0.85);
  shelf_reflection_set_extent(reflection, 70, BAND(0.52, 48));
  gtk_widget_get_size_request(reflection, &width, &height);
  g_assert_cmpint(width, ==, 70);
  g_assert_cmpint(height, ==, BAND(0.52, 48));

  g_object_ref_sink(reflection);
  g_object_unref(reflection);
}

/* ============================================================================
 * FRAME WRAPPER
 * ============================================================================ */

static void test_frame_measure_delegates_to_child(void) {
  GtkWidget *child = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request(child, 120, 40);

  GtkWidget *frame = shelf_frame_new(child);

  int minimum = -1;
  int natural = -1;
  gtk_widget_measure(frame, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural,
                     NULL, NULL);
  g_assert_cmpint(natural, ==, 120);

  gtk_widget_measure(frame, GTK_ORIENTATION_VERTICAL, -1, &minimum, &natural,
                     NULL, NULL);
  g_assert_cmpint(natural, ==, 40);

  /* The tilt only affects painting, never the geometry. */
  shelf_frame_set_tilt(frame, GTK_LAYER_SHELL_EDGE_BOTTOM, 14.0, 400.0);
  gtk_widget_measure(frame, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural,
                     NULL, NULL);
  g_assert_cmpint(natural, ==, 120);

  g_object_ref_sink(frame);
  g_object_unref(frame);
}

static void test_frame_without_child(void) {
  GtkWidget *frame = shelf_frame_new(NULL);

  int minimum = -1;
  int natural = -1;
  gtk_widget_measure(frame, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural,
                     NULL, NULL);
  g_assert_cmpint(minimum, ==, 0);
  g_assert_cmpint(natural, ==, 0);

  g_object_ref_sink(frame);
  g_object_unref(frame);
}

/* ============================================================================
 * ITEM CONTAINER ORDERING
 * ============================================================================ */

static void test_wrap_item_ordering(void) {
  GtkWidget *reflection = shelf_reflection_new(NULL);
  GtkWidget *button = gtk_button_new();

  LambdockState bottom = {0};
  bottom.orientation = GTK_ORIENTATION_HORIZONTAL;
  bottom.edge = GTK_LAYER_SHELL_EDGE_BOTTOM;

  GtkWidget *container = shelf_wrap_item(&bottom, button, reflection);
  g_assert_nonnull(container);
  g_assert_true(gtk_widget_has_css_class(container, "shelf-item"));
  /* Bottom dock: icon first, reflection underneath it. */
  g_assert_true(gtk_widget_get_first_child(container) == button);
  g_assert_true(gtk_widget_get_last_child(container) == reflection);

  /* Top dock: the reflection goes above the icon. */
  GtkWidget *top_button = gtk_button_new();
  GtkWidget *top_reflection = shelf_reflection_new(NULL);
  LambdockState top = {0};
  top.orientation = GTK_ORIENTATION_HORIZONTAL;
  top.edge = GTK_LAYER_SHELL_EDGE_TOP;

  GtkWidget *top_container = shelf_wrap_item(&top, top_button, top_reflection);
  g_assert_true(gtk_widget_get_first_child(top_container) == top_reflection);
  g_assert_true(gtk_widget_get_last_child(top_container) == top_button);

  /* A left dock stacks the items horizontally (reflection to the right). */
  GtkWidget *left_button = gtk_button_new();
  GtkWidget *left_reflection = shelf_reflection_new(NULL);
  LambdockState left = {0};
  left.orientation = GTK_ORIENTATION_VERTICAL;
  left.edge = GTK_LAYER_SHELL_EDGE_LEFT;

  GtkWidget *left_container =
      shelf_wrap_item(&left, left_button, left_reflection);
  g_assert_cmpint(gtk_orientable_get_orientation(GTK_ORIENTABLE(left_container)),
                  ==, GTK_ORIENTATION_HORIZONTAL);
  g_assert_true(gtk_widget_get_last_child(left_container) == left_reflection);

  /* No button means no container. */
  g_assert_null(shelf_wrap_item(&bottom, NULL, reflection));

  g_object_ref_sink(container);
  g_object_unref(container);
  g_object_ref_sink(top_container);
  g_object_unref(top_container);
  g_object_ref_sink(left_container);
  g_object_unref(left_container);
}

/* ============================================================================
 * PARAMETER-DRIVEN LAYOUT
 * ============================================================================ */

static void test_apply_shelf_layout_toggles_visibility(void) {
  LambdockState state = {0};
  state.icon_size = 48;
  state.edge = GTK_LAYER_SHELL_EDGE_BOTTOM;
  state.orientation = GTK_ORIENTATION_HORIZONTAL;
  state.shelf = lambdock_shelf_params_default();

  DockItem *item = g_new0(DockItem, 1);
  item->state = &state;
  item->button = gtk_button_new();
  item->reflection = shelf_reflection_new(NULL);
  item->container = shelf_wrap_item(&state, item->button, item->reflection);
  state.dock_items = g_list_append(state.dock_items, item);

  /* Reflection ratio 0 (the default): nothing is drawn. */
  apply_shelf_layout(&state);
  g_assert_false(gtk_widget_get_visible(item->reflection));

  /* The 'big-cat preset turns the reflections on, sized from the icon. */
  state.shelf = big_cat_params();
  apply_shelf_layout(&state);
  g_assert_true(gtk_widget_get_visible(item->reflection));

  int width = 0;
  int height = 0;
  gtk_widget_get_size_request(item->reflection, &width, &height);
  g_assert_cmpint(width, ==, 48);
  g_assert_cmpint(height, ==, BAND(0.52, 48));

  /* A different ratio resizes the band without touching the icon size. */
  state.shelf.reflection_ratio = 0.25;
  apply_shelf_layout(&state);
  gtk_widget_get_size_request(item->reflection, &width, &height);
  g_assert_cmpint(height, ==, BAND(0.25, 48));

  /* Turning reflections off hides them again. */
  state.shelf.reflection_ratio = 0.0;
  apply_shelf_layout(&state);
  g_assert_false(gtk_widget_get_visible(item->reflection));

  /* NULL / empty states are tolerated. */
  apply_shelf_layout(NULL);
  LambdockState empty = {0};
  apply_shelf_layout(&empty);

  state.dock_items = g_list_remove(state.dock_items, item);
  g_object_ref_sink(item->container);
  g_object_unref(item->container);
  g_free(item);
}

static void test_cross_dim_helpers(void) {
  LambdockState state = {0};
  state.icon_size = 48;
  state.shelf = lambdock_shelf_params_default();

  /* Defaults keep the historic capsule/overlay dimensions. */
  g_assert_cmpint(dock_capsule_cross_dim(&state), ==, 48 + CAPSULE_PADDING_PX);
  g_assert_cmpint(dock_cross_axis_dim(&state), ==,
                  (int)(48 * CROSS_DIM_SCALE_FACTOR) + CROSS_DIM_PADDING_PX);

  /* The 'big-cat preset: glass covers 58% of the icon, plus the reflection
   * band, the glass floor and no extra padding. */
  state.shelf = big_cat_params();

  int band = BAND(0.52, 48);
  int expected_capsule = (int)round(48 * 0.58) + band + 8 + 0;
  g_assert_cmpint(dock_capsule_cross_dim(&state), ==, expected_capsule);
  g_assert_cmpint(dock_capsule_cross_dim(&state), ==, 61);

  g_assert_cmpint(dock_cross_axis_dim(&state), ==,
                  (int)(48 * CROSS_DIM_SCALE_FACTOR) + CROSS_DIM_PADDING_PX +
                      band + 8);

  /* The icons stick out of the glass: the covered part is less than an icon. */
  g_assert_cmpint((int)round(48 * state.shelf.cover), <, 48);

  /* Every parameter is honoured. */
  state.shelf.reflection_ratio = 0.0;
  state.shelf.floor_px = 0;
  state.shelf.cover = 0.5;
  state.shelf.padding_px = 10;
  g_assert_cmpint(dock_capsule_cross_dim(&state), ==, 24 + 0 + 0 + 10);

  /* NULL and zero-sized states fall back to the default parameters. */
  g_assert_cmpint(dock_capsule_cross_dim(NULL), ==,
                  DEFAULT_ICON_SIZE + CAPSULE_PADDING_PX);
  LambdockState zero = {0};
  g_assert_cmpint(dock_capsule_cross_dim(&zero), ==,
                  DEFAULT_ICON_SIZE + CAPSULE_PADDING_PX);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  if (!gtk_init_check()) {
    g_print("Skipping shelf tests: No display server available\n");
    return 77;
  }

  g_test_add_func("/ui/shelf/default_params", test_default_params);
  g_test_add_func("/ui/shelf/shelf_active", test_shelf_active);
  g_test_add_func("/ui/shelf/type_guards", test_type_guards);
  g_test_add_func("/ui/shelf/reflection_measure", test_reflection_measure);
  g_test_add_func("/ui/shelf/reflection_size_request",
                  test_reflection_size_request_per_edge);
  g_test_add_func("/ui/shelf/frame_measure", test_frame_measure_delegates_to_child);
  g_test_add_func("/ui/shelf/frame_without_child", test_frame_without_child);
  g_test_add_func("/ui/shelf/wrap_item_ordering", test_wrap_item_ordering);
  g_test_add_func("/ui/shelf/layout_toggle",
                  test_apply_shelf_layout_toggles_visibility);
  g_test_add_func("/ui/shelf/cross_dim_helpers", test_cross_dim_helpers);

  return g_test_run();
}
