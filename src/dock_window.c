// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "dock_window.h"
#include "accessibility.h"
#include "guile_bridge.h"
#include "logger.h"
#include "toplevel_tracker.h"
#include "ui/shelf.h"
#include "ui/dock_item.h"
#include "ui/icon_resolver.h"
#include "ui/icon_zoom.h"
#include "ui/launchpad.h"
#include "ui/popover_menu.h"
#include "ui/theme_engine.h"
#include <glib/gi18n.h>
#include <locale.h>
#include <stdlib.h>

/* =========================================================================
 * CROSS-AXIS GEOMETRY (SHARED BY DIMENSIONS & AUTO-HIDE)
 * ========================================================================= */

/**
 * dock_capsule_cross_dim:
 * Returns the capsule thickness for the current shelf parameters:
 *
 *   icon + margin (glass on the inner side of the icons)
 *        + max(front, reflection band)   (glass on the outer side)
 *
 * With the defaults (margin = capsule padding, no front glass, no reflections)
 * this is exactly the classic `icon + CAPSULE_PADDING_PX` capsule.
 */
int dock_capsule_cross_dim(const LambdockState *state) {
  int icon =
      (state && state->icon_size > 0) ? state->icon_size : DEFAULT_ICON_SIZE;

  return lambdock_shelf_geometry_for(state, icon).capsule_px;
}

/**
 * dock_cross_axis_dim:
 * Returns the cross-axis size requested for the overlay: fisheye magnification
 * headroom, plus the outer glass, plus any icon overhang past the inner edge.
 */
int dock_cross_axis_dim(const LambdockState *state) {
  int icon =
      (state && state->icon_size > 0) ? state->icon_size : DEFAULT_ICON_SIZE;
  LambdockShelfGeometry geometry = lambdock_shelf_geometry_for(state, icon);

  int dim = (int)(icon * CROSS_DIM_SCALE_FACTOR) + CROSS_DIM_PADDING_PX;
  dim += geometry.outer_px + geometry.overhang_px;

  return dim;
}

/* =========================================================================
 * ANIMATION ENGINE & EASING FUNCTIONS
 * ========================================================================= */

/**
 * ease_out_cubic:
 * Calculates a Cubic Ease-Out curve for smooth deceleration.
 * * Formula: f(t) = 1 - (1 - t)^3
 *
 * @param t Progress normalized between 0.0 (start) and 1.0 (end).
 * @return Eased interpolation value between 0.0 and 1.0.
 */
static double ease_out_cubic(double t) {
  double p = 1.0 - t;
  return 1.0 - (p * p * p);
}

/**
 * animate_margin_cb:
 * GTK Tick callback executed on every display frame update while animating.
 * * Smoothly interpolates `current_margin` towards `target_margin` using
 * elapsed frame-clock timestamps.
 *
 * @param widget The GTK window widget being animated.
 * @param frame_clock The monitor frame clock driving updates.
 * @param user_data Pointer to the active LambdockState structure.
 * @return G_SOURCE_CONTINUE to keep ticking, or G_SOURCE_REMOVE when finished.
 */
static gboolean animate_margin_cb(GtkWidget *widget, GdkFrameClock *frame_clock,
                                  gpointer user_data) {
  (void)widget; // Unused parameter
  LambdockState *state = (LambdockState *)user_data;

  // Retrieve current frame timestamp in microseconds
  gint64 now = gdk_frame_clock_get_frame_time(frame_clock);

  // Initialize animation start time on first frame tick
  if (state->anim_start_time == 0) {
    state->anim_start_time = now;
  }

  // Calculate normalized animation progress ratio [0.0, 1.0]
  double progress =
      (double)(now - state->anim_start_time) / (double)ANIMATION_DURATION_US;

  // --- Animation Complete Handler ---
  if (progress >= 1.0) {
    state->current_margin = state->target_margin;
    gtk_layer_set_margin(GTK_WINDOW(state->window), state->edge,
                         state->current_margin);
    state->tick_cb_id = 0;  // Reset callback ID
    /* Crossings for a moment after this are echoes of the slide, not the
     * pointer moving: see dock_is_settling(). */
    state->margin_anim_end_us = g_get_monotonic_time();
    return G_SOURCE_REMOVE; // Stop frame tick callback loop
  }

  // --- Intermediate Frame Interpolation ---
  double eased = ease_out_cubic(progress);
  state->current_margin =
      state->start_margin +
      (int)((state->target_margin - state->start_margin) * eased);

  // Apply new calculated margin to Wayland layer shell surface
  gtk_layer_set_margin(GTK_WINDOW(state->window), state->edge,
                       state->current_margin);

  return G_SOURCE_CONTINUE; // Request next frame tick
}

/**
 * start_margin_animation:
 * Initiates or updates a margin transition animation toward a target value.
 *
 * @param state Pointer to LambdockState.
 * @param target_margin Target layer shell margin (in pixels).
 */
/* gtk_layer_set_margin() only means something on a layer-shell surface: the
 * preferences window and the test fixtures build ordinary windows. */
static void set_window_margin(LambdockState *state, int margin) {
  if (!state || !state->window || !GTK_IS_WINDOW(state->window))
    return;
  if (gtk_layer_is_layer_window(GTK_WINDOW(state->window)))
    gtk_layer_set_margin(GTK_WINDOW(state->window), state->edge, margin);
}

static void start_margin_animation(LambdockState *state, int target_margin) {
  // Ignore if already positioned at target
  if (state->current_margin == target_margin)
    return;

  /* Reduced motion: the dock still has to get out of the way (or come back),
   * it just does it in one step instead of sliding. */
  if (!lambdock_animations_enabled(state)) {
    if (state->tick_cb_id != 0) {
      gtk_widget_remove_tick_callback(state->window, state->tick_cb_id);
      state->tick_cb_id = 0;
    }
    state->start_margin = target_margin;
    state->current_margin = target_margin;
    state->target_margin = target_margin;
    state->anim_start_time = 0;
    state->margin_anim_end_us = g_get_monotonic_time();
    set_window_margin(state, target_margin);
    return;
  }

  state->start_margin = state->current_margin;
  state->target_margin = target_margin;
  state->anim_start_time = 0; // Reset timer anchor
  state->margin_anim_end_us = 0;

  // Register frame tick callback if not currently active
  if (state->tick_cb_id == 0) {
    state->tick_cb_id = gtk_widget_add_tick_callback(
        state->window, animate_margin_cb, state, NULL);
  }
}

