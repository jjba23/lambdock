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

/* What the fisheye needs for one frame, computed once per pointer position. */
typedef struct FisheyeFrame {
  const LambdockState *state;
  double pointer_axis; /* pointer position along the dock's main axis */
  int base_size_px;
  double radius_px;
  double max_scale;
  GArray *centres;      /* item centres along the main axis, cached */
  guint index;          /* item being visited */
  gboolean geometry_changed; /* set when a size request moved an item */
} FisheyeFrame;

/* Callback for the dock-row walk. */
typedef void (*DockItemVisitor)(DockItem *item, gpointer data);

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
 * FISHEYE HOVER MAGNIFICATION
 * =========================================================================
 * The bell curve itself is in rust/src/core/dock_motion.rs; this half only reads
 * widget bounds and pushes the resulting sizes into the row.
 */

/** Grows the glass reflection with a magnified icon, keeping the band fixed. */
static void apply_reflection_extent(const DockItem *item,
                                    const LambdockState *state, int icon_px,
                                    int base_px) {
  if (!item->reflection || !lambdock_shelf_active(state))
    return;

  double band_ratio =
      lambdock_shelf_params_effective(state).reflection_ratio;
  int band_px = (int)round((double)base_px * band_ratio);

  if (band_px > 0)
    shelf_reflection_set_extent(item->reflection, icon_px, band_px);
}

/**
 * refresh_item_centres:
 * Rebuilds the cache of item centres along the dock's main axis.
 *
 * One walk of the row (a transform query per item) instead of one per pointer
 * frame: the fisheye needs to know where each item *is*, and that only changes
 * when something resizes the row -- an icon size, the item list, a mode change,
 * or the magnification itself stepping to a new size.
 */
static void refresh_item_centres(LambdockState *state) {
  if (!state)
    return;

  if (!state->item_centres)
    state->item_centres = g_array_new(FALSE, FALSE, sizeof(gint));
  g_array_set_size(state->item_centres, 0);

  gboolean horizontal = state->orientation == GTK_ORIENTATION_HORIZONTAL;

  /* The row is the reference, not the window: it is the frame the items live in,
   * and measuring against it is a shorter walk than measuring against the
   * surface. The pointer arrives in window coordinates, so the row's own origin
   * is what has to be subtracted from it. */
  GtkWidget *reference = state->box ? state->box : state->window;

  state->box_origin_px = 0.0;
  if (state->window && reference && reference != state->window) {
    graphene_rect_t origin;
    if (gtk_widget_compute_bounds(reference, state->window, &origin))
      state->box_origin_px =
          horizontal ? origin.origin.x : origin.origin.y;
  }

  for (GList *node = state->dock_items; node != NULL; node = node->next) {
    DockItem *item = node->data;
    gint centre = 0;

    graphene_rect_t bounds;
    if (item && item->button && reference &&
        gtk_widget_compute_bounds(item->button, reference, &bounds)) {
      centre = (gint)lround(horizontal
                                ? bounds.origin.x + bounds.size.width / 2.0
                                : bounds.origin.y + bounds.size.height / 2.0);
    }

    g_array_append_val(state->item_centres, centre);
  }

  state->centres_dirty = FALSE;
  lambdock_stats_bump(LAMBDOCK_STAT_GEOMETRY_REFRESHES);
}

/**
 * apply_fisheye_to_item:
 * Sizes one dock item for the pointer position described by @data.
 */
