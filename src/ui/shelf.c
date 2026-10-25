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

#include "shelf.h"
#include "../logger.h"
#include <math.h>

/* ============================================================================
 * BUILT-IN PARAMETERS
 * ============================================================================ */

LambdockShelfParams lambdock_shelf_params_default(void) {
  LambdockShelfParams params;

  params.tilt_deg = SHELF_DEFAULT_TILT;
  params.perspective = SHELF_DEFAULT_PERSPECTIVE;
  params.margin_px = SHELF_DEFAULT_MARGIN_PX;
  params.front_px = SHELF_DEFAULT_FRONT_PX;
  params.reflection_ratio = SHELF_DEFAULT_REFLECTION;
  params.reflection_intensity = SHELF_DEFAULT_REFLECTION_INTENSITY;
  params.reflection_blur_ratio = SHELF_DEFAULT_REFLECTION_BLUR;

  return params;
}

/**
 * lambdock_shelf_params_effective:
 * Returns the shelf parameters that should be used for @state.
 *
 * A zero-initialised state (a partially built dock, a unit test) carries no
 * shelf configuration at all; it is treated as the plain dock rather than as a
 * capsule with zero margin, which would collapse the layout.
 */
LambdockShelfParams lambdock_shelf_params_effective(const LambdockState *state) {
  if (!state)
    return lambdock_shelf_params_default();

  LambdockShelfParams params = state->shelf;

  if (params.margin_px == 0 && params.front_px == 0 &&
      params.reflection_ratio == 0.0 && params.tilt_deg == 0.0)
    return lambdock_shelf_params_default();

  return params;
}

gboolean lambdock_shelf_active(const LambdockState *state) {
  if (!state)
    return FALSE;

  LambdockShelfParams defaults = lambdock_shelf_params_default();
  LambdockShelfParams params = lambdock_shelf_params_effective(state);

  return params.tilt_deg != defaults.tilt_deg ||
         params.margin_px != defaults.margin_px ||
         params.front_px != defaults.front_px ||
         params.reflection_ratio > defaults.reflection_ratio;
}

LambdockShelfGeometry lambdock_shelf_geometry_for(const LambdockState *state,
                                                  int icon_size) {
  LambdockShelfParams params = lambdock_shelf_params_effective(state);
  int icon = (icon_size > 0) ? icon_size : DEFAULT_ICON_SIZE;

  LambdockShelfGeometry geometry;
  geometry.band_px = (int)round((double)icon * params.reflection_ratio);
  if (geometry.band_px < 0)
    geometry.band_px = 0;

  geometry.outer_px = MAX(params.front_px, geometry.band_px);
  geometry.capsule_px = icon + params.margin_px + geometry.outer_px;
  if (geometry.capsule_px < 0)
    geometry.capsule_px = 0;

  geometry.overhang_px = MAX(0, -params.margin_px);

  return geometry;
}

/* ============================================================================
 * LambdockReflection — mirrored, blurred, fading icon reflection
 * ============================================================================
 *
 * The widget paints, on the glass in front of one icon:
 *
 *   1. a soft contact shadow where the icon meets the shelf,
 *   2. the icon itself, mirrored across that contact line, squashed into the
 *      reflection band and blurred with a gaussian blur,
 *   3. an alpha falloff (band by band, power curve) so it fades as it recedes,
 *   4. the running-app glow dot near the outer edge of the glass.
 *
 * The fade is drawn as SHELF_REFLECTION_BANDS stacked semi-transparent bands;
 * the blur pass smooths their boundaries back into a continuous gradient.
 */

typedef struct _LambdockReflection {
  GtkWidget parent_instance;

  GtkWidget *source;        /* Weak ref to the mirrored widget */
  GdkPaintable *paintable;  /* Rendering source (bound lazily) */
  GtkLayerShellEdge edge;   /* Shelf edge: decides mirror + fade axis */
  int icon_size;            /* Fallback size while the source is unsized */
  double band_ratio;        /* Reflection band / icon size */
  double blur_ratio;        /* Blur radius / icon size */
  double intensity;         /* Alpha at the icon-side edge of the band */
  gboolean running;         /* Draw the running-app glow dot */
  gboolean has_shadow;      /* Draw the contact shadow under the icon */
} LambdockReflection;

