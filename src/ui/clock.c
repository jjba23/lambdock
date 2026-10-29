// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "clock.h"

#include "../core/clock_face.h"
#include <math.h>

/* ============================================================================
 * THE CLOCK
 * ============================================================================
 *
 * A watch face, drawn at icon size: a squircle body with a brushed bezel, a
 * paper dial, tick marks, three hands and a sweep of glass across the top-left.
 *
 * The whole thing is geometry, so it stays crisp at every dock-icon-size and
 * needs no artwork. Everything is proportional to the face's size, and the
 * palette lives here rather than in the theme: a watch face is an object -- the
 * same one on a light dock and a dark one -- which is also why it wears a body
 * of its own instead of hanging its numbers next to the icons.
 */

/* How square the body is: 2 is an ellipse, 4 a squircle, 8 nearly a rounded
 * rectangle. Four reads as "squircle" at 48 pixels without looking like a
 * button. */
#define CLOCK_BODY_SQUARENESS 4.2
#define CLOCK_DIAL_SQUARENESS 4.0
/* Samples per turn for the body and the dial. Enough that the curve reads as
 * smooth at 96 pixels, cheap enough to rebuild on every redraw. */
#define CLOCK_CURVE_STEPS 96
/* Below this the face has no room for a dial, ticks and hands. */
#define CLOCK_MIN_DRAWABLE_PX 8

typedef struct _LambdockClock {
  GtkWidget parent_instance;

  int seconds;   /* seconds since local midnight */
  int icon_size; /* requested square size */
} LambdockClock;

typedef struct _LambdockClockClass {
  GtkWidgetClass parent_class;
} LambdockClockClass;

GType lambdock_clock_get_type(void);
G_DEFINE_FINAL_TYPE(LambdockClock, lambdock_clock, GTK_TYPE_WIDGET)

#define LAMBDOCK_CLOCK(obj)                                                   \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), lambdock_clock_get_type(),               \
                              LambdockClock))

/* ============================================================================
 * PALETTE
 * ============================================================================
 */

static const GdkRGBA bezel_light = {0.98f, 0.97f, 0.94f, 1.0f};
static const GdkRGBA bezel_mid = {0.84f, 0.82f, 0.78f, 1.0f};
static const GdkRGBA bezel_dark = {0.60f, 0.58f, 0.54f, 1.0f};
static const GdkRGBA bezel_edge = {0.30f, 0.28f, 0.26f, 0.45f};
static const GdkRGBA dial_light = {0.999f, 0.995f, 0.980f, 1.0f};
static const GdkRGBA dial_shade = {0.862f, 0.828f, 0.752f, 1.0f};
static const GdkRGBA dial_edge = {0.26f, 0.24f, 0.21f, 0.20f};
static const GdkRGBA tick_major = {0.22f, 0.20f, 0.17f, 0.95f};
static const GdkRGBA tick_minor = {0.44f, 0.41f, 0.36f, 0.60f};
static const GdkRGBA hand_ink = {0.17f, 0.15f, 0.13f, 1.0f};
static const GdkRGBA second_ink = {0.74f, 0.17f, 0.13f, 1.0f};
static const GdkRGBA cap_highlight = {1.0f, 1.0f, 1.0f, 0.60f};

/* ============================================================================
 * GEOMETRY HELPERS
 * ============================================================================
 */

/** A closed squircle path centred on (@cx, @cy). */
static GskPath *build_squircle(double cx, double cy, double half_width,
                               double half_height, double squareness) {
  GskPathBuilder *builder = gsk_path_builder_new();

  for (int i = 0; i <= CLOCK_CURVE_STEPS; i++) {
    double x = 0.0;
    double y = 0.0;
    lambdock_squircle_point((double)i / CLOCK_CURVE_STEPS, half_width,
                            half_height, squareness, &x, &y);

    if (i == 0)
      gsk_path_builder_move_to(builder, (float)(cx + x), (float)(cy + y));
    else
      gsk_path_builder_line_to(builder, (float)(cx + x), (float)(cy + y));
  }

  return gsk_path_builder_free_to_path(builder);
}

/** A straight run from one distance out of the centre to another. */
static GskPath *build_ray(double cx, double cy, double turn, double from_px,
                          double to_px) {
  double angle = turn * 2.0 * G_PI;
  double dx = sin(angle);
  double dy = -cos(angle); /* turns run clockwise from 12 o'clock */

  GskPathBuilder *builder = gsk_path_builder_new();
  gsk_path_builder_move_to(builder, (float)(cx + dx * from_px),
                           (float)(cy + dy * from_px));
  gsk_path_builder_line_to(builder, (float)(cx + dx * to_px),
                           (float)(cy + dy * to_px));

  return gsk_path_builder_free_to_path(builder);
}