/* =========================================================================
 * FISHEYE HOVER MAGNIFICATION ENGINE
 * ========================================================================= */

/**
 * get_item_zoom_widget:
 * Locates the fisheye zoom container of a dock item, the widget that actually
 * scales the icon while hovering.
 *
 * Returns: (transfer none) (nullable): the zoom widget, or NULL for items that
 * have no icon (separators, meters).
 */
static GtkWidget *get_item_zoom_widget(DockItem *item) {
  if (!item || !item->button || !GTK_IS_BUTTON(item->button))
    return NULL;

  GtkWidget *child = gtk_button_get_child(GTK_BUTTON(item->button));
  if (!child)
    return NULL;

  if (lambdock_icon_zoom_is_zoom(child))
    return child;

  if (GTK_IS_BOX(child)) {
    for (GtkWidget *w = gtk_widget_get_first_child(child); w != NULL;
         w = gtk_widget_get_next_sibling(w)) {
      if (lambdock_icon_zoom_is_zoom(w))
        return w;
    }
  }

  return NULL;
}

/**
 * reset_dock_items_scale:
 * Resets all dock item icons back to their default base pixel size and resets
 * widget size requests cleanly.
 */
static void reset_dock_items_scale(LambdockState *state) {
  if (!state || !state->dock_items)
    return;

  int base_sz = (state->icon_size > 0) ? state->icon_size : DEFAULT_ICON_SIZE;

  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    if (!item || !item->button)
      continue;

    // Preserve cross-axis base size for all buttons
    if (state->orientation == GTK_ORIENTATION_HORIZONTAL) {
      gtk_widget_set_size_request(item->button, -1, base_sz);
    } else {
      gtk_widget_set_size_request(item->button, base_sz, -1);
    }

    /* The icon itself is never re-rasterised: it rests at its base scale. */
    GtkWidget *zoom = get_item_zoom_widget(item);
    if (zoom)
      lambdock_icon_zoom_set_scale(zoom, 1.0);

    /* Keep the glass-shelf reflection at the base icon size too. */
    if (item->reflection)
      shelf_reflection_set_icon_size(item->reflection, base_sz);
  }

  /* The row is back at rest, so the next frame has to apply the pointer again
   * even if it has not moved. */
  state->pointer_applied = FALSE;
}

/**
 * dock_apply_motion_preference:
 * @state: the dock whose motion setting (and the desktop's) should be honoured.
 *
 * Called when the dock reloads its settings and when the desktop's animation
 * setting changes. With motion reduced, a slide already in flight is finished
 * immediately, icons are put back at their resting size and the fisheye is
 * stopped; nothing is animated until motion is allowed again.
 */
void dock_apply_motion_preference(LambdockState *state) {
  if (!state || !state->window || !GTK_IS_WINDOW(state->window))
    return;

  if (lambdock_animations_enabled(state))
    return;

  if (state->tick_cb_id != 0) {
    gtk_widget_remove_tick_callback(state->window, state->tick_cb_id);
    state->tick_cb_id = 0;
  }

  state->current_margin = state->target_margin;
  set_window_margin(state, state->current_margin);

  if (state->pointer_tick_id != 0) {
    gtk_widget_remove_tick_callback(state->window, state->pointer_tick_id);
    state->pointer_tick_id = 0;
  }

  reset_dock_items_scale(state);
}

/**
 * update_dock_items_fisheye:
 * Computes fisheye magnification for dock icons based on current
 * cursor coordinates relative to dock item centers.
 */
static void update_dock_items_fisheye(LambdockState *state, double x,
                                      double y) {
  // Do not run hover magnification if disabled, hidden, or animating slide
  if (!state || !state->dock_items || !state->hover_animate ||
      !lambdock_animations_enabled(state) || state->is_hidden ||
      state->tick_cb_id != 0 || state->current_margin != 0)
    return;

  int base_sz = (state->icon_size > 0) ? state->icon_size : DEFAULT_ICON_SIZE;
  double base_size = (double)base_sz;
  const double max_scale = FISHEYE_MAX_SCALE; // Max size magnification
  const double radius = FISHEYE_RADIUS_PX;    // Influence radius in pixels

  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;

    // Guard: check that button is a GtkButton AND item allows hover animation
    if (!item || !item->hover_animate || !item->button ||
        !GTK_IS_BUTTON(item->button))
      continue;

    graphene_rect_t bounds;
    if (gtk_widget_compute_bounds(item->button, state->window, &bounds)) {
      double center_x = bounds.origin.x + bounds.size.width / 2.0;
      double center_y = bounds.origin.y + bounds.size.height / 2.0;

      double dist = (state->orientation == GTK_ORIENTATION_HORIZONTAL)
                        ? fabs(x - center_x)
                        : fabs(y - center_y);

      double scale = 1.0;
      if (dist < radius) {
        // Cosine bell curve scaling from 1.0 (at radius edge) to max_scale (at
        // center)
        double factor = 0.5 * (1.0 + cos(M_PI * dist / radius));
        scale = 1.0 + (max_scale - 1.0) * factor;
      }

      int target_size = (int)lround(base_size * scale);
      GtkWidget *zoom = get_item_zoom_widget(item);

      if (!zoom)
        continue;

      if (lambdock_icon_zoom_target_size(zoom) != target_size) {
        if (state->orientation == GTK_ORIENTATION_HORIZONTAL) {
          // Expand width along X axis; preserve base_sz along Y axis
          gtk_widget_set_size_request(item->button, target_size, base_sz);
        } else {
          // Expand height along Y axis; preserve base_sz along X axis
          gtk_widget_set_size_request(item->button, base_sz, target_size);
        }

        // Mirror the magnified icon in its reflection: the copy grows along
        // the dock axis but keeps the band thickness, so the row of icons
        // does not shift up while hovering.
        if (item->reflection && lambdock_shelf_active(state)) {
          int band = (int)round(base_size * lambdock_shelf_params_effective(state)
                                               .reflection_ratio);
          if (band > 0)
            shelf_reflection_set_extent(item->reflection, target_size, band);
        }
      }

      /* The layout steps in whole pixels, the icon does not: the zoom paints
       * the artwork through a transform, so magnification follows the pointer
       * continuously and costs no rasterisation at all. */
      lambdock_icon_zoom_set_scale(zoom, scale);
    }
  }
}