typedef struct _LambdockReflectionClass {
  GtkWidgetClass parent_class;
} LambdockReflectionClass;

GType lambdock_reflection_get_type(void);
G_DEFINE_TYPE(LambdockReflection, lambdock_reflection, GTK_TYPE_WIDGET)

#define LAMBDOCK_TYPE_REFLECTION (lambdock_reflection_get_type())
#define LAMBDOCK_REFLECTION(obj)                                              \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), LAMBDOCK_TYPE_REFLECTION,                \
                              LambdockReflection))

static gboolean reflection_is_vertical_band(const LambdockReflection *self) {
  /* A dock attached to the top or bottom edge of the screen has its
   * reflections stacked vertically (a band below or above each icon). */
  return self->edge == GTK_LAYER_SHELL_EDGE_TOP ||
         self->edge == GTK_LAYER_SHELL_EDGE_BOTTOM;
}

/**
 * reflection_sync_paintable:
 * (Re)binds the GtkWidgetPaintable used as the mirror source.
 *
 * A widget paintable created before its widget is mapped and sized can latch a
 * zero intrinsic size and then never paint anything, so the binding happens
 * from the first snapshot, when the icon certainly has an allocation.
 */
static void reflection_sync_paintable(LambdockReflection *self) {
  if (!self->source || !gtk_widget_get_mapped(self->source))
    return;
  if (gtk_widget_get_width(self->source) <= 0 ||
      gtk_widget_get_height(self->source) <= 0)
    return;
  if (self->paintable && gdk_paintable_get_intrinsic_width(self->paintable) > 0)
    return;

  g_set_object(&self->paintable, gtk_widget_paintable_new(self->source));
  gtk_widget_queue_resize(GTK_WIDGET(self));
}

/**
 * reflection_source_size:
 * Resolves the pixel size of the reflected source, falling back to the
 * configured icon size while the source widget has not been allocated yet.
 */
static void reflection_source_size(const LambdockReflection *self, double *width,
                                   double *height) {
  double w = 0.0;
  double h = 0.0;

  if (self->paintable) {
    w = (double)gdk_paintable_get_intrinsic_width(self->paintable);
    h = (double)gdk_paintable_get_intrinsic_height(self->paintable);
  }

  if (w <= 0.0)
    w = (double)((self->icon_size > 0) ? self->icon_size : DEFAULT_ICON_SIZE);
  if (h <= 0.0)
    h = w;

  *width = w;
  *height = h;
}

/**
 * reflection_band_size:
 * Height of the reflection band for a given icon size.
 */
static int reflection_band_size(const LambdockReflection *self, int icon_size) {
  double ratio = self->band_ratio > 0.0 ? self->band_ratio
                                        : SHELF_DEFAULT_REFLECTION;
  int band = (int)round((double)icon_size * ratio);
  return band > 0 ? band : 0;
}

/* ============================================================================
 * REFLECTION DECORATIONS (contact shadow, running dot)
 * ============================================================================ */

/**
 * reflection_edge_rect:
 * Builds a gradient from the icon-side edge of the band towards its far edge
 * (the mirror axis depends on the docked edge).
 */
static void reflection_axis_gradient(GtkSnapshot *snapshot, double w, double h,
                                     gboolean vertical, gboolean from_begin,
                                     double length,
                                     const GskColorStop *stops, gsize n_stops) {
  graphene_rect_t bounds;
  graphene_point_t start;
  graphene_point_t end;

  if (vertical) {
    graphene_rect_init(&bounds, 0.0f, 0.0f, (float)w, (float)h);
    float y0 = from_begin ? 0.0f : (float)h;
    float y1 = from_begin ? (float)length : (float)(h - length);
    graphene_point_init(&start, (float)(w / 2.0), y0);
    graphene_point_init(&end, (float)(w / 2.0), y1);
  } else {
    graphene_rect_init(&bounds, 0.0f, 0.0f, (float)w, (float)h);
    float x0 = from_begin ? 0.0f : (float)w;
    float x1 = from_begin ? (float)length : (float)(w - length);
    graphene_point_init(&start, x0, (float)(h / 2.0));
    graphene_point_init(&end, x1, (float)(h / 2.0));
  }

  gtk_snapshot_append_linear_gradient(snapshot, &bounds, &start, &end, stops,
                                      n_stops);
}

