// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAMBDOCK_CORE_RENDER_STATS_H
#define LAMBDOCK_CORE_RENDER_STATS_H

#include <glib.h>

/*
 * =========================================================================
 * RENDER STATISTICS (PURE)
 * =========================================================================
 * Counters for the work the dock does per frame. They exist because the
 * interesting questions about a dock's rendering -- "does hovering relayout the
 * row?", "does the surface keep its size?", "how many draws is one reflection?"
 * -- are measurements, not opinions, and a picture cannot answer them.
 *
 * Every counter is a plain integer bump on the GTK main thread: no allocation,
 * no locking, no formatting. Tests reset them, drive the dock, and assert the
 * budgets; `lambdock_stats_describe()` prints the same numbers in the test log
 * so a regression shows up as a number rather than as a judgement call.
 */

typedef enum {
  /* Pointer frames that reached the fish-eye. */
  LAMBDOCK_STAT_HOVER_FRAMES,
  /* Frames that changed nothing and were dropped before touching a widget. */
  LAMBDOCK_STAT_HOVER_SKIPS,
  /* Item size requests changed (each one is a layout step for the row). */
  LAMBDOCK_STAT_LAYOUT_STEPS,
  /* Times the item geometry cache had to be rebuilt (one walk of the row). */
  LAMBDOCK_STAT_GEOMETRY_REFRESHES,
  /* Icon snapshots. */
  LAMBDOCK_STAT_ZOOM_PAINTS,
  /* Reflection snapshots, and the draws one of them costs. */
  LAMBDOCK_STAT_REFLECTION_PAINTS,
  LAMBDOCK_STAT_REFLECTION_DRAWS,
  /* Times the dock's surface changed size (each one is a buffer re-allocation
   * and a compositor reconfigure). */
  LAMBDOCK_STAT_SURFACE_RESIZES,
  /* Times the surface's input region was actually changed: each one is a
   * wl_region created, a request sent and a commit. */
  LAMBDOCK_STAT_INPUT_REGION_SETS,
  /* Microseconds spent inside the dock's own hover path, so the part of a
   * frame that is ours can be told apart from the part GTK spends laying the
   * row out and painting it. */
  LAMBDOCK_STAT_HOVER_US,
  LAMBDOCK_STAT_COUNT
} LambdockStat;

/** Clears every counter. */
void lambdock_stats_reset(void);

/** Adds one to @stat. Called from the frame paths, so it does no bounds check
 * beyond the cost of the array index itself. */
void lambdock_stats_bump(LambdockStat stat);

/** Adds @count to @stat. */
void lambdock_stats_add(LambdockStat stat, guint64 count);

/** Returns the current value of @stat. */
guint64 lambdock_stats_get(LambdockStat stat);

/** Returns a stable name for @stat ("hover_frames", ...). */
const char *lambdock_stats_name(LambdockStat stat);

/** Returns "name=value name=value ..." for every counter, for test logs. */
char *lambdock_stats_describe(void);

#endif // LAMBDOCK_CORE_RENDER_STATS_H