/* =========================================================================
 * AUTO-HIDE & REVEAL LOGIC
 * ========================================================================= */

/**
 * reveal_dock:
 * Cancels pending hide timers and animates the dock back into full view (margin
 * = 0).
 *
 * @param state Pointer to LambdockState.
 */
void reveal_dock(LambdockState *state) {
  // Cancel any scheduled auto-hide timeout
  if (state->hide_timer_id > 0) {
    g_source_remove(state->hide_timer_id);
    state->hide_timer_id = 0;
  }

  // Trigger reveal animation if hidden or moving away from visible state
  if (state->is_hidden || state->target_margin != 0) {
    start_margin_animation(state, 0);
    state->is_hidden = FALSE;
  }
}

/* How long after a slide settles a pointer crossing is still treated as an echo
 * of the slide itself. The crossings that make the dock bounce arrive within a
 * frame or two of the animation stopping. */
#define AUTO_HIDE_SETTLE_MS 300

/**
 * dock_is_settling:
 * @state: Pointer to the dock state.
 *
 * Whether the dock is sliding, or has only just stopped.
 *
 * While the dock slides it moves under the pointer: a pointer that is not moving
 * still ends up over a different part of the dock, or off it, and compositors
 * report the crossings that follow from that as if the pointer had moved. Those
 * reports are what used to bounce the dock at the screen edge -- a leave hides it,
 * the strip comes back under the pointer, an enter reveals it, and round it goes.
 *
 * Returns: TRUE while crossings are echoes of the dock's own movement.
 */
gboolean dock_is_settling(const LambdockState *state) {
  if (!state)
    return FALSE;

  if (state->tick_cb_id != 0)
    return TRUE;

  if (state->margin_anim_end_us == 0)
    return FALSE;

  gint64 settled_for_us = g_get_monotonic_time() - state->margin_anim_end_us;
  return settled_for_us < (gint64)AUTO_HIDE_SETTLE_MS * 1000;
}

/**
 * on_hide_timeout:
 * Timer callback triggered after mouse-leave grace period expires.
 *
 * @param user_data Pointer to LambdockState.
 * @return G_SOURCE_REMOVE to unregister timer.
 */
static gboolean on_hide_timeout(gpointer user_data) {
  LambdockState *state = (LambdockState *)user_data;
  state->hide_timer_id = 0;

  hide_dock(state);
  return G_SOURCE_REMOVE;
}

/**
 * hide_dock:
 * Calculates window dimensions and slides the dock off-screen into hidden
 * margin. Keeps a trigger strip visible on screen edge to receive mouse hover
 * events.
 *
 * @param state Pointer to LambdockState.
 */
void hide_dock(LambdockState *state) {
  // Do not hide if autohide is disabled or user has open popover menus
  if (!state->auto_hide || state->active_popovers > 0)
    return;

  // Do not hide if show-when-clear is active and no open windows exist
  if (state->show_when_clear && state->window_trackers == NULL)
    return;

  // Reset icon scaling to base size before calculating window dimensions
  if (state->hover_animate) {
    reset_dock_items_scale(state);
  }

  GtkOrientation dim_orient = (state->edge == GTK_LAYER_SHELL_EDGE_TOP ||
                               state->edge == GTK_LAYER_SHELL_EDGE_BOTTOM)
                                  ? GTK_ORIENTATION_VERTICAL
                                  : GTK_ORIENTATION_HORIZONTAL;

  int window_dim = 0;
  if (state->window) {
    window_dim = (dim_orient == GTK_ORIENTATION_VERTICAL)
                     ? gtk_widget_get_height(state->window)
                     : gtk_widget_get_width(state->window);
  }

  // Ensure window_dim is deterministic even before initial GTK layout/mapping.
  // If GTK layout hasn't run yet after an orientation hot-reload, window_dim
  // can hold stale main-axis dimensions from the previous orientation.
  int max_cross_dim = dock_cross_axis_dim(state);
  if (window_dim <= 0 || window_dim > max_cross_dim + CROSS_DIM_TOLERANCE_PX) {
    window_dim = max_cross_dim;
  }

  // Leave exposed strip on screen edge to trigger hover re-entry
  int hide_margin = -(window_dim - AUTOHIDE_TRIGGER_STRIP_PX);

  if (!state->is_hidden || state->target_margin != hide_margin) {
    start_margin_animation(state, hide_margin);
    state->is_hidden = TRUE;
  }
}

/* =========================================================================
 * EVENT CONTROLLERS & CALLBACKS
 * ========================================================================= */

/**
 * on_popover_closed:
 * Decrements active popover counter when a context menu is dismissed.
 * Re-schedules auto-hide if no open popovers remain.
 */
void on_popover_closed(GtkPopover *popover, gpointer user_data) {
  (void)popover;
  LambdockState *state = (LambdockState *)user_data;

  if (state->active_popovers > 0) {
    state->active_popovers--;
  }

  // If no menus remain open, schedule autohide after delay
  if (state->auto_hide && state->active_popovers == 0) {
    if (state->hide_timer_id != 0) {
      g_source_remove(state->hide_timer_id);
    }
    state->hide_timer_id =
        g_timeout_add(AUTOHIDE_POPOVER_DELAY_MS, on_hide_timeout, state);
  }
}

