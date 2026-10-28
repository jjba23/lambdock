// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef METER_H
#define METER_H

#include "../lambdock.h"

/**
 * LambdockMeter:
 * A circular gauge that fits in one dock icon slot.
 *
 * Dynamic items used to be text only, which reads badly on a dock: the readings
 * hang next to the icons instead of looking like one. A meter is the icon
 * version of the same reading -- a ring that fills clockwise with the value and
 * changes colour as it gets more worrying, with the value itself written in the
 * middle ("42%", "54°C").
 *
 * It is drawn from scratch (GskPath arcs, stroked with round caps) so it scales
 * with dock-icon-size, and it takes its text colour from the widget's CSS
 * `color`, so it follows whatever theme the dock is wearing.
 */

/**
 * lambdock_meter_new:
 * Creates an empty meter (0%, no text).
 *
 * Returns: (transfer none): the meter widget.
 */
GtkWidget *lambdock_meter_new(void);

/**
 * lambdock_meter_set_value:
 * @meter: A widget created by lambdock_meter_new().
 * @fraction: How full the ring is, 0.0 .. 1.0 (clamped).
 * @text: The reading to write in the middle ("42%"). May be NULL to keep the
 * previous text.
 *
 * Updates the gauge. Cheap enough to call from a poll timer: it only stores the
 * values and queues a redraw.
 */
void lambdock_meter_set_value(GtkWidget *meter, double fraction,
                              const char *text);

/**
 * lambdock_meter_set_icon_size:
 * @meter: A widget created by lambdock_meter_new().
 * @icon_size: Square size to request for the widget, in pixels.
 *
 * Sizes the gauge like a dock icon, so it lines up with the launchers next to
 * it.
 */
void lambdock_meter_set_icon_size(GtkWidget *meter, int icon_size);

/**
 * lambdock_meter_is_meter:
 * @widget: (nullable): pointer to inspect.
 *
 * Returns: TRUE when @widget is a LambdockMeter.
 */
gboolean lambdock_meter_is_meter(gpointer widget);

/**
 * lambdock_meter_fraction:
 * @meter: A widget created by lambdock_meter_new().
 *
 * Returns: the stored fraction (0.0 when @meter is not a meter). Exposed for
 * the tests.
 */
double lambdock_meter_fraction(GtkWidget *meter);

#endif // METER_H
