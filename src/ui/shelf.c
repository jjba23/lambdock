// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "shelf.h"
#include "../core/render_stats.h"
#include "icon_zoom.h"
#include "../logger.h"
#include "dock_item.h"
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
  params.flare_px = SHELF_DEFAULT_FLARE_PX;

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
      params.reflection_ratio == 0.0 && params.tilt_deg == 0.0 &&
      params.flare_px == 0)
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
         params.flare_px != defaults.flare_px ||
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
  GdkPaintable *paintable;  /* What gets mirrored */
  gboolean paintable_ready; /* FALSE until a lazy (widget) paintable is bound */
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
  if (self->paintable_ready)
    return;
  if (!self->source || !gtk_widget_get_mapped(self->source))
    return;
  if (gtk_widget_get_width(self->source) <= 0 ||
      gtk_widget_get_height(self->source) <= 0)
    return;

  /* Second chance for a widget paintable: once the source is mapped and sized,
   * a fresh one does paint. */
  g_set_object(&self->paintable, gtk_widget_paintable_new(self->source));
  self->paintable_ready = TRUE;
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
 * The small glowing dot the OS X dock showed near the outer edge of the glass
 * for apps with open windows.
 *
 * The dot is white -- that is what the real one was on the 3D shelf -- but a
 * white glow disappears into a pale slab (the platinum Mavericks and Aqua Tiger
 * shelves are exactly that), so it is drawn over a soft dark halo. On the light
 * material the halo reads as the bead's shadow and the dot stays visible; on
 * dark glass the halo blends into the shelf and only the glow shows, which is
 * what the dark themes want. The halo is nearly twice the dot's size, so it
 * never turns the glow into a grey ring.
 */