/**
 * on_motion_enter:
 * Mouse motion controller event when pointer enters the dock region.
 */
static void on_motion_enter(GtkEventControllerMotion *controller, double x,
                            double y, gpointer user_data) {
  (void)controller;
  (void)x;
  (void)y;
  LambdockState *state = (LambdockState *)user_data;
  state->pointer_moved_since_enter = FALSE;
  state->pointer_applied = FALSE;
  reveal_dock(state);
}

/**
 * on_hover_tick:
 * GTK frame clock callback that applies the newest pointer position to the
 * fisheye, once per frame.
 *
 * Motion events can arrive several times per frame (high-rate mice, libinput
 * batching, a compositor that coalesces late), and every one of them used to
 * resize the whole row of icons and repaint the dock. Applying only the latest
 * position keeps the work proportional to the frame rate instead of to the event
 * rate, and it does so right before the frame is laid out, so the icons are never
 * a frame behind the pointer.
 */
static gboolean on_hover_tick(GtkWidget *widget, GdkFrameClock *frame_clock,
                              gpointer user_data) {
  (void)widget;
  (void)frame_clock;

  LambdockState *state = (LambdockState *)user_data;
  if (!state)
    return G_SOURCE_REMOVE;

  state->pointer_tick_id = 0; // one-shot: the next motion schedules the next one

  /* Nothing moved since the last frame (the pointer can repeat a position), so
   * the icons are already right where they belong. */
  if (state->pointer_applied &&
      fabs(state->pointer_x - state->applied_pointer_x) < 1.0 &&
      fabs(state->pointer_y - state->applied_pointer_y) < 1.0)
    return G_SOURCE_REMOVE;

  state->applied_pointer_x = state->pointer_x;
  state->applied_pointer_y = state->pointer_y;
  state->pointer_applied = TRUE;

  update_dock_items_fisheye(state, state->pointer_x, state->pointer_y);

  return G_SOURCE_REMOVE;
}

/**
 * on_motion_leave:
 * Mouse motion controller event when pointer leaves the dock surface.
 * Resets icon scaling and schedules an auto-hide check after interval.
 */
static void on_motion_leave(GtkEventControllerMotion *controller,
                            gpointer user_data) {
  (void)controller;
  LambdockState *state = (LambdockState *)user_data;

  if (state) {
    /* Not every leave means the pointer went anywhere. The dock slides under a
     * pointer that is not moving -- the window moves, the pointer does not -- and
     * compositors report crossings that follow from that, which is what used to
     * bounce the dock at its screen edge: a leave hides it, the revealing strip
     * comes back under the pointer, an enter reveals it, and round it goes. So a
     * leave only counts when the dock is sitting still *and* the pointer has
     * actually moved since it arrived; a pointer that has not moved cannot have
     * left. */
    if (dock_is_settling(state) || !state->pointer_moved_since_enter)
      return;

    /* A frame that is already queued must not magnify anything after the
     * pointer has left. */
    if (state->pointer_tick_id != 0 && state->window) {
      gtk_widget_remove_tick_callback(state->window, state->pointer_tick_id);
      state->pointer_tick_id = 0;
    }
    state->pointer_applied = FALSE;

    if (state->hover_animate) {
      reset_dock_items_scale(state);
    }
    if (state->auto_hide) {
      if (state->hide_timer_id != 0) {
        g_source_remove(state->hide_timer_id);
      }
      state->hide_timer_id =
          g_timeout_add(AUTOHIDE_LEAVE_DELAY_MS, on_hide_timeout, state);
    }
  }
}

/**
 * on_motion_notify:
 * Mouse motion callback tracking cursor position across dock items for fisheye.
 */
static void on_motion_notify(GtkEventControllerMotion *controller, double x,
                             double y, gpointer user_data) {
  (void)controller;
  LambdockState *state = (LambdockState *)user_data;
  if (!state || !state->hover_animate || state->is_hidden ||
      state->tick_cb_id != 0 || state->current_margin != 0)
    return;

  /* Remember where the pointer is and let the frame clock do the work, at most
   * once per frame. A motion only counts as the pointer moving when the dock
   * itself is still: while it slides, the window moves under the pointer and the
   * positions that come back are its own movement. */
  if (!dock_is_settling(state))
    state->pointer_moved_since_enter = TRUE;

  state->pointer_x = x;
  state->pointer_y = y;

  if (state->pointer_tick_id == 0 && state->window)
    state->pointer_tick_id =
        gtk_widget_add_tick_callback(state->window, on_hover_tick, state, NULL);
}

/**
 * on_right_click:
 * Gesture controller callback for right-click context menu activation.
 */
void on_right_click(GtkGestureClick *gesture, gint n_press, gdouble x,
                    gdouble y, gpointer user_data) {
  (void)gesture;
  (void)n_press;
  LambdockState *state = (LambdockState *)user_data;

  if (!state->popover)
    return;

  // Set popover arrow positioning anchor at right-click coordinates
  GdkRectangle rect = {(int)x, (int)y, 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(state->popover), &rect);

  if (!gtk_widget_get_visible(state->popover)) {
    state->active_popovers++;
  }

  gtk_popover_popup(GTK_POPOVER(state->popover));
}

/* =========================================================================
 * LAYER SHELL & STATE INITIALIZATION
 * ========================================================================= */

/**
 * setup_layer_shell_window:
 * Converts standard GTK window to a Wayland Layer Shell surface and sets
 * anchors.
 *
 * @param state Pointer to LambdockState.
 */