static void apply_fisheye_to_item(DockItem *item, gpointer data) {
  FisheyeFrame *frame = (FisheyeFrame *)data;
  const LambdockState *state = frame->state;
  gboolean horizontal = (state->orientation == GTK_ORIENTATION_HORIZONTAL);

  /* Guard: the button has to be a GtkButton, and the item has to allow hover
   * magnification (separators and meters do not). */
  if (!item->hover_animate || !item->button || !GTK_IS_BUTTON(item->button))
    return;

  GtkWidget *zoom = get_item_zoom_widget(item);
  if (!zoom)
    return;

  if (!frame->centres || frame->index >= frame->centres->len)
    return;

  double center =
      (double)g_array_index(frame->centres, gint, frame->index);

  double scale =
      lambdock_fisheye_scale(frame->pointer_axis - center, frame->radius_px,
                             frame->max_scale);
  int target_px = lambdock_scaled_size(frame->base_size_px, scale);

  /* The paint follows the pointer continuously (below); only the layout steps.
   * Coarsening the magnified sizes to a couple of pixels cuts the row's
   * re-measures by about half, and the resting size stays exact. */
  int layout_px = (target_px > frame->base_size_px)
                      ? lambdock_size_step(target_px, FISHEYE_LAYOUT_STEP_PX)
                      : target_px;

  if (lambdock_icon_zoom_target_size(zoom) != layout_px) {
    lambdock_stats_bump(LAMBDOCK_STAT_LAYOUT_STEPS);

    /* Expand along the dock axis; preserve the base size across it. */
    if (horizontal)
      gtk_widget_set_size_request(item->button, layout_px, frame->base_size_px);
    else
      gtk_widget_set_size_request(item->button, frame->base_size_px, layout_px);

    /* Mirror the magnified icon in its reflection: the copy grows along the
     * dock axis but keeps the band thickness, so the row of icons does not
     * shift while hovering. */
    apply_reflection_extent(item, state, layout_px, frame->base_size_px);

    /* The row just changed size: the cached centres are stale for the next
     * frame. */
    frame->geometry_changed = TRUE;
  }

  /* The layout steps in whole pixels, the icon does not: the zoom paints the
   * artwork through a transform, so magnification follows the pointer
   * continuously and costs no rasterisation at all. */
  lambdock_icon_zoom_set_scale(zoom, scale);
}

/** Visitor wrapper: hands each item its index in the geometry cache. */
static void apply_fisheye_to_item_at(DockItem *item, gpointer data) {
  FisheyeFrame *frame = (FisheyeFrame *)data;

  apply_fisheye_to_item(item, frame);
  frame->index++;
}

/**
 * dock_apply_hover_at:
 * Applies the fish-eye for a pointer at (@x, @y) in window coordinates.
 *
 * This is the whole hover path: a pointer frame costs one cached geometry lookup
 * and some arithmetic per item, plus (only when an item's layout step actually
 * changed) one size request. Everything else -- the magnification itself -- is
 * painted through a transform, so the row does not relayout to look magnified.
 *
 * Hover magnification only applies to a settled, visible dock: while it slides
 * (or is hidden) the widget bounds below are stale and the pointer position is
 * not the user's.
 */
void dock_apply_hover_at(LambdockState *state, double x, double y) {
  if (!state || !state->dock_items || !state->hover_animate ||
      !lambdock_animations_enabled(state) || state->is_hidden ||
      state->tick_cb_id != 0 || state->current_margin != 0)
    return;

  lambdock_stats_bump(LAMBDOCK_STAT_HOVER_FRAMES);

  gboolean horizontal = (state->orientation == GTK_ORIENTATION_HORIZONTAL);
  double axis = horizontal ? x : y;

  /* A trackpad reports sub-pixel movement continuously, and half a pixel changes
   * nothing anyone can see: the icons would be repainted at the same layout step
   * and a magnification nobody can perceive. The frame is dropped before it
   * touches a widget. */
  if (state->hover_axis_valid &&
      fabs(axis - state->applied_hover_axis) < FISHEYE_POINTER_STEP_PX) {
    lambdock_stats_bump(LAMBDOCK_STAT_HOVER_SKIPS);
    return;
  }

  state->hover_axis_valid = TRUE;
  state->applied_hover_axis = axis;

  /* What one hover frame costs *us*, as opposed to what GTK spends laying the
   * row out and painting it afterwards: the two are worth telling apart before
   * optimising either. */
  gint64 started_us = g_get_monotonic_time();

  if (state->centres_dirty || !state->item_centres)
    refresh_item_centres(state);

  FisheyeFrame frame = {
      .state = state,
      /* The centres are relative to the row, the pointer to the window. */
      .pointer_axis = axis - state->box_origin_px,
      .base_size_px = dock_icon_size(state),
      .radius_px = FISHEYE_RADIUS_PX,
      .max_scale = FISHEYE_MAX_SCALE,
      .centres = state->item_centres,
      .index = 0,
  };

  for_each_dock_item(state->dock_items, apply_fisheye_to_item_at, &frame);

  if (frame.geometry_changed)
    state->centres_dirty = TRUE;

  /* The capsule grows under the pointer, so the clickable region has to follow
   * it; the tick that sizes the surface is the same one that cuts it out. */
  state->fisheye_active = TRUE;
  dock_schedule_input_region(state);

  lambdock_stats_add(LAMBDOCK_STAT_HOVER_US,
                     (guint64)(g_get_monotonic_time() - started_us));
}

