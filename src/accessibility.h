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

#ifndef LAMBDOCK_ACCESSIBILITY_H
#define LAMBDOCK_ACCESSIBILITY_H

#include "lambdock.h"

/**
 * lambdock_system_animations_enabled:
 *
 * Reads GTK's `gtk-enable-animations` setting, which the desktop's
 * accessibility settings drive. TRUE means the system is happy with motion.
 */
gboolean lambdock_system_animations_enabled(void);

/**
 * lambdock_reduce_motion:
 * @state: (nullable): a dock, or NULL for the session-wide default.
 *
 * TRUE when motion should be reduced either because the dock's own
 * `dock-reduce-motion?` setting is on, or because the desktop disabled
 * animations. NULL only consults the desktop setting.
 */
gboolean lambdock_reduce_motion(const LambdockState *state);

/**
 * lambdock_animations_enabled:
 * @state: (nullable): a dock.
 *
 * The inverse of lambdock_reduce_motion(), for the animation call sites.
 */
gboolean lambdock_animations_enabled(const LambdockState *state);

/**
 * lambdock_accessible_update:
 * @widget: a widget implementing GtkAccessible.
 * @label: (nullable): the name a screen reader announces for the widget.
 * @description: (nullable): extra state or instructions.
 *
 * Sets the accessible label and description in one call. Passing NULL for
 * either keeps it unset. Safe to call repeatedly as an item's state changes.
 */
void lambdock_accessible_update(GtkWidget *widget, const char *label,
                                const char *description);

/**
 * lambdock_accessible_announce:
 * @widget: a widget implementing GtkAccessible, used to pick the screen reader.
 * @message: the sentence to read out.
 * @assertive: interrupt the screen reader instead of waiting for a pause.
 *
 * Speaks a change that has no visible focus move of its own, such as the
 * selection inside the stack's fan view. GTK gained announcements in 4.14, so
 * older toolkits simply say nothing and keep the description up to date.
 */
void lambdock_accessible_announce(GtkWidget *widget, const char *message,
                                  gboolean assertive);

/**
 * lambdock_watch_system_motion:
 * @callback: called whenever the desktop turns animations on or off.
 * @data: user data for @callback.
 *
 * Connects to the toolkit's animation setting so a running dock can stop
 * animating the moment the user enables "reduce motion" in the desktop's
 * accessibility panel, instead of at the next restart.
 *
 * Returns: the signal handler id, or 0 when GTK has no settings object.
 */
guint lambdock_watch_system_motion(GCallback callback, gpointer data);

#endif // LAMBDOCK_ACCESSIBILITY_H
