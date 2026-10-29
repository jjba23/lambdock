// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "icon_zoom.h"
#include "../core/render_stats.h"

#include <math.h>

/* ============================================================================
 * WIDGET STATE
 * ============================================================================
 */

/* Magnification at which the artwork is re-resolved at its largest size, and the
 * lower one at which it goes back to the resting size. The gap between them is
 * hysteresis: without it an icon hovering exactly on the threshold would
 * re-resolve its artwork on every frame, which is the cost this widget exists to
 * avoid. Between the two the artwork is scaled from whichever size it holds,
 * which is invisible at these ratios (4-10%). */
#define ICON_ZOOM_HIRES_IN 1.10
#define ICON_ZOOM_HIRES_OUT 1.05

struct _LambdockIconZoom {
  GtkWidget parent_instance;

  GtkWidget *child;   /* The icon widget, kept as a raster of `raster` px. */
  int base_size;      /* Size the dock lays out at rest. */
  int max_size;       /* Largest size the icon is ever drawn at. */
  int raster;         /* Size the child is currently resolved at. */
  double scale;       /* 1.0 = base_size. */
  gboolean hi_res;    /* Currently holding the large raster. */
  gboolean paint_at_target; /* The child resamples itself: draw it at size. */
};

G_DEFINE_FINAL_TYPE(LambdockIconZoom, lambdock_icon_zoom, GTK_TYPE_WIDGET)

/* ============================================================================
 * PRIVATE HELPERS
 * ============================================================================
 */

/**
 * zoom_allocated_size:
 * The size the widget hands to its parent: the icon drawn at the current scale,
 * in whole pixels.
 */
static int zoom_allocated_size(const LambdockIconZoom *self) {
  int size = (int)lround((double)self->base_size * self->scale);
  return size > 0 ? size : self->base_size;
}

/**
 * zoom_sync_raster:
 * Switches the artwork between its resting and its largest size.
 *
 * Icon themes answer each pixel size with a fresh lookup and resample, which is
 * why the dock no longer asks for one per frame -- but the two sizes that matter
 * (how the icon rests, and how big it gets) are worth resolving properly. The
 * switch happens twice per hover at most, and only for icons whose artwork is
 * resolved by size (icon names and gicons); a paintable resamples itself, so it
 * keeps its one copy.
 */
static void zoom_sync_raster(LambdockIconZoom *self) {
  if (self->paint_at_target)
    return;

  gboolean want_hi = self->hi_res;
  if (self->max_size > self->base_size) {
    if (self->hi_res)
      want_hi = self->scale > ICON_ZOOM_HIRES_OUT;
    else
      want_hi = self->scale >= ICON_ZOOM_HIRES_IN;
  }

  if (want_hi == self->hi_res)
    return;

  self->hi_res = want_hi;
  self->raster = want_hi ? self->max_size : self->base_size;

  if (GTK_IS_IMAGE(self->child))
    gtk_image_set_pixel_size(GTK_IMAGE(self->child), self->raster);

  gtk_widget_queue_resize(GTK_WIDGET(self));
}

/* ============================================================================
 * GTK WIDGET VIRTUAL METHODS
 * ============================================================================
 */

static void lambdock_icon_zoom_measure(GtkWidget *widget,
                                       GtkOrientation orientation, int for_size,
                                       int *minimum, int *natural,
                                       int *minimum_baseline,
                                       int *natural_baseline) {
  LambdockIconZoom *self = LAMBDOCK_ICON_ZOOM(widget);

  /* Measure the child even though its answer is not used: GTK requires a
   * container to have measured a child before it allocates it, and this one
   * allocates the child at the size of the raster it holds. */
  if (self->child) {
    int child_minimum = 0;
    int child_natural = 0;
    gtk_widget_measure(self->child, orientation, for_size, &child_minimum,
                       &child_natural, NULL, NULL);
  }

  int size = zoom_allocated_size(self);

  /* Both axes: the dock's buttons grow with the icon in every direction, which
   * is how the row of icons spreads as the pointer passes. */
  *minimum = size;
  *natural = size;

  if (minimum_baseline)
    *minimum_baseline = -1;
  if (natural_baseline)
    *natural_baseline = -1;
}

static void lambdock_icon_zoom_size_allocate(GtkWidget *widget, int width,
                                             int height, int baseline) {
  LambdockIconZoom *self = LAMBDOCK_ICON_ZOOM(widget);

  if (!self->child)
    return;

  /* The child is allocated at the size of the raster it holds, centred in the
   * (usually smaller) allocation; the snapshot scales it down to the size on
   * screen, so it is never re-rasterised. A paintable child resamples itself and
   * is allocated the target size instead. */
  int size = self->paint_at_target ? MAX(width, height) : self->raster;
  GtkAllocation allocation = {(width - size) / 2, (height - size) / 2, size,
                              size};

  gtk_widget_size_allocate(self->child, &allocation, baseline);
}