/**
 * update_dock_items_fisheye:
 * The dock's own entry point for a pointer position (see dock_apply_hover_at).
 */
static void update_dock_items_fisheye(LambdockState *state, double x,
                                      double y) {
  dock_apply_hover_at(state, x, y);
}

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

  update_dock_items_fisheye(state, state->pointer_x, state->pointer_y);

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

  /* Point the popover's arrow at the click, so the menu grows away from it. */
  GdkRectangle rect = {(int)x, (int)y, 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(state->popover), &rect);

  if (!gtk_widget_get_visible(state->popover))
    state->active_popovers++;

  gtk_popover_popup(GTK_POPOVER(state->popover));
}

/**
 * on_popover_closed:
 * Decrements active popover counter when a context menu is dismissed, and
 * resumes auto-hide once the last one is gone.
 */
void on_popover_closed(GtkPopover *popover, gpointer user_data) {
  (void)popover;
  LambdockState *state = (LambdockState *)user_data;

  if (state->active_popovers > 0)
    state->active_popovers--;

  if (state->auto_hide && state->active_popovers == 0)
    schedule_hide_timer(state, AUTOHIDE_POPOVER_DELAY_MS);
}

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

/* =========================================================================
 * APPLICATION ACTIONS (QUIT & ABOUT)
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

/* The About dialog is created from an app-level action and must be singular.
 * The live instance is hung off the GtkApplication -- the object that outlives
 * every window here -- instead of a file-scope pointer, so there is no hidden
 * global and its lifetime is the application's. */
#define ABOUT_DIALOG_KEY "lambdock-about-dialog"
#define ABOUT_LOGO_RESOURCE "/org/jointhefreeworld/lambdock/img/agnus-dei.png"

/** The open About dialog, or NULL. Borrowed: the window owns itself. */
static GtkWindow *about_dialog_open(GtkApplication *app) {
  GtkWindow *about = (GtkWindow *)g_object_get_data(G_OBJECT(app),
                                                    ABOUT_DIALOG_KEY);

  return (about != NULL && GTK_IS_WINDOW(about)) ? about : NULL;
}

/* Adapter: the "close-request" signal carries no application, so the
 * application rides along as the handler's user data. */
static gboolean on_about_close_request(GtkWindow *window, gpointer user_data) {
  GtkApplication *app = GTK_APPLICATION(user_data);

  /* Forget first: the dialog is about to go away, and the next invocation of
   * the action has to build a fresh one. */
  g_object_set_data(G_OBJECT(app), ABOUT_DIALOG_KEY, NULL);
  gtk_window_destroy(window);

  return TRUE; /* handled: no default close behaviour */
}

/** Fills the identity fields: name, version, description, copyright. */
static void about_dialog_set_identity(GtkAboutDialog *about) {
  gtk_about_dialog_set_program_name(about, "lambdock");

#ifdef PACKAGE_VERSION
  gtk_about_dialog_set_version(about, PACKAGE_VERSION);
#endif

  gtk_about_dialog_set_comments(
      about,
      _("A modern Wayland-native hackable desktop dock bar.\n\n"
        "Engineered with the performance of C and GTK4 Layer Shell for smooth "
        "auto-hide and animations, featuring dynamic Wayland toplevel window "
        "tracking, live CSS hot-reloading, and dynamic configuration powered "
        "by GNU Guile Scheme."));

  gtk_about_dialog_set_copyright(about,
                                 "Copyright © Josep Bigorra "
                                 "<jjbigorra@gmail.com>");
}

/** Fills the credits: authors, license and project links. */
static void about_dialog_set_credits(GtkAboutDialog *about) {
  static const char *authors[] = {"Josep Bigorra (jjbigorra@gmail.com)", NULL};

  gtk_about_dialog_set_authors(about, authors);
  gtk_about_dialog_set_license_type(about, GTK_LICENSE_MIT_X11);
  gtk_about_dialog_set_website(about, "https://github.com/jjba23/lambdock");
  gtk_about_dialog_set_website_label(about,
                                     _("Find the project's source code here"));
}

