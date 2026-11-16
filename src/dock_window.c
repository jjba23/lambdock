// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

/* =========================================================================
 * THE DOCK WINDOW
 * =========================================================================
 * This module wires one dock's window: the layer-shell surface, the slide and
 * hover animations, auto-hide, the mode CSS classes, and the teardown that
 * takes it all down again.
 *
 * It is organised in four layers, in this order:
 *
 *   1. types            -- values that describe what to do (no widgets touched)
 *   2. pure helpers     -- decisions from the state alone; no side effects
 *   3. small helpers    -- one widget concern each, plus the row walk and the
 *                          timer plumbing every callback goes through
 *   4. callbacks & API  -- thin GTK entry points that read inputs, call the
 *                          helpers above, and return
 *
 * The arithmetic that used to be inline in the animation and fisheye code now
 * lives in `core/dock_motion.h`, where it is unit tested without a display.
 */

#include "dock_window.h"

#include "accessibility.h"
#include "core/dock_dims.h"
#include "core/dock_motion.h"
#include "core/render_stats.h"
#include "guile_bridge.h"
#include "logger.h"
#include "toplevel_tracker.h"
#include "ui/dock_item.h"
#include "ui/icon_resolver.h"
#include "ui/icon_zoom.h"
#include "ui/launchpad.h"
#include "ui/popover_menu.h"
#include "ui/shelf.h"
#include "ui/theme_engine.h"
#include "ui/widget_utils.h"
#include "x11_tracker.h"
#include <glib/gi18n.h>
#include <math.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

/* =========================================================================
 * TYPES
 * =========================================================================
 * Small value structs that say *what* the window should look like. Each one is
 * produced by a pure function below and consumed by a single applier, so the
 * decision and the widget calls never mix.
 */

/* Everything the layer-shell surface needs to know, as a value. */
typedef struct DockSurfacePolicy {
  gboolean anchor_top;
  gboolean anchor_bottom;
  gboolean anchor_left;
  gboolean anchor_right;
  int exclusive_zone_px;
} DockSurfacePolicy;

/* Monitor geometry in pixels; 0 x 0 means "no monitor". */
typedef struct DockMonitorSize {
  int width;
  int height;
} DockMonitorSize;

/* How long after a slide settles a pointer crossing is still treated as an echo
 * of the slide itself. The crossings that make the dock bounce arrive within a
 * frame or two of the animation stopping. */
#define AUTO_HIDE_SETTLE_MS 300

/* =========================================================================
 * DIAGNOSTICS
 * =========================================================================
 * Auto-hide is a state machine driven by events we do not get to see: pointer
 * crossings on the user's compositor, the surface position the compositor
 * chose, and the input region it was given. "It hides but it does not reveal"
 * is then a report about a sequence, not about a value, and the sequence is not
 * reproducible anywhere but on that machine.
 *
 * With LAMBDOCK_DEBUG=1 every decision that sequence turns on is printed once,
 * on the transition it happens at -- so the log reads as the dock's own account
 * of why it stayed hidden. It is off by default and costs one getenv() per
 * process, so it can stay.
 */
static gboolean dock_trace_enabled(void) {
  static int enabled = -1;

  if (enabled < 0)
    enabled = (g_getenv("LAMBDOCK_DEBUG") != NULL) ? 1 : 0;

  return enabled == 1;
}

