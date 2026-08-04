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

#ifndef DOCK_WINDOW_H
#define DOCK_WINDOW_H

#include "lambdock.h"

/**
 * setup_layer_shell_window:
 * @state: Pointer to global application state structure.
 *
 * Initializes Wayland Layer Shell protocols for the dock window surface,
 * configuring screen edge anchoring, layering level, and space reservation
 * rules.
 */
void setup_layer_shell_window(LambdockState *state);

/**
 * setup_dock_motion_controller:
 * @state: Pointer to global application state structure.
 *
 * Attaches a GTK 4 EventControllerMotion to track mouse enter/leave events,
 * triggering auto-hide timers and reveal animations.
 */
void setup_dock_motion_controller(LambdockState *state);

/**
 * reveal_dock:
 * @state: Pointer to global application state structure.
 *
 * Cancels pending auto-hide timers and triggers a smooth slide-in animation to
 * bring the dock into view.
 */
void reveal_dock(LambdockState *state);

/**
 * hide_dock:
 * @state: Pointer to global application state structure.
 *
 * Calculates off-screen negative margins and initiates a slide-out animation to
 * hide the dock surface.
 */
void hide_dock(LambdockState *state);

/**
 * schedule_initial_autohide:
 * @state: Pointer to global application state structure.
 *
 * Schedules an initial 1000ms delay timer upon application launch to hide the
 * dock if auto-hide mode is turned on.
 */
void schedule_initial_autohide(LambdockState *state);

/**
 * free_lambdock_state:
 * @state: Pointer to global application state structure to deallocate.
 *
 * Safely cleans up all allocated memory, iterating through linked lists and
 * freeing custom structs upon application shutdown.
 */
void free_lambdock_state(LambdockState *state);

/**
 * on_quit_action:
 * @action: Triggered GSimpleAction object.
 * @parameter: Action target variant parameter (unused).
 * @user_data: Pointer to running GtkApplication context.
 *
 * Callback signal handler to exit GLib's main event processing loop.
 */
void on_quit_action(GSimpleAction *action, GVariant *parameter,
                    gpointer user_data);

/**
 * on_right_click:
 * @gesture: GtkGestureClick object handling click input.
 * @n_press: Count of mouse presses.
 * @x: Relative X mouse coordinate of press.
 * @y: Relative Y mouse coordinate of press.
 * @user_data: Pointer to LambdockState context.
 *
 * Gesture handler displaying the background context popover menu on secondary
 * click.
 */
void on_right_click(GtkGestureClick *gesture, gint n_press, gdouble x,
                    gdouble y, gpointer user_data);

/**
 * on_popover_closed:
 * @popover: Closed GTK Popover widget.
 * @user_data: Pointer to LambdockState context.
 *
 * Decrements active popover counters when context menus close and resumes
 * auto-hide timers.
 */
void on_popover_closed(GtkPopover *popover, gpointer user_data);

/**
 * on_about_action:
 * @action: Triggered GSimpleAction object.
 * @parameter: Action target variant parameter (unused).
 * @user_data: Pointer to running GtkApplication context.
 *
 * Callback signal handler displaying the "About Lambdock" dialog window.
 */
void on_about_action(GSimpleAction *action, GVariant *parameter,
                     gpointer user_data);

#endif // DOCK_WINDOW_H