/**
 * reflection_paint_contact_shadow:
 * Soft dark halo where the icon meets the glass, so the icon reads as resting
 * on the shelf instead of floating above it.
 */
static void reflection_paint_contact_shadow(const LambdockReflection *self,
                                            GtkSnapshot *snapshot, double w,
                                            double h) {
  gboolean vertical = reflection_is_vertical_band(self);
  double axis_len = vertical ? h : w;
  double shadow_len = MIN(axis_len * 0.8, MAX(5.0, axis_len * 0.45));
  if (shadow_len <= 0.0)
    return;

  GskColorStop stops[3] = {
      {(float)0.0, {0.05f, 0.07f, 0.10f, 0.18f}},
      {(float)0.35, {0.05f, 0.07f, 0.10f, 0.10f}},
      {(float)1.0, {0.05f, 0.07f, 0.10f, 0.0f}},
  };

  /* The icon-side edge is the beginning of the band for a bottom dock and the
   * end for a top dock; side docks use the horizontal axis. */
  reflection_axis_gradient(snapshot, w, h, vertical,
                           self->edge == GTK_LAYER_SHELL_EDGE_BOTTOM ||
                               self->edge == GTK_LAYER_SHELL_EDGE_LEFT,
                           shadow_len, stops, 3);
}

/**
 * reflection_paint_running_dot:
 * The small white glow the OS X dock showed near the outer edge of the glass
 * for apps with open windows.
 */
static void reflection_paint_running_dot(const LambdockReflection *self,
                                         GtkSnapshot *snapshot, double w,
                                         double h) {
  gboolean vertical = reflection_is_vertical_band(self);
  double cross = vertical ? w : h;

  double dot_cross = MAX(4.0, cross * 0.16);
  double dot_along = MAX(3.0, dot_cross * 0.55);
  double margin = 3.0;

  double cx = w / 2.0;
  double cy = h / 2.0;

  if (vertical) {
    cy = (self->edge == GTK_LAYER_SHELL_EDGE_BOTTOM) ? (h - dot_along - margin)
                                                     : (dot_along + margin);
  } else {
    cx = (self->edge == GTK_LAYER_SHELL_EDGE_LEFT) ? (w - dot_along - margin)
                                                   : (dot_along + margin);
  }

  double radius_cross = MAX(dot_cross / 2.0, 2.0);
  double radius_along = MAX(dot_along / 2.0, 2.0);

  graphene_rect_t bounds = GRAPHENE_RECT_INIT(
      (float)(cx - radius_cross), (float)(cy - radius_along),
      (float)(radius_cross * 2.0), (float)(radius_along * 2.0));

  GskColorStop stops[3] = {
      {(float)0.0, {1.0f, 1.0f, 1.0f, 0.90f}},
      {(float)0.45, {0.94f, 0.98f, 1.0f, 0.35f}},
      {(float)1.0, {0.92f, 0.96f, 1.0f, 0.0f}},
  };

  graphene_point_t center;
  graphene_point_init(&center, (float)cx, (float)cy);

  gtk_snapshot_append_radial_gradient(snapshot, &bounds, &center,
                                      (float)radius_cross,
                                      (float)radius_along, 0.0f, 1.0f, stops,
                                      3);
}

/* ============================================================================
 * GTK WIDGET VIRTUAL METHODS
 * ============================================================================ */

