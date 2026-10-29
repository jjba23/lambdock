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

/** The icon size actually in use, with the default for partly built states. */
static int dock_icon_size(const LambdockState *state) {
  return (state && state->icon_size > 0) ? state->icon_size : DEFAULT_ICON_SIZE;
}

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
  return lambdock_shelf_geometry_for(state, dock_icon_size(state)).capsule_px;
}

/**
 * dock_cross_axis_dim:
 * Returns the cross-axis size requested for the overlay: fisheye magnification
 * headroom, plus the outer glass, plus any icon overhang past the inner edge.
 */
int dock_cross_axis_dim(const LambdockState *state) {
  int icon_px = dock_icon_size(state);
  LambdockShelfGeometry geometry = lambdock_shelf_geometry_for(state, icon_px);

  int headroom_px =
      (int)(icon_px * CROSS_DIM_SCALE_FACTOR) + CROSS_DIM_PADDING_PX;

  return headroom_px + geometry.outer_px + geometry.overhang_px;
}

/**
 * dock_is_settling:
 * @state: Pointer to the dock state.
 *
 * Whether the dock is sliding, or has only just stopped. Crossings reported in
 * that moment are echoes of the dock's own movement rather than the pointer
 * moving, and auto-hide must not act on them: believing one hides the dock,
 * which puts the revealing strip back under a pointer that never moved, which
 * reveals it again -- the dock bounces at the screen edge.
 *
 * Returns: TRUE while crossings are echoes of the dock's movement.
 */
gboolean dock_is_settling(const LambdockState *state) {
  if (!state)
    return FALSE;

  return lambdock_settling(state->tick_cb_id != 0, state->margin_anim_end_us,
                           g_get_monotonic_time(), AUTO_HIDE_SETTLE_MS);
}

/** CSS class naming the screen edge a shelf is docked to. */
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

/** The axis running across the dock: vertical for a top or bottom dock. */
static GtkOrientation dock_cross_orientation(GtkLayerShellEdge edge) {
  gboolean horizontal_edge = (edge == GTK_LAYER_SHELL_EDGE_TOP ||
                              edge == GTK_LAYER_SHELL_EDGE_BOTTOM);

  return horizontal_edge ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL;
}

/**
 * dock_surface_policy_for:
 * Reads the dock's configuration and returns the layer-shell surface it
 * describes: which edges to anchor, and how much desktop to reserve.
 *
 * Two rules, both about not taking space the dock is not using:
 *   - taskbar mode also anchors the perpendicular edges, so the surface is
 *     stretched across the monitor while a floating dock keeps its natural,
 *     content-hugging size;
 *   - an auto-hiding dock reserves nothing, or the space would stay reserved
 *     while the dock sits off screen.
 */
static DockSurfacePolicy dock_surface_policy_for(const LambdockState *state) {
  gboolean horizontal_edge = (state->edge == GTK_LAYER_SHELL_EDGE_TOP ||
                              state->edge == GTK_LAYER_SHELL_EDGE_BOTTOM);

  DockSurfacePolicy policy = {
      .anchor_top = (state->edge == GTK_LAYER_SHELL_EDGE_TOP),
      .anchor_bottom = (state->edge == GTK_LAYER_SHELL_EDGE_BOTTOM),
      .anchor_left = (state->edge == GTK_LAYER_SHELL_EDGE_LEFT),
      .anchor_right = (state->edge == GTK_LAYER_SHELL_EDGE_RIGHT),
      .exclusive_zone_px = state->auto_hide ? 0 : dock_capsule_cross_dim(state),
  };

  if (!state->taskbar)
    return policy;

  if (horizontal_edge) {
    policy.anchor_left = TRUE;
    policy.anchor_right = TRUE;
  } else {
    policy.anchor_top = TRUE;
    policy.anchor_bottom = TRUE;
  }

  return policy;
}

/* =========================================================================
 * SMALL SIDE-EFFECTING HELPERS
 * =========================================================================
 * One widget concern each. Everything below is idempotent, so it can be
 * re-applied on every reload or frame without bookkeeping.
 */

/* gtk_layer_set_margin() only means something on a layer-shell surface: the
 * preferences window and the test fixtures build ordinary windows. */
/* Declared here because the hover path, the input-region tick and the geometry
 * update call each other without being next to one another in this file. */
static void dock_schedule_input_region(LambdockState *state);
static int dock_content_main_px(const LambdockState *state, gboolean horizontal);
static void dock_reserve_hover_room(LambdockState *state);

static void set_window_margin(LambdockState *state, int margin) {
  if (!state || !state->window || !GTK_IS_WINDOW(state->window))
    return;
  if (gtk_layer_is_layer_window(GTK_WINDOW(state->window)))
    gtk_layer_set_margin(GTK_WINDOW(state->window), state->edge, margin);
}

/** Stops the slide frame callback, if one is running. */
static void stop_margin_animation(LambdockState *state) {
  if (state->tick_cb_id != 0 && state->window)
    gtk_widget_remove_tick_callback(state->window, state->tick_cb_id);

  state->tick_cb_id = 0;
}

/** Parks the dock at @margin without animating there. */
static void snap_margin_to(LambdockState *state, int margin) {
  state->start_margin = margin;
  state->current_margin = margin;
  state->target_margin = margin;
  state->anim_start_time = 0;
  /* Crossings for a moment after this are echoes of the move, not the pointer
   * moving: see dock_is_settling(). */
  state->margin_anim_end_us = g_get_monotonic_time();

  set_window_margin(state, margin);

  /* Hidden and shown want different input regions: the whole surface while the
   * dock is a strip, the dock itself once it is out. */
  dock_schedule_input_region(state);
}