#define DOCK_TRACE(fmt, ...)                                                   \
  do {                                                                         \
    if (dock_trace_enabled()) {                                                \
      LOG_C_DEBUG("auto-hide", "t=%" G_GINT64_FORMAT " " fmt,                  \
                  g_get_monotonic_time() / 1000, ##__VA_ARGS__);               \
      /* Redirected stdout is block buffered, and a trace that arrives after   \
       * the fact is useless for correlating with what the user did. */        \
      fflush(stdout);                                                          \
    }                                                                          \
  } while (0)

/* =========================================================================
 * PURE HELPERS
 * =========================================================================
 * No widget is mutated below this line until the next section: everything here
 * is a function of the state (and, at most, of the monotonic clock).
 */

/* =========================================================================
 * SMALL SIDE-EFFECTING HELPERS
 * =========================================================================
 * One widget concern each. Everything below is idempotent, so it can be
 * re-applied on every reload or frame without bookkeeping.
 */

/* What is left of the layout and the slide lives in `rust/src/dock_window.rs`
 * now: the hover path, the geometry update and the crossing logic call into it,
 * so it crosses the seam by name. */
void dock_schedule_input_region(LambdockState *state);
void dock_reserve_hover_room(LambdockState *state);
int dock_icon_size(const LambdockState *state);
const char *shelf_edge_class(GtkLayerShellEdge edge);
double shelf_tilt_for_edge(GtkLayerShellEdge edge, double degrees);
DockSurfacePolicy dock_surface_policy_for(const LambdockState *state);
void stop_pointer_tick(LambdockState *state);
void reset_dock_items_scale(LambdockState *state);
void cancel_hide_timer(LambdockState *state);
void schedule_hide_timer(LambdockState *state, guint delay_ms);
void for_each_dock_item(GList *items,
                        void (*visit)(DockItem *item, gpointer data),
                        gpointer data);
GtkWidget *get_item_zoom_widget(DockItem *item);

/* =========================================================================
 * AUTO-HIDE & REVEAL
 * ========================================================================= */

/* =========================================================================
 * MOTION CONTROLLER CALLBACKS
 * =========================================================================
 * Thin: read the inputs, call one helper, return. The decisions they encode are
 * documented where they are made.
 */

/** Pointer entered the dock: whatever was queued is stale, and it must be
 * revealed. */
static void on_motion_enter(GtkEventControllerMotion *controller, double x,
                            double y, gpointer user_data) {
  (void)controller;

  LambdockState *state = (LambdockState *)user_data;
  DOCK_TRACE("enter at %.1f,%.1f (hidden %d, margin %d)", x, y,
             state->is_hidden, state->current_margin);

  state->pointer_applied = FALSE;

  /* Make room for the spread before it starts: sizing the surface now, with the
   * row still at rest, is what keeps the hover itself free of resizes. */
  dock_reserve_hover_room(state);
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

  /* One-shot: the next motion schedules the next one. */
  state->pointer_tick_id = 0;

  /* Nothing moved since the last frame (the pointer can repeat a position), so
   * the icons are already where they belong. */
  if (state->pointer_applied &&
      !lambdock_points_differ(state->pointer_x, state->pointer_y,
                              state->applied_pointer_x,
                              state->applied_pointer_y, 1.0)) {
    return G_SOURCE_REMOVE;
  }

  state->applied_pointer_x = state->pointer_x;
  state->applied_pointer_y = state->pointer_y;
  state->pointer_applied = TRUE;

  dock_apply_hover_at(state, state->pointer_x, state->pointer_y);

  return G_SOURCE_REMOVE;
}

/**
 * on_motion_leave:
 * Pointer left the dock surface: stop magnifying, and start the hide countdown.
 */
static void on_motion_leave(GtkEventControllerMotion *controller,
                            gpointer user_data) {
  (void)controller;
  LambdockState *state = (LambdockState *)user_data;

  if (!state)
    return;

  /* Not every leave means the pointer went anywhere. The dock slides out from
   * under a pointer that is not moving -- the window moves, the pointer does
   * not -- and compositors report a crossing that follows from that, which is
   * what used to bounce the dock at its screen edge: a leave hides it, the
   * revealing strip comes back under the pointer, an enter reveals it, and
   * round it goes. Those echoes all arrive while the dock is moving, or within
   * a frame or two of it stopping, so the settle window is what tells them
   * apart.
   *
   * It used to also require that the pointer had moved since it entered, on the
   * reasoning that a pointer that has not moved cannot have left. It can: a
   * pointer that touches the edge to reveal the dock and then moves away has
   * been still for the whole reveal, and its leave was thrown away with it, so
   * the dock stayed out until the pointer came back and did the whole thing
   * again -- auto-hide that quietly stops hiding. */
  if (dock_is_settling(state)) {
    DOCK_TRACE("leave ignored: the dock is sliding, or just stopped");
    return;
  }

  DOCK_TRACE("leave: hiding in %d ms", AUTOHIDE_LEAVE_DELAY_MS);

  stop_pointer_tick(state);
  state->pointer_applied = FALSE;

  if (state->hover_animate)
    reset_dock_items_scale(state);

  if (state->auto_hide)
    schedule_hide_timer(state, AUTOHIDE_LEAVE_DELAY_MS);
}

/**
 * on_motion_notify:
 * Records where the pointer is; the frame clock does the magnifying.
 *
 * It is also the dock's *reveal* trigger, which is the one thing about it that
 * is not obvious. A pointer only has to cross into the dock once to be inside
 * it, and a hidden dock is on screen only as the trigger strip: a pointer that
 * rests on the strip while the surface slides away underneath it never leaves
 * -- the compositor keeps sending it motion, not a crossing -- so it can never
 * cross back in. Waiting for an enter therefore stranded the dock off screen
 * for exactly as long as the pointer stayed at the screen edge, which is where
 * a pointer that just used the dock tends to be. Motion inside the surface
 * while the dock is away means the same thing an enter would: the pointer is
 * on the strip, and the dock belongs on screen.
 */
static void on_motion_notify(GtkEventControllerMotion *controller, double x,
                             double y, gpointer user_data) {
  (void)controller;
  LambdockState *state = (LambdockState *)user_data;

  if (!state)
    return;

  if (state->is_hidden) {
    DOCK_TRACE("motion at %.0f,%.0f on the strip: revealing", x, y);
    reveal_dock(state);
    return;
  }

  if (!state->hover_animate || state->tick_cb_id != 0 ||
      state->current_margin != 0)
    return;

  state->pointer_x = x;
  state->pointer_y = y;

  if (state->pointer_tick_id == 0 && state->window) {
    state->pointer_tick_id =
        gtk_widget_add_tick_callback(state->window, on_hover_tick, state, NULL);
  }
}

/* =========================================================================
 * BACKGROUND CONTEXT MENU
 * ========================================================================= */

/**
 * on_surface_resized:
 * GTK decides its toplevels' sizes by itself, on the first frames of a window
 * and whenever a size request changes -- and X11 has no protocol where the
 * dock's placement wins. So the placement is re-run whenever the surface's size
 * changes: it is idempotent and reads the window back, so a dock that is
 * already where it belongs costs two round trips and no move.
 */
static void on_surface_resized(GObject *surface, GParamSpec *pspec,
                               gpointer user_data) {
  (void)surface;
  (void)pspec;

  lambdock_x11_dock_apply((LambdockState *)user_data);
}

/** The same, for `GdkSurface::layout`, which passes the new size rather than a
 * property. Connecting it to the handler above would hand it the height as a
 * state pointer. */
static void on_surface_layout(GdkSurface *surface, int width, int height,
                              gpointer user_data) {
  (void)surface;
  (void)width;
  (void)height;

  lambdock_x11_dock_apply((LambdockState *)user_data);
}

/** Applies the input region whenever the dock window (re)appears. */
static void on_window_map(GtkWidget *widget, gpointer user_data) {
  (void)widget;

  LambdockState *state = (LambdockState *)user_data;
  dock_schedule_input_region(state);

  /* The window id only exists once the surface is realised, so this is the
   * first moment an X11 dock can be typed, moved and given its strut. On
   * Wayland it is a no-op. */
  if (!state->x11_surface) {
    GtkNative *native = gtk_widget_get_native(state->window);
    GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;

    if (surface) {
      /* `layout` is the surface telling us it was laid out; the size
       * notifications cover a resize that arrives without a layout. */
      g_signal_connect(surface, "layout", G_CALLBACK(on_surface_layout), state);
      g_signal_connect(surface, "notify::width",
                       G_CALLBACK(on_surface_resized), state);
      g_signal_connect(surface, "notify::height",
                       G_CALLBACK(on_surface_resized), state);
      state->x11_surface = surface;
    }
  }

  lambdock_x11_dock_apply(state);
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

  /* The surface is bigger than the dock, so its clickable area has to be cut
   * down to the dock every time the window appears. */
  g_signal_connect(state->window, "map", G_CALLBACK(on_window_map), state);
  dock_schedule_input_region(state);
}