void setup_layer_shell_window(LambdockState *state) {
  GtkWindow *window = GTK_WINDOW(state->window);

  // Initialize GTK Layer Shell integration for window
  gtk_layer_init_for_window(window);
  gtk_layer_set_namespace(window, "lambdock");

  /* Layer surfaces get no keyboard by default, which would leave the dock
   * unreachable for keyboard-only users: icons could not be tabbed to, and
   * Escape would never reach an open stack. On-demand keeps the compositor in
   * charge while letting a click (or a compositor shortcut) hand the dock the
   * keyboard. */
  gtk_layer_set_keyboard_mode(window, GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

  // Assign window to specific GdkMonitor output if specified
  if (state->monitor != NULL) {
    gtk_layer_set_monitor(window, state->monitor);
  }

  // Set desktop render layer (Top layer renders above normal application
  // windows)
  gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_TOP);

  // Anchor window to screen edge (Top, Bottom, Left, or Right)
  gtk_layer_set_anchor(window, state->edge, TRUE);

  // Taskbar mode: also anchor the two perpendicular edges so the surface is
  // stretched to span the whole monitor (full-width bottom/top bar, or
  // full-height left/right bar). A floating dock leaves them unanchored and
  // keeps its natural, content-hugging size.
  if (state->taskbar) {
    if (state->edge == GTK_LAYER_SHELL_EDGE_TOP ||
        state->edge == GTK_LAYER_SHELL_EDGE_BOTTOM) {
      gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
      gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    } else {
      gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
      gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    }
  }

  // Exclusive zone reserves desktop space so windows do not overlap dock.
  // Reserve exact background capsule thickness when auto-hide is disabled.
  if (state->auto_hide) {
    gtk_layer_set_exclusive_zone(window, 0);
  } else {
    gtk_layer_set_exclusive_zone(window, dock_capsule_cross_dim(state));
  }
}

/**
 * setup_dock_motion_controller:
 * Attaches GtkEventControllerMotion to monitor cursor entry, leave, and motion.
 */
void setup_dock_motion_controller(LambdockState *state) {
  GtkEventController *motion = gtk_event_controller_motion_new();

  g_signal_connect(motion, "enter", G_CALLBACK(on_motion_enter), state);
  g_signal_connect(motion, "leave", G_CALLBACK(on_motion_leave), state);
  g_signal_connect(motion, "motion", G_CALLBACK(on_motion_notify), state);

  gtk_widget_add_controller(state->window, motion);
}

/**
 * schedule_initial_autohide:
 * Schedules initial hide timer on application launch if auto-hide is enabled.
 */
void schedule_initial_autohide(LambdockState *state) {
  if (state->auto_hide) {
    state->hide_timer_id =
        g_timeout_add(AUTOHIDE_INITIAL_DELAY_MS, on_hide_timeout, state);
  }
}

/* =========================================================================
 * GTK APPLICATION ACTIONS (ABOUT & QUIT)
 * ========================================================================= */

/**
 * on_quit_action:
 * Action handler for menu 'Quit' option. Terminates GTK application loop.
 */
void on_quit_action(GSimpleAction *action, GVariant *parameter,
                    gpointer user_data) {
  (void)action;
  (void)parameter;
  GtkApplication *app = GTK_APPLICATION(user_data);
  g_application_quit(G_APPLICATION(app));
}

/**
 * on_about_action:
 * Action handler for menu 'About Lambdock' option. Constructs and displays
 * modal `GtkAboutDialog` with metadata, license, and logo assets.
 */
/* Single About dialog instance: repeatedly invoking the action must reuse it
 * rather than accumulate windows. */
static GtkWidget *g_about_dialog = NULL;

static gboolean on_about_close_request(GtkWindow *window, gpointer user_data) {
  (void)user_data;
  g_about_dialog = NULL;
  gtk_window_destroy(window);
  return TRUE; /* handled: do not fall back to the default close behaviour */
}

void on_about_action(GSimpleAction *action, GVariant *parameter,
                     gpointer user_data) {
  (void)action;
  (void)parameter;
  (void)user_data;

  if (g_about_dialog != NULL && GTK_IS_WINDOW(g_about_dialog)) {
    gtk_window_present(GTK_WINDOW(g_about_dialog));
    return;
  }

  GtkWidget *about = gtk_about_dialog_new();
  g_signal_connect(about, "close-request",
                   G_CALLBACK(on_about_close_request), NULL);
  g_about_dialog = about;
  gtk_widget_add_css_class(about, "about-dialog");

  // Basic Details
  gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(about), "lambdock");

#ifdef PACKAGE_VERSION
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(about), PACKAGE_VERSION);
#endif

  gtk_about_dialog_set_comments(
      GTK_ABOUT_DIALOG(about),
      _("A modern Wayland-native hackable desktop dock bar.\n\n"
        "Engineered with the performance of C and GTK4 Layer Shell for smooth "
        "auto-hide and animations, featuring dynamic Wayland toplevel window "
        "tracking, live CSS hot-reloading, and dynamic configuration powered "
        "by GNU Guile Scheme."));

  // Authors & Copyright
  const char *authors[] = {"Josep Bigorra (jjbigorra@gmail.com)", NULL};
  gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(about), authors);

  gtk_about_dialog_set_copyright(
      GTK_ABOUT_DIALOG(about),
      "Copyright © Josep Bigorra <jjbigorra@gmail.com>");

  // License (MIT)
  gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(about),
                                    GTK_LICENSE_MIT_X11);

  // Repository Web Links
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(about),
                               "https://codeberg.org/jjba23/lambdock");
  gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(about),
                                     _("Find the project's source code here"));

  // Logo Loading Logic from compiled GResource bundle
  const char *logo_resource =
      "/org/jointhefreeworld/lambdock/img/agnus-dei.png";
  GdkTexture *texture = gdk_texture_new_from_resource(logo_resource);

  if (texture) {
    gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(about), GDK_PAINTABLE(texture));
    g_object_unref(texture); // Drop local texture reference
  } else {
    LOG_C_WARN("About", "Failed to load logo from GResource: %s",
               logo_resource);
    gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(about), "lambdock");
  }

  // Display dialog modally
  gtk_window_set_modal(GTK_WINDOW(about), TRUE);
  gtk_window_present(GTK_WINDOW(about));
}

/* =========================================================================
 * MONITOR VALIDATION & DIMENSION UPDATES
 * ========================================================================= */