static void reflection_paint_running_dot(const LambdockReflection *self,
                                         GtkSnapshot *snapshot, double w,
                                         double h) {
  gboolean vertical = reflection_is_vertical_band(self);
  double cross = vertical ? w : h;

  /* The OS X dot reads as a small spotlight on the glass: wide enough to see at a
   * glance, shallow enough to stay a dot rather than a bar. */
  double dot_cross = MAX(7.0, cross * 0.24);
  double dot_along = MAX(4.0, dot_cross * 0.52);
  double margin = 2.5;

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

  graphene_point_t center;
  graphene_point_init(&center, (float)cx, (float)cy);

  /* The bead's shadow, drawn first so the glow sits on top of it. */
  const double halo_scale = 1.8;
  graphene_rect_t halo_bounds = GRAPHENE_RECT_INIT(
      (float)(cx - radius_cross * halo_scale),
      (float)(cy - radius_along * halo_scale),
      (float)(radius_cross * 2.0 * halo_scale),
      (float)(radius_along * 2.0 * halo_scale));

  GskColorStop halo_stops[3] = {
      {(float)0.0, {0.0f, 0.0f, 0.0f, 0.30f}},
      {(float)0.55, {0.0f, 0.0f, 0.0f, 0.14f}},
      {(float)1.0, {0.0f, 0.0f, 0.0f, 0.0f}},
  };

  gtk_snapshot_append_radial_gradient(snapshot, &halo_bounds, &center,
                                      (float)(radius_cross * halo_scale),
                                      (float)(radius_along * halo_scale), 0.0f,
                                      1.0f, halo_stops, 3);

  graphene_rect_t bounds = GRAPHENE_RECT_INIT(
      (float)(cx - radius_cross), (float)(cy - radius_along),
      (float)(radius_cross * 2.0), (float)(radius_along * 2.0));

  GskColorStop stops[3] = {
      {(float)0.0, {1.0f, 1.0f, 1.0f, 0.90f}},
      {(float)0.45, {0.94f, 0.98f, 1.0f, 0.35f}},
      {(float)1.0, {0.92f, 0.96f, 1.0f, 0.0f}},
  };

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

  lambdock_stats_bump(LAMBDOCK_STAT_REFLECTION_PAINTS);

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
    /* Mirror the artwork at full scale across the edge that touches the icon and
     * let the glass crop it: a reflection does not shrink, it just disappears
     * into the surface it fades on. Only the part of the icon within the band's
     * depth is ever visible, which is what makes the mirror read as a reflection
     * instead of a squashed copy.
     *
     * GskTransform composes the other way round from the way it reads: the
     * argument is applied *after* the new operation, so `scale(translate(..))'
     * means "translate, then scale" and it is the form that keeps the translation
     * in the widget's own coordinates. */
    float sx = (float)(w / sw);
    float sy = 1.0f;
    float tx = 0.0f;
    float ty = 0.0f;

    if (vertical_band) {
      if (self->edge == GTK_LAYER_SHELL_EDGE_BOTTOM) {
        /* Mirror about the icon's bottom edge: the shelf reflects downwards. */
        sy = -1.0f;
        ty = (float)sh;
      } else {
        /* Top dock: the icon's top edge is the mirror plane. */
        ty = 0.0f;
      }
    } else if (self->edge == GTK_LAYER_SHELL_EDGE_LEFT) {
      /* Mirror about the icon's right edge: the shelf reflects sideways. */
      sx = -sx;
      tx = (float)sw;
    }

    double blur_unit = vertical_band ? w : h;
    double blur = blur_unit * self->blur_ratio;
    if (blur < 0.5)
      blur = 0.5;

    graphene_point_t mirror_offset = GRAPHENE_POINT_INIT(tx, ty);
    GskTransform *mirror =
        gsk_transform_scale(gsk_transform_translate(NULL, &mirror_offset), sx,
                            sy);

    gtk_snapshot_push_blur(snapshot, blur);

    /* The reflection fades as it recedes from the icon: draw the mirrored artwork
     * once per slice, with a power-curve alpha. Each slice's clip is pushed
     * *before* the mirror transform, so it is expressed in the widget's own
     * coordinates. */
    double axis_len = vertical_band ? h : w;
    int bands = lambdock_reflection_band_count(axis_len);
    double band_len = axis_len / (double)bands;

    for (int i = 0; i < bands; i++) {
      double d0 = band_len * (double)i;
      double t = (d0 + band_len * 0.5) / axis_len; /* 0 = icon edge, 1 = far */

      double alpha = self->intensity * pow(1.0 - t, 1.1);
      if (alpha <= 0.001)
        continue;

      graphene_rect_t clip =
          vertical_band
              ? GRAPHENE_RECT_INIT(0.0f, (float)d0, (float)w, (float)band_len)
              : GRAPHENE_RECT_INIT((float)d0, 0.0f, (float)band_len, (float)h);

      gtk_snapshot_push_clip(snapshot, &clip);
      gtk_snapshot_push_opacity(snapshot, alpha);
      gtk_snapshot_save(snapshot);
      gtk_snapshot_transform(snapshot, mirror);
      gdk_paintable_snapshot(self->paintable, snapshot, sw, sh);
      lambdock_stats_bump(LAMBDOCK_STAT_REFLECTION_DRAWS);
      gtk_snapshot_restore(snapshot);
      gtk_snapshot_pop(snapshot); /* opacity */
      gtk_snapshot_pop(snapshot); /* clip */
    }

    gtk_snapshot_pop(snapshot); /* blur */
    gsk_transform_unref(mirror);
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
  /* The contact shadow is off: drawn through a linear gradient its fade ends up
   * covering the whole band instead of the few pixels under the icon, which is
   * exactly the flat grey strip a reflection must not have. The mirror alone is
   * what the OS X dock shows. */
  self->has_shadow = FALSE;

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
 * Two independent projections live here:
 *
 *   - the *tilt*, a rotation away from the viewer around the docked edge, and
 *   - the *flare*, a pure keystone that keeps the edge touching the screen at
 *     full width and pulls the inner edge in by a number of pixels per side.
 *
 * The flare is the one that makes the OS X dock's silhouette: its slab is wider
 * where it meets the screen than at the edge the icons overhang, which is what
 * reads as a 3D shelf.
 *
 * With `tilt == 0` and `flare == 0` it is a transparent pass-through, so the
 * flat dock is pixel-identical to the unwrapped capsule.
 */

typedef struct _LambdockShelfFrame {
  GtkWidget parent_instance;

  GtkLayerShellEdge edge; /* Docked edge: tilt axis + pinned edge */
  double tilt;            /* Tilt angle in degrees (0 = disabled) */
  double perspective;     /* Projective distance in pixels */
  int flare;              /* Keystone lean per side, in pixels */
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
  LambdockShelfFrame *self = LAMBDOCK_SHELF_FRAME(widget);

  if (child && gtk_widget_should_layout(child)) {
    gtk_widget_measure(child, orientation, for_size, minimum, natural,
                       minimum_baseline, natural_baseline);

    /* The keystone widens the screen-side edge, so the frame has to reserve that
     * much room across the dock or the flare would be clipped by the surface. */
    gboolean horizontal =
        (self->edge == GTK_LAYER_SHELL_EDGE_TOP ||
         self->edge == GTK_LAYER_SHELL_EDGE_BOTTOM);
    gboolean cross_axis = (horizontal && orientation == GTK_ORIENTATION_HORIZONTAL) ||
                          (!horizontal && orientation == GTK_ORIENTATION_VERTICAL);
    if (cross_axis && self->flare > 0) {
      *minimum += 2 * self->flare;
      *natural += 2 * self->flare;
    }
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
  if (!child || !gtk_widget_should_layout(child))
    return;

  LambdockShelfFrame *self = LAMBDOCK_SHELF_FRAME(widget);
  gboolean horizontal =
      (self->edge == GTK_LAYER_SHELL_EDGE_TOP ||
       self->edge == GTK_LAYER_SHELL_EDGE_BOTTOM);

  /* The child is the slab as the theme draws it: it keeps the dock's width on
   * the inner edge, and the projection below spreads it out to fill the room
   * reserved for the flare. */
  int inset = (self->flare > 0) ? self->flare : 0;
  GskTransform *offset = NULL;
  if (inset > 0) {
    graphene_point_t origin = horizontal
                                  ? GRAPHENE_POINT_INIT((float)inset, 0.0f)
                                  : GRAPHENE_POINT_INIT(0.0f, (float)inset);
    offset = gsk_transform_translate(NULL, &origin);
  }

  /* gtk_widget_allocate() takes ownership of the transform. */
  gtk_widget_allocate(child, horizontal ? width - 2 * inset : width,
                      horizontal ? height : height - 2 * inset, baseline,
                      offset);
}

/**
 * shelf_frame_keystone:
 * @width: Frame width in pixels.
 * @height: Frame height in pixels.
 * @horizontal: TRUE when the dock runs horizontally (top/bottom edge).
 * @flare: Pixels the edge on the screen is wider than the inner edge, per side.
 * @pinned_at_end: TRUE when the edge on the screen is the bottom/right one.
 * @out: Destination matrix.
 *
 * Builds the keystone that gives the dock its OS X silhouette: the inner edge
 * keeps the dock's width, and the edge lying on the screen is drawn @flare pixels
 * wider per side, as if the slab were a shelf seen slightly from above.
 *
 * A keystone has to be a real projection -- a shear would slant both sides the
 * same way, while this widens symmetrically around the middle -- so the matrix
 * carries a `w` that depends on depth. It is written out directly instead of
 * being composed from translate/project/translate, because translations do not
 * commute with a projective row and the composed version drifts sideways.
 *
 * The mapping it builds, for a bottom dock (depth = y, cross = x, cx = width/2):
 *
 *   w(y) = 1 - c * y                    with c = (1 - pinned_scale) / height
 *   x'   = (x - cx) / w(y) + cx         ->  (x - cx*c*y, y) / w(y)
 *   y'   = y / w(y)
 *
 * so the top edge (y = 0, w = 1) is untouched and the bottom edge is magnified by
 * 1 / pinned_scale around the centre line.
 *
 * Returns: TRUE when @out was filled in.
 */
static gboolean shelf_frame_keystone(int width, int height,
                                     gboolean horizontal, int flare,
                                     gboolean pinned_at_end,
                                     graphene_matrix_t *out) {
  double depth = horizontal ? (double)height : (double)width;
  /* The frame reserves `2 * flare` on the cross axis for the widened edge, so the
   * slab itself spans the rest. */
  double cross = (horizontal ? (double)width : (double)height) - 2.0 * flare;

  if (flare <= 0 || depth <= 0.0 || cross <= 0.0 || !out)
    return FALSE;

  /* Keep the slab at most 1.5x as wide at the screen edge: past that the
   * projection folds in on itself. */
  double max_flare = cross * 0.25;
  double amount = MIN((double)flare, max_flare);

  /* w on the edge that touches the screen; dividing by it magnifies that edge. */
  double pinned_scale = (cross - 2.0 * amount) / cross;
  double sweep = (1.0 - pinned_scale) / depth; /* d(w)/d(depth) */
  double centre = (horizontal ? (double)width : (double)height) / 2.0;

  /* Row 0 and 2 of the matrix: the cross coordinate picks up the projected
   * offset, and (for a top/left dock) the constant part of w. */
  double cross_from_depth = 0.0; /* the cross coordinate picks up depth */
  double cross_constant = 0.0;
  double w_from_depth;
  double w_constant;

  if (pinned_at_end) {
    /* w(depth) = 1 - sweep * depth, so the far edge (depth 0) is untouched. */
    cross_from_depth = -centre * sweep;
    w_from_depth = -sweep;
    w_constant = 1.0;
  } else {
    /* w(depth) = pinned_scale + sweep * depth, so the pinned edge (depth 0) is
     * the magnified one. */
    cross_constant = centre * (pinned_scale - 1.0);
    cross_from_depth = centre * sweep;
    w_from_depth = sweep;
    w_constant = pinned_scale;
  }

  /* graphene stores matrices row-major and applies them to row vectors, so
   * (x', y', w') = (x, y, 1) * M and the result is divided by w'. The `w' row is
   * row 3; the cross coordinate is row 0 for a horizontal dock and row 1 for a
   * vertical one, and it is the *column* of the depth axis that picks up the
   * projection. */
  float values[16] = {
      1.0f, 0.0f, 0.0f, 0.0f, /* row 0: x */
      0.0f, 1.0f, 0.0f, 0.0f, /* row 1: y */
      0.0f, 0.0f, 1.0f, 0.0f, /* row 2: z */
      0.0f, 0.0f, 0.0f, 1.0f, /* row 3: w */
  };

  if (horizontal) {
    values[4] = (float)cross_from_depth;  /* m10: x picks up y */
    values[12] = (float)cross_constant;   /* m30 */
    values[7] = (float)w_from_depth;      /* m13: w picks up y */
    values[15] = (float)w_constant;       /* m33 */
  } else {
    values[1] = (float)cross_from_depth;  /* m01: y picks up x */
    values[13] = (float)cross_constant;   /* m31 */
    values[3] = (float)w_from_depth;      /* m03: w picks up x */
    values[15] = (float)w_constant;       /* m33 */
  }

  graphene_matrix_init_from_float(out, values);
  return TRUE;
}

/**
 * shelf_frame_transform_new:
 * The projection the frame applies to the slab, as one transform: the keystone
 * (which gives the slab its trapezoid) followed by the tilt (which tips it away
 * from the viewer).
 *
 * Both the frame's own painting and anything that has to sit on the slab -- dock
 * separators, which must lean with it -- go through this, so they cannot drift
 * apart.
 *
 * Returns: (transfer full) (nullable): the transform, or NULL when the shelf is
 * a plain rectangle that needs no projection.
 */
static GskTransform *shelf_frame_transform_new(const LambdockShelfFrame *self,
                                               int width, int height) {
  if (!self || (self->tilt == 0.0 && self->flare == 0))
    return NULL;
  if (width <= 0 || height <= 0)
    return NULL;

  gboolean depth_axis = (self->edge == GTK_LAYER_SHELL_EDGE_TOP ||
                         self->edge == GTK_LAYER_SHELL_EDGE_BOTTOM);
  gboolean pinned_at_end = (self->edge == GTK_LAYER_SHELL_EDGE_BOTTOM ||
                            self->edge == GTK_LAYER_SHELL_EDGE_RIGHT);

  graphene_matrix_t keystone;
  GskTransform *keystone_transform = NULL;
  if (shelf_frame_keystone(width, height, depth_axis, self->flare,
                           pinned_at_end, &keystone))
    keystone_transform = gsk_transform_matrix(NULL, &keystone);

  GskTransform *tilt_transform = NULL;
  if (self->tilt != 0.0) {
    /* Tilt around the docked edge: that edge stays pinned on screen while the
     * inner edge recedes and is projected slightly narrower. */
    graphene_point_t pivot;
    switch (self->edge) {
    case GTK_LAYER_SHELL_EDGE_TOP:
      graphene_point_init(&pivot, (float)width / 2.0f, 0.0f);
      break;
    case GTK_LAYER_SHELL_EDGE_LEFT:
      graphene_point_init(&pivot, 0.0f, (float)height / 2.0f);
      break;
    case GTK_LAYER_SHELL_EDGE_RIGHT:
      graphene_point_init(&pivot, (float)width, (float)height / 2.0f);
      break;
    case GTK_LAYER_SHELL_EDGE_BOTTOM:
    default:
      graphene_point_init(&pivot, (float)width / 2.0f, (float)height);
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
    tilt_transform = gsk_transform_translate(transform, &pivot);
    gsk_transform_unref(transform);
  }

  if (!keystone_transform)
    return tilt_transform;
  if (!tilt_transform)
    return keystone_transform;

  /* The keystone is applied to the tilted slab, as the frame has always drawn
   * it: `gsk_transform_transform(next, previous)` applies previous first. */
  GskTransform *composed =
      gsk_transform_transform(keystone_transform, tilt_transform);
  gsk_transform_unref(keystone_transform);
  gsk_transform_unref(tilt_transform);
  return composed;
}

static void lambdock_shelf_frame_snapshot(GtkWidget *widget,
                                          GtkSnapshot *snapshot) {
  LambdockShelfFrame *self = LAMBDOCK_SHELF_FRAME(widget);
  GtkWidget *child = frame_child(widget);

  if (!child || !gtk_widget_should_layout(child))
    return;

  GskTransform *transform = shelf_frame_transform_new(
      self, gtk_widget_get_width(widget), gtk_widget_get_height(widget));

  if (!transform) {
    gtk_widget_snapshot_child(widget, child, snapshot);
    return;
  }

  gtk_snapshot_save(snapshot);
  gtk_snapshot_transform(snapshot, transform);
  gtk_widget_snapshot_child(widget, child, snapshot);
  gtk_snapshot_restore(snapshot);

  gsk_transform_unref(transform);
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

/**
 * reflection_artwork_for:
 * @source: The widget to mirror (normally the item's GtkImage).
 *
 * Returns the *artwork* behind an icon as a paintable, taking a new reference.
 *
 * A GtkImage usually holds an icon name rather than a paintable, so mirroring the
 * widget (a GtkWidgetPaintable) is the only other option -- and that one depends
 * on GTK having drawn the widget through its parent already, which it often has
 * not when the dock is being built. Resolving the icon name (or gicon) against
 * the icon theme gives the real picture immediately, at exactly the size the dock
 * draws it.
 *
 * Returns: (transfer full) (nullable): the artwork, or NULL when the image holds
 * something this helper does not understand.
 */
static GdkPaintable *reflection_artwork_for(GtkWidget *source) {
  if (!GTK_IS_IMAGE(source))
    return NULL;

  GtkImage *image = GTK_IMAGE(source);
  int size = gtk_image_get_pixel_size(image);
  if (size <= 0)
    size = DEFAULT_ICON_SIZE;

  int scale = gtk_widget_get_scale_factor(source);
  if (scale <= 0)
    scale = 1;

  GtkIconTheme *theme =
      gtk_icon_theme_get_for_display(gdk_display_get_default());

  switch (gtk_image_get_storage_type(image)) {
  case GTK_IMAGE_ICON_NAME: {
    const char *name = gtk_image_get_icon_name(image);
    if (name && *name != '\0' && theme) {
      GtkIconPaintable *paintable =
          gtk_icon_theme_lookup_icon(theme, name, NULL, size, scale,
                                     GTK_TEXT_DIR_NONE, (GtkIconLookupFlags)0);
      if (paintable)
        return GDK_PAINTABLE(paintable);
    }
    break;
  }
  case GTK_IMAGE_GICON: {
    GIcon *icon = gtk_image_get_gicon(image);
    if (icon && theme) {
      GtkIconPaintable *paintable = gtk_icon_theme_lookup_by_gicon(
          theme, icon, size, scale, GTK_TEXT_DIR_NONE, (GtkIconLookupFlags)0);
      if (paintable)
        return GDK_PAINTABLE(paintable);
    }
    break;
  }
  case GTK_IMAGE_PAINTABLE: {
    GdkPaintable *paintable = gtk_image_get_paintable(image);
    if (paintable)
      return g_object_ref(paintable);
    break;
  }
  default:
    break;
  }

  return NULL;
}

int lambdock_reflection_band_count(double axis_len) {
  if (!(axis_len > 0.0))
    return SHELF_REFLECTION_BANDS;

  int bands = (int)round(axis_len);

  return CLAMP(bands, 6, SHELF_REFLECTION_BANDS);
}

GtkWidget *shelf_reflection_new(GtkWidget *source) {
  LambdockReflection *self = g_object_new(LAMBDOCK_TYPE_REFLECTION, NULL);

  if (source && GTK_IS_WIDGET(source)) {
    self->source = source;
    g_object_add_weak_pointer(G_OBJECT(source), (gpointer *)&self->source);

    /* Mirror the artwork itself, not the widget that shows it: see
     * reflection_artwork_for(). */
    self->paintable = reflection_artwork_for(source);
    if (self->paintable) {
      self->paintable_ready = TRUE;
    } else {
      self->paintable = gtk_widget_paintable_new(source);
      self->paintable_ready = FALSE;
    }
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

void shelf_reflection_clear_source(GtkWidget *reflection) {
  if (!shelf_reflection_is_reflection(reflection))
    return;

  LambdockReflection *self = LAMBDOCK_REFLECTION(reflection);

  if (self->source) {
    g_object_remove_weak_pointer(G_OBJECT(self->source),
                                 (gpointer *)&self->source);
    self->source = NULL;
  }
  g_clear_object(&self->paintable);

  /* Nothing left to bind, and nothing that could ever appear again: the widget
   * is now a plain slot of the band's height. */
  self->paintable_ready = TRUE;
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
  shelf_frame_set_shape(frame, edge, degrees, perspective,
                        shelf_frame_is_frame(frame)
                            ? LAMBDOCK_SHELF_FRAME(frame)->flare
                            : 0);
}

void shelf_frame_set_shape(GtkWidget *frame, GtkLayerShellEdge edge,
                           double degrees, double perspective, int flare_px) {
  if (!shelf_frame_is_frame(frame))
    return;

  LambdockShelfFrame *self = LAMBDOCK_SHELF_FRAME(frame);
  gboolean geometry_changed = (self->edge != edge) || (self->flare != flare_px);
  gboolean paint_changed = geometry_changed || (self->tilt != degrees);

  self->edge = edge;
  self->tilt = degrees;
  self->flare = flare_px;
  if (perspective > 0.0)
    self->perspective = perspective;

  /* The flare reserves room in the layout, so a change in it has to re-measure;
   * the tilt only changes how the same slab is painted. */
  if (geometry_changed)
    gtk_widget_queue_resize(frame);
  else if (paint_changed)
    gtk_widget_queue_draw(frame);
}

gboolean shelf_frame_is_frame(gpointer widget) {
  return widget != NULL && GTK_IS_WIDGET(widget) &&
         G_TYPE_CHECK_INSTANCE_TYPE(widget, LAMBDOCK_TYPE_SHELF_FRAME);
}

gboolean shelf_frame_is_projected(GtkWidget *frame) {
  if (!shelf_frame_is_frame(frame))
    return FALSE;

  LambdockShelfFrame *self = LAMBDOCK_SHELF_FRAME(frame);
  return self->tilt != 0.0 || self->flare != 0;
}

GtkOrientation shelf_frame_cross_orientation(GtkWidget *frame) {
  if (!shelf_frame_is_frame(frame))
    return GTK_ORIENTATION_VERTICAL;

  LambdockShelfFrame *self = LAMBDOCK_SHELF_FRAME(frame);

  /* A dock on the top or bottom edge runs horizontally, so the slab's depth --
   * the axis across the dock -- is vertical. */
  gboolean horizontal_dock = (self->edge == GTK_LAYER_SHELL_EDGE_TOP ||
                              self->edge == GTK_LAYER_SHELL_EDGE_BOTTOM);
  return horizontal_dock ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL;
}

gboolean shelf_frame_project_point(GtkWidget *frame, graphene_point_t *point) {
  if (!shelf_frame_is_frame(frame) || !point)
    return FALSE;

  LambdockShelfFrame *self = LAMBDOCK_SHELF_FRAME(frame);
  GskTransform *transform =
      shelf_frame_transform_new(self, gtk_widget_get_width(frame),
                                gtk_widget_get_height(frame));
  if (!transform)
    return FALSE;

  gsk_transform_transform_point(transform, point, point);
  gsk_transform_unref(transform);
  return TRUE;
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

/**
 * item_icon_size:
 * @item: a dock item.
 *
 * The size the item's icon is drawn at right now: the magnified size while the
 * fisheye is scaling it, the resting size otherwise. (This lives here because
 * the reflection is the only consumer; it used to sit in dock_item.c and made
 * the shelf unusable without linking the whole item implementation.)
 */
static int item_icon_size(const DockItem *item) {
  if (!item || !item->button || !GTK_IS_BUTTON(item->button))
    return 0;

  GtkWidget *child = gtk_button_get_child(GTK_BUTTON(item->button));
  if (!child)
    return 0;

  /* App and dynamic items hold their icon inside a zoom container; stacks, the
   * launchpad and meter items put it straight in the content box. */
  if (GTK_IS_BOX(child)) {
    for (GtkWidget *w = gtk_widget_get_first_child(child); w != NULL;
         w = gtk_widget_get_next_sibling(w)) {
      if (lambdock_icon_zoom_is_zoom(w))
        return lambdock_icon_zoom_target_size(w);
      if (GTK_IS_IMAGE(w) && gtk_image_get_pixel_size(GTK_IMAGE(w)) > 0)
        return gtk_image_get_pixel_size(GTK_IMAGE(w));
    }
    return 0;
  }

  if (lambdock_icon_zoom_is_zoom(child))
    return lambdock_icon_zoom_target_size(child);
  if (GTK_IS_IMAGE(child) && gtk_image_get_pixel_size(GTK_IMAGE(child)) > 0)
    return gtk_image_get_pixel_size(GTK_IMAGE(child));

  return 0;
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
    int icon_px = item_icon_size(item);
    if (icon_px <= 0)
      icon_px = default_icon;

    shelf_reflection_configure(item->reflection, state->edge, icon_px,
                               params.reflection_ratio,
                               params.reflection_blur_ratio,
                               params.reflection_intensity);
    shelf_reflection_set_extent(item->reflection, icon_px, geometry.band_px);
    shelf_reflection_set_running(item->reflection, item->windows != NULL);
    gtk_widget_set_visible(item->reflection, TRUE);
  }
}