static void lambdock_icon_zoom_snapshot(GtkWidget *widget,
                                        GtkSnapshot *snapshot) {
  LambdockIconZoom *self = LAMBDOCK_ICON_ZOOM(widget);

  if (!self->child)
    return;

  if (self->paint_at_target) {
    gtk_widget_snapshot_child(widget, self->child, snapshot);
    return;
  }

  double target = (double)zoom_allocated_size(self);
  double factor = target / (double)self->raster;

  if (fabs(factor - 1.0) < 0.001) {
    gtk_widget_snapshot_child(widget, self->child, snapshot);
    return;
  }

  /* Scale about the widget's centre. GskTransform applies the new operation
   * after the one it wraps, so translating first and scaling after means the
   * translation is in the widget's own coordinates. */
  float centre_x = (float)gtk_widget_get_width(widget) / 2.0f;
  float centre_y = (float)gtk_widget_get_height(widget) / 2.0f;
  float sx = (float)factor;
  float sy = (float)factor;

  graphene_point_t offset = GRAPHENE_POINT_INIT(centre_x * (1.0f - sx) / sx,
                                                centre_y * (1.0f - sy) / sy);
  GskTransform *transform =
      gsk_transform_scale(gsk_transform_translate(NULL, &offset), sx, sy);

  gtk_snapshot_save(snapshot);
  gtk_snapshot_transform(snapshot, transform);
  gtk_widget_snapshot_child(widget, self->child, snapshot);
  gtk_snapshot_restore(snapshot);

  gsk_transform_unref(transform);
}

static void lambdock_icon_zoom_dispose(GObject *object) {
  LambdockIconZoom *self = LAMBDOCK_ICON_ZOOM(object);

  if (self->child) {
    GtkWidget *child = self->child;
    self->child = NULL;
    gtk_widget_unparent(child);
  }

  G_OBJECT_CLASS(lambdock_icon_zoom_parent_class)->dispose(object);
}

static void lambdock_icon_zoom_class_init(LambdockIconZoomClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  widget_class->measure = lambdock_icon_zoom_measure;
  widget_class->size_allocate = lambdock_icon_zoom_size_allocate;
  widget_class->snapshot = lambdock_icon_zoom_snapshot;

  object_class->dispose = lambdock_icon_zoom_dispose;
}

static void lambdock_icon_zoom_init(LambdockIconZoom *self) {
  self->scale = 1.0;

  /* A magnified icon is drawn outside the box it was allocated, and every
   * ancestor the dock builds is built the same way. */
  gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_VISIBLE);
}

/* ============================================================================
 * PUBLIC API
 * ============================================================================
 */

GtkWidget *lambdock_icon_zoom_new(GtkWidget *child, int base_size,
                                  int max_size) {
  LambdockIconZoom *self = g_object_new(LAMBDOCK_TYPE_ICON_ZOOM, NULL);

  if (base_size <= 0)
    base_size = 48;
  if (max_size < base_size)
    max_size = base_size;

  self->base_size = base_size;
  self->max_size = max_size;
  self->raster = base_size;

  if (child) {
    /* Icons resolved by name are re-rasterised for every size they are asked
     * for, so they are held as a raster; anything else (a texture loaded from a
     * file) already resamples on the GPU at whatever size it is given. */
    GtkImageType storage = GTK_IS_IMAGE(child)
                               ? gtk_image_get_storage_type(GTK_IMAGE(child))
                               : GTK_IMAGE_EMPTY;
    self->paint_at_target = GTK_IS_IMAGE(child) &&
                            storage != GTK_IMAGE_ICON_NAME &&
                            storage != GTK_IMAGE_GICON;

    gtk_widget_set_overflow(child, GTK_OVERFLOW_VISIBLE);
    if (GTK_IS_IMAGE(child))
      gtk_image_set_pixel_size(GTK_IMAGE(child), self->raster);

    self->child = child;
    gtk_widget_set_parent(child, GTK_WIDGET(self));
  }

  return GTK_WIDGET(self);
}

void lambdock_icon_zoom_set_scale(GtkWidget *zoom, double scale) {
  if (!lambdock_icon_zoom_is_zoom(zoom))
    return;

  LambdockIconZoom *self = LAMBDOCK_ICON_ZOOM(zoom);

  if (!(scale > 0.0) || !isfinite(scale))
    scale = 1.0;
  if (scale < 1.0)
    scale = 1.0;

  if (fabs(scale - self->scale) < 0.002)
    return;

  int previous_size = zoom_allocated_size(self);

  self->scale = scale;
  zoom_sync_raster(self);

  /* Only the layout steps in whole pixels; the painting is continuous. */
  if (zoom_allocated_size(self) != previous_size)
    gtk_widget_queue_resize(zoom);
  else
    gtk_widget_queue_draw(zoom);
}

double lambdock_icon_zoom_get_scale(GtkWidget *zoom) {
  if (!lambdock_icon_zoom_is_zoom(zoom))
    return 1.0;

  return LAMBDOCK_ICON_ZOOM(zoom)->scale;
}

int lambdock_icon_zoom_target_size(GtkWidget *zoom) {
  if (!lambdock_icon_zoom_is_zoom(zoom))
    return 0;

  return zoom_allocated_size(LAMBDOCK_ICON_ZOOM(zoom));
}

GtkWidget *lambdock_icon_zoom_get_child(GtkWidget *zoom) {
  if (!lambdock_icon_zoom_is_zoom(zoom))
    return NULL;

  return LAMBDOCK_ICON_ZOOM(zoom)->child;
}

gboolean lambdock_icon_zoom_is_zoom(gpointer widget) {
  return widget != NULL && GTK_IS_WIDGET(widget) &&
         G_TYPE_CHECK_INSTANCE_TYPE(widget, LAMBDOCK_TYPE_ICON_ZOOM);
}