/** Drops a queued fisheye frame, so a stale position cannot be applied. */
static void stop_pointer_tick(LambdockState *state) {
  if (state->pointer_tick_id != 0 && state->window &&
      GTK_IS_WIDGET(state->window)) {
    gtk_widget_remove_tick_callback(state->window, state->pointer_tick_id);
  }

  state->pointer_tick_id = 0;
}

/**
 * for_each_dock_item:
 * Runs @visit over the dock row, skipping NULL items.
 *
 * g_list_foreach() would do the walk, but it takes a GFunc over gpointer, and
 * casting dock-item callbacks to it gives up the type checking that keeps these
 * visitors honest. The loop is four lines; the signature is worth it.
 */
static void for_each_dock_item(GList *items, DockItemVisitor visit,
                               gpointer data) {
  for (GList *node = items; node != NULL; node = node->next) {
    if (node->data)
      visit((DockItem *)node->data, data);
  }
}

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

/** Puts one dock item back at its resting size, reflection included. */
static void reset_item_scale(DockItem *item, gpointer data) {
  const LambdockState *state = (const LambdockState *)data;

  if (!item->button)
    return;

  int base_px = dock_icon_size(state);

  if (state->orientation == GTK_ORIENTATION_HORIZONTAL)
    gtk_widget_set_size_request(item->button, -1, base_px);
  else
    gtk_widget_set_size_request(item->button, base_px, -1);

  /* The icon itself is never re-rasterised: it rests at its base scale. */
  GtkWidget *zoom = get_item_zoom_widget(item);
  if (zoom)
    lambdock_icon_zoom_set_scale(zoom, 1.0);

  /* Keep the glass-shelf reflection at the base icon size too. */
  if (item->reflection)
    shelf_reflection_set_icon_size(item->reflection, base_px);
}

/**
 * reset_dock_items_scale:
 * Resets every dock item to its default base pixel size.
 */
static void reset_dock_items_scale(LambdockState *state) {
  if (!state || !state->dock_items)
    return;

  for_each_dock_item(state->dock_items, reset_item_scale, state);

  /* The row is back at rest, so the next frame has to apply the pointer again
   * even if it has not moved, and the surface's room can be re-measured. */
  state->pointer_applied = FALSE;
  state->hover_axis_valid = FALSE;
  state->fisheye_active = FALSE;

  /* The row is back at rest: let the surface hug it again. */
  dock_reserve_hover_room(state);
}

/* =========================================================================
 * SLIDE ANIMATION
 * ========================================================================= */

/**
 * animate_margin_cb:
 * GTK tick callback executed on every display frame update while animating.
 * Smoothly interpolates `current_margin` towards `target_margin` using elapsed
 * frame-clock timestamps.
 *
 * Returns: G_SOURCE_CONTINUE to keep ticking, G_SOURCE_REMOVE when finished.
 */
static gboolean animate_margin_cb(GtkWidget *widget, GdkFrameClock *frame_clock,
                                  gpointer user_data) {
  (void)widget;
  LambdockState *state = (LambdockState *)user_data;

  gint64 now_us = gdk_frame_clock_get_frame_time(frame_clock);

  /* Anchor the clock on the first frame: the animation starts when the
   * compositor first ticked, not when it was requested. */
  if (state->anim_start_time == 0)
    state->anim_start_time = now_us;

  double progress = lambdock_slide_progress(state->anim_start_time, now_us,
                                            ANIMATION_DURATION_US);

  /* --- Animation complete --- */
  if (progress >= 1.0) {
    state->current_margin = state->target_margin;
    set_window_margin(state, state->current_margin);
    state->tick_cb_id = 0;
    state->margin_anim_end_us = g_get_monotonic_time();
    DOCK_TRACE("slide done at margin %d", state->current_margin);

    /* The dock has arrived, so the input region it needs may have changed:
     * out in the open it is the dock, mid-slide it was the whole surface. */
    dock_schedule_input_region(state);
    return G_SOURCE_REMOVE;
  }

  /* --- Intermediate frame --- */
  state->current_margin =
      lambdock_slide_position(state->start_margin, state->target_margin,
                              progress);
  set_window_margin(state, state->current_margin);

  return G_SOURCE_CONTINUE;
}

/**
 * start_margin_animation:
 * Initiates or updates a margin transition animation toward a target value.
 */
static void start_margin_animation(LambdockState *state, int target_margin) {
  /* Nothing to do if the dock is already where it is wanted -- and not on its
   * way somewhere else. `current_margin` is where the dock *is*, which is not
   * the same as where it is going: a hide that has been requested but has not
   * had its first frame yet still reads as margin 0 with a target below it, and
   * so does a hide that has only just started sliding. Returning early then
   * threw away the reveal that asked for this -- the dock went on sliding away
   * believing it was visible, and could not be brought back while the pointer
   * stayed on the strip. */
  if (state->current_margin == target_margin &&
      (state->tick_cb_id == 0 || state->target_margin == target_margin))
    return;

  /* Reduced motion: the dock still has to get out of the way (or come back),
   * it just does it in one step instead of sliding. */
  if (!lambdock_animations_enabled(state)) {
    stop_margin_animation(state);
    snap_margin_to(state, target_margin);
    return;
  }

  state->start_margin = state->current_margin;
  state->target_margin = target_margin;
  state->anim_start_time = 0; /* reset the timer anchor */
  state->margin_anim_end_us = 0;

  /* A slide is about to move the dock somewhere the surface's input region was
   * not shaped for, and whether the whole surface has to take input depends on
   * the margin: ask for the region again on the frame the slide starts, not
   * only on the frames something else happens to reapply it. */
  dock_schedule_input_region(state);

  if (state->tick_cb_id == 0) {
    state->tick_cb_id = gtk_widget_add_tick_callback(
        state->window, animate_margin_cb, state, NULL);
  }
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

  stop_margin_animation(state);
  state->current_margin = state->target_margin;
  set_window_margin(state, state->current_margin);

  stop_pointer_tick(state);
  reset_dock_items_scale(state);
}

