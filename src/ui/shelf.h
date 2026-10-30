// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef SHELF_H
#define SHELF_H

#include "../core/shelf_params.h"
#include "../lambdock.h"

/*
 * =========================================================================
 * THE GLASS SHELF
 * =========================================================================
 * The dock has no modes. It is one widget tree whose shape is described by
 * `LambdockShelfParams`:
 *
 *   - `LambdockShelfFrame` projects the themed capsule onto a tilted plane
 *     (a real GskTransform rotate + perspective, which GTK CSS cannot do), and
 *   - `LambdockReflection` paints a mirrored, blurred and alpha-faded copy of
 *     each icon on the glass, plus the contact shadow and running indicator.
 *
 * Geometry (all values in pixels, `icon` = icon size):
 *
 *   band   = round(icon * reflection_ratio)
 *   outer  = max(front_px, band)          // glass on the screen-edge side
 *   capsule = icon + margin_px + outer
 *
 * With the defaults (tilt 0, reflection 0, front 0, margin = capsule padding)
 * that is exactly the classic `icon + 24px` capsule, the frame is a pass-through
 * and the reflections stay hidden.
 */

/* The geometry value (`LambdockShelfGeometry`) and the parameters
 * (`LambdockShelfParams`) live in core/shelf_params.h, so the pure core headers
 * can name them without including a widget header. */


/**
 * lambdock_shelf_geometry:
 * Computes the derived geometry described above for @state (NULL is allowed and
 * means "defaults") at the given icon size.
 */
LambdockShelfGeometry lambdock_shelf_geometry_for(const LambdockState *state,
                                                  int icon_size);

/**
 * shelf_reflection_set_running:
 * @reflection: A widget created by shelf_reflection_new().
 * @running: TRUE when the item has at least one open window.
 *
 * Toggles the running-app glow dot drawn near the outer edge of the glass.
 */
void shelf_reflection_set_running(GtkWidget *reflection, gboolean running);

/**
 * shelf_reflection_new:
 * @source: Widget whose rendering should be mirrored (usually the item's
 * GtkImage). May be NULL, in which case the reflection stays blank.
 *
 * Creates a `LambdockReflection` widget: a mirrored, alpha-faded and gaussian
 * blurred copy of @source that is meant to be placed on the dock's glass shelf
 * right next to the icon it reflects.
 *
 * Returns: (transfer none): the newly created reflection widget.
 */
GtkWidget *shelf_reflection_new(GtkWidget *source);

/**
 * lambdock_reflection_band_count:
 * @axis_len: length of the reflection band along its fade axis, in pixels.
 *
 * Returns: how many clipped draws one reflection repaint costs. The fade is
 * painted as a stack of bands, so this is the per-repaint budget of the whole
 * widget: it is a pure function, and unit tested, because "a few bands" is not a
 * budget.
 */
int lambdock_reflection_band_count(double axis_len);

/**
 * shelf_reflection_clear_source:
 * @reflection: A widget created by shelf_reflection_new().
 *
 * Drops the artwork being mirrored while keeping the widget, so the band it
 * reserves in the row stays reserved. Used when an item's artwork goes away (a
 * dynamic item that turns into a gauge) but its slot must still line up with
 * the icons.
 */
void shelf_reflection_clear_source(GtkWidget *reflection);

/**
 * shelf_reflection_configure:
 * @reflection: A widget created by shelf_reflection_new().
 * @edge: Screen edge the dock is attached to. Decides the mirror axis and the
 * direction the reflection recedes towards.
 * @icon_size: Icon size in pixels, used while the source has no allocation.
 * @band_ratio: Reflection band height as a fraction of the icon size.
 * @blur_ratio: Gaussian blur radius as a fraction of the icon size.
 * @intensity: Alpha of the reflection where it touches the icon (0..1).
 *
 * Applies the shelf parameters to one reflection and queues a redraw.
 */
void shelf_reflection_configure(GtkWidget *reflection, GtkLayerShellEdge edge,
                                int icon_size, double band_ratio,
                                double blur_ratio, double intensity);

/**
 * shelf_reflection_set_extent:
 * @reflection: A widget created by shelf_reflection_new().
 * @icon_size: Pixel size of the icon across the dock axis.
 * @band_size: Thickness of the reflection band along the mirror axis.
 *
 * Sizes the reflection explicitly. The fisheye magnification uses this to grow
 * the reflection sideways with the icon while keeping the band thickness fixed,
 * so magnifying one icon does not shift the whole row vertically.
 */
void shelf_reflection_set_extent(GtkWidget *reflection, int icon_size,
                                 int band_size);

/**
 * shelf_reflection_set_icon_size:
 * @reflection: A widget created by shelf_reflection_new().
 * @icon_size: Pixel size of the icon being reflected.
 *
 * Convenience wrapper around shelf_reflection_set_extent() that derives the
 * band thickness from the configured band ratio.
 */