/** A stroked run with round ends, the shape every tick and hand is made of. */
static void paint_ray(GtkSnapshot *snapshot, double cx, double cy, double turn,
                      double from_px, double to_px, double thickness,
                      const GdkRGBA *color) {
  GskPath *ray = build_ray(cx, cy, turn, from_px, to_px);

  GskStroke *stroke = gsk_stroke_new((float)thickness);
  gsk_stroke_set_line_cap(stroke, GSK_LINE_CAP_ROUND);
  gtk_snapshot_append_stroke(snapshot, ray, stroke, color);

  gsk_stroke_free(stroke);
  gsk_path_unref(ray);
}

static void paint_circle(GtkSnapshot *snapshot, double cx, double cy,
                         double radius, const GdkRGBA *color) {
  GskPathBuilder *builder = gsk_path_builder_new();
  graphene_point_t center = GRAPHENE_POINT_INIT((float)cx, (float)cy);
  gsk_path_builder_add_circle(builder, &center, (float)radius);

  GskPath *path = gsk_path_builder_free_to_path(builder);
  gtk_snapshot_append_fill(snapshot, path, GSK_FILL_RULE_WINDING, color);
  gsk_path_unref(path);
}

/* ============================================================================
 * DRAWING
 * ============================================================================
 */

static void paint_bezel(GtkSnapshot *snapshot, GskPath *body, double size) {
  graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, (float)size, (float)size);
  if (!gsk_path_get_bounds(body, &bounds))
    bounds = GRAPHENE_RECT_INIT(0, 0, (float)size, (float)size);

  /* Brushed metal: light where the window is, darker towards the far corner. */
  GskColorStop stops[3] = {
      {0.0f, bezel_light},
      {0.45f, bezel_mid},
      {1.0f, bezel_dark},
  };
  graphene_point_t start = GRAPHENE_POINT_INIT(0.0f, 0.0f);
  graphene_point_t end = GRAPHENE_POINT_INIT((float)size, (float)size);
  gtk_snapshot_append_linear_gradient(snapshot, &bounds, &start, &end, stops,
                                      3);

  /* A crisp outline instead of a blurred one: at 48 pixels a soft edge reads as
   * a smudge. */
  GskStroke *edge = gsk_stroke_new((float)MAX(1.0, size * 0.015));
  gtk_snapshot_append_stroke(snapshot, body, edge, &bezel_edge);
  gsk_stroke_free(edge);
}

static void paint_dial(GtkSnapshot *snapshot, double size, double half,
                       GskPath **out_dial) {
  double dial_half = half * 0.82;
  GskPath *dial = build_squircle(half, half, dial_half, dial_half,
                                 CLOCK_DIAL_SQUARENESS);

  graphene_rect_t bounds =
      GRAPHENE_RECT_INIT(0, 0, (float)size, (float)size);
  if (!gsk_path_get_bounds(dial, &bounds))
    bounds = GRAPHENE_RECT_INIT(0, 0, (float)size, (float)size);

  /* Paper, lit from the top-left: the radial gradient is centred off-centre so
   * the dial has a warm side and a shaded one. */
  GskColorStop stops[2] = {
      {0.0f, dial_light},
      {1.0f, dial_shade},
  };
  graphene_point_t center =
      GRAPHENE_POINT_INIT((float)(half - dial_half * 0.35),
                          (float)(half - dial_half * 0.45));
  gtk_snapshot_append_radial_gradient(snapshot, &bounds, &center,
                                       (float)(dial_half * 1.05),
                                       (float)(dial_half * 1.05),
                                       (float)(dial_half * 0.1),
                                       (float)(dial_half * 1.5), stops, 2);

  /* The rim the bezel casts on the dial. */
  GskStroke *rim = gsk_stroke_new((float)MAX(0.8, size * 0.02));
  gtk_snapshot_append_stroke(snapshot, dial, rim, &dial_edge);
  gsk_stroke_free(rim);

  *out_dial = dial;
}

static void paint_ticks(GtkSnapshot *snapshot, double half) {
  /* Minute ticks are the first thing to go mushy at 48 pixels, so they only
   * appear once the face has room to draw them apart. */
  if (half >= 20.0) {
    double minor_thickness = MAX(0.5, half * 0.022);

    for (int i = 0; i < 60; i++) {
      if (i % 5 == 0)
        continue;
      paint_ray(snapshot, half, half, (double)i / 60.0, half * 0.55,
                half * 0.62, minor_thickness, &tick_minor);
    }
  }

  double major_thickness = MAX(1.0, half * 0.055);

  for (int i = 0; i < 12; i++)
    paint_ray(snapshot, half, half, (double)i / 12.0, half * 0.50,
              half * 0.63, major_thickness, &tick_major);
}

static void paint_hands(GtkSnapshot *snapshot, double half,
                        LambdockClockTurns turns) {
  /* The minute hand over the hour hand, the second hand over both: the order
   * they are stacked in on a real watch. */
  paint_ray(snapshot, half, half, turns.hour, -half * 0.05, half * 0.40,
            MAX(1.4, half * 0.115), &hand_ink);
  paint_ray(snapshot, half, half, turns.minute, -half * 0.05, half * 0.60,
            MAX(1.0, half * 0.075), &hand_ink);
  paint_ray(snapshot, half, half, turns.second, -half * 0.18, half * 0.66,
            MAX(0.6, half * 0.028), &second_ink);

  /* The hub, with one specular dot so it reads as a pin and not a blob. */
  paint_circle(snapshot, half, half, MAX(1.4, half * 0.065), &hand_ink);
  paint_circle(snapshot, half - half * 0.018, half - half * 0.022,
               MAX(0.6, half * 0.024), &cap_highlight);
}