static void lambdock_reflection_measure(GtkWidget *widget,
                                        GtkOrientation orientation,
                                        int for_size, int *minimum,
                                        int *natural, int *minimum_baseline,
                                        int *natural_baseline) {
  (void)for_size;
  LambdockReflection *self = LAMBDOCK_REFLECTION(widget);

  double sw = 0.0;
  double sh = 0.0;
  reflection_source_size(self, &sw, &sh);

  /* The band keeps the source's cross size and is band_ratio of it along the
   * mirror axis. */
  gboolean vertical_band = reflection_is_vertical_band(self);
  double cross = vertical_band ? sw : sh;
  double band = (vertical_band ? sh : sw) * self->band_ratio;

  int cross_px = (int)round(cross);
  int band_px = (int)round(band);
  if (cross_px < 0)
    cross_px = 0;
  if (band_px < 0)
    band_px = 0;

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    *minimum = 0;
    *natural = cross_px;
  } else {
    *minimum = 0;
    *natural = band_px;
  }

  if (minimum_baseline)
    *minimum_baseline = -1;
  if (natural_baseline)
    *natural_baseline = -1;
}

static void lambdock_reflection_snapshot(GtkWidget *widget,
                                         GtkSnapshot *snapshot) {
  LambdockReflection *self = LAMBDOCK_REFLECTION(widget);

  reflection_sync_paintable(self);

  double w = (double)gtk_widget_get_width(widget);
  double h = (double)gtk_widget_get_height(widget);
  if (w <= 0.0 || h <= 0.0)
    return;

  gboolean vertical_band = reflection_is_vertical_band(self);

  if (self->has_shadow)
    reflection_paint_contact_shadow(self, snapshot, w, h);

  double sw = 0.0;
  double sh = 0.0;
  reflection_source_size(self, &sw, &sh);

  if (self->paintable && sw > 0.0 && sh > 0.0) {
    /* Mirror + squash: map the source box onto the reflection band so that the
     * edge touching the icon becomes the mirror plane. */
    float sx = (float)(w / sw);
    float sy = (float)(h / sh);
    float tx = 0.0f;
    float ty = 0.0f;

    if (vertical_band) {
      if (self->edge == GTK_LAYER_SHELL_EDGE_BOTTOM) {
        /* Mirror about the icon's bottom edge: the shelf reflects downwards. */
        sy = -sy;
        ty = (float)h;
      }
    } else if (self->edge == GTK_LAYER_SHELL_EDGE_LEFT) {
      /* Mirror about the icon's right edge: the shelf reflects sideways. */
      sx = -sx;
      tx = (float)w;
    }

    double blur_unit = vertical_band ? w : h;
    double blur = blur_unit * self->blur_ratio;
    if (blur < 0.5)
      blur = 0.5;

    gtk_snapshot_push_blur(snapshot, blur);

    gtk_snapshot_save(snapshot);
    gtk_snapshot_transform(
        snapshot, gsk_transform_matrix_2d(NULL, sx, 0.0f, 0.0f, sy, tx, ty));

    /* Draw the mirrored source as a stack of bands with a power-curve alpha
     * falloff, so the reflection fades away as it recedes from the icon. The
     * count follows the band height: sub-pixel bands would be quantised away
     * and leave visible gaps. */
    double axis_len = vertical_band ? h : w;
    int bands = (int)round(axis_len);
    bands = CLAMP(bands, 6, SHELF_REFLECTION_BANDS);
    double band_len = axis_len / (double)bands;

    for (int i = 0; i < bands; i++) {
      double d0 = band_len * (double)i;
      double d1 = d0 + band_len;
      double t = (d0 + band_len * 0.5) / axis_len; /* 0 = icon edge, 1 = far */

      double alpha = self->intensity * pow(1.0 - t, 1.35);
      if (alpha <= 0.001)
        continue;

      /* Map the destination band back into source coordinates for clipping. */
      graphene_rect_t clip;
      if (vertical_band) {
        if (self->edge == GTK_LAYER_SHELL_EDGE_BOTTOM) {
          double y1 = sh - (d0 / h) * sh;
          double y0 = sh - (d1 / h) * sh;
          graphene_rect_init(&clip, 0.0f, (float)y0, (float)sw,
                             (float)(y1 - y0));
        } else {
          double y0 = (d0 / h) * sh;
          double y1 = (d1 / h) * sh;
          graphene_rect_init(&clip, 0.0f, (float)y0, (float)sw,
                             (float)(y1 - y0));
        }
      } else {
        if (self->edge == GTK_LAYER_SHELL_EDGE_LEFT) {
          double x1 = sw - (d0 / w) * sw;
          double x0 = sw - (d1 / w) * sw;
          graphene_rect_init(&clip, (float)x0, 0.0f, (float)(x1 - x0),
                             (float)sh);
        } else {
          double x0 = (d0 / w) * sw;
          double x1 = (d1 / w) * sw;
          graphene_rect_init(&clip, (float)x0, 0.0f, (float)(x1 - x0),
                             (float)sh);
        }
      }

      gtk_snapshot_push_clip(snapshot, &clip);
      gtk_snapshot_push_opacity(snapshot, alpha);
      gdk_paintable_snapshot(self->paintable, snapshot, sw, sh);
      gtk_snapshot_pop(snapshot);
      gtk_snapshot_pop(snapshot);
    }

    gtk_snapshot_restore(snapshot);
    gtk_snapshot_pop(snapshot); /* blur */
  }

  if (self->running)
    reflection_paint_running_dot(self, snapshot, w, h);
}

