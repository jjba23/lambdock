// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "meter.h"
#include "../core/config_values.h"
#include <math.h>

/* ============================================================================
 * THE METER
 * ============================================================================
 *
 * A ring, a value arc and the reading in the middle. Everything is proportional
 * to the icon size, so the gauge lines up with the launcher icons around it at
 * any dock-icon-size.
 *
 * Colour carries the meaning: comfortable values are green, getting tight is
 * amber and nearly full is red, which is the one thing a plain number cannot say
 * at a glance from across the room.
 */

#define METER_ARC_STEPS_PER_TURN 180

typedef struct _LambdockMeter {
  GtkWidget parent_instance;

  double fraction; /* 0.0 .. 1.0 */
  char *text;      /* "42%" */
  int icon_size;   /* requested square size */
} LambdockMeter;

typedef struct _LambdockMeterClass {
  GtkWidgetClass parent_class;
} LambdockMeterClass;

GType lambdock_meter_get_type(void);
G_DEFINE_FINAL_TYPE(LambdockMeter, lambdock_meter, GTK_TYPE_WIDGET)

#define LAMBDOCK_METER(obj)                                                   \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), lambdock_meter_get_type(),               \
                              LambdockMeter))

/* ============================================================================
 * DRAWING HELPERS
 * ============================================================================
 */

GdkRGBA lambdock_meter_tone_for_fraction(double fraction) {
  /* NaN compares false against everything, so it lands on red: a reading the
   * dock cannot understand should not look comfortable. */
  if (fraction < 0.60)
    return (GdkRGBA){0.22f, 0.76f, 0.36f, 1.0f};
  if (fraction < 0.85)
    return (GdkRGBA){0.97f, 0.68f, 0.20f, 1.0f};
  return (GdkRGBA){0.90f, 0.26f, 0.26f, 1.0f};
}

LambdockMeterGeometry lambdock_meter_geometry_compute(int width, int height) {
  LambdockMeterGeometry geometry = {0};

  if (width <= 0 || height <= 0)
    return geometry;

  double size = MIN(width, height);

  geometry.tile_radius = (float)(size * 0.22);
  geometry.center_x = (float)(width / 2.0);
  geometry.center_y = (float)(height / 2.0);

  /* The gauge wears a tile of its own: a bare ring would be drawn half over the
   * glass and half over whatever is behind the dock (a dock whose icons overhang
   * its slab puts the icon row above the glass centre), and a reading that
   * changes colour has to stay legible on both. */
  double padding = size * 0.04;
  geometry.tile = GRAPHENE_RECT_INIT(
      (float)padding, (float)padding, (float)(size - 2 * padding),
      (float)(size - 2 * padding));

  /* Thick enough to read at a glance, thin enough to leave room for the number
   * in the middle. */
  geometry.line_width = (float)MAX(2.0, size * 0.10);
  geometry.radius =
      (float)((size - geometry.line_width) / 2.0 - size * 0.16);

  /* The reading has to fit inside the ring: the snapshot function starts at a
   * comfortable size and shrinks until it fits into this box. */
  geometry.text_box = (float)(size * 0.66);

  return geometry;
}

/* Builds an arc from the top of the circle, clockwise, covering @fraction of a
 * full turn. GskPathBuilder has no arc primitive, so the curve is a polyline
 * with a step fine enough that the round caps and joins hide the seams. */
static GskPath *arc_path(double center_x, double center_y, double radius,
                         double fraction) {
  GskPathBuilder *builder = gsk_path_builder_new();

  int steps = (int)ceil(fraction * METER_ARC_STEPS_PER_TURN);
  if (steps < 2)
    steps = 2;

  for (int i = 0; i <= steps; i++) {
    double turn = fraction * ((double)i / (double)steps);
    double angle = -G_PI_2 + turn * 2.0 * G_PI;
    float x = (float)(center_x + radius * cos(angle));
    float y = (float)(center_y + radius * sin(angle));

    if (i == 0)
      gsk_path_builder_move_to(builder, x, y);
    else
      gsk_path_builder_line_to(builder, x, y);
  }

  return gsk_path_builder_free_to_path(builder);
}

