// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/dock_window.h"
#include "../src/lambdock.h"
#include "../src/ui/dock_item.h"
#include "../src/ui/shelf.h"
#include "../src/ui/shelf_follow.h"
#include <gtk/gtk.h>

#define BAND(ratio, size) ((int)round((size) * (ratio)))
#define ICON 48

/* A "3D shelf" parameter set: icons overhang the glass, reflections in front. */
static LambdockShelfParams shelf_params(void) {
  LambdockShelfParams p = lambdock_shelf_params_default();
  p.tilt_deg = 12.0;
  p.perspective = 400.0;
  p.margin_px = -22;
  p.front_px = 18;
  p.reflection_ratio = 0.5;
  p.reflection_intensity = 0.45;
  p.reflection_blur_ratio = 0.06;
  return p;
}

/* ============================================================================
 * DEFAULTS, ACTIVATION & DERIVED GEOMETRY
 * ============================================================================ */

static void test_default_params(void) {
  LambdockShelfParams p = lambdock_shelf_params_default();

  g_assert_cmpfloat(p.tilt_deg, ==, 0.0);
  g_assert_cmpfloat(p.perspective, ==, 400.0);
  g_assert_cmpint(p.margin_px, ==, CAPSULE_PADDING_PX);
  g_assert_cmpint(p.front_px, ==, 0);
  g_assert_cmpfloat(p.reflection_ratio, ==, 0.0);
  g_assert_cmpfloat(p.reflection_intensity, ==, 0.55);
  g_assert_cmpfloat(p.reflection_blur_ratio, ==, 0.05);
}

static void test_shelf_active(void) {
  LambdockState state = {0};

  /* Defaults describe the plain capsule: the shelf is not active. */
  state.shelf = lambdock_shelf_params_default();
  g_assert_false(lambdock_shelf_active(&state));
  g_assert_false(lambdock_shelf_active(NULL));

  /* Any geometric deviation switches the shelf implementation on. */
  state.shelf.tilt_deg = 5.0;
  g_assert_true(lambdock_shelf_active(&state));
  state.shelf = lambdock_shelf_params_default();

  state.shelf.reflection_ratio = 0.1;
  g_assert_true(lambdock_shelf_active(&state));
  state.shelf = lambdock_shelf_params_default();

  state.shelf.front_px = 4;
  g_assert_true(lambdock_shelf_active(&state));
  state.shelf = lambdock_shelf_params_default();

  state.shelf.margin_px = 12;
  g_assert_true(lambdock_shelf_active(&state));

  /* A zero-initialised state means "defaults", never a zero-sized capsule. */
  LambdockState zero = {0};
  g_assert_false(lambdock_shelf_active(&zero));
}