gboolean is_monitor_valid(GdkMonitor *mon) {
  if (!mon)
    return FALSE;
  GdkDisplay *display = gdk_display_get_default();
  if (!display)
    return FALSE;
  GListModel *monitors = gdk_display_get_monitors(display);
  if (!monitors)
    return FALSE;
  guint n = g_list_model_get_n_items(monitors);
  for (guint i = 0; i < n; i++) {
    gpointer item = g_list_model_get_item(monitors, i);
    gboolean match = (item == mon);
    if (item)
      g_object_unref(item);
    if (match)
      return TRUE;
  }
  return FALSE;
}

void update_dock_dimensions(LambdockState *state) {
  if (!state || !state->bg_capsule || !state->overlay)
    return;

  int bg_fixed_dim = dock_capsule_cross_dim(state);
  int max_cross_dim = dock_cross_axis_dim(state);

  GdkMonitor *mon = state->monitor;
  GdkDisplay *display = gdk_display_get_default();

  // Safely verify if stored monitor is still valid; if not, fall back to
  // default
  if (!mon || !is_monitor_valid(mon)) {
    mon = NULL;
    if (display) {
      GListModel *monitors = gdk_display_get_monitors(display);
      if (monitors && g_list_model_get_n_items(monitors) > 0) {
        gpointer item = g_list_model_get_item(monitors, 0);
        if (item) {
          mon = GDK_MONITOR(item); // Keep item ref until done
        }
      }
    }
  } else {
    g_object_ref(mon);
  }

  int mon_width = 1920;
  int mon_height = 1080;
  if (mon) {
    GdkRectangle geom;
    gdk_monitor_get_geometry(mon, &geom);
    if (geom.width > 0)
      mon_width = geom.width;
    if (geom.height > 0)
      mon_height = geom.height;
    g_object_unref(mon); // Clean up reference ONLY after reading geometry
  }

  int max_main_dim_h = mon_width - 32;
  int max_main_dim_v = mon_height - 32;
  if (max_main_dim_h < 100)
    max_main_dim_h = 100;
  if (max_main_dim_v < 100)
    max_main_dim_v = 100;

  if (state->size_group) {
    GtkSizeGroupMode mode = (state->orientation == GTK_ORIENTATION_HORIZONTAL)
                                ? GTK_SIZE_GROUP_HORIZONTAL
                                : GTK_SIZE_GROUP_VERTICAL;
    gtk_size_group_set_mode(state->size_group, mode);
  }

  if (state->scrolled_window) {
    if (state->orientation == GTK_ORIENTATION_HORIZONTAL) {
      gtk_scrolled_window_set_policy(
          GTK_SCROLLED_WINDOW(state->scrolled_window), GTK_POLICY_EXTERNAL,
          GTK_POLICY_NEVER);
      gtk_scrolled_window_set_max_content_width(
          GTK_SCROLLED_WINDOW(state->scrolled_window), max_main_dim_h);
    } else {
      gtk_scrolled_window_set_policy(
          GTK_SCROLLED_WINDOW(state->scrolled_window), GTK_POLICY_NEVER,
          GTK_POLICY_EXTERNAL);
      gtk_scrolled_window_set_max_content_height(
          GTK_SCROLLED_WINDOW(state->scrolled_window), max_main_dim_v);
    }
  }

  /* The room between the capsule's ends and the icons: the theme's button
   * padding sits on top of it. */
  int padding = (state->capsule_padding >= 0) ? state->capsule_padding
                                              : CAPSULE_MAIN_AXIS_PADDING_PX;

  if (state->orientation == GTK_ORIENTATION_HORIZONTAL) {
    gtk_widget_set_margin_start(state->box, padding);
    gtk_widget_set_margin_end(state->box, padding);
    gtk_widget_set_margin_top(state->box, 0);
    gtk_widget_set_margin_bottom(state->box, 0);

    /* Keep the icons off the outer edge by the front glass thickness, so the
     * mirrors (and any front face) have room in front of them. The reflection
     * widget already occupies the band inside that glass, hence the difference:
     * the icons' bottom edge ends up exactly `front` pixels from the screen
     * edge, with the mirror in the glass below it. That is what puts the icons'
     * centre on the slab's top edge, as in the OS X dock. */
    LambdockShelfGeometry geometry =
        lambdock_shelf_geometry_for(state, state->icon_size);
    int outer_margin = geometry.outer_px - geometry.band_px;
    if (outer_margin < 0)
      outer_margin = 0;
    if (outer_margin > 0) {
      if (state->edge == GTK_LAYER_SHELL_EDGE_TOP)
        gtk_widget_set_margin_top(state->box, outer_margin);
      else
        gtk_widget_set_margin_bottom(state->box, outer_margin);
    }

    gtk_widget_set_size_request(state->overlay, -1, max_cross_dim);
    gtk_widget_set_size_request(state->bg_capsule, -1, bg_fixed_dim);

    if (state->scrolled_window) {
      gtk_widget_set_valign(state->scrolled_window,
                            (state->edge == GTK_LAYER_SHELL_EDGE_TOP)
                                ? GTK_ALIGN_START
                                : GTK_ALIGN_END);
      gtk_widget_set_halign(state->scrolled_window,
                            state->taskbar ? GTK_ALIGN_FILL
                                           : GTK_ALIGN_CENTER);
      /* In taskbar mode the content viewport must expand to fill the bar so
       * icons grow from the left edge. */
      gtk_widget_set_hexpand(state->scrolled_window, state->taskbar);
    }

    if (state->taskbar) {
      gtk_widget_set_halign(state->bg_capsule, GTK_ALIGN_FILL);
      gtk_widget_set_halign(state->box, GTK_ALIGN_START);
    } else {
      gtk_widget_set_halign(state->bg_capsule, GTK_ALIGN_CENTER);
      gtk_widget_set_halign(state->box, GTK_ALIGN_CENTER);
    }

    if (state->edge == GTK_LAYER_SHELL_EDGE_TOP) {
      gtk_widget_set_valign(state->bg_capsule, GTK_ALIGN_START);
      gtk_widget_set_valign(state->box, GTK_ALIGN_START);
    } else {
      gtk_widget_set_valign(state->bg_capsule, GTK_ALIGN_END);
      gtk_widget_set_valign(state->box, GTK_ALIGN_END);
    }
  } else {
    gtk_widget_set_margin_top(state->box, padding);
    gtk_widget_set_margin_bottom(state->box, padding);
    gtk_widget_set_margin_start(state->box, 0);
    gtk_widget_set_margin_end(state->box, 0);

    /* Same on side docks. */
    LambdockShelfGeometry geometry =
        lambdock_shelf_geometry_for(state, state->icon_size);
    int outer_margin = geometry.outer_px - geometry.band_px;
    if (outer_margin < 0)
      outer_margin = 0;
    if (outer_margin > 0) {
      if (state->edge == GTK_LAYER_SHELL_EDGE_LEFT)
        gtk_widget_set_margin_start(state->box, outer_margin);
      else
        gtk_widget_set_margin_end(state->box, outer_margin);
    }

    gtk_widget_set_size_request(state->overlay, max_cross_dim, -1);
    gtk_widget_set_size_request(state->bg_capsule, bg_fixed_dim, -1);

    if (state->scrolled_window) {
      gtk_widget_set_halign(state->scrolled_window,
                            (state->edge == GTK_LAYER_SHELL_EDGE_LEFT)
                                ? GTK_ALIGN_START
                                : GTK_ALIGN_END);
      gtk_widget_set_valign(state->scrolled_window,
                            state->taskbar ? GTK_ALIGN_FILL
                                           : GTK_ALIGN_CENTER);
      gtk_widget_set_vexpand(state->scrolled_window, state->taskbar);
    }

    if (state->taskbar) {
      gtk_widget_set_valign(state->bg_capsule, GTK_ALIGN_FILL);
      gtk_widget_set_valign(state->box, GTK_ALIGN_START);
    } else {
      gtk_widget_set_valign(state->bg_capsule, GTK_ALIGN_CENTER);
      gtk_widget_set_valign(state->box, GTK_ALIGN_CENTER);
    }

    if (state->edge == GTK_LAYER_SHELL_EDGE_LEFT) {
      gtk_widget_set_halign(state->bg_capsule, GTK_ALIGN_START);
      gtk_widget_set_halign(state->box, GTK_ALIGN_START);
    } else {
      gtk_widget_set_halign(state->bg_capsule, GTK_ALIGN_END);
      gtk_widget_set_halign(state->box, GTK_ALIGN_END);
    }
  }

  /* Size, show or hide the glass-shelf reflections so they always match the
   * current icon size and shelf parameters. */
  apply_shelf_layout(state);

  /* The shelf wrapper is what the overlay actually positions, so mirror the
   * capsule's alignment onto it (the capsule itself stays aligned for themes
   * and for the size group). */
  if (state->bg_shelf) {
    gtk_widget_set_halign(state->bg_shelf,
                          gtk_widget_get_halign(state->bg_capsule));
    gtk_widget_set_valign(state->bg_shelf,
                          gtk_widget_get_valign(state->bg_capsule));
  }
}