static GskPath *ring_path(double center_x, double center_y, double radius) {
  GskPathBuilder *builder = gsk_path_builder_new();
  graphene_point_t center = GRAPHENE_POINT_INIT((float)center_x,
                                                (float)center_y);
  gsk_path_builder_add_circle(builder, &center, (float)radius);
  return gsk_path_builder_free_to_path(builder);
}

/* ============================================================================
 * GTK WIDGET PLUMBING
 * ============================================================================
 */

static void lambdock_meter_measure(GtkWidget *widget, GtkOrientation orientation,
                                   int for_size, int *minimum, int *natural,
                                   int *minimum_baseline, int *natural_baseline) {
  (void)orientation;
  (void)for_size;

  LambdockMeter *self = LAMBDOCK_METER(widget);
  int size = self->icon_size > 0 ? self->icon_size : DEFAULT_ICON_SIZE;

  *minimum = size;
  *natural = size;
  if (minimum_baseline)
    *minimum_baseline = -1;
  if (natural_baseline)
    *natural_baseline = -1;
}

static void lambdock_meter_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  LambdockMeter *self = LAMBDOCK_METER(widget);

  int width = gtk_widget_get_width(widget);
  int height = gtk_widget_get_height(widget);
  LambdockMeterGeometry geometry =
      lambdock_meter_geometry_compute(width, height);
  if (width <= 0 || height <= 0)
    return;

  double size = MIN(width, height);

  GskRoundedRect tile_outline;
  gsk_rounded_rect_init_from_rect(&tile_outline, &geometry.tile,
                                  geometry.tile_radius);

  gtk_snapshot_push_rounded_clip(snapshot, &tile_outline);
  {
    GskColorStop stops[2] = {
        {0.0f, {0.24f, 0.26f, 0.31f, 0.94f}},
        {1.0f, {0.10f, 0.11f, 0.14f, 0.96f}},
    };
    graphene_point_t start =
        GRAPHENE_POINT_INIT(geometry.tile.origin.x, geometry.tile.origin.y);
    graphene_point_t end =
        GRAPHENE_POINT_INIT(geometry.tile.origin.x + geometry.tile.size.width,
                            geometry.tile.origin.y + geometry.tile.size.height);
    gtk_snapshot_append_linear_gradient(snapshot, &geometry.tile, &start, &end,
                                        stops, 2);
  }
  gtk_snapshot_pop(snapshot);

  {
    GdkRGBA edge = {1.0f, 1.0f, 1.0f, 0.22f};
    float widths[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    GdkRGBA edges[4] = {edge, edge, edge, edge};
    gtk_snapshot_append_border(snapshot, &tile_outline, widths, edges);
  }

  /* The tile is dark, so the track and the reading are drawn in light ink,
   * whatever theme the dock wears. */
  GdkRGBA foreground = {0.93f, 0.95f, 0.98f, 1.0f};

  GskStroke *stroke = gsk_stroke_new(geometry.line_width);
  gsk_stroke_set_line_cap(stroke, GSK_LINE_CAP_ROUND);
  gsk_stroke_set_line_join(stroke, GSK_LINE_JOIN_ROUND);

  /* The empty track first, so the filled arc sits on top of it. */
  GskPath *track =
      ring_path(geometry.center_x, geometry.center_y, geometry.radius);
  GdkRGBA track_color = foreground;
  track_color.alpha = 0.18f;
  gtk_snapshot_append_stroke(snapshot, track, stroke, &track_color);
  gsk_path_unref(track);

  if (self->fraction > 0.0) {
    GskPath *arc = arc_path(geometry.center_x, geometry.center_y,
                            geometry.radius, CLAMP(self->fraction, 0.0, 1.0));
    GdkRGBA tone = lambdock_meter_tone_for_fraction(self->fraction);
    gtk_snapshot_append_stroke(snapshot, arc, stroke, &tone);
    gsk_path_unref(arc);
  }

  gsk_stroke_free(stroke);

  if (!self->text || *self->text == '\0')
    return;

  /* The reading has to fit inside the ring, whatever it says ("42%", "54°C"):
   * start at a comfortable size and shrink until it does. Two or three passes
   * are enough; a floor keeps it from vanishing on tiny icons. */
  double available = geometry.text_box;
  int font_size = (int)(size * 0.40);

  PangoLayout *layout = gtk_widget_create_pango_layout(widget, self->text);
  PangoAttrList *attributes = NULL;
  int text_width = 0;
  int text_height = 0;

  for (int attempt = 0; attempt < 6; attempt++) {
    if (font_size < 7)
      font_size = 7;

    if (attributes)
      pango_attr_list_unref(attributes);
    attributes = pango_attr_list_new();
    pango_attr_list_insert(attributes,
                           pango_attr_size_new(font_size * PANGO_SCALE));
    pango_attr_list_insert(attributes,
                           pango_attr_weight_new(PANGO_WEIGHT_SEMIBOLD));
    pango_layout_set_attributes(layout, attributes);

    pango_layout_get_pixel_size(layout, &text_width, &text_height);
    if (text_width <= available && text_height <= available)
      break;

    font_size = (int)(font_size * 0.85);
  }

  if (attributes)
    pango_attr_list_unref(attributes);

  graphene_point_t origin = GRAPHENE_POINT_INIT(
      (float)(geometry.center_x - text_width / 2.0),
      (float)(geometry.center_y - text_height / 2.0));
  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &origin);
  gtk_snapshot_append_layout(snapshot, layout, &foreground);
  gtk_snapshot_restore(snapshot);

  g_object_unref(layout);
}

