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
#include "ui_helpers.h"

/* =========================================================================
 * ANIMATION UTILITIES & EASING
 * ========================================================================= */

/**
 * ease_out_cubic:
 * @t: Normalized progress value between 0.0 (start) and 1.0 (completion).
 *
 * Mathematical cubic easing curve: f(t) = 1 - (1 - t)^3.
 * Starts motion quickly and decelerates smoothly near completion.
 *
 * Returns: Eased double floating-point progress factor (0.0 to 1.0).
 */
static double ease_out_cubic(double t) {
  double p = 1.0 - t;
  return 1.0 - (p * p * p);
}

/**
 * animate_margin_cb:
 * @widget: GTK widget associated with tick callback (unused directly).
 * @frame_clock: Hardware display V-sync synchronized frame clock.
 * @user_data: Generic gpointer pointing to LambdockState context struct.
 *
 * Frame clock callback executed on every display refresh tick during
 * animations. Calculates interpolated pixel margins frame-by-frame for smooth
 * rendering.
 *
 * Returns: G_SOURCE_CONTINUE to request next frame update, or G_SOURCE_REMOVE
 * to stop.
 */
static gboolean animate_margin_cb(GtkWidget *widget, GdkFrameClock *frame_clock,
                                  gpointer user_data) {
  (void)
      widget; // Unused parameter explicitly cast to suppress compiler warnings

  // In C, generic callback data arrives as a void pointer ('gpointer').
  // We must explicitly cast it back to our concrete struct pointer type.
  LambdockState *state = (LambdockState *)user_data;

  // Fetch high-resolution monotonic clock time in microseconds
  gint64 now = gdk_frame_clock_get_frame_time(frame_clock);

  // Initialize start timestamp on first frame tick
  if (state->anim_start_time == 0) {
    state->anim_start_time = now;
  }

  // Calculate normalized time progress ratio (0.0 to 1.0)
  double progress =
      (double)(now - state->anim_start_time) / (double)ANIMATION_DURATION_US;

  // --- Case 1: Animation Complete ---
  if (progress >= 1.0) {
    state->current_margin = state->target_margin;
    gtk_layer_set_margin(GTK_WINDOW(state->window), state->edge,
                         state->current_margin);
    state->tick_cb_id = 0;  // Reset callback identifier
    return G_SOURCE_REMOVE; // Tell GTK frame clock to unregister this callback
  }

  // --- Case 2: Animation In Progress ---
  double eased = ease_out_cubic(progress);
  state->current_margin =
      state->start_margin +
      (int)((state->target_margin - state->start_margin) * eased);

  // Apply updated pixel offset via Wayland Layer Shell protocol
  gtk_layer_set_margin(GTK_WINDOW(state->window), state->edge,
                       state->current_margin);

  return G_SOURCE_CONTINUE; // Request execution on next frame tick
}

/**
 * start_margin_animation:
 * @state: Pointer to application state structure.
 * @target_margin: Desired target pixel margin depth.
 *
 * Prepares easing parameters and registers the tick callback with GTK's frame
 * clock.
 */
static void start_margin_animation(LambdockState *state, int target_margin) {
  // Avoid re-triggering if already animating toward or residing at target
  // position
  if (state->current_margin == target_margin)
    return;

  state->start_margin = state->current_margin;
  state->target_margin = target_margin;
  state->anim_start_time = 0;

  // Only hook into frame clock if an animation is not actively running
  if (state->tick_cb_id == 0) {
    state->tick_cb_id = gtk_widget_add_tick_callback(
        state->window, animate_margin_cb, state, NULL);
  }
}

/* =========================================================================
 * DOCK VISIBILITY & AUTO-HIDE LOGIC
 * ========================================================================= */

void reveal_dock(LambdockState *state) {
  // If an auto-hide countdown timer is pending, cancel it immediately
  if (state->hide_timer_id > 0) {
    g_source_remove(state->hide_timer_id);
    state->hide_timer_id = 0;
  }

  // Slide dock back to margin offset 0 (fully visible)
  if (state->is_hidden || state->target_margin != 0) {
    start_margin_animation(state, 0);
    state->is_hidden = FALSE;
  }
}

/**
 * on_hide_timeout:
 * @user_data: Pointer to LambdockState passed from GLib timer loop.
 *
 * Timer callback executed when the mouse leave grace period expires.
 *
 * Returns: G_SOURCE_REMOVE to terminate single-shot GLib timer.
 */
static gboolean on_hide_timeout(gpointer user_data) {
  LambdockState *state = (LambdockState *)user_data;
  hide_dock(state);
  state->hide_timer_id = 0;
  return G_SOURCE_REMOVE;
}