/* =========================================================================
 * FISHEYE HOVER MAGNIFICATION
 * =========================================================================
 * The bell curve itself is in core/dock_motion.c; this half only reads widget
 * bounds and pushes the resulting sizes into the row.
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

/* Adapter: GLib timeouts hand the state back as a plain pointer. */
static gboolean on_hide_timeout(gpointer user_data) {
  LambdockState *state = (LambdockState *)user_data;
  state->hide_timer_id = 0;

  hide_dock(state);
  return G_SOURCE_REMOVE;
}

/** Drops any pending auto-hide timer. Safe to call at any time. */
static void cancel_hide_timer(LambdockState *state) {
  if (!state || state->hide_timer_id == 0)
    return;

  g_source_remove(state->hide_timer_id);
  state->hide_timer_id = 0;
}

/** (Re)schedules the auto-hide timeout, replacing a pending one. */
static void schedule_hide_timer(LambdockState *state, guint delay_ms) {
  cancel_hide_timer(state);
  state->hide_timer_id = g_timeout_add(delay_ms, on_hide_timeout, state);
}

/**
 * dock_measured_main_axis_px:
 * The surface's current size along the dock's main axis, or 0 before GTK has
 * laid it out (which lambdock_hide_margin() knows how to handle).
 */
static int dock_measured_main_axis_px(const LambdockState *state) {
  if (!state->window)
    return 0;

  return (dock_cross_orientation(state->edge) == GTK_ORIENTATION_VERTICAL)
             ? gtk_widget_get_height(state->window)
             : gtk_widget_get_width(state->window);
}

/**
 * reveal_dock:
 * Cancels pending hide timers and animates the dock back into full view (margin
 * = 0).
 */
void reveal_dock(LambdockState *state) {
  cancel_hide_timer(state);

  if (state->is_hidden || state->target_margin != 0) {
    DOCK_TRACE("reveal: hidden %d, margin %d -> 0", state->is_hidden,
               state->current_margin);
    start_margin_animation(state, 0);
    state->is_hidden = FALSE;

    /* The pollers stood still while the dock was away; make every clock, gauge
     * and trash count current before the first frame is drawn. */
    dock_items_refresh_polls(state);
  } else {
    DOCK_TRACE("reveal: already out (margin %d)", state->current_margin);
  }
}

/**
 * hide_dock:
 * Slides the dock off screen, keeping a trigger strip visible on the screen
 * edge so the pointer can bring it back.
 */
void hide_dock(LambdockState *state) {
  /* --- Guards: nothing to hide behind --- */

  /* Auto-hide slides the surface off-screen with a layer-shell margin. Without
   * layer shell (X11, GNOME) there is no margin to move, so "hiding" would only
   * freeze the fisheye while the dock stays visible. */
  if (!state->window || !gtk_layer_is_layer_window(GTK_WINDOW(state->window)))
    return;

  /* Respect the auto-hide setting, and never hide under an open menu. */
  if (!state->auto_hide || state->active_popovers > 0)
    return;

  /* `show-when-clear' means the dock belongs on screen while nothing runs. */
  if (state->show_when_clear && state->window_trackers == NULL)
    return;

  /* The row has to be at rest before its size is measured, or the measurement
   * is the magnified one. */
  if (state->hover_animate)
    reset_dock_items_scale(state);

  int measured_px = dock_measured_main_axis_px(state);
  int cross_px = dock_cross_axis_dim(state);
  int hidden_margin = lambdock_hide_margin(measured_px, cross_px,
                                           CROSS_DIM_TOLERANCE_PX,
                                           AUTOHIDE_TRIGGER_STRIP_PX);

  DOCK_TRACE("hide: window %dx%d (cross axis %d) -> margin %d, strip %d px",
             gtk_widget_get_width(state->window),
             gtk_widget_get_height(state->window), cross_px, hidden_margin,
             AUTOHIDE_TRIGGER_STRIP_PX);

  if (!state->is_hidden || state->target_margin != hidden_margin) {
    start_margin_animation(state, hidden_margin);
    state->is_hidden = TRUE;
  }
}

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

/* =========================================================================
 * DOCK MODE & CSS CLASSES
 * ========================================================================= */

/**
 * apply_shelf_css_classes:
 * Tags the capsule and the window with the shelf's state classes, plus the
 * class naming the edge the shelf is docked to.
 *
 * Both widgets are tagged on purpose: user CSS can then target the whole dock
 * (the window) or the shelf itself (the capsule, which carries the theme).
 */
static void apply_shelf_css_classes(const LambdockState *state,
                                    gboolean shelf_active,
                                    gboolean mirroring) {
  static const char *const edge_classes[] = {"shelf-bottom", "shelf-top",
                                             "shelf-left", "shelf-right"};
  const gsize n_edge_classes = G_N_ELEMENTS(edge_classes);

  /* Edges are mutually exclusive: clear them all, then set the current one. */
  lambdock_toggle_css_classes(state->bg_capsule, edge_classes, n_edge_classes,
                             FALSE);
  lambdock_toggle_css_classes(state->window, edge_classes, n_edge_classes, FALSE);

  lambdock_set_css_class(state->bg_capsule, "shelf-mode", shelf_active);
  lambdock_set_css_class(state->window, "shelf-mode", shelf_active);

  /* `shelf-mirror` means the glass actually shows icon reflections: the
   * reflection widget then draws the running indicator itself, so the CSS one
   * must stay quiet. */
  lambdock_set_css_class(state->bg_capsule, "shelf-mirror", mirroring);
  lambdock_set_css_class(state->window, "shelf-mirror", mirroring);

  if (!shelf_active)
    return;

  const char *edge_class = shelf_edge_class(state->edge);
  lambdock_set_css_class(state->bg_capsule, edge_class, TRUE);
  lambdock_set_css_class(state->window, edge_class, TRUE);
}