static void lambdock_reflection_dispose(GObject *object) {
  LambdockReflection *self = LAMBDOCK_REFLECTION(object);

  if (self->source) {
    g_object_remove_weak_pointer(G_OBJECT(self->source),
                                 (gpointer *)&self->source);
    self->source = NULL;
  }
  g_clear_object(&self->paintable);

  G_OBJECT_CLASS(lambdock_reflection_parent_class)->dispose(object);
}

static void lambdock_reflection_class_init(LambdockReflectionClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = lambdock_reflection_dispose;

  widget_class->measure = lambdock_reflection_measure;
  widget_class->snapshot = lambdock_reflection_snapshot;
}

static void lambdock_reflection_init(LambdockReflection *self) {
  self->source = NULL;
  self->paintable = NULL;
  self->edge = GTK_LAYER_SHELL_EDGE_BOTTOM;
  self->icon_size = DEFAULT_ICON_SIZE;
  self->band_ratio = SHELF_DEFAULT_REFLECTION;
  self->blur_ratio = SHELF_DEFAULT_REFLECTION_BLUR;
  self->intensity = SHELF_DEFAULT_REFLECTION_INTENSITY;
  self->running = FALSE;
  self->has_shadow = TRUE;

  gtk_widget_set_can_focus(GTK_WIDGET(self), FALSE);
  gtk_widget_set_can_target(GTK_WIDGET(self), FALSE);
  gtk_widget_set_halign(GTK_WIDGET(self), GTK_ALIGN_CENTER);
  gtk_widget_set_valign(GTK_WIDGET(self), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(GTK_WIDGET(self), "shelf-reflection");
}

/* ============================================================================
 * LambdockShelfFrame — projects the styled capsule onto a tilted 3D plane
 * ============================================================================
 *
 * GTK's CSS `transform` is affine only (no usable perspective()), so the tilt
 * cannot be done from CSS. This container wraps the themed capsule box and
 * re-renders it through a real projective GskTransform, which gives the
 * trapezoidal shelf (narrower on the far side) while the icons keep standing
 * upright on top of it.
 *
 * With `tilt == 0` it is a transparent pass-through, so the flat dock is
 * pixel-identical to the unwrapped capsule.
 */

typedef struct _LambdockShelfFrame {
  GtkWidget parent_instance;

  GtkLayerShellEdge edge; /* Docked edge: tilt axis + pinned edge */
  double tilt;            /* Tilt angle in degrees (0 = disabled) */
  double perspective;     /* Projective distance in pixels */
} LambdockShelfFrame;

typedef struct _LambdockShelfFrameClass {
  GtkWidgetClass parent_class;
} LambdockShelfFrameClass;

GType lambdock_shelf_frame_get_type(void);
G_DEFINE_TYPE(LambdockShelfFrame, lambdock_shelf_frame, GTK_TYPE_WIDGET)

#define LAMBDOCK_TYPE_SHELF_FRAME (lambdock_shelf_frame_get_type())
#define LAMBDOCK_SHELF_FRAME(obj)                                             \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), LAMBDOCK_TYPE_SHELF_FRAME,               \
                              LambdockShelfFrame))

