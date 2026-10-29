// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "shelf_follow.h"

#include "shelf.h"
#include "widget_utils.h"

#include <math.h>

/* ============================================================================
 * WIDGET STATE
 * ============================================================================
 */

struct _LambdockShelfFollow {
  GtkWidget parent_instance;

  GtkWidget *child;
  GtkWidget *frame; /* Weak: the shelf frame whose surface to follow. */
  GtkWidget *slab;  /* Weak: the capsule, i.e. how deep the glass is. */
};

G_DEFINE_FINAL_TYPE(LambdockShelfFollow, shelf_follow, GTK_TYPE_WIDGET)

/* ============================================================================
 * THE PROJECTION, MEASURED AT THE CHILD'S PLACE
 * ============================================================================
 */

/* The separator's lean is that of the slab under it, exaggerated. The shelf's
 * keystone is gentle -- the flare it was built from is a couple of dozen pixels
 * over the slab's depth -- while the dock this look comes from is shallower and
 * much more strongly projected, so at the same distance from the middle its
 * separators lean two or three times as far. Doubling the shear gives a
 * separator that reads as standing on a tilted surface at a glance instead of
 * one that merely looks a little off vertical. */
#define SHELF_FOLLOW_LEAN_EMPHASIS 2.0

/**
 * follow_local_matrix:
 * The shelf's projection, linearised around the child's own position.
 *
 * The slab's projection is projective: a vertical line on it leans further the
 * further it is from the middle of the dock, and stretches as it comes forward.
 * Nothing of that is affine, but a widget is only a few dozen pixels tall, so the
 * Jacobian of the projection at its centre describes it to within a fraction of a
 * pixel -- and unlike the full projection it can be applied as a transform.
 *
 * Returns: TRUE when @out holds a transform that moves the child, FALSE when
 * there is nothing to follow (no frame, no allocation, or a flat shelf).
 */
static gboolean follow_local_matrix(LambdockShelfFollow *self, GtkWidget *widget,
                                    graphene_matrix_t *out) {
  if (!self->frame || !shelf_frame_is_frame(self->frame))
    return FALSE;
  if (!shelf_frame_is_projected(self->frame))
    return FALSE;

  double width = (double)gtk_widget_get_width(widget);
  double height = (double)gtk_widget_get_height(widget);
  if (width <= 0.0 || height <= 0.0)
    return FALSE;

  /* Where the child sits on the slab, in the frame's coordinates. */
  graphene_rect_t bounds;
  if (!gtk_widget_compute_bounds(widget, self->frame, &bounds))
    return FALSE;

  graphene_point_t centre = GRAPHENE_POINT_INIT(
      bounds.origin.x + bounds.size.width / 2.0f,
      bounds.origin.y + bounds.size.height / 2.0f);
  graphene_point_t across = GRAPHENE_POINT_INIT(centre.x + 1.0f, centre.y);
  graphene_point_t down = GRAPHENE_POINT_INIT(centre.x, centre.y + 1.0f);

  if (!shelf_frame_project_point(self->frame, &centre) ||
      !shelf_frame_project_point(self->frame, &across) ||
      !shelf_frame_project_point(self->frame, &down))
    return FALSE;

  /* Columns of the Jacobian: where one pixel right and one pixel down end up. */
  float j00 = across.x - centre.x;
  float j10 = across.y - centre.y;
  float j01 = down.x - centre.x;
  float j11 = down.y - centre.y;

  /* The shear is the lean; the diagonal is the slab's own stretch, which stays
   * as it is. */
  j10 = (float)(j10 * SHELF_FOLLOW_LEAN_EMPHASIS);
  j01 = (float)(j01 * SHELF_FOLLOW_LEAN_EMPHASIS);

  if (!isfinite(j00) || !isfinite(j10) || !isfinite(j01) || !isfinite(j11))
    return FALSE;

  /* A projection that does nothing is not worth a render node. */
  if (fabs(j00 - 1.0f) < 0.0005f && fabs(j11 - 1.0f) < 0.0005f &&
      fabs(j01) < 0.0005f && fabs(j10) < 0.0005f)
    return FALSE;

  /* Applied to row vectors, and pivoting on the child's own centre so it stays
   * where the layout put it: p' = J (p - c) + c. */
  float cx = (float)(width / 2.0);
  float cy = (float)(height / 2.0);

  float values[16] = {
      j00, j10, 0.0f, 0.0f, /* row 0: x picks up x (j00) and y (j01) */
      j01, j11, 0.0f, 0.0f, /* row 1: y picks up x (j10) and y (j11) */
      0.0f, 0.0f, 1.0f, 0.0f,
      cx - (j00 * cx + j01 * cy), cy - (j10 * cx + j11 * cy), 0.0f, 1.0f,
  };

  graphene_matrix_init_from_float(out, values);
  return TRUE;
}