/**
 * apply_shelf_shape:
 * Projects the styled capsule onto the shape described by the shelf parameters:
 * the tilt, the keystone (flare) or both. With neither, the frame is a
 * pass-through.
 */
static void apply_shelf_shape(const LambdockState *state) {
  if (!state->bg_shelf)
    return;

  shelf_frame_set_shape(state->bg_shelf, state->edge,
                        shelf_tilt_for_edge(state->edge, state->shelf.tilt_deg),
                        state->shelf.perspective, state->shelf.flare_px);
}

/**
 * apply_dock_mode_classes:
 * Toggles the `taskbar` CSS class on the background capsule so themes can give
 * taskbar mode its own rectangular, edge-to-edge styling, and the `shelf-*`
 * classes that let any theme restyle the glass shelf.
 */
void apply_dock_mode_classes(LambdockState *state) {
  if (!state || !state->bg_capsule)
    return;

  gboolean shelf_active = lambdock_shelf_active(state);
  gboolean mirroring =
      shelf_active &&
      lambdock_shelf_geometry_for(state, state->icon_size).band_px > 0;

  lambdock_set_css_class(state->bg_capsule, "taskbar", state->taskbar);
  apply_shelf_css_classes(state, shelf_active, mirroring);
  apply_shelf_shape(state);
}

/* =========================================================================
 * LAYER-SHELL SURFACE & MODE
 * ========================================================================= */

/** Puts @policy's anchors onto the surface. */
static void apply_surface_anchors(GtkWindow *window,
                                  const DockSurfacePolicy *policy) {
  gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_TOP, policy->anchor_top);
  gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_BOTTOM,
                       policy->anchor_bottom);
  gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_LEFT, policy->anchor_left);
  gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_RIGHT,
                       policy->anchor_right);
}

/**
 * dock_apply_exclusive_zone:
 * Reserves desktop space for the dock, or nothing at all while it auto-hides.
 *
 * The rule lives in dock_surface_policy_for(), and this is the one way to apply
 * it: the surface is created with it, and every configuration reload re-applies
 * it (auto-hide and the capsule thickness can both change).
 */
void dock_apply_exclusive_zone(LambdockState *state) {
  if (!state || !state->window || !GTK_IS_WINDOW(state->window))
    return;
  if (!gtk_layer_is_layer_window(GTK_WINDOW(state->window)))
    return;

  gtk_layer_set_exclusive_zone(
      GTK_WINDOW(state->window),
      dock_surface_policy_for(state).exclusive_zone_px);
}

/**
 * lambdock_layer_shell_available:
 * Returns: TRUE when the current display is Wayland /and/ the compositor
 * implements the wlr-layer-shell protocol.
 *
 * gtk_layer_is_supported() asserts that the display is a Wayland one, so it
 * cannot be used as the guard on X11 (it logs a critical right there, before it
 * has a chance to answer FALSE). This checks the backend first.
 */
gboolean lambdock_layer_shell_available(void) {
  GdkDisplay *display = gdk_display_get_default();
  if (!display)
    return FALSE;

#ifdef GDK_WINDOWING_WAYLAND
  if (!GDK_IS_WAYLAND_DISPLAY(display))
    return FALSE;

  return gtk_layer_is_supported();
#else
  /* A GTK without the Wayland backend cannot ever have layer shell. */
  return FALSE;
#endif
}

/**
 * setup_layer_shell_window:
 * Converts a standard GTK window to a Wayland Layer Shell surface and anchors
 * it to the docked edge.
 *
 * Layer shell is a Wayland protocol that not every compositor implements (and
 * that X11 does not have at all). Where it is missing the dock is still a
 * perfectly good normal window, so this warns once and leaves it alone rather
 * than letting every later gtk_layer_* call log its own critical.
 */
void setup_layer_shell_window(LambdockState *state) {
  GtkWindow *window = GTK_WINDOW(state->window);

  if (!lambdock_layer_shell_available()) {
    LOG_C_WARN("Dock",
               "This compositor has no wlr-layer-shell support: showing the "
               "dock as an ordinary window (no screen-edge docking, no "
               "auto-hide).");
    return;
  }

  gtk_layer_init_for_window(window);
  gtk_layer_set_namespace(window, "lambdock");

  /* Layer surfaces get no keyboard by default, which would leave the dock
   * unreachable for keyboard-only users: icons could not be tabbed to, and
   * Escape would never reach an open stack. On-demand keeps the compositor in
   * charge while letting a click (or a compositor shortcut) hand the dock the
   * keyboard. Protocol versions below 4 have no on-demand mode and fall back to
   * "none"; the dock stays usable by pointer there. */
  gtk_layer_set_keyboard_mode(window, GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

  /* A monitor that has already been unplugged must not be handed to the
   * compositor; with none set the surface follows the default output. */
  if (is_monitor_valid(state->monitor))
    gtk_layer_set_monitor(window, state->monitor);

  /* The dock renders on the top layer, above normal application windows. */
  gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_TOP);

  DockSurfacePolicy policy = dock_surface_policy_for(state);
  apply_surface_anchors(window, &policy);

  /* Exclusive zone reserves desktop space so windows do not overlap the dock. */
  gtk_layer_set_exclusive_zone(window, policy.exclusive_zone_px);
}