/** Uses the bundled logo when it loads, and the icon theme otherwise. */
static void about_dialog_set_logo(GtkAboutDialog *about) {
  g_autoptr(GdkTexture) texture =
      gdk_texture_new_from_resource(ABOUT_LOGO_RESOURCE);

  if (!texture) {
    LOG_C_WARN("About", "Failed to load logo from GResource: %s",
               ABOUT_LOGO_RESOURCE);
    gtk_about_dialog_set_logo_icon_name(about, "lambdock");
    return;
  }

  gtk_about_dialog_set_logo(about, GDK_PAINTABLE(texture));
}

/** Builds the dialog, registers it as the live instance, and returns it. */
static GtkWindow *about_dialog_build(GtkApplication *app) {
  GtkWidget *about = gtk_about_dialog_new();

  gtk_widget_add_css_class(about, "about-dialog");
  about_dialog_set_identity(GTK_ABOUT_DIALOG(about));
  about_dialog_set_credits(GTK_ABOUT_DIALOG(about));
  about_dialog_set_logo(GTK_ABOUT_DIALOG(about));
  gtk_window_set_modal(GTK_WINDOW(about), TRUE);

  g_object_set_data(G_OBJECT(app), ABOUT_DIALOG_KEY, about);
  g_signal_connect(about, "close-request",
                   G_CALLBACK(on_about_close_request), app);

  return GTK_WINDOW(about);
}

/**
 * on_about_action:
 * Action handler for menu 'About Lambdock' option. Shows the About dialog,
 * reusing the one already open if there is one.
 */
void on_about_action(GSimpleAction *action, GVariant *parameter,
                     gpointer user_data) {
  (void)action;
  (void)parameter;

  GtkApplication *app = GTK_APPLICATION(user_data);

  GtkWindow *about = about_dialog_open(app);
  if (about == NULL)
    about = about_dialog_build(app);

  gtk_window_present(about);
}

/* =========================================================================
 * MEMORY & DESTRUCTORS
 * =========================================================================
 * destroy_dock_instance() is a sequence of named steps, one per kind of
 * resource a dock owns. Each step checks and clears its own pointers, so the
 * body of the destructor reads as an inventory and a step can never be run
 * twice by accident.
 */

/** Removes and drops this dock's CSS providers. */
static void destroy_css_providers(LambdockState *state) {
  GdkDisplay *display = gdk_display_get_default();
  if (!display)
    return;

  GtkCssProvider **providers[] = {
      &state->base_css_provider,
      &state->taskbar_css_provider,
      &state->shelf_css_provider,
      &state->user_css_provider,
  };

  for (gsize i = 0; i < G_N_ELEMENTS(providers); i++) {
    if (*providers[i] == NULL)
      continue;

    gtk_style_context_remove_provider_for_display(display,
                                                  GTK_STYLE_PROVIDER(
                                                      *providers[i]));
    g_clear_object(providers[i]);
  }
}

/** Stops the auto-hide timeout and the frame callbacks this dock owns. */
static void destroy_timers(LambdockState *state) {
  cancel_hide_timer(state);

  if (state->tick_cb_id != 0 && state->window && GTK_IS_WIDGET(state->window))
    gtk_widget_remove_tick_callback(state->window, state->tick_cb_id);
  state->tick_cb_id = 0;

  stop_pointer_tick(state);
}

/** Detaches every handler carrying @state, so teardown cannot re-enter. */
static void detach_window_handlers(LambdockState *state) {
  if (state->window && GTK_IS_WIDGET(state->window))
    g_signal_handlers_disconnect_by_data(state->window, state);

  /* The surface outlives this state: it is GTK's, it is destroyed after the
   * last signal has been emitted for it, and a `layout` or a resize during that
   * teardown reaches the handler below with a dangling state. Dropping the
   * handlers here is why the X11 placement does not need a liveness check. */
  if (state->x11_surface) {
    g_signal_handlers_disconnect_by_data(state->x11_surface, state);
    state->x11_surface = NULL;
  }
}

