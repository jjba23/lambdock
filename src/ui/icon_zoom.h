// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef ICON_ZOOM_H
#define ICON_ZOOM_H

#include <gtk/gtk.h>

/* =========================================================================
 * FISHEYE WIDGET
 * =========================================================================
 * A GtkWidget that magnifies its child by painting a transform, so hovering
 * never re-rasters the artwork at every intermediate size.
 *
 * Holds no dock state: the base size and maximum scale arrive at construction.
 */

/**
 * LambdockIconZoom:
 * A dock icon that can be drawn larger without being rasterised again.
 *
 * Fisheye magnification used to resize the icon's GtkImage for every pixel size
 * the pointer passed through (gtk_image_set_pixel_size()). An icon theme answers
 * each of those with a fresh lookup, decode and resample of the artwork --
 * milliseconds per icon, per frame -- which is what made hovering feel slow, and
 * it stepped through whole pixels, which made it look coarse.
 *
 * This widget keeps the artwork as a raster and paints it through a scale
 * transform. Magnifying then costs a matrix on the GPU, and because the transform
 * is a double the icon follows the pointer continuously instead of jumping a
 * pixel at a time. The artwork is resolved at the two sizes that matter -- the
 * resting size and the largest one -- and the widget switches between them while
 * magnifying, so neither state is drawn from a resampled copy of the other.
 *
 * The widget still reports the magnified size as its natural size, so the item
 * around it grows exactly as it did before and the row keeps spreading the
 * neighbours apart.
 */
#define LAMBDOCK_TYPE_ICON_ZOOM (lambdock_icon_zoom_get_type())
G_DECLARE_FINAL_TYPE(LambdockIconZoom, lambdock_icon_zoom, LAMBDOCK, ICON_ZOOM,
                     GtkWidget)

/**
 * lambdock_icon_zoom_new:
 * @child: (transfer none): the icon widget to hold, normally a GtkImage.
 * @base_size: the icon size the dock lays out at rest, in pixels.
 * @max_size: the largest size the icon is ever drawn at, in pixels.
 *
 * Takes ownership of @child. The icon is held at a fixed raster size and painted
 * through a transform, so magnifying it costs no rasterisation; the two sizes
 * that matter (resting and largest) are resolved properly, and only those.
 *
 * Returns: (transfer full): the zooming container.
 */
GtkWidget *lambdock_icon_zoom_new(GtkWidget *child, int base_size,
                                  int max_size);

/**
 * lambdock_icon_zoom_set_scale:
 * @zoom: a #LambdockIconZoom.
 * @scale: magnification factor, 1.0 being @base_size.
 *
 * Sets the painted size (and, in whole pixels, the size the widget reports to
 * its parent). Values below 1.0 are clamped: the dock never draws icons smaller
 * than their resting size. Repeated calls with the same value are free.
 */
void lambdock_icon_zoom_set_scale(GtkWidget *zoom, double scale);

/**
 * lambdock_icon_zoom_get_scale:
 * @zoom: a #LambdockIconZoom.
 *
 * Returns: the current magnification factor, or 1.0 for a non-zoom widget.
 */
double lambdock_icon_zoom_get_scale(GtkWidget *zoom);

/**
 * lambdock_icon_zoom_target_size:
 * @zoom: a #LambdockIconZoom.
 *
 * The size the icon is drawn at, rounded the way the layout sees it.
 *
 * Returns: the target size in pixels, or 0 for a non-zoom widget.
 */
int lambdock_icon_zoom_target_size(GtkWidget *zoom);

/**
 * lambdock_icon_zoom_get_child:
 * @zoom: a #LambdockIconZoom.
 *
 * Returns: (transfer none) (nullable): the icon widget the zoom holds.
 */
GtkWidget *lambdock_icon_zoom_get_child(GtkWidget *zoom);

/**
 * lambdock_icon_zoom_is_zoom:
 * @widget: (nullable): a widget.
 *
 * Returns: TRUE when @widget is a #LambdockIconZoom.
 */
gboolean lambdock_icon_zoom_is_zoom(gpointer widget);

#endif // ICON_ZOOM_H