void hide_dock(LambdockState *state) {
  // Do not hide if feature is disabled or if context menu popovers are open
  if (!state->auto_hide || state->active_popovers > 0)
    return;

  // Measure dimension along screen attachment axis
  int window_dim = (state->edge == GTK_LAYER_SHELL_EDGE_TOP ||
                    state->edge == GTK_LAYER_SHELL_EDGE_BOTTOM)
                       ? gtk_widget_get_height(state->window)
                       : gtk_widget_get_width(state->window);

  // Leave a 2px strip visible so pointer hover events can re-trigger reveal
  int hide_margin = -(window_dim - 2);

  if (!state->is_hidden || state->target_margin != hide_margin) {
    start_margin_animation(state, hide_margin);
    state->is_hidden = TRUE;
  }
}

/* =========================================================================
 * EVENT CONTROLLER CALLBACKS & MENU HANDLING
 * ========================================================================= */

void on_popover_closed(GtkPopover *popover, gpointer user_data) {
  (void)popover;
  LambdockState *state = (LambdockState *)user_data;

  if (state->active_popovers > 0) {
    state->active_popovers--;
  }

  // Schedule auto-hide timer (400ms delay) now that popover menus have closed
  if (state->auto_hide && state->active_popovers == 0) {
    if (state->hide_timer_id != 0) {
      g_source_remove(state->hide_timer_id);
    }
    state->hide_timer_id = g_timeout_add(400, on_hide_timeout, state);
  }
}

static void on_motion_enter(GtkEventControllerMotion *controller, double x,
                            double y, gpointer user_data) {
  (void)controller;
  (void)x;
  (void)y;
  LambdockState *state = (LambdockState *)user_data;
  reveal_dock(state);
}

static void on_motion_leave(GtkEventControllerMotion *controller,
                            gpointer user_data) {
  (void)controller;
  LambdockState *state = (LambdockState *)user_data;

  // Give user a 720ms grace period before initiating hide animation
  if (state->auto_hide) {
    if (state->hide_timer_id != 0) {
      g_source_remove(state->hide_timer_id);
    }
    state->hide_timer_id = g_timeout_add(720, on_hide_timeout, state);
  }
}

void on_right_click(GtkGestureClick *gesture, gint n_press, gdouble x,
                    gdouble y, gpointer user_data) {
  (void)gesture;
  (void)n_press;
  LambdockState *state = (LambdockState *)user_data;

  if (!state->popover)
    return;

  // Align popover context menu pointer to 1x1 mouse click coordinates
  GdkRectangle rect = {(int)x, (int)y, 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(state->popover), &rect);

  if (!gtk_widget_get_visible(state->popover)) {
    state->active_popovers++;
  }

  gtk_popover_popup(GTK_POPOVER(state->popover));
}

/* =========================================================================
 * INITIALIZATION & CLEANUP HELPERS
 * ========================================================================= */

void setup_layer_shell_window(LambdockState *state) {
  GtkWindow *window = GTK_WINDOW(state->window);

  // Initialize GTK Layer Shell protocol support on main window surface
  gtk_layer_init_for_window(window);

  // Render layer above standard application windows
  gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_TOP);

  // Anchor window to target screen edge
  gtk_layer_set_anchor(window, state->edge, TRUE);

  // Exclusive zone controls desktop window layout behavior around dock:
  // 0 = Desktop windows overlap dock when auto-hiding.
  // Auto-exclusive = Reserve screen pixels so other windows align around
  // Lambdock.
  if (state->auto_hide) {
    gtk_layer_set_exclusive_zone(window, 0);
  } else {
    gtk_layer_auto_exclusive_zone_enable(window);
  }
}

void setup_dock_motion_controller(LambdockState *state) {
  // In GTK 4, user interactions are handled by attached EventControllers
  GtkEventController *motion = gtk_event_controller_motion_new();

  g_signal_connect(motion, "enter", G_CALLBACK(on_motion_enter), state);
  g_signal_connect(motion, "leave", G_CALLBACK(on_motion_leave), state);

  // Attach motion event controller component to main dock window widget
  gtk_widget_add_controller(state->window, motion);
}

void schedule_initial_autohide(LambdockState *state) {
  if (state->auto_hide) {
    state->hide_timer_id = g_timeout_add(1000, on_hide_timeout, state);
  }
}

void free_lambdock_state(LambdockState *state) {
  if (!state)
    return;

  // Iterate doubly linked list (GList*) and free custom heap allocations
  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    free_dock_item(item);
  }
  g_list_free(state->dock_items);
  state->dock_items = NULL;

  g_free(state); // Free central state structure buffer
}

void on_quit_action(GSimpleAction *action, GVariant *parameter,
                    gpointer user_data) {
  (void)action;
  (void)parameter;
  GtkApplication *app = GTK_APPLICATION(user_data);
  g_application_quit(G_APPLICATION(app)); // Shutdown main GLib event loop
}
