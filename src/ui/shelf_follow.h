// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef SHELF_FOLLOW_H
#define SHELF_FOLLOW_H

#include <gtk/gtk.h>

/* =========================================================================
 * SHELF-FOLLOWING WIDGET
 * =========================================================================
 * A GtkWidget that re-projects itself so it appears to stand on the glass
 * shelf, used by separators.
 *
 * Reads the shelf geometry; it never owns or mutates the shelf itself.
 */

/**
 * LambdockShelfFollow:
 * A widget that lies on the glass shelf instead of in front of it.
 *
 * The shelf is not a rectangle: the keystone and the tilt project it into a
 * shape whose vertical lines lean away from the middle of the dock and stretch as
 * they come towards the viewer. A `GtkSeparator` drawn upright next to a
 * projected slab looks pasted on, because it keeps standing straight while the
 * surface behind it leans -- the real OS X dock's separators lean with the glass
 * they sit on.
 *
 * This container paints its child through the *local* part of the shelf's
 * projection, measured at the child's own place along the dock, so the child
 * follows the surface under it. With a flat capsule (no tilt, no flare) the
 * projection is the identity and the child is drawn exactly as it would be
 * outside this widget.
 */
#define LAMBDOCK_TYPE_SHELF_FOLLOW (shelf_follow_get_type())
G_DECLARE_FINAL_TYPE(LambdockShelfFollow, shelf_follow, LAMBDOCK, SHELF_FOLLOW,
                     GtkWidget)

/**
 * shelf_follow_new:
 * @child: (transfer none): the widget to draw on the shelf.
 * @frame: (nullable): the `LambdockShelfFrame` whose surface to follow.
 * @slab: (nullable): the capsule, i.e. the widget that is the glass itself. A
 * follower spanning it draws over the glass's depth instead of over the
 * icon-sized box the dock laid out.
 *
 * Takes ownership of @child. The other widgets are held weakly: when they go away
 * the follower simply stops projecting.
 *
 * Returns: (transfer full): the container to put in the dock.
 */
GtkWidget *shelf_follow_new(GtkWidget *child, GtkWidget *frame,
                            GtkWidget *slab);

/**
 * shelf_follow_is_follow:
 * @widget: (nullable): a widget.
 *
 * Returns: TRUE when @widget is a #LambdockShelfFollow.
 */
gboolean shelf_follow_is_follow(gpointer widget);

/**
 * shelf_follow_is_projected:
 * @follow: a #LambdockShelfFollow.
 *
 * Whether the child is currently being drawn through the shelf's projection.
 * Stylesheets use it to give the shelf-borne widgets the look of the tilted
 * slab rather than the flat one.
 *
 * Returns: TRUE when a projection is being applied.
 */
gboolean shelf_follow_is_projected(GtkWidget *follow);

#endif // SHELF_FOLLOW_H
