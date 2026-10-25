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

#ifndef SHELF_H
#define SHELF_H

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

/**
 * lambdock_shelf_geometry:
 * Derived shelf geometry for a state and icon size.
 *
 * @band_px: Height of the reflection band (0 when reflections are off).
 * @outer_px: Glass on the outer (screen edge) side of the icons.
 * @capsule_px: Capsule thickness across the dock.
 * @overhang_px: How far the icons stick out on the inner side (>= 0).
 */
typedef struct LambdockShelfGeometry {
  int band_px;
  int outer_px;
  int capsule_px;
  int overhang_px;
} LambdockShelfGeometry;

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
