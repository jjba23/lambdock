// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock. If not, see <https://www.gnu.org/licenses/>.

/* * INCLUDE GUARD: Prevents double-inclusion errors during compilation.
 * If DOCK_WINDOW_H is not yet defined, define it and parse the file.
 */
#ifndef DOCK_WINDOW_H
#define DOCK_WINDOW_H

/* Include core project types and structures (such as LambdockState) */
#include "lambdock.h"

/* =========================================================================
 * WINDOW & LAYER SHELL SETUP PROTOTYPES
 * ========================================================================= */

/**
 * setup_layer_shell_window:
 * @state: Pointer to main Lambdock application state struct.
 *
 * Configures Wayland Layer Shell surface properties based on user configuration
 * stored in state (anchoring edge, exclusive zone depth, layer level).
 */
void setup_layer_shell_window(LambdockState *state);

/**
 * setup_dock_motion_controller:
 * @state: Pointer to the central application state (LambdockState).
 *
 * Instantiates and attaches a GTK 4 EventControllerMotion to track mouse
 * cursor enter and leave events over the dock window.
 */
void setup_dock_motion_controller(LambdockState *state);

/* =========================================================================
 * DOCK VISIBILITY & ANIMATION PROTOTYPES
 * ========================================================================= */

/**
 * reveal_dock:
 * @state: Pointer to the central application state.
 *
 * Cancels any pending auto-hide timer and triggers a smooth slide-in
 * animation to display the dock at full visibility.
 */
void reveal_dock(LambdockState *state);

/**
 * hide_dock:
 * @state: Pointer to the central application state.
 *
 * Triggers a slide-out animation to move the dock off-screen, provided
 * auto-hide is enabled and no active popover context menus are open.
 */
void hide_dock(LambdockState *state);

/**
 * schedule_initial_autohide:
 * @state: Pointer to the central application state.
 *
 * Sets up an initial delay timer (1000ms) to slide the dock away after
 * application launch if auto-hide mode is turned on.
 */
void schedule_initial_autohide(LambdockState *state);

/* =========================================================================
 * SIGNAL CALLBACK PROTOTYPES (GTK / GLib Event Handlers)
 * ========================================================================= */

/**
 * on_quit_action:
 * @action: The GSimpleAction triggered by user interface/menu.
 * @parameter: Action target parameter (unused).
 * @user_data: Pointer to the running GtkApplication context.
 *
 * Callback triggered to gracefully close and exit the GTK application.
 */
void on_quit_action(GSimpleAction *action, GVariant *parameter,
                    gpointer user_data);

/**
 * on_right_click:
 * @gesture: The GtkGestureClick object handling press input.
 * @n_press: Click count (1 for single click, 2 for double, etc.).
 * @x: Relative X mouse coordinate of the click.
 * @y: Relative Y mouse coordinate of the click.
 * @user_data: Pointer to the application state struct (LambdockState).
 *
 * Callback triggered on secondary mouse press (right-click) to position
 * and display a context popover menu.
 */
void on_right_click(GtkGestureClick *gesture, gint n_press, gdouble x,
                    gdouble y, gpointer user_data);

/**
 * on_popover_closed:
 * @popover: The GtkPopover widget that was closed.
 * @user_data: Pointer to the application state struct (LambdockState).
 *
 * Callback executed when a popover menu closes, decrementing active menu
 * counts and resuming auto-hide timers.
 */
void on_popover_closed(GtkPopover *popover, gpointer user_data);

#endif // DOCK_WINDOW_H