/** Unparents the background context menu without letting its handlers run. */
static void destroy_background_popover(LambdockState *state) {
  if (!state->popover)
    return;

  if (GTK_IS_WIDGET(state->popover)) {
    /* The popover "closed" handler is connected with `state` as data and would
     * schedule a hide timer on this soon-to-be-freed state; disconnect it
     * before unparenting so no callback runs against freed memory. */
    g_signal_handlers_disconnect_by_data(state->popover, state);

    if (gtk_widget_get_parent(state->popover))
      gtk_widget_unparent(state->popover);
  }

  state->popover = NULL;
}

/** Frees the dock items, and their individual polling timers and popovers. */
static void destroy_dock_items(LambdockState *state) {
  GList *items = state->dock_items;
  state->dock_items = NULL; /* clear first: freeing an item may call back */

  for (GList *node = items; node != NULL; node = node->next)
    free_dock_item((DockItem *)node->data);

  g_list_free(items);
}

/** Frees the Wayland toplevel window trackers. */
static void destroy_window_trackers(LambdockState *state) {
  GList *trackers = state->window_trackers;
  state->window_trackers = NULL;

  for (GList *node = trackers; node != NULL; node = node->next)
    free_window_tracker(node->data);

  g_list_free(trackers);
}

/** Destroys the GTK window, safing the widget pointers before GTK can emit
 * "destroy". */
static void destroy_window(LambdockState *state) {
  if (!state->window || !GTK_IS_WINDOW(state->window))
    return;

  GtkWidget *window = state->window;
  state->window = NULL;
  state->bg_shelf = NULL;
  state->bg_capsule = NULL;

  if (gtk_layer_is_layer_window(GTK_WINDOW(window)))
    gtk_layer_set_monitor(GTK_WINDOW(window), NULL);
  gtk_window_destroy(GTK_WINDOW(window));
}

/** Drops the borrowed monitor reference, and its handlers, together. */
static void destroy_monitor(LambdockState *state) {
  if (!state->monitor)
    return;

  g_signal_handlers_disconnect_by_data(state->monitor, state);
  g_clear_object(&state->monitor);
}

/**
 * destroy_dock_instance:
 * @state: Pointer to LambdockState context.
 *
 * Unparents popovers and widgets, cleans up window trackers and dock items,
 * and destroys the GTK window safely without disposal warnings. Takes the state
 * with it: the pointer is invalid on return.
 */
void destroy_dock_instance(LambdockState *state) {
  /* Re-entrancy guard: a "destroy" signal emitted while this runs must not
   * free the state a second time. */
  if (!state || state->is_destroying)
    return;

  state->is_destroying = TRUE;

  destroy_css_providers(state);
  g_clear_pointer(&state->active_theme_class, g_free);

  /* The launchpad is a global overlay that belongs to no single dock: take it
   * down with the dock it was opened from (it is recreated lazily). */
  lambdock_launchpad_shutdown();

  unregister_dock_state(state);

  destroy_timers(state);

  /* Window handlers go before the widgets: nothing they carry may fire while
   * the tree below is being taken apart. */
  detach_window_handlers(state);
  destroy_background_popover(state);
  destroy_dock_items(state);
  destroy_window_trackers(state);
  destroy_window(state);

  g_clear_object(&state->size_group);
  g_clear_pointer(&state->item_centres, g_array_unref);
  destroy_monitor(state);

  g_clear_pointer(&state->config_path, g_free);
  g_clear_pointer(&state->theme, g_free);

  /* The window id and the rectangle it had are the X11 placement's memory: a
   * dock that is rebuilt gets a new window, and it has to be placed again. */
  lambdock_x11_dock_forget(state);

  g_free(state);
}

/* ============================================================================
 * THE RECONNECTING STATE
 * ============================================================================
 * Window tracking is a listener: when the compositor restarts, its
 * foreign-toplevel proxies disappear with it and the tracker binds the new
 * manager when it is announced. The dock's items are configuration, not
 * tracking state, so they are left exactly where they are; the label is the
 * only visible difference, and it says why nothing is being tracked right now.
 */

void dock_window_set_tracker_connected(LambdockState *state,
                                       gboolean connected) {
  if (!state)
    return;

  state->tracker_connected = connected;

  if (state->reconnect_label)
    gtk_widget_set_visible(state->reconnect_label, !connected);

  if (state->window && GTK_IS_WINDOW(state->window)) {
    if (connected)
      gtk_widget_remove_css_class(state->window, "tracker-disconnected");
    else
      gtk_widget_add_css_class(state->window, "tracker-disconnected");
  }
}