static void test_geometry_helpers(void) {
  LambdockState state = {0};
  state.icon_size = ICON;

  /* Defaults: capsule = icon + margin, no outer glass, no overhang. */
  state.shelf = lambdock_shelf_params_default();
  LambdockShelfGeometry g = lambdock_shelf_geometry_for(&state, ICON);
  g_assert_cmpint(g.band_px, ==, 0);
  g_assert_cmpint(g.outer_px, ==, 0);
  g_assert_cmpint(g.capsule_px, ==, ICON + CAPSULE_PADDING_PX);
  g_assert_cmpint(g.overhang_px, ==, 0);

  /* Reflection band taller than the front glass wins the outer side. */
  state.shelf = shelf_params(); /* -22 margin, 18 front, 0.5 reflection */
  g = lambdock_shelf_geometry_for(&state, ICON);
  g_assert_cmpint(g.band_px, ==, BAND(0.5, ICON));
  g_assert_cmpint(g.outer_px, ==, MAX(18, BAND(0.5, ICON)));
  g_assert_cmpint(g.capsule_px, ==, ICON - 22 + g.outer_px);
  g_assert_cmpint(g.overhang_px, ==, 22);

  /* A tall front face keeps the capsule taller than the reflection band. */
  state.shelf.reflection_ratio = 0.2;
  state.shelf.front_px = 40;
  g = lambdock_shelf_geometry_for(&state, ICON);
  g_assert_cmpint(g.band_px, ==, BAND(0.2, ICON));
  g_assert_cmpint(g.outer_px, ==, 40);

  /* NULL state means defaults; a zero icon size falls back to the default. */
  g = lambdock_shelf_geometry_for(NULL, 0);
  g_assert_cmpint(g.capsule_px, ==, DEFAULT_ICON_SIZE + CAPSULE_PADDING_PX);
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
  shelf_reflection_configure(NULL, GTK_LAYER_SHELL_EDGE_BOTTOM, ICON, 0.5, 0.03,
                             0.8);
  shelf_reflection_configure(box, GTK_LAYER_SHELL_EDGE_BOTTOM, ICON, 0.5, 0.03,
                             0.8);
  shelf_reflection_set_icon_size(NULL, ICON);
  shelf_reflection_set_icon_size(box, ICON);
  shelf_reflection_set_extent(NULL, ICON, 24);
  shelf_reflection_set_extent(box, ICON, 24);
  shelf_reflection_set_running(NULL, TRUE);
  shelf_reflection_set_running(box, TRUE);
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

  shelf_reflection_configure(reflection, GTK_LAYER_SHELL_EDGE_BOTTOM, ICON, 0.5,
                             0.05, 0.55);

  int minimum = -1;
  int natural = -1;
  gtk_widget_measure(reflection, GTK_ORIENTATION_HORIZONTAL, -1, &minimum,
                     &natural, NULL, NULL);
  g_assert_cmpint(minimum, ==, 0);
  g_assert_cmpint(natural, ==, ICON);

  gtk_widget_measure(reflection, GTK_ORIENTATION_VERTICAL, -1, &minimum,
                     &natural, NULL, NULL);
  g_assert_cmpint(minimum, ==, 0);
  g_assert_cmpint(natural, ==, BAND(0.5, ICON));

  /* A different icon size rescales the band. */
  shelf_reflection_configure(reflection, GTK_LAYER_SHELL_EDGE_BOTTOM, 64, 0.5,
                             0.05, 0.55);
  gtk_widget_measure(reflection, GTK_ORIENTATION_VERTICAL, -1, &minimum,
                     &natural, NULL, NULL);
  g_assert_cmpint(natural, ==, BAND(0.5, 64));

  /* A different band ratio too. */
  shelf_reflection_configure(reflection, GTK_LAYER_SHELL_EDGE_BOTTOM, 64, 0.25,
                             0.05, 0.55);
  gtk_widget_measure(reflection, GTK_ORIENTATION_VERTICAL, -1, &minimum,
                     &natural, NULL, NULL);
  g_assert_cmpint(natural, ==, BAND(0.25, 64));

  g_object_ref_sink(reflection);
  g_object_unref(reflection);
}

