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

#include "dock_window.h"

/* =========================================================================
 * ANIMATION UTILITIES & EASING
 * ========================================================================= */

/**
 * ease_out_cubic:
 * @t: Normalized progress of animation between 0.0 (start) and 1.0 (end).
 *
 * Mathematical cubic ease-out function: f(t) = 1 - (1 - t)^3.
 * This starts the animation fast and smoothly decelerates near the end.
 *
 * Returns: Eased value between 0.0 and 1.0.
 */
static double ease_out_cubic(double t) {
  double p = 1.0 - t;
  return 1.0 - (p * p * p);
}

/**
 * animate_margin_cb:
 * @widget: The GTK widget being animated (unused directly here).
 * @frame_clock: Hardware-synced clock managing frame timing (v-sync).
 * @user_data: Pointer to our custom application state structure
 * (LambdockState).
 *
 * Callback triggered by GTK on every monitor frame update while animating.
 * It updates the window's bottom margin frame-by-frame to create a smooth
 * sliding effect.
 *
 * Returns: G_SOURCE_CONTINUE to keep animating next frame, or G_SOURCE_REMOVE
 * to stop.
 */
static gboolean animate_margin_cb(GtkWidget *widget, GdkFrameClock *frame_clock,
                                  gpointer user_data) {
  // Explicitly tell compiler we aren't using 'widget' to silence compiler
  // warnings
  (void)widget;

  // In C, generic callback data arrives as a void pointer ('gpointer').
  // We must cast it back to our concrete struct pointer type.
  LambdockState *state = (LambdockState *)user_data;

  // Fetch current frame time in microseconds (1 second = 1,000,000 us)
  gint64 now = gdk_frame_clock_get_frame_time(frame_clock);

  // Initialize start time on the very first animation frame
  if (state->anim_start_time == 0) {
    state->anim_start_time = now;
  }

  // Calculate normalized animation progress (0.0 to 1.0)
  double progress =
      (double)(now - state->anim_start_time) / (double)ANIMATION_DURATION_US;

  // --- Animation Complete ---
  if (progress >= 1.0) {
    state->current_margin = state->target_margin;
    // GTK_WINDOW macro safe-casts GtkWidget pointer to GtkWindow pointer
    // expected by layer shell
    gtk_layer_set_margin(GTK_WINDOW(state->window), GTK_LAYER_SHELL_EDGE_BOTTOM,
                         state->current_margin);
    state->tick_cb_id = 0;  // Reset tick callback identifier
    return G_SOURCE_REMOVE; // Tell GTK to remove this frame callback
  }

  // --- Animation In Progress ---
  double eased = ease_out_cubic(progress);
  state->current_margin =
      state->start_margin +
      (int)((state->target_margin - state->start_margin) * eased);

  // Apply updated position margin to Wayland Layer Shell configuration
  gtk_layer_set_margin(GTK_WINDOW(state->window), GTK_LAYER_SHELL_EDGE_BOTTOM,
                       state->current_margin);

  return G_SOURCE_CONTINUE; // Request next frame update from GTK
}

/**
 * start_margin_animation:
 * @state: Pointer to application state.
 * @target_margin: Desired final pixel margin offset for the bottom edge.
 *
 * Prepares animation parameters and hooks into GTK frame tick clock if not
 * running.
 */
static void start_margin_animation(LambdockState *state, int target_margin) {
  // No need to animate if we are already at the target
  if (state->current_margin == target_margin)
    return;

  state->start_margin = state->current_margin;
  state->target_margin = target_margin;
  state->anim_start_time = 0; // Reset timer anchor

  // Only add tick callback if an animation isn't already actively running
  if (state->tick_cb_id == 0) {
    state->tick_cb_id = gtk_widget_add_tick_callback(
        state->window, animate_margin_cb, state, NULL);
  }
}

/* =========================================================================
 * DOCK VISIBILITY & AUTO-HIDE LOGIC
 * ========================================================================= */

/**
 * reveal_dock:
 * @state: Pointer to application state.
 *
 * Cancels pending auto-hide timers and animates dock back to bottom position (0
 * margin).
 */
void reveal_dock(LambdockState *state) {
  // If an auto-hide countdown is actively running, cancel it
  if (state->hide_timer_id > 0) {
    g_source_remove(state->hide_timer_id);
    state->hide_timer_id = 0;
  }

  // Animate back to fully visible state if currently hidden or moving elsewhere
  if (state->is_hidden || state->target_margin != 0) {
    start_margin_animation(state, 0);
    state->is_hidden = FALSE;
  }
}

/**
 * on_hide_timeout:
 * @user_data: Pointer to LambdockState passed from g_timeout_add.
 *
 * GLib timer callback triggered when auto-hide timeout expires.
 */
static gboolean on_hide_timeout(gpointer user_data) {
  LambdockState *state = (LambdockState *)user_data;
  hide_dock(state);
  state->hide_timer_id = 0; // Mark timer as inactive
  return G_SOURCE_REMOVE;   // Don't repeat this timer
}

/**
 * hide_dock:
 * @state: Pointer to application state.
 *
 * Calculates off-screen negative margin offset and starts hide animation,
 * provided auto-hide is enabled and context menus are closed.
 */