/*
 * apply_dock_mode_classes:
 * Toggles the `taskbar` CSS class on the background capsule so themes can give
 * taskbar mode its own rectangular, edge-to-edge styling, and the `shelf-*`
 * classes that let any theme restyle the glass shelf.
 */
static const char *shelf_edge_class(GtkLayerShellEdge edge) {
  switch (edge) {
  case GTK_LAYER_SHELL_EDGE_TOP:
    return "shelf-top";
  case GTK_LAYER_SHELL_EDGE_LEFT:
    return "shelf-left";
  case GTK_LAYER_SHELL_EDGE_RIGHT:
    return "shelf-right";
  case GTK_LAYER_SHELL_EDGE_BOTTOM:
  default:
    return "shelf-bottom";
  }
}

/**
 * shelf_tilt_for_edge:
 * Sign of the shelf tilt: the edge touching the screen stays pinned while the
 * inner edge recedes away from the viewer, so top/left docks tilt backwards.
 */
static double shelf_tilt_for_edge(GtkLayerShellEdge edge, double degrees) {
  switch (edge) {
  case GTK_LAYER_SHELL_EDGE_TOP:
  case GTK_LAYER_SHELL_EDGE_LEFT:
    return -degrees;
  case GTK_LAYER_SHELL_EDGE_RIGHT:
  case GTK_LAYER_SHELL_EDGE_BOTTOM:
  default:
    return degrees;
  }
}

static void set_css_class(GtkWidget *widget, const char *name, gboolean enabled) {
  if (!widget)
    return;

  if (enabled) {
    if (!gtk_widget_has_css_class(widget, name))
      gtk_widget_add_css_class(widget, name);
  } else if (gtk_widget_has_css_class(widget, name)) {
    gtk_widget_remove_css_class(widget, name);
  }
}

void apply_dock_mode_classes(LambdockState *state) {
  if (!state || !state->bg_capsule)
    return;

  set_css_class(state->bg_capsule, "taskbar", state->taskbar);

  /* Shelf classes. They are set on both the window (so user CSS can target the
   * whole dock) and the capsule (so the shelf itself, and the matching
   * modifier stylesheet, can be styled per docked edge). They appear whenever
   * the shelf parameters differ from the plain capsule. */
  gboolean shelf_active = lambdock_shelf_active(state);
  static const char *edge_classes[] = {"shelf-bottom", "shelf-top",
                                       "shelf-left", "shelf-right"};
  for (gsize i = 0; i < G_N_ELEMENTS(edge_classes); i++) {
    set_css_class(state->bg_capsule, edge_classes[i], FALSE);
    set_css_class(state->window, edge_classes[i], FALSE);
  }

  set_css_class(state->bg_capsule, "shelf-mode", shelf_active);
  set_css_class(state->window, "shelf-mode", shelf_active);

  /* `shelf-mirror` means the glass actually shows icon reflections: the
   * reflection widget then draws the running indicator itself, so the CSS one
   * must stay quiet. */
  gboolean mirroring =
      shelf_active &&
      lambdock_shelf_geometry_for(state, state->icon_size).band_px > 0;
  set_css_class(state->bg_capsule, "shelf-mirror", mirroring);
  set_css_class(state->window, "shelf-mirror", mirroring);

  if (shelf_active) {
    const char *edge_class = shelf_edge_class(state->edge);
    set_css_class(state->bg_capsule, edge_class, TRUE);
    set_css_class(state->window, edge_class, TRUE);
  }

  /* Project the styled capsule onto the shape described by the shelf
   * parameters: the tilt, the keystone (flare) or both. With neither, the frame
   * is a pass-through. */
  if (state->bg_shelf) {
    shelf_frame_set_shape(state->bg_shelf, state->edge,
                          shelf_tilt_for_edge(state->edge,
                                              state->shelf.tilt_deg),
                          state->shelf.perspective, state->shelf.flare_px);
  }
}