static void test_reflection_size_request_per_edge(void) {
  GtkWidget *reflection = shelf_reflection_new(NULL);

  /* A bottom/top dock stacks the reflection vertically: icon-wide, band-tall. */
  shelf_reflection_configure(reflection, GTK_LAYER_SHELL_EDGE_BOTTOM, ICON, 0.5,
                             0.05, 0.55);
  shelf_reflection_set_icon_size(reflection, ICON);

  int width = 0;
  int height = 0;
  gtk_widget_get_size_request(reflection, &width, &height);
  g_assert_cmpint(width, ==, ICON);
  g_assert_cmpint(height, ==, BAND(0.5, ICON));

  /* A side dock mirrors sideways: band-wide, icon-tall. */
  shelf_reflection_configure(reflection, GTK_LAYER_SHELL_EDGE_RIGHT, ICON, 0.5,
                             0.05, 0.55);
  shelf_reflection_set_icon_size(reflection, ICON);

  gtk_widget_get_size_request(reflection, &width, &height);
  g_assert_cmpint(width, ==, BAND(0.5, ICON));
  g_assert_cmpint(height, ==, ICON);

  /* Invalid sizes are ignored rather than clearing the request. */
  shelf_reflection_set_icon_size(reflection, 0);
  gtk_widget_get_size_request(reflection, &width, &height);
  g_assert_cmpint(width, ==, BAND(0.5, ICON));

  /* A fixed band thickness keeps the row stable while the icon magnifies. */
  shelf_reflection_configure(reflection, GTK_LAYER_SHELL_EDGE_BOTTOM, ICON, 0.5,
                             0.05, 0.55);
  shelf_reflection_set_extent(reflection, 70, BAND(0.5, ICON));
  gtk_widget_get_size_request(reflection, &width, &height);
  g_assert_cmpint(width, ==, 70);
  g_assert_cmpint(height, ==, BAND(0.5, ICON));

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

static void test_frame_reserves_room_for_the_flare(void) {
  GtkWidget *child = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request(child, 400, 44);

  GtkWidget *frame = shelf_frame_new(child);
  shelf_frame_set_shape(frame, GTK_LAYER_SHELL_EDGE_BOTTOM, 0.0, 400.0, 26);

  /* The keystone widens the screen-side edge, so the frame asks for twice the
   * flare on the cross axis (horizontal, for a top/bottom dock) and nothing on
   * the main axis (vertical). */
  int minimum = -1;
  int natural = -1;
  gtk_widget_measure(frame, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural,
                     NULL, NULL);
  g_assert_cmpint(natural, ==, 400 + 2 * 26);

  gtk_widget_measure(frame, GTK_ORIENTATION_VERTICAL, -1, &minimum, &natural,
                     NULL, NULL);
  g_assert_cmpint(natural, ==, 44);

  /* A side dock flares along the other axis. */
  shelf_frame_set_shape(frame, GTK_LAYER_SHELL_EDGE_LEFT, 0.0, 400.0, 12);
  gtk_widget_measure(frame, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural,
                     NULL, NULL);
  g_assert_cmpint(natural, ==, 400);
  gtk_widget_measure(frame, GTK_ORIENTATION_VERTICAL, -1, &minimum, &natural,
                     NULL, NULL);
  g_assert_cmpint(natural, ==, 44 + 2 * 12);

  /* No flare, no reservation. */
  shelf_frame_set_shape(frame, GTK_LAYER_SHELL_EDGE_BOTTOM, 0.0, 400.0, 0);
  gtk_widget_measure(frame, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural,
                     NULL, NULL);
  g_assert_cmpint(natural, ==, 400);

  g_object_ref_sink(frame);
  g_object_unref(frame);
}

static void test_flare_activates_the_shelf(void) {
  LambdockState state = {0};
  state.shelf = lambdock_shelf_params_default();
  g_assert_false(lambdock_shelf_active(&state));

  /* The keystone alone is enough to need the shelf: it changes the silhouette
   * and therefore the material that has to go with it. */
  state.shelf.flare_px = 20;
  g_assert_true(lambdock_shelf_active(&state));

  LambdockShelfParams params = lambdock_shelf_params_effective(&state);
  g_assert_cmpint(params.flare_px, ==, 20);

  /* A zero-initialised state still means "the plain capsule". */
  LambdockState zero = {0};
  g_assert_false(lambdock_shelf_active(&zero));
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
  state.icon_size = ICON;
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

  /* Reflections on: visible, sized from the icon and the band ratio. */
  state.shelf = shelf_params();
  apply_shelf_layout(&state);
  g_assert_true(gtk_widget_get_visible(item->reflection));

  int width = 0;
  int height = 0;
  gtk_widget_get_size_request(item->reflection, &width, &height);
  g_assert_cmpint(width, ==, ICON);
  g_assert_cmpint(height, ==, BAND(0.5, ICON));

  /* A different ratio resizes the band without touching the icon size. */
  state.shelf.reflection_ratio = 0.25;
  apply_shelf_layout(&state);
  gtk_widget_get_size_request(item->reflection, &width, &height);
  g_assert_cmpint(height, ==, BAND(0.25, ICON));

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

/* ============================================================================
 * THE SHELF'S PROJECTION, AND THE WIDGETS THAT FOLLOW IT
 * ============================================================================ */

static gboolean pump_until_allocated(GtkWidget *widget) {
  for (int i = 0; i < 600; i++) {
    if (gtk_widget_get_width(widget) > 0 && gtk_widget_get_height(widget) > 0)
      return TRUE;
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
  }
  return gtk_widget_get_width(widget) > 0;
}

static graphene_rect_t painted_bounds(GtkWidget *parent, GtkWidget *child) {
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

/* A dock that has not been given a shape is a rectangle: nothing to project, and
 * nothing to follow. */
static void test_frame_projection_flat(void) {
  GtkWidget *frame = shelf_frame_new(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

  g_assert_false(shelf_frame_is_projected(frame));
  g_assert_cmpint(shelf_frame_cross_orientation(frame), ==,
                  GTK_ORIENTATION_VERTICAL);

  graphene_point_t point = GRAPHENE_POINT_INIT(10.0f, 20.0f);
  g_assert_false(shelf_frame_project_point(frame, &point));
  g_assert_cmpfloat(point.x, ==, 10.0f);
  g_assert_cmpfloat(point.y, ==, 20.0f);

  /* A side dock's slab runs across the dock instead. */
  shelf_frame_set_tilt(frame, GTK_LAYER_SHELL_EDGE_RIGHT, 0.0, 400.0);
  g_assert_cmpint(shelf_frame_cross_orientation(frame), ==,
                  GTK_ORIENTATION_HORIZONTAL);

  g_object_ref_sink(frame);
  g_object_unref(frame);
}

/* The keystone's own geometry: the inner edge is untouched, the screen-side edge
 * is magnified around the middle, so a vertical line on the slab leans away from
 * the centre as it comes towards the viewer. */
static void test_frame_projection_keystone(void) {
  const int slab_w = 400;
  const int slab_h = 60;
  const int flare = 26;

  GtkWidget *slab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request(slab, slab_w, slab_h);
  GtkWidget *frame = shelf_frame_new(slab);
  shelf_frame_set_shape(frame, GTK_LAYER_SHELL_EDGE_BOTTOM, 0.0, 400.0, flare);

  GtkWidget *window = gtk_window_new();
  gtk_window_set_child(GTK_WINDOW(window), frame);
  gtk_window_set_default_size(GTK_WINDOW(window), slab_w + 2 * flare + 40,
                              slab_h + 60);
  gtk_window_present(GTK_WINDOW(window));

  if (!pump_until_allocated(frame)) {
    g_test_skip("window was never allocated");
    gtk_window_destroy(GTK_WINDOW(window));
    return;
  }

  g_assert_true(shelf_frame_is_projected(frame));

  double width = (double)gtk_widget_get_width(frame);
  double height = (double)gtk_widget_get_height(frame);
  double centre = width / 2.0;

  /* The slab is `flare` narrower than the frame on each side, and the keystone
   * magnifies the pinned edge by 1 / pinned_scale. */
  double cross = width - 2.0 * flare;
  double pinned_scale = (cross - 2.0 * flare) / cross;

  /* The inner edge does not move... */
  graphene_point_t top = GRAPHENE_POINT_INIT((float)centre, 0.0f);
  g_assert_true(shelf_frame_project_point(frame, &top));
  g_assert_cmpfloat(fabs(top.x - centre), <, 0.5);
  g_assert_cmpfloat(fabs(top.y), <, 0.5);

  /* ...and the middle of the screen-side edge stays in the middle, only
   * further down. */
  graphene_point_t bottom = GRAPHENE_POINT_INIT((float)centre, (float)height);
  g_assert_true(shelf_frame_project_point(frame, &bottom));
  g_assert_cmpfloat(fabs(bottom.x - centre), <, 0.5);
  g_assert_cmpfloat(bottom.y, >, height);

  /* A point off the middle leans outward, by the same amount on both sides. */
  const float d = 100.0f;
  graphene_point_t right =
      GRAPHENE_POINT_INIT((float)centre + d, (float)height);
  graphene_point_t left =
      GRAPHENE_POINT_INIT((float)centre - d, (float)height);
  g_assert_true(shelf_frame_project_point(frame, &right));
  g_assert_true(shelf_frame_project_point(frame, &left));

  g_assert_cmpfloat(right.x, >, centre + d);
  g_assert_cmpfloat(left.x, <, centre - d);
  g_assert_cmpfloat(fabs((right.x - centre) - (centre - left.x)), <, 0.5);

  /* The magnification is the keystone's own: d / pinned_scale. */
  g_assert_cmpfloat(fabs((right.x - centre) - d / pinned_scale), <, 1.0);

  gtk_window_destroy(GTK_WINDOW(window));
}

/* Builds the dock's arrangement: a slab, its frame, and a separator-sized widget
 * laid over them by a follower. */
typedef struct {
  GtkWidget *window;
  GtkWidget *overlay;
  GtkWidget *frame;
  GtkWidget *slab;
  GtkWidget *follow;
} FollowFixture;

static FollowFixture follow_fixture_new(int flare) {
  FollowFixture f = {0};

  f.window = gtk_window_new();
  f.overlay = gtk_overlay_new();

  f.slab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request(f.slab, 300, 50);

  f.frame = shelf_frame_new(f.slab);
  shelf_frame_set_shape(f.frame, GTK_LAYER_SHELL_EDGE_BOTTOM, 0.0, 400.0, flare);
  gtk_overlay_set_child(GTK_OVERLAY(f.overlay), f.frame);

  GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(body, 2, 40);
  /* It has to paint something, or there is nothing to measure the transform
   * with. */
  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(
      provider, ".follow-test-body { background-color: rgba(255, 0, 0, 0.9); }");
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
  gtk_widget_add_css_class(body, "follow-test-body");

  f.follow = shelf_follow_new(body, f.frame, f.slab);
  /* Off the middle: the lean is proportional to the distance from the dock's
   * centre, so a separator in the middle would not lean at all. */
  gtk_widget_set_halign(f.follow, GTK_ALIGN_START);
  gtk_widget_set_margin_start(f.follow, 60);
  gtk_widget_set_valign(f.follow, GTK_ALIGN_END);
  gtk_overlay_add_overlay(GTK_OVERLAY(f.overlay), f.follow);

  gtk_window_set_child(GTK_WINDOW(f.window), f.overlay);
  gtk_window_set_default_size(GTK_WINDOW(f.window), 300 + 2 * flare + 40, 160);
  gtk_window_present(GTK_WINDOW(f.window));

  return f;
}

static void follow_fixture_free(FollowFixture *f) {
  gtk_window_destroy(GTK_WINDOW(f->window));
}

/* A flat capsule leaves the separator exactly where the layout put it. */
static void test_follow_flat_pass_through(void) {
  FollowFixture f = follow_fixture_new(0);

  if (!pump_until_allocated(f.follow)) {
    g_test_skip("window was never allocated");
    follow_fixture_free(&f);
    return;
  }

  g_assert_false(shelf_follow_is_projected(f.follow));
  g_assert_false(gtk_widget_has_css_class(f.follow, "shelf-projected"));

  graphene_rect_t bounds = painted_bounds(f.overlay, f.follow);
  g_assert_cmpfloat(fabs(bounds.size.width - gtk_widget_get_width(f.follow)), <,
                    1.5);
  g_assert_cmpfloat(fabs(bounds.size.height - gtk_widget_get_height(f.follow)),
                    <, 1.5);

  follow_fixture_free(&f);
}

/* On a keystoned dock the separator follows the glass: it leans, and it spans the
 * slab's depth instead of the icon-sized box the dock laid out for it. */
static void test_follow_projected_leans(void) {
  FollowFixture f = follow_fixture_new(24);

  if (!pump_until_allocated(f.follow)) {
    g_test_skip("window was never allocated");
    follow_fixture_free(&f);
    return;
  }

  g_assert_true(shelf_follow_is_projected(f.follow));
  g_assert_true(gtk_widget_has_css_class(f.follow, "shelf-projected"));

  graphene_rect_t bounds = painted_bounds(f.overlay, f.follow);

  /* The shear widens what the separator covers... */
  g_assert_cmpfloat(bounds.size.width, >, 2.0 + 1.0);

  /* ...and it now reaches down the slab's face, past its own 40px box. */
  g_assert_cmpfloat(bounds.size.height, >, 40.0);

  follow_fixture_free(&f);
}

static void test_cross_dim_helpers(void) {
  LambdockState state = {0};
  state.icon_size = ICON;
  state.shelf = lambdock_shelf_params_default();

  /* Defaults keep the historic capsule/overlay dimensions. */
  g_assert_cmpint(dock_capsule_cross_dim(&state), ==, ICON + CAPSULE_PADDING_PX);
  g_assert_cmpint(dock_cross_axis_dim(&state), ==,
                  (int)(ICON * CROSS_DIM_SCALE_FACTOR) + CROSS_DIM_PADDING_PX);

  /* The 3D shelf set: icons overhang, reflections sit in front. */
  state.shelf = shelf_params();

  int band = BAND(0.5, ICON);
  int outer = MAX(18, band);
  g_assert_cmpint(dock_capsule_cross_dim(&state), ==, ICON - 22 + outer);
  g_assert_cmpint(dock_capsule_cross_dim(&state), ==, 50);

  g_assert_cmpint(dock_cross_axis_dim(&state), ==,
                  (int)(ICON * CROSS_DIM_SCALE_FACTOR) + CROSS_DIM_PADDING_PX +
                      outer + 22);

  /* Every parameter is honoured independently. */
  state.shelf.margin_px = 0;
  state.shelf.front_px = 40;
  state.shelf.reflection_ratio = 0.0;
  g_assert_cmpint(dock_capsule_cross_dim(&state), ==, ICON + 40);

  /* NULL and zero-sized states fall back to the default parameters. */
  g_assert_cmpint(dock_capsule_cross_dim(NULL), ==,
                  DEFAULT_ICON_SIZE + CAPSULE_PADDING_PX);
  LambdockState zero = {0};
  g_assert_cmpint(dock_capsule_cross_dim(&zero), ==,
                  DEFAULT_ICON_SIZE + CAPSULE_PADDING_PX);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  /* The tests below open real windows, so render the way the dock does (main.c
   * sets this too) instead of depending on whichever renderer this environment
   * would pick. */
  g_setenv("GSK_RENDERER", "gl", FALSE);

  if (!gtk_init_check()) {
    g_print("Skipping shelf tests: No display server available\n");
    return 77;
  }

  g_test_add_func("/ui/shelf/default_params", test_default_params);
  g_test_add_func("/ui/shelf/shelf_active", test_shelf_active);
  g_test_add_func("/ui/shelf/geometry_helpers", test_geometry_helpers);
  g_test_add_func("/ui/shelf/type_guards", test_type_guards);
  g_test_add_func("/ui/shelf/reflection_measure", test_reflection_measure);
  g_test_add_func("/ui/shelf/reflection_size_request",
                  test_reflection_size_request_per_edge);
  g_test_add_func("/ui/shelf/frame_measure", test_frame_measure_delegates_to_child);
  g_test_add_func("/ui/shelf/frame_flare_room",
                  test_frame_reserves_room_for_the_flare);
  g_test_add_func("/ui/shelf/flare_activates_shelf",
                  test_flare_activates_the_shelf);
  g_test_add_func("/ui/shelf/frame_without_child", test_frame_without_child);
  g_test_add_func("/ui/shelf/wrap_item_ordering", test_wrap_item_ordering);
  g_test_add_func("/ui/shelf/layout_toggle",
                  test_apply_shelf_layout_toggles_visibility);
  g_test_add_func("/ui/shelf/cross_dim_helpers", test_cross_dim_helpers);
  g_test_add_func("/ui/shelf/frame_projection_flat", test_frame_projection_flat);
  g_test_add_func("/ui/shelf/frame_projection_keystone",
                  test_frame_projection_keystone);
  g_test_add_func("/ui/shelf/follow_flat", test_follow_flat_pass_through);
  g_test_add_func("/ui/shelf/follow_projected", test_follow_projected_leans);

  return g_test_run();
}