/**
 * refresh_dock_mode:
 * Re-applies layer-shell anchors, mode CSS classes and dimension requests after
 * a live configuration reload toggles dock <-> taskbar mode.
 */
void refresh_dock_mode(LambdockState *state) {
  if (!state || !state->window)
    return;

  /* Anchors only exist on a layer surface (no layer shell on X11 / GNOME). */
  if (gtk_layer_is_layer_window(GTK_WINDOW(state->window))) {
    DockSurfacePolicy policy = dock_surface_policy_for(state);
    apply_surface_anchors(GTK_WINDOW(state->window), &policy);
  }

  apply_dock_mode_classes(state);
  update_dock_dimensions(state);
}

/** Applies the input region whenever the dock window (re)appears. */
static void on_window_map(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  dock_schedule_input_region((LambdockState *)user_data);
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

/**
 * schedule_initial_autohide:
 * Schedules initial hide timer on application launch if auto-hide is enabled.
 */
void schedule_initial_autohide(LambdockState *state) {
  /* Same reason as hide_dock(): without a layer surface there is no slide. */
  if (state->auto_hide && state->window &&
      gtk_layer_is_layer_window(GTK_WINDOW(state->window)))
    schedule_hide_timer(state, AUTOHIDE_INITIAL_DELAY_MS);
}

/* =========================================================================
 * MONITORS & DIMENSIONS
 * =========================================================================
 * The numbers are decided by the pure `lambdock_dock_dims_compute()`; the
 * functions below only gather what it needs and apply the result, one widget
 * concern each.
 */

/** First monitor of the default display. Caller owns the reference. */
static GdkMonitor *first_monitor(void) {
  GdkDisplay *display = gdk_display_get_default();
  GListModel *monitors = display ? gdk_display_get_monitors(display) : NULL;

  if (!monitors || g_list_model_get_n_items(monitors) == 0)
    return NULL;

  return g_list_model_get_item(monitors, 0);
}

/**
 * is_monitor_valid:
 * Checks if a given GdkMonitor pointer is currently present in the display's
 * active monitor list.
 */
gboolean is_monitor_valid(GdkMonitor *mon) {
  if (!mon)
    return FALSE;

  GdkDisplay *display = gdk_display_get_default();
  GListModel *monitors = display ? gdk_display_get_monitors(display) : NULL;
  if (!monitors)
    return FALSE;

  guint n_monitors = g_list_model_get_n_items(monitors);
  for (guint i = 0; i < n_monitors; i++) {
    g_autoptr(GObject) item = g_list_model_get_item(monitors, i);

    /* Plain pointer comparison on purpose: @mon may be a stale (freed) monitor,
     * and G_OBJECT() would dereference it to check its type. */
    if ((gpointer)item == (gpointer)mon)
      return TRUE;
  }

  return FALSE;
}

/** The monitor the dock's surface is on, or NULL before it is realized. */
static GdkMonitor *surface_monitor(const LambdockState *state) {
  if (!state->window || !GTK_IS_WINDOW(state->window))
    return NULL;

  GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(state->window));
  if (!surface)
    return NULL;

  return gdk_display_get_monitor_at_surface(
      gtk_widget_get_display(state->window), surface);
}

/** The dock's monitor geometry, falling back to the first monitor. */
static DockMonitorSize dock_monitor_size(const LambdockState *state) {
  DockMonitorSize size = {0, 0};

  g_autoptr(GdkMonitor) fallback = NULL;
  GdkMonitor *monitor = state->monitor;

  /* A monitor that has been unplugged is a dangling pointer: only trust one the
   * display still lists. */
  if (!is_monitor_valid(monitor)) {
    /* A dock with no monitor of its own is placed by the compositor, which may
     * well be a monitor that is not the first one: measure the surface's. */
    monitor = surface_monitor(state);

    if (!monitor) {
      fallback = first_monitor();
      monitor = fallback;
    }
  }

  if (!monitor)
    return size;

  GdkRectangle geometry;
  gdk_monitor_get_geometry(monitor, &geometry);
  size.width = geometry.width;
  size.height = geometry.height;

  return size;
}

/** Size-group mode follows the dock's orientation. */
static void dock_dims_apply_size_group(const LambdockState *state) {
  if (!state->size_group)
    return;

  GtkSizeGroupMode mode = (state->orientation == GTK_ORIENTATION_HORIZONTAL)
                              ? GTK_SIZE_GROUP_HORIZONTAL
                              : GTK_SIZE_GROUP_VERTICAL;
  gtk_size_group_set_mode(state->size_group, mode);
}

/** The scrolled viewport only ever scrolls along the dock's main axis. */
static void dock_dims_apply_scroll_policy(const LambdockState *state,
                                          const LambdockDockDims *dims) {
  if (!state->scrolled_window)
    return;

  GtkScrolledWindow *scrolled = GTK_SCROLLED_WINDOW(state->scrolled_window);

  if (state->orientation == GTK_ORIENTATION_HORIZONTAL) {
    gtk_scrolled_window_set_policy(scrolled, GTK_POLICY_EXTERNAL,
                                   GTK_POLICY_NEVER);
    gtk_scrolled_window_set_max_content_width(scrolled, dims->max_main_h_px);
  } else {
    gtk_scrolled_window_set_policy(scrolled, GTK_POLICY_NEVER,
                                   GTK_POLICY_EXTERNAL);
    gtk_scrolled_window_set_max_content_height(scrolled, dims->max_main_v_px);
  }
}