static GtkWidget *frame_child(GtkWidget *frame) {
  return gtk_widget_get_first_child(frame);
}

static void lambdock_shelf_frame_measure(GtkWidget *widget,
                                         GtkOrientation orientation,
                                         int for_size, int *minimum,
                                         int *natural, int *minimum_baseline,
                                         int *natural_baseline) {
  GtkWidget *child = frame_child(widget);

  if (child && gtk_widget_should_layout(child)) {
    gtk_widget_measure(child, orientation, for_size, minimum, natural,
                       minimum_baseline, natural_baseline);
    return;
  }

  *minimum = 0;
  *natural = 0;
  if (minimum_baseline)
    *minimum_baseline = -1;
  if (natural_baseline)
    *natural_baseline = -1;
}

static void lambdock_shelf_frame_size_allocate(GtkWidget *widget, int width,
                                               int height, int baseline) {
  GtkWidget *child = frame_child(widget);
  if (child && gtk_widget_should_layout(child))
    gtk_widget_allocate(child, width, height, baseline, NULL);
}

static void lambdock_shelf_frame_snapshot(GtkWidget *widget,
                                          GtkSnapshot *snapshot) {
  LambdockShelfFrame *self = LAMBDOCK_SHELF_FRAME(widget);
  GtkWidget *child = frame_child(widget);

  if (!child || !gtk_widget_should_layout(child))
    return;

  if (self->tilt == 0.0) {
    gtk_widget_snapshot_child(widget, child, snapshot);
    return;
  }

  double width = (double)gtk_widget_get_width(widget);
  double height = (double)gtk_widget_get_height(widget);
  gboolean depth_axis = (self->edge == GTK_LAYER_SHELL_EDGE_TOP ||
                         self->edge == GTK_LAYER_SHELL_EDGE_BOTTOM);

  /* Tilt around the docked edge: that edge stays pinned on screen while the
   * inner edge recedes and is projected slightly narrower. */
  graphene_point_t pivot;
  switch (self->edge) {
  case GTK_LAYER_SHELL_EDGE_TOP:
    graphene_point_init(&pivot, (float)(width / 2.0), 0.0f);
    break;
  case GTK_LAYER_SHELL_EDGE_LEFT:
    graphene_point_init(&pivot, 0.0f, (float)(height / 2.0));
    break;
  case GTK_LAYER_SHELL_EDGE_RIGHT:
    graphene_point_init(&pivot, (float)width, (float)(height / 2.0));
    break;
  case GTK_LAYER_SHELL_EDGE_BOTTOM:
  default:
    graphene_point_init(&pivot, (float)(width / 2.0), (float)height);
    break;
  }

  graphene_vec3_t axis;
  if (depth_axis)
    graphene_vec3_init(&axis, 1.0f, 0.0f, 0.0f);
  else
    graphene_vec3_init(&axis, 0.0f, 1.0f, 0.0f);

  graphene_point_t origin = GRAPHENE_POINT_INIT(-pivot.x, -pivot.y);
  GskTransform *transform = gsk_transform_translate(NULL, &origin);
  transform = gsk_transform_rotate_3d(transform, self->tilt, &axis);
  transform = gsk_transform_perspective(transform, (float)self->perspective);
  transform = gsk_transform_translate(transform, &pivot);

  gtk_snapshot_save(snapshot);
  gtk_snapshot_transform(snapshot, transform);
  gtk_widget_snapshot_child(widget, child, snapshot);
  gtk_snapshot_restore(snapshot);
}

static void lambdock_shelf_frame_dispose(GObject *object) {
  GtkWidget *child = frame_child(GTK_WIDGET(object));
  if (child)
    gtk_widget_unparent(child);

  G_OBJECT_CLASS(lambdock_shelf_frame_parent_class)->dispose(object);
}

