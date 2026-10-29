// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef CLOCK_H
#define CLOCK_H

#include "../lambdock.h"

/* =========================================================================
 * THE CLOCK
 * =========================================================================
 * The analogue face for clock items: a squircle body with a brushed bezel, a
 * paper-coloured dial, tick marks, three hands and a glass highlight.
 *
 * Drawing only. The time is read from the system clock by the item's poll
 * callback, which calls lambdock_clock_set_time() once a second.
 */

/**
 * LambdockClock:
 * An analogue clock that fits in one dock icon slot.
 *
 * A clock item used to be a text label ("Thu Oct 29 13:07"), which made the end
 * of the dock read as a string rather than as part of a row of icons. The face
 * is drawn from scratch -- squircle geometry, gradients for the bezel and the
 * dial, strokes for the ticks and the hands -- so it scales with
 * dock-icon-size and needs no artwork at any size, and it keeps its own
 * skeuomorphic colours instead of following the theme (like the meter's tile):
 * a watch face is an object, and it reads the same on a light dock and a dark
 * one.
 */

/**
 * lambdock_clock_new:
 * Creates a face showing midnight, with no second hand drawn until it is set.
 *
 * Returns: (transfer none): the clock widget.
 */
GtkWidget *lambdock_clock_new(void);

/**
 * lambdock_clock_set_time:
 * @clock: A widget created by lambdock_clock_new().
 * @seconds_of_day: Seconds since local midnight (wrapped into a day).
 *
 * Points the hands at that time. Cheap enough to call from a poll timer: it only
 * stores the reading and queues a redraw.
 */
void lambdock_clock_set_time(GtkWidget *clock, int seconds_of_day);

/**
 * lambdock_clock_get_time:
 * @clock: A widget created by lambdock_clock_new().
 *
 * Returns: the reading the face was last given, in seconds since midnight.
 */
int lambdock_clock_get_time(GtkWidget *clock);

/**
 * lambdock_clock_set_icon_size:
 * @clock: A widget created by lambdock_clock_new().
 * @icon_size: Square size to request for the widget, in pixels.
 *
 * Resizes the face to one icon slot. Ignored for sizes below 8 pixels, which
 * could not draw a readable face.
 */
void lambdock_clock_set_icon_size(GtkWidget *clock, int icon_size);

/**
 * lambdock_clock_is_clock:
 * @widget: (nullable): any widget.
 *
 * Returns: TRUE when @widget is a clock face. The setter and getter above are
 * no-ops on anything else, so a caller that got a widget from somewhere else
 * cannot crash the dock.
 */
gboolean lambdock_clock_is_clock(gpointer widget);

#endif // CLOCK_H