static void lambdock_clock_measure(GtkWidget *widget, GtkOrientation orientation,
                                   int for_size, int *minimum, int *natural,
                                   int *minimum_baseline, int *natural_baseline) {
  (void)orientation;
  (void)for_size;

  LambdockClock *self = LAMBDOCK_CLOCK(widget);
  int size = self->icon_size > 0 ? self->icon_size : DEFAULT_ICON_SIZE;

  *minimum = size;
  *natural = size;
  if (minimum_baseline)
    *minimum_baseline = -1;
  if (natural_baseline)
    *natural_baseline = -1;
}

static void lambdock_clock_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  LambdockClock *self = LAMBDOCK_CLOCK(widget);

  double size = MIN(gtk_widget_get_width(widget), gtk_widget_get_height(widget));
  if (size < CLOCK_MIN_DRAWABLE_PX)
    return;

  double half = size / 2.0;

  GskPath *body =
      build_squircle(half, half, half, half, CLOCK_BODY_SQUARENESS);
  paint_bezel(snapshot, body, size);

  GskPath *dial = NULL;
  paint_dial(snapshot, size, half, &dial);
  paint_ticks(snapshot, half);
  paint_hands(snapshot, half, lambdock_clock_turns_for_seconds(self->seconds));

  /* The crystal: one sweep of light across the body, clipped to it rather than to
   * the dial -- a highlight that stops at the dial's edge leaves a visible seam
   * inside the bezel, which at 64 pixels reads as two stacked faces. */
  gtk_snapshot_push_fill(snapshot, body, GSK_FILL_RULE_WINDING);
  {
    graphene_rect_t bounds =
        GRAPHENE_RECT_INIT(0, 0, (float)size, (float)size);
    if (!gsk_path_get_bounds(body, &bounds))
      bounds = GRAPHENE_RECT_INIT(0, 0, (float)size, (float)size);

    GskColorStop stops[3] = {
        {0.0f, {1.0f, 1.0f, 1.0f, 0.40f}},
        {0.50f, {1.0f, 1.0f, 1.0f, 0.10f}},
        {1.0f, {1.0f, 1.0f, 1.0f, 0.0f}},
    };
    graphene_point_t start = GRAPHENE_POINT_INIT(0.0f, 0.0f);
    graphene_point_t end = GRAPHENE_POINT_INIT((float)size, (float)size);
    gtk_snapshot_append_linear_gradient(snapshot, &bounds, &start, &end, stops,
                                        3);
  }
  gtk_snapshot_pop(snapshot);

  gsk_path_unref(dial);
  gsk_path_unref(body);
}

/* ============================================================================
 * GTK WIDGET PLUMBING
 * ============================================================================
 */

static void lambdock_clock_class_init(LambdockClockClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  widget_class->measure = lambdock_clock_measure;
  widget_class->snapshot = lambdock_clock_snapshot;

  gtk_widget_class_set_css_name(widget_class, "clock");
}

static void lambdock_clock_init(LambdockClock *self) {
  self->seconds = 0;
  self->icon_size = DEFAULT_ICON_SIZE;

  gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_VISIBLE);
}

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

GtkWidget *lambdock_clock_new(void) {
  return g_object_new(lambdock_clock_get_type(), NULL);
}

void lambdock_clock_set_time(GtkWidget *clock, int seconds_of_day) {
  if (!lambdock_clock_is_clock(clock))
    return;

  LambdockClock *self = LAMBDOCK_CLOCK(clock);

  /* Wrap here rather than trusting the caller: a poll that reads the clock
   * across midnight can hand over 86400, and the face must not draw that as a
   * full extra turn. */
  int seconds = seconds_of_day % LAMBDOCK_SECONDS_PER_DAY;
  if (seconds < 0)
    seconds += LAMBDOCK_SECONDS_PER_DAY;

  if (seconds == self->seconds)
    return;

  self->seconds = seconds;
  gtk_widget_queue_draw(clock);
}

int lambdock_clock_get_time(GtkWidget *clock) {
  if (!lambdock_clock_is_clock(clock))
    return 0;

  return LAMBDOCK_CLOCK(clock)->seconds;
}

void lambdock_clock_set_icon_size(GtkWidget *clock, int icon_size) {
  if (!lambdock_clock_is_clock(clock) || icon_size <= 0)
    return;

  LambdockClock *self = LAMBDOCK_CLOCK(clock);
  if (self->icon_size == icon_size)
    return;

  self->icon_size = icon_size;
  gtk_widget_queue_resize(clock);
}

gboolean lambdock_clock_is_clock(gpointer widget) {
  return widget != NULL &&
         G_TYPE_CHECK_INSTANCE_TYPE(widget, lambdock_clock_get_type());
}