void shelf_reflection_set_icon_size(GtkWidget *reflection, int icon_size);

/**
 * shelf_reflection_is_reflection:
 * @widget: Pointer to inspect (may be NULL).
 *
 * Returns: TRUE when @widget is a `LambdockReflection`.
 */
gboolean shelf_reflection_is_reflection(gpointer widget);

/**
 * shelf_frame_new:
 * @child: The styled capsule box (the widget that carries the theme's CSS).
 *
 * Wraps @child in a frame that can project it onto a tilted 3D plane. GTK CSS
 * only supports affine transforms, so the projective perspective that turns a
 * flat capsule into the glass shelf has to be applied through a snapshot
 * transform instead.
 *
 * Returns: (transfer none): the shelf frame widget.
 */
GtkWidget *shelf_frame_new(GtkWidget *child);

/**
 * shelf_frame_set_tilt:
 * @frame: A widget created by shelf_frame_new().
 * @edge: Screen edge the dock is attached to (decides the tilt axis and which
 * edge stays pinned on screen).
 * @degrees: Tilt angle. 0 disables the projection, turning the frame into a
 * plain pass-through container.
 * @perspective: Perspective distance in pixels (smaller = stronger 3D).
 *
 * Updates the frame geometry. NULL and non-frame widgets are ignored.
 */
void shelf_frame_set_tilt(GtkWidget *frame, GtkLayerShellEdge edge,
                          double degrees, double perspective);

/**
 * shelf_frame_set_shape:
 * @frame: A widget created by shelf_frame_new().
 * @edge: Screen edge the dock is attached to (decides the tilt axis, the
 * keystone direction and which edge stays pinned on screen).
 * @degrees: Tilt angle. 0 disables the tilt projection.
 * @perspective: Perspective distance in pixels (smaller = stronger 3D).
 * @flare_px: How much narrower the inner edge is than the edge touching the
 * screen, in pixels per side. 0 keeps the slab rectangular; a positive value
 * gives it the OS X dock's keystone silhouette.
 *
 * Sets the whole shape of the shelf in one call. NULL and non-frame widgets are
 * ignored.
 */
void shelf_frame_set_shape(GtkWidget *frame, GtkLayerShellEdge edge,
                           double degrees, double perspective, int flare_px);

/**
 * shelf_frame_is_frame:
 * @widget: Pointer to inspect (may be NULL).
 *
 * Returns: TRUE when @widget is a `LambdockShelfFrame`.
 */
gboolean shelf_frame_is_frame(gpointer widget);

/**
 * shelf_frame_project_point:
 * @frame: A widget created by shelf_frame_new().
 * @point: (inout): a point in the frame's own coordinate space. On success it is
 * replaced by where the slab draws it.
 *
 * Maps a point through the shelf's projection -- the keystone and the tilt, in
 * the same order the frame draws them -- so a widget that has to lie *on* the
 * slab can follow it. The perspective divide is applied: the result is where the
 * point ends up on screen, not a matrix product.
 *
 * Returns: TRUE when @point was projected, FALSE when the frame is a plain
 * rectangle (no tilt, no flare) or has no allocation yet.
 */
gboolean shelf_frame_project_point(GtkWidget *frame, graphene_point_t *point);

/**
 * shelf_frame_is_projected:
 * @frame: A widget created by shelf_frame_new().
 *
 * Whether the slab is drawn as a shape rather than a rectangle, i.e. whether
 * shelf_frame_project_point() would move anything.
 *
 * Returns: TRUE when the frame has a tilt or a keystone.
 */
gboolean shelf_frame_is_projected(GtkWidget *frame);

/**
 * shelf_frame_cross_orientation:
 * @frame: A widget created by shelf_frame_new().
 *
 * The axis the slab's depth runs along: vertical for a dock on the top or bottom
 * edge, horizontal for one on the left or right.
 *
 * Returns: the cross-axis orientation.
 */
GtkOrientation shelf_frame_cross_orientation(GtkWidget *frame);

/**
 * shelf_wrap_item:
 * @state: Pointer to the dock state.
 * @button: The item's button (or separator) widget.
 * @reflection: Reflection widget, or NULL for items without an icon.
 *
 * Builds the per-item container that stacks @button and @reflection along the
 * dock's cross axis, with the reflection placed on the outward side of the
 * shelf (below for a bottom dock, above for a top dock, and equivalently for
 * side docks).
 *
 * Returns: (transfer none): the container to append to state->box.
 */
GtkWidget *shelf_wrap_item(LambdockState *state, GtkWidget *button,
                           GtkWidget *reflection);

/**
 * apply_shelf_layout:
 * @state: Pointer to the dock state.
 *
 * Shows/hides and sizes every item's reflection according to the shelf
 * parameters (a reflection ratio of 0 hides them) and follows the fisheye
 * magnification. Safe to call on every reload/dimension update.
 */
void apply_shelf_layout(LambdockState *state);

#endif // SHELF_H