void hide_dock(LambdockState *state) {
  if (!state->auto_hide)
    return;

  // IMPORTANT: Prevent dock from collapsing while user interacts with context
  // popups!
  if (state->active_popovers > 0) {
    return;
  }

  // Get dynamic window height to hide almost completely (leaving 2px visual
  // trigger line)
  int window_height = gtk_widget_get_height(state->window);
  int hide_margin = -(window_height - 2);

  if (!state->is_hidden || state->target_margin != hide_margin) {
    start_margin_animation(state, hide_margin);
    state->is_hidden = TRUE;
  }
}

/* =========================================================================
 * EVENT CONTROLLER CALLBACKS & MENU HANDLING
 * ========================================================================= */

/**
 * on_popover_closed:
 * @popover: Pointer to GTK Popover widget that closed.
 * @user_data: Pointer to application state.
 *
 * Decrements active popover count when context menus close and schedules
 * auto-hide timer.
 */
void on_popover_closed(GtkPopover *popover, gpointer user_data) {
  (void)popover; // Parameter unused directly
  LambdockState *state = (LambdockState *)user_data;

  if (state->active_popovers > 0) {
    state->active_popovers--;
  }

  // Reschedule auto-hide timer (400ms delay) now that context menu has closed
  if (state->auto_hide && state->active_popovers == 0) {
    if (state->hide_timer_id != 0) {
      g_source_remove(state->hide_timer_id);
    }
    state->hide_timer_id = g_timeout_add(400, on_hide_timeout, state);
  }
}

/**
 * on_motion_enter:
 * Callback triggered when pointer enters dock window boundary.
 */
static void on_motion_enter(GtkEventControllerMotion *controller, double x,
                            double y, gpointer user_data) {
  (void)controller;
  (void)x;
  (void)y;
  LambdockState *state = (LambdockState *)user_data;
  reveal_dock(state);
}

/**
 * on_motion_leave:
 * Callback triggered when pointer leaves dock window boundary.
 */
static void on_motion_leave(GtkEventControllerMotion *controller,
                            gpointer user_data) {
  (void)controller;
  LambdockState *state = (LambdockState *)user_data;

  // Give user a 400ms grace period before hiding dock
  if (state->auto_hide) {
    if (state->hide_timer_id != 0) {
      g_source_remove(state->hide_timer_id);
    }
    state->hide_timer_id = g_timeout_add(400, on_hide_timeout, state);
  }
}

/**
 * on_right_click:
 * Callback for mouse click gestures (configured for right click).
 */
void on_right_click(GtkGestureClick *gesture, gint n_press, gdouble x,
                    gdouble y, gpointer user_data) {
  (void)gesture;
  (void)n_press;
  LambdockState *state = (LambdockState *)user_data;

  if (!state->popover)
    return;

  // Target rectangle (1x1 region at mouse click coordinates X,Y) for popover
  // alignment
  GdkRectangle rect = {(int)x, (int)y, 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(state->popover), &rect);

  if (!gtk_widget_get_visible(state->popover)) {
    state->active_popovers++;
  }

  // Open context menu popover UI
  gtk_popover_popup(GTK_POPOVER(state->popover));
}

/* =========================================================================
 * INITIALIZATION & SETUP HELPERS
 * ========================================================================= */

/**
 * setup_layer_shell_window:
 * @window: GTK window to transform into a desktop layer shell component.
 * @auto_hide: Boolean indicating if auto-hide capability is active.
 *
 * Configures GTK Layer Shell protocol settings for Wayland compositors.
 */
void setup_layer_shell_window(GtkWindow *window, gboolean auto_hide) {
  // Initialize Wayland Layer Shell protocol support on this window
  gtk_layer_init_for_window(window);

  // Set window layer depth (TOP means rendered above normal app windows)
  gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_TOP);

  // Pin window edge anchor to screen bottom edge
  gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);

  // Setup screen space reservation (Exclusive Zone):
  // 0 = allow windows to overlap panel space (useful when dock autohides).
  // Auto-exclusive = reserve screen pixels so normal apps don't cover dock.
  if (auto_hide) {
    gtk_layer_set_exclusive_zone(window, 0);
  } else {
    gtk_layer_auto_exclusive_zone_enable(window);
  }
}

/**
 * setup_dock_motion_controller:
 * @state: Pointer to application state.
 *
 * Attaches GTK 4 Event Controller to capture mouse enter and leave events.
 */
void setup_dock_motion_controller(LambdockState *state) {
  // GTK 4 event handling paradigm uses GtkEventController objects
  GtkEventController *motion = gtk_event_controller_motion_new();

  // Connect pointer enter/leave signals to callback handlers
  g_signal_connect(motion, "enter", G_CALLBACK(on_motion_enter), state);
  g_signal_connect(motion, "leave", G_CALLBACK(on_motion_leave), state);

  // Attach motion controller component to main dock GTK window widget
  gtk_widget_add_controller(state->window, motion);
}

/**
 * schedule_initial_autohide:
 * @state: Pointer to application state.
 *
 * Schedules initial dock hide timer 1000ms after launch if auto-hide is
 * enabled.
 */
void schedule_initial_autohide(LambdockState *state) {
  if (state->auto_hide) {
    state->hide_timer_id = g_timeout_add(1000, on_hide_timeout, state);
  }
}

/**
 * on_quit_action:
 * Signal handler for desktop environment or menu app exit actions.
 */
void on_quit_action(GSimpleAction *action, GVariant *parameter,
                    gpointer user_data) {
  (void)action;
  (void)parameter;
  GtkApplication *app = GTK_APPLICATION(user_data);
  g_application_quit(G_APPLICATION(app));
}