static void lambdock_shelf_frame_class_init(LambdockShelfFrameClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = lambdock_shelf_frame_dispose;

  widget_class->measure = lambdock_shelf_frame_measure;
  widget_class->size_allocate = lambdock_shelf_frame_size_allocate;
  widget_class->snapshot = lambdock_shelf_frame_snapshot;
}

static void lambdock_shelf_frame_init(LambdockShelfFrame *self) {
  self->edge = GTK_LAYER_SHELL_EDGE_BOTTOM;
  self->tilt = 0.0;
  self->perspective = SHELF_DEFAULT_PERSPECTIVE;

  gtk_widget_set_can_focus(GTK_WIDGET(self), FALSE);
  gtk_widget_set_can_target(GTK_WIDGET(self), FALSE);
  gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_VISIBLE);
  gtk_widget_add_css_class(GTK_WIDGET(self), "shelf-frame");
}

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

GtkWidget *shelf_reflection_new(GtkWidget *source) {
  LambdockReflection *self = g_object_new(LAMBDOCK_TYPE_REFLECTION, NULL);

  if (source && GTK_IS_WIDGET(source)) {
    self->source = source;
    g_object_add_weak_pointer(G_OBJECT(source), (gpointer *)&self->source);
    self->paintable = gtk_widget_paintable_new(source);
  }

  return GTK_WIDGET(self);
}

void shelf_reflection_configure(GtkWidget *reflection, GtkLayerShellEdge edge,
                                int icon_size, double band_ratio,
                                double blur_ratio, double intensity) {
  if (!shelf_reflection_is_reflection(reflection))
    return;

  LambdockReflection *self = LAMBDOCK_REFLECTION(reflection);
  gboolean changed = self->edge != edge;

  self->edge = edge;
  if (icon_size > 0 && icon_size != self->icon_size) {
    self->icon_size = icon_size;
    changed = TRUE;
  }
  if (self->band_ratio != band_ratio) {
    self->band_ratio = band_ratio;
    changed = TRUE;
  }
  self->blur_ratio = blur_ratio;
  self->intensity = intensity;

  if (changed)
    gtk_widget_queue_resize(reflection);
  gtk_widget_queue_draw(reflection);
}

void shelf_reflection_set_extent(GtkWidget *reflection, int icon_size,
                                 int band_size) {
  if (!shelf_reflection_is_reflection(reflection) || icon_size <= 0 ||
      band_size <= 0)
    return;

  LambdockReflection *self = LAMBDOCK_REFLECTION(reflection);

  if (reflection_is_vertical_band(self)) {
    gtk_widget_set_size_request(reflection, icon_size, band_size);
  } else {
    gtk_widget_set_size_request(reflection, band_size, icon_size);
  }
}

void shelf_reflection_set_icon_size(GtkWidget *reflection, int icon_size) {
  if (!shelf_reflection_is_reflection(reflection) || icon_size <= 0)
    return;

  LambdockReflection *self = LAMBDOCK_REFLECTION(reflection);
  shelf_reflection_set_extent(reflection, icon_size,
                              reflection_band_size(self, icon_size));
}

void shelf_reflection_set_running(GtkWidget *reflection, gboolean running) {
  if (!shelf_reflection_is_reflection(reflection))
    return;

  LambdockReflection *self = LAMBDOCK_REFLECTION(reflection);
  if (self->running == running)
    return;

  self->running = running;
  gtk_widget_queue_draw(reflection);
}

gboolean shelf_reflection_is_reflection(gpointer widget) {
  return widget != NULL && GTK_IS_WIDGET(widget) &&
         G_TYPE_CHECK_INSTANCE_TYPE(widget, LAMBDOCK_TYPE_REFLECTION);
}

GtkWidget *shelf_frame_new(GtkWidget *child) {
  LambdockShelfFrame *self = g_object_new(LAMBDOCK_TYPE_SHELF_FRAME, NULL);

  if (child && GTK_IS_WIDGET(child))
    gtk_widget_set_parent(child, GTK_WIDGET(self));

  return GTK_WIDGET(self);
}