/** Padding along the main axis, and the margin that keeps icons off the edge. */
static void dock_dims_apply_box_margins(const LambdockState *state,
                                        const LambdockDockDims *dims,
                                        gboolean horizontal) {
  if (horizontal) {
    gtk_widget_set_margin_start(state->box, dims->padding_px);
    gtk_widget_set_margin_end(state->box, dims->padding_px);
    gtk_widget_set_margin_top(state->box, 0);
    gtk_widget_set_margin_bottom(state->box, 0);
  } else {
    gtk_widget_set_margin_top(state->box, dims->padding_px);
    gtk_widget_set_margin_bottom(state->box, dims->padding_px);
    gtk_widget_set_margin_start(state->box, 0);
    gtk_widget_set_margin_end(state->box, 0);
  }

  /* Keep the icons off the outer edge by the front glass thickness, so the
   * mirrors (and any front face) have room in front of them. The reflection
   * widget already occupies the band inside that glass, hence the difference:
   * the icons' edge ends up exactly `front` pixels from the screen edge. */
  if (dims->outer_margin_px <= 0)
    return;

  gboolean edge_is_start =
      horizontal ? (state->edge == GTK_LAYER_SHELL_EDGE_TOP)
                 : (state->edge == GTK_LAYER_SHELL_EDGE_LEFT);

  if (horizontal) {
    if (edge_is_start)
      gtk_widget_set_margin_top(state->box, dims->outer_margin_px);
    else
      gtk_widget_set_margin_bottom(state->box, dims->outer_margin_px);
  } else {
    if (edge_is_start)
      gtk_widget_set_margin_start(state->box, dims->outer_margin_px);
    else
      gtk_widget_set_margin_end(state->box, dims->outer_margin_px);
  }
}

/** The row's natural size along the dock's main axis, right now. */
static int dock_content_main_px(const LambdockState *state,
                                gboolean horizontal) {
  if (!state || !state->scrolled_window)
    return 0;

  int natural = 0;
  gtk_widget_measure(
      state->scrolled_window,
      horizontal ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL, -1,
      NULL, &natural, NULL, NULL);

  return natural;
}

/**
 * dock_reserve_hover_room:
 * Reserves the room the row spreads into while the pointer runs over the dock.
 *
 * Only the size requests change here, and only along the main axis: everything
 * else about the dock is left alone, so this is cheap enough to call on every
 * hover (and it is called at rest, never mid-spread).
 *
 * Reserving the room *once*, before the icons start to grow, is what lets a
 * hover run without ever changing the surface's size. A layer surface that is
 * resized on every pointer frame makes the compositor reconfigure and
 * re-allocate its buffer per frame, which is exactly the flicker and the torn
 * pixels along the dock's edges this is here to prevent. The transparent room is
 * cut out of the input region (see dock_apply_input_region), so it never
 * swallows a click meant for the windows underneath.
 */
static void dock_reserve_hover_room(LambdockState *state) {
  if (!state || !state->overlay)
    return;

  gboolean horizontal = state->orientation == GTK_ORIENTATION_HORIZONTAL;

  /* Only at rest: measuring while the row is spread would bake the magnified
   * width in. */
  int headroom = (state->hover_animate && !state->taskbar &&
                  !state->fisheye_active)
                     ? state->hover_headroom_px
                     : 0;
  int main_px = headroom > 0
                    ? dock_content_main_px(state, horizontal) + 2 * headroom
                    : -1;

  int width = -1;
  int height = -1;
  gtk_widget_get_size_request(state->overlay, &width, &height);

  if (horizontal)
    gtk_widget_set_size_request(state->overlay, main_px, height);
  else
    gtk_widget_set_size_request(state->overlay, width, main_px);

  dock_schedule_input_region(state);
}

/** Cross-axis size requests for the overlay and the capsule. */
static void dock_dims_apply_size_requests(LambdockState *state,
                                          const LambdockDockDims *dims,
                                          gboolean horizontal) {
  if (horizontal) {
    gtk_widget_set_size_request(state->overlay, -1, dims->overlay_cross_px);
    gtk_widget_set_size_request(state->bg_capsule, -1,
                                dims->capsule_cross_px);
  } else {
    gtk_widget_set_size_request(state->overlay, dims->overlay_cross_px, -1);
    gtk_widget_set_size_request(state->bg_capsule, dims->capsule_cross_px,
                                -1);
  }
}