/* ============================================================================
 * GTK WIDGET VIRTUAL METHODS
 * ============================================================================
 */

static void shelf_follow_measure(GtkWidget *widget, GtkOrientation orientation,
                                 int for_size, int *minimum, int *natural,
                                 int *minimum_baseline, int *natural_baseline) {
  LambdockShelfFollow *self = LAMBDOCK_SHELF_FOLLOW(widget);

  if (self->child && gtk_widget_should_layout(self->child)) {
    gtk_widget_measure(self->child, orientation, for_size, minimum, natural,
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

/**
 * follow_slab_span:
 * Where the glass runs, relative to this widget: from its inner edge (the line
 * the icons stand on) to its front edge. A widget that belongs to the slab is
 * drawn over that span instead of over the box the dock laid out for it, which is
 * only as tall as an icon.
 *
 * Returns: TRUE when @top and @bottom were filled in with offsets from this
 * widget's own top edge.
 */
static gboolean follow_slab_span(LambdockShelfFollow *self, GtkWidget *widget,
                                 double *top, double *bottom) {
  if (!self->frame || !shelf_frame_is_frame(self->frame) || !self->slab)
    return FALSE;

  graphene_rect_t mine;
  graphene_rect_t slab;
  if (!gtk_widget_compute_bounds(widget, self->frame, &mine) ||
      !gtk_widget_compute_bounds(self->slab, self->frame, &slab))
    return FALSE;

  if (slab.size.height <= 0.0f || mine.size.height <= 0.0f)
    return FALSE;

  /* A couple of pixels short of either edge: the separator fades out before it
   * reaches them, and a line drawn right up to the lip reads as a scratch. */
  const double inset = 2.0;

  *top = (double)(slab.origin.y - mine.origin.y) + inset;
  *bottom = (double)(slab.origin.y + slab.size.height - mine.origin.y) - inset;
  return *bottom > *top + 1.0;
}

static void shelf_follow_size_allocate(GtkWidget *widget, int width, int height,
                                       int baseline) {
  LambdockShelfFollow *self = LAMBDOCK_SHELF_FOLLOW(widget);

  if (!self->child || !gtk_widget_should_layout(self->child))
    return;

  /* Stylesheets get to know that this widget sits on a projected slab: the
   * tilted separators have their own look, and the flat dock keeps the theme's. */
  lambdock_set_css_class(widget, "shelf-projected",
                         shelf_follow_is_projected(widget));

  GtkAllocation allocation = {0, 0, width, height};

  double top = 0.0;
  double bottom = 0.0;
  double span = 0.0;
  if (shelf_follow_is_projected(widget) && follow_slab_span(self, widget, &top, &bottom)) {
    /* Span the glass. A dock on the top or bottom edge stacks its items
     * vertically, so the slab's depth is the vertical extent; a side dock's is
     * the horizontal one. Across the dock the child keeps its own size, centred
     * where the layout put it. */
    span = bottom - top;

    GtkOrientation cross = shelf_frame_cross_orientation(self->frame);
    int min_span = 0;
    int natural_span = 0;
    gtk_widget_measure(self->child, cross, -1, &min_span, &natural_span, NULL,
                       NULL);
    if (span < (double)min_span)
      span = (double)min_span;

    int along = 0;
    int natural_along = 0;
    gtk_widget_measure(self->child, 1 - cross, -1, &along, &natural_along, NULL,
                       NULL);
    int across = (int)lround(span);

    if (cross == GTK_ORIENTATION_VERTICAL)
      allocation = (GtkAllocation){(width - natural_along) / 2, (int)lround(top),
                                   natural_along, across};
    else
      allocation = (GtkAllocation){(int)lround(top), (height - natural_along) / 2,
                                   across, natural_along};
  }

  gtk_widget_size_allocate(self->child, &allocation, baseline);
}

static void shelf_follow_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  LambdockShelfFollow *self = LAMBDOCK_SHELF_FOLLOW(widget);

  if (!self->child)
    return;

  graphene_matrix_t matrix;
  if (!follow_local_matrix(self, widget, &matrix)) {
    gtk_widget_snapshot_child(widget, self->child, snapshot);
    return;
  }

  GskTransform *transform = gsk_transform_matrix(NULL, &matrix);

  gtk_snapshot_save(snapshot);
  gtk_snapshot_transform(snapshot, transform);
  gtk_widget_snapshot_child(widget, self->child, snapshot);
  gtk_snapshot_restore(snapshot);

  gsk_transform_unref(transform);
}

static void shelf_follow_dispose(GObject *object) {
  LambdockShelfFollow *self = LAMBDOCK_SHELF_FOLLOW(object);

  if (self->frame) {
    g_object_remove_weak_pointer(G_OBJECT(self->frame), (gpointer *)&self->frame);
    self->frame = NULL;
  }

  if (self->slab) {
    g_object_remove_weak_pointer(G_OBJECT(self->slab), (gpointer *)&self->slab);
    self->slab = NULL;
  }

  if (self->child) {
    GtkWidget *child = self->child;
    self->child = NULL;
    gtk_widget_unparent(child);
  }

  G_OBJECT_CLASS(shelf_follow_parent_class)->dispose(object);
}

static void shelf_follow_class_init(LambdockShelfFollowClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  widget_class->measure = shelf_follow_measure;
  widget_class->size_allocate = shelf_follow_size_allocate;
  widget_class->snapshot = shelf_follow_snapshot;

  object_class->dispose = shelf_follow_dispose;
}

static void shelf_follow_init(LambdockShelfFollow *self) {
  (void)self;

  /* The projected child leans past the box the layout gave it. */
  gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_VISIBLE);
}

/* ============================================================================
 * PUBLIC API
 * ============================================================================
 */

GtkWidget *shelf_follow_new(GtkWidget *child, GtkWidget *frame,
                            GtkWidget *slab) {
  LambdockShelfFollow *self = g_object_new(LAMBDOCK_TYPE_SHELF_FOLLOW, NULL);

  if (frame && shelf_frame_is_frame(frame)) {
    self->frame = frame;
    g_object_add_weak_pointer(G_OBJECT(frame), (gpointer *)&self->frame);
  }

  if (slab && GTK_IS_WIDGET(slab)) {
    self->slab = slab;
    g_object_add_weak_pointer(G_OBJECT(slab), (gpointer *)&self->slab);
  }

  if (child) {
    self->child = child;
    gtk_widget_set_parent(child, GTK_WIDGET(self));
  }

  return GTK_WIDGET(self);
}

gboolean shelf_follow_is_follow(gpointer widget) {
  return widget != NULL && GTK_IS_WIDGET(widget) &&
         G_TYPE_CHECK_INSTANCE_TYPE(widget, LAMBDOCK_TYPE_SHELF_FOLLOW);
}

gboolean shelf_follow_is_projected(GtkWidget *follow) {
  if (!shelf_follow_is_follow(follow))
    return FALSE;

  LambdockShelfFollow *self = LAMBDOCK_SHELF_FOLLOW(follow);
  return self->frame != NULL && shelf_frame_is_projected(self->frame);
}