static void lambdock_meter_dispose(GObject *object) {
  LambdockMeter *self = LAMBDOCK_METER(object);
  g_clear_pointer(&self->text, g_free);

  G_OBJECT_CLASS(lambdock_meter_parent_class)->dispose(object);
}

static void lambdock_meter_class_init(LambdockMeterClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = lambdock_meter_dispose;

  widget_class->measure = lambdock_meter_measure;
  widget_class->snapshot = lambdock_meter_snapshot;

  gtk_widget_class_set_css_name(widget_class, "meter");
}

static void lambdock_meter_init(LambdockMeter *self) {
  self->fraction = 0.0;
  self->text = NULL;
  self->icon_size = DEFAULT_ICON_SIZE;

  gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_VISIBLE);
}

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

GtkWidget *lambdock_meter_new(void) {
  return g_object_new(lambdock_meter_get_type(), NULL);
}

void lambdock_meter_set_value(GtkWidget *meter, double fraction,
                              const char *text) {
  if (!lambdock_meter_is_meter(meter))
    return;

  LambdockMeter *self = LAMBDOCK_METER(meter);
  /* CLAMP() lets NaN through (every comparison against NaN is false), which
   * would reach the paint code; the sanitiser maps it to a safe value. */
  self->fraction = lambdock_sanitize_double(fraction, 0.0, 0.0, 1.0);

  if (text) {
    g_free(self->text);
    self->text = g_strdup(text);
  }

  gtk_widget_queue_draw(meter);
}

void lambdock_meter_set_icon_size(GtkWidget *meter, int icon_size) {
  if (!lambdock_meter_is_meter(meter) || icon_size <= 0)
    return;

  LambdockMeter *self = LAMBDOCK_METER(meter);
  if (self->icon_size == icon_size)
    return;

  self->icon_size = icon_size;
  gtk_widget_queue_resize(meter);
}

gboolean lambdock_meter_is_meter(gpointer widget) {
  return widget != NULL && G_TYPE_CHECK_INSTANCE_TYPE(widget,
                                                      lambdock_meter_get_type());
}

double lambdock_meter_fraction(GtkWidget *meter) {
  if (!lambdock_meter_is_meter(meter))
    return 0.0;
  return LAMBDOCK_METER(meter)->fraction;
}