/** The scrolled viewport fills a taskbar and centres in a floating capsule. */
static void dock_dims_apply_scroll_alignment(const LambdockState *state,
                                             gboolean horizontal) {
  if (!state->scrolled_window)
    return;

  GtkWidget *scrolled = state->scrolled_window;

  if (horizontal) {
    gtk_widget_set_valign(scrolled, (state->edge == GTK_LAYER_SHELL_EDGE_TOP)
                                       ? GTK_ALIGN_START
                                       : GTK_ALIGN_END);
    gtk_widget_set_halign(scrolled, state->taskbar ? GTK_ALIGN_FILL
                                                   : GTK_ALIGN_CENTER);
    /* In taskbar mode the content viewport must expand to fill the bar so
     * icons grow from the leading edge. */
    gtk_widget_set_hexpand(scrolled, state->taskbar);
  } else {
    gtk_widget_set_halign(scrolled, (state->edge == GTK_LAYER_SHELL_EDGE_LEFT)
                                        ? GTK_ALIGN_START
                                        : GTK_ALIGN_END);
    gtk_widget_set_valign(scrolled, state->taskbar ? GTK_ALIGN_FILL
                                                   : GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(scrolled, state->taskbar);
  }
}

/** A taskbar fills its cross axis; a floating capsule centres and hugs edge. */
static void dock_dims_apply_capsule_alignment(const LambdockState *state,
                                              gboolean horizontal) {
  gboolean edge_is_start =
      horizontal ? (state->edge == GTK_LAYER_SHELL_EDGE_TOP)
                 : (state->edge == GTK_LAYER_SHELL_EDGE_LEFT);

  if (horizontal) {
    gtk_widget_set_halign(state->bg_capsule,
                          state->taskbar ? GTK_ALIGN_FILL : GTK_ALIGN_CENTER);
    gtk_widget_set_halign(state->box,
                          state->taskbar ? GTK_ALIGN_START : GTK_ALIGN_CENTER);
    gtk_widget_set_valign(state->bg_capsule, edge_is_start ? GTK_ALIGN_START
                                                           : GTK_ALIGN_END);
    gtk_widget_set_valign(state->box,
                          edge_is_start ? GTK_ALIGN_START : GTK_ALIGN_END);
  } else {
    gtk_widget_set_valign(state->bg_capsule,
                          state->taskbar ? GTK_ALIGN_FILL : GTK_ALIGN_CENTER);
    gtk_widget_set_valign(state->box,
                          state->taskbar ? GTK_ALIGN_START : GTK_ALIGN_CENTER);
    gtk_widget_set_halign(state->bg_capsule, edge_is_start ? GTK_ALIGN_START
                                                           : GTK_ALIGN_END);
    gtk_widget_set_halign(state->box,
                          edge_is_start ? GTK_ALIGN_START : GTK_ALIGN_END);
  }
}

/** The shelf wrapper is what the overlay positions, so it mirrors the capsule. */
static void dock_dims_mirror_shelf_alignment(const LambdockState *state) {
  if (!state->bg_shelf)
    return;

  gtk_widget_set_halign(state->bg_shelf,
                        gtk_widget_get_halign(state->bg_capsule));
  gtk_widget_set_valign(state->bg_shelf,
                        gtk_widget_get_valign(state->bg_capsule));
}

/**
 * dock_apply_input_region:
 * Cuts the dock surface's input region down to the dock itself.
 *
 * The surface is deliberately bigger than the capsule: it carries the room the
 * icons magnify into, and the room the row spreads into while the pointer runs
 * over it (see dock_dims_apply_size_requests). None of that is part of the dock,
 * so a click there has to reach whatever is underneath instead of being eaten by
 * an invisible layer surface. Without this the dock would quietly swallow clicks
 * in a band around itself.
 *
 * Called from a one-shot frame callback, because a size request only turns into
 * an allocation on the next frame: the bounds read before that are stale.
 */
/** TRUE when @rect is the one the surface's input region already has. */
static gboolean input_region_is_current(const LambdockState *state,
                                        gboolean whole,
                                        const cairo_rectangle_int_t *rect) {
  if (!state->input_region_set || state->input_region_whole != whole)
    return FALSE;

  if (whole)
    return TRUE;

  return state->input_region_x == rect->x &&
         state->input_region_y == rect->y &&
         state->input_region_width == rect->width &&
         state->input_region_height == rect->height;
}

/** Applies a region (NULL for the whole surface) unless it is already current. */
static void apply_input_region(LambdockState *state, GdkSurface *surface,
                               gboolean whole,
                               const cairo_rectangle_int_t *rect) {
  if (input_region_is_current(state, whole, rect))
    return;

  if (whole) {
    gdk_surface_set_input_region(surface, NULL);
    DOCK_TRACE("input region: whole surface (%dx%d), margin %d, hidden %d",
               gdk_surface_get_width(surface), gdk_surface_get_height(surface),
               state->current_margin, state->is_hidden);
  } else {
    cairo_region_t *region = cairo_region_create_rectangle(rect);
    gdk_surface_set_input_region(surface, region);
    cairo_region_destroy(region);
    DOCK_TRACE("input region: dock only rect %d,%d %dx%d (surface %dx%d, "
               "margin %d)",
               rect->x, rect->y, rect->width, rect->height,
               gdk_surface_get_width(surface), gdk_surface_get_height(surface),
               state->current_margin);
  }

  state->input_region_set = TRUE;
  state->input_region_whole = whole;
  if (!whole) {
    state->input_region_x = rect->x;
    state->input_region_y = rect->y;
    state->input_region_width = rect->width;
    state->input_region_height = rect->height;
  }

  /* One wl_region, one request, one commit: worth counting, because it happens
   * on the hover path and a region that changes every frame is churn. */
  lambdock_stats_bump(LAMBDOCK_STAT_INPUT_REGION_SETS);

  /* Surface state in Wayland is double buffered: a new input region only
   * reaches the compositor with the surface's next commit. A dock that has
   * finished sliding, though, is drawing nothing -- so the region that would
   * let the pointer touch the strip can sit in the queue indefinitely, and the
   * strip stays dead exactly when it is the only part of the dock on screen.
   * Asking for a frame is what commits it; GTK has no partial invalidation, but
   * it does coalesce the request into the frame it is already drawing, so this
   * costs nothing during a hover, which is the only time it repeats. */
  gtk_widget_queue_draw(state->window);
}

static gboolean dock_apply_input_region(LambdockState *state) {
  if (!state || !state->window || !state->bg_shelf)
    return FALSE;

  GtkNative *native = gtk_widget_get_native(state->window);
  GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
  if (!surface || gdk_surface_get_width(surface) <= 1 ||
      gdk_surface_get_height(surface) <= 1)
    return FALSE;

  /* Sample the surface size where the layout is known to have landed: a change
   * is a buffer re-allocation and a compositor reconfigure, so it is worth
   * counting (and asserting) rather than eyeballing. */
  int surface_width = gdk_surface_get_width(surface);
  int surface_height = gdk_surface_get_height(surface);
  if (state->last_surface_width != surface_width ||
      state->last_surface_height != surface_height) {
    if (state->last_surface_width != 0)
      lambdock_stats_bump(LAMBDOCK_STAT_SURFACE_RESIZES);
    state->last_surface_width = surface_width;
    state->last_surface_height = surface_height;
  }

  /* A tucked-away dock is a strip at the screen edge, and that strip is the
   * surface's *headroom* -- the part the dock-shaped region below cuts away. It
   * has to stay clickable, or the pointer could never bring the dock back (a
   * slide moves the window every frame, so this is also the state to be in for
   * the whole animation). While the dock is away the whole surface is the strip,
   * so the whole surface keeps its input. */
  if (lambdock_dock_needs_full_input(state->is_hidden,
                                     state->current_margin)) {
    apply_input_region(state, surface, TRUE, NULL);
    return TRUE;
  }

  /* The dock is the slab plus the row of icons that overhangs it, so the region
   * has to be the union of the two: the slab alone would leave the icons -- the
   * part people actually click -- outside the clickable area. */
  graphene_rect_t bounds;
  if (!gtk_widget_compute_bounds(state->bg_shelf, state->window, &bounds) ||
      bounds.size.width < 1.0f || bounds.size.height < 1.0f)
    return FALSE;

  graphene_rect_t row;
  if (state->scrolled_window &&
      gtk_widget_compute_bounds(state->scrolled_window, state->window, &row) &&
      row.size.width >= 1.0f && row.size.height >= 1.0f)
    graphene_rect_union(&bounds, &row, &bounds);

  /* One pixel of slack on each side keeps the rounded edge clickable. */
  cairo_rectangle_int_t rect = {
      .x = (int)floor(bounds.origin.x) - 1,
      .y = (int)floor(bounds.origin.y) - 1,
      .width = (int)ceil(bounds.size.width) + 2,
      .height = (int)ceil(bounds.size.height) + 2,
  };

  apply_input_region(state, surface, FALSE, &rect);

  return TRUE;
}

/**
 * on_input_region_tick:
 * Frame callback that applies the input region once the window is really laid
 * out, retrying while it is not.
 *
 * The first frames of a window have a 1x1 surface and no bounds to speak of, and
 * an input region computed then would be a handful of pixels somewhere: it would
 * make the dock ignore input entirely. So the tick keeps running until it has a
 * real answer, and gives up after a second or so rather than spinning forever on
 * a dock that never maps.
 */
static gboolean on_input_region_tick(GtkWidget *widget, GdkFrameClock *clock,
                                     gpointer user_data) {
  (void)widget;
  (void)clock;

  LambdockState *state = (LambdockState *)user_data;
  if (!state)
    return G_SOURCE_REMOVE;

  if (dock_apply_input_region(state)) {
    state->input_region_tick_id = 0;
    state->input_region_attempts = 0;
    return G_SOURCE_REMOVE;
  }

  if (++state->input_region_attempts > 120) {
    state->input_region_tick_id = 0;
    state->input_region_attempts = 0;
    return G_SOURCE_REMOVE;
  }

  return G_SOURCE_CONTINUE;
}

/**
 * dock_schedule_input_region:
 * Asks for the input region to be recomputed after the next frame.
 *
 * Cheap to call repeatedly: only one update is ever queued, so a pointer that
 * moves every frame adds no more than one region computation per frame.
 */
static void dock_schedule_input_region(LambdockState *state) {
  if (!state || !state->window || state->input_region_tick_id != 0)
    return;

  state->input_region_tick_id = gtk_widget_add_tick_callback(
      state->window, on_input_region_tick, state, NULL);
}

/**
 * update_dock_dimensions:
 * Recalculates and updates size requests for overlay surface and background
 * capsule based on the active icon size, orientation, and screen edge.
 */
void update_dock_dimensions(LambdockState *state) {
  if (!state || !state->bg_capsule || !state->overlay)
    return;

  DockMonitorSize monitor = dock_monitor_size(state);
  LambdockShelfGeometry shelf =
      lambdock_shelf_geometry_for(state, state->icon_size);
  LambdockDockDims dims =
      lambdock_dock_dims_compute(state->icon_size, state->capsule_padding,
                                 shelf, monitor.width, monitor.height,
                                 state->hover_animate);

  gboolean horizontal = (state->orientation == GTK_ORIENTATION_HORIZONTAL);

  dock_dims_apply_size_group(state);
  dock_dims_apply_scroll_policy(state, &dims);
  dock_dims_apply_box_margins(state, &dims, horizontal);
  state->hover_headroom_px = dims.hover_headroom_px;
  dock_dims_apply_size_requests(state, &dims, horizontal);
  dock_reserve_hover_room(state);
  dock_dims_apply_scroll_alignment(state, horizontal);
  dock_dims_apply_capsule_alignment(state, horizontal);

  /* Size, show or hide the glass-shelf reflections so they always match the
   * current icon size and shelf parameters. */
  apply_shelf_layout(state);
  dock_dims_mirror_shelf_alignment(state);

  /* Icon size, spacing, padding, mode and shelf all move the items: the hover
   * path's cached geometry is stale. */
  state->centres_dirty = TRUE;

  /* The surface may be bigger than the dock now: keep the input region on the
   * dock itself. */
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
  g_free(state);
}