/*
 * refresh_dock_mode:
 * Re-applies layer-shell anchors, mode CSS classes and dimension requests so a
 * live config reload can switch between floating dock and full-width taskbar
 * without restarting.
 */
void refresh_dock_mode(LambdockState *state) {
  if (!state || !state->window)
    return;

  gboolean horizontal_edge = (state->edge == GTK_LAYER_SHELL_EDGE_TOP ||
                              state->edge == GTK_LAYER_SHELL_EDGE_BOTTOM);
  GtkWindow *window = GTK_WINDOW(state->window);

  if (horizontal_edge) {
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_LEFT, state->taskbar);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_RIGHT, state->taskbar);
  } else {
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_TOP, state->taskbar);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_BOTTOM, state->taskbar);
  }

  apply_dock_mode_classes(state);
  update_dock_dimensions(state);
}

/* =========================================================================
 * MEMORY & DESTRUCTORS
 * ========================================================================= */

void destroy_dock_instance(LambdockState *state) {
  // Re-entrancy Guard: Prevent recursive re-entry during GTK signal emission
  if (!state || state->is_destroying)
    return;

  state->is_destroying = TRUE;

  GdkDisplay *display = gdk_display_get_default();
  if (display) {
    if (state->base_css_provider) {
      gtk_style_context_remove_provider_for_display(
          display, GTK_STYLE_PROVIDER(state->base_css_provider));
      g_clear_object(&state->base_css_provider);
    }
    if (state->taskbar_css_provider) {
      gtk_style_context_remove_provider_for_display(
          display, GTK_STYLE_PROVIDER(state->taskbar_css_provider));
      g_clear_object(&state->taskbar_css_provider);
    }
    if (state->shelf_css_provider) {
      gtk_style_context_remove_provider_for_display(
          display, GTK_STYLE_PROVIDER(state->shelf_css_provider));
      g_clear_object(&state->shelf_css_provider);
    }
    if (state->user_css_provider) {
      gtk_style_context_remove_provider_for_display(
          display, GTK_STYLE_PROVIDER(state->user_css_provider));
      g_clear_object(&state->user_css_provider);
    }
  }

  if (state->active_theme_class) {
    g_free(state->active_theme_class);
    state->active_theme_class = NULL;
  }

  /* The launchpad is a global overlay that belongs to no single dock: take it
   * down with the dock it was opened from (it is recreated lazily). */
  lambdock_launchpad_shutdown();

  // Unregister from active global state list
  unregister_dock_state(state);

  // Cancel pending GLib autohide timers immediately
  if (state->hide_timer_id > 0) {
    g_source_remove(state->hide_timer_id);
    state->hide_timer_id = 0;
  }

  // Remove active GTK tick animation callbacks
  if (state->tick_cb_id > 0 && state->window && GTK_IS_WIDGET(state->window)) {
    gtk_widget_remove_tick_callback(state->window, state->tick_cb_id);
    state->tick_cb_id = 0;
  }

  // Disconnect signal handlers from window to prevent "destroy" signal
  // re-entrancy
  if (state->window && GTK_IS_WIDGET(state->window)) {
    g_signal_handlers_disconnect_by_data(state->window, state);
  }

  // Unparent the main background context menu popover
  if (state->popover) {
    if (GTK_IS_WIDGET(state->popover)) {
      /* The popover "closed" handler is connected with `state` as data and would
       * schedule a hide timer on this soon-to-be-freed state; disconnect it
       * before unparenting so no callback runs against freed memory. */
      g_signal_handlers_disconnect_by_data(state->popover, state);
      if (gtk_widget_get_parent(state->popover)) {
        gtk_widget_unparent(state->popover);
      }
    }
    state->popover = NULL;
  }

  // Clear and free dock items (and their individual polling timers/popovers)
  GList *items = state->dock_items;
  state->dock_items = NULL; // Clear pointer first to prevent dangling access
  for (GList *l = items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    free_dock_item(item);
  }
  g_list_free(items);

  // Clear and free Wayland window trackers
  GList *trackers = state->window_trackers;
  state->window_trackers = NULL; // Clear pointer first
  for (GList *l = trackers; l != NULL; l = l->next) {
    free_window_tracker(l->data);
  }
  g_list_free(trackers);

  // Destroy the GTK Window widget safely
  if (state->window && GTK_IS_WINDOW(state->window)) {
    GtkWidget *win = state->window;
    state->window = NULL; // Nullify reference prior to GTK destruction
    state->bg_shelf = NULL;
    state->bg_capsule = NULL;
    gtk_layer_set_monitor(GTK_WINDOW(win), NULL);
    gtk_window_destroy(GTK_WINDOW(win));
  }

  if (state->size_group) {
    g_object_unref(state->size_group);
    state->size_group = NULL;
  }

  if (state->monitor) {
    g_signal_handlers_disconnect_by_data(state->monitor, state);
    g_object_unref(state->monitor);
    state->monitor = NULL;
  }

  g_free(state->config_path);
  free(state->theme); /* get_theme_config_for may return a scm_to_utf8_string result */
  g_free(state);
}