void shelf_frame_set_tilt(GtkWidget *frame, GtkLayerShellEdge edge,
                          double degrees, double perspective) {
  if (!shelf_frame_is_frame(frame))
    return;

  LambdockShelfFrame *self = LAMBDOCK_SHELF_FRAME(frame);
  self->edge = edge;
  self->tilt = degrees;
  if (perspective > 0.0)
    self->perspective = perspective;

  gtk_widget_queue_draw(frame);
}

gboolean shelf_frame_is_frame(gpointer widget) {
  return widget != NULL && GTK_IS_WIDGET(widget) &&
         G_TYPE_CHECK_INSTANCE_TYPE(widget, LAMBDOCK_TYPE_SHELF_FRAME);
}

GtkWidget *shelf_wrap_item(LambdockState *state, GtkWidget *button,
                           GtkWidget *reflection) {
  if (!button)
    return NULL;

  /* Stack along the dock's cross axis: a bottom dock stacks icons vertically
   * with the reflection underneath, a side dock stacks them horizontally. */
  GtkOrientation stack_orientation =
      (state && state->orientation == GTK_ORIENTATION_VERTICAL)
          ? GTK_ORIENTATION_HORIZONTAL
          : GTK_ORIENTATION_VERTICAL;

  GtkWidget *container = gtk_box_new(stack_orientation, 0);
  gtk_widget_add_css_class(container, "shelf-item");
  gtk_widget_set_overflow(container, GTK_OVERFLOW_VISIBLE);
  gtk_widget_set_valign(container, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(container, GTK_ALIGN_CENTER);

  GtkLayerShellEdge edge = state ? state->edge : GTK_LAYER_SHELL_EDGE_BOTTOM;

  /* The reflection always goes on the outward side of the shelf. */
  gboolean reflection_first =
      (edge == GTK_LAYER_SHELL_EDGE_TOP || edge == GTK_LAYER_SHELL_EDGE_RIGHT);

  if (reflection && reflection_first)
    gtk_box_append(GTK_BOX(container), reflection);

  gtk_box_append(GTK_BOX(container), button);

  if (reflection && !reflection_first)
    gtk_box_append(GTK_BOX(container), reflection);

  return container;
}

void apply_shelf_layout(LambdockState *state) {
  if (!state || !state->dock_items)
    return;

  LambdockShelfParams params = lambdock_shelf_params_effective(state);
  int default_icon =
      (state->icon_size > 0) ? state->icon_size : DEFAULT_ICON_SIZE;
  LambdockShelfGeometry geometry =
      lambdock_shelf_geometry_for(state, default_icon);

  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    if (!item || !item->reflection)
      continue;

    if (geometry.band_px <= 0) {
      gtk_widget_set_visible(item->reflection, FALSE);
      continue;
    }

    /* Follow the current icon size so fisheye magnification is mirrored too,
     * but keep the band thickness tied to the *base* icon size: growing the
     * band would push the whole row of icons up while hovering. */
    int icon_px = default_icon;
    GtkWidget *image = NULL;
    if (item->button && GTK_IS_BUTTON(item->button)) {
      GtkWidget *child = gtk_button_get_child(GTK_BUTTON(item->button));
      if (GTK_IS_IMAGE(child)) {
        image = child;
      } else if (GTK_IS_BOX(child)) {
        for (GtkWidget *w = gtk_widget_get_first_child(child); w != NULL;
             w = gtk_widget_get_next_sibling(w)) {
          if (GTK_IS_IMAGE(w)) {
            image = w;
            break;
          }
        }
      }
    }
    if (image && gtk_image_get_pixel_size(GTK_IMAGE(image)) > 0) {
      icon_px = gtk_image_get_pixel_size(GTK_IMAGE(image));
    }

    shelf_reflection_configure(item->reflection, state->edge, icon_px,
                               params.reflection_ratio,
                               params.reflection_blur_ratio,
                               params.reflection_intensity);
    shelf_reflection_set_extent(item->reflection, icon_px, geometry.band_px);
    shelf_reflection_set_running(item->reflection, item->windows != NULL);
    gtk_widget_set_visible(item->reflection, TRUE);
  }
}
