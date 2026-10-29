// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "render_stats.h"

#include <glib.h>

/* One array, one bump per event: this runs on the frame path, so it stays a
 * plain increment. GTK drives everything from the main thread, so there is
 * nothing to synchronise. */
static guint64 counters[LAMBDOCK_STAT_COUNT];

static const char *const stat_names[LAMBDOCK_STAT_COUNT] = {
    [LAMBDOCK_STAT_HOVER_FRAMES] = "hover_frames",
    [LAMBDOCK_STAT_HOVER_SKIPS] = "hover_skips",
    [LAMBDOCK_STAT_LAYOUT_STEPS] = "layout_steps",
    [LAMBDOCK_STAT_GEOMETRY_REFRESHES] = "geometry_refreshes",
    [LAMBDOCK_STAT_ZOOM_PAINTS] = "zoom_paints",
    [LAMBDOCK_STAT_REFLECTION_PAINTS] = "reflection_paints",
    [LAMBDOCK_STAT_REFLECTION_DRAWS] = "reflection_draws",
    [LAMBDOCK_STAT_SURFACE_RESIZES] = "surface_resizes",
    [LAMBDOCK_STAT_INPUT_REGION_SETS] = "input_region_sets",
    [LAMBDOCK_STAT_HOVER_US] = "hover_us",
};

void lambdock_stats_reset(void) {
  for (gsize i = 0; i < LAMBDOCK_STAT_COUNT; i++)
    counters[i] = 0;
}

void lambdock_stats_bump(LambdockStat stat) {
  if ((gsize)stat < LAMBDOCK_STAT_COUNT)
    counters[stat]++;
}

void lambdock_stats_add(LambdockStat stat, guint64 count) {
  if ((gsize)stat < LAMBDOCK_STAT_COUNT)
    counters[stat] += count;
}

guint64 lambdock_stats_get(LambdockStat stat) {
  return ((gsize)stat < LAMBDOCK_STAT_COUNT) ? counters[stat] : 0;
}

const char *lambdock_stats_name(LambdockStat stat) {
  return ((gsize)stat < LAMBDOCK_STAT_COUNT) ? stat_names[stat] : "?";
}

char *lambdock_stats_describe(void) {
  GString *text = g_string_new(NULL);

  for (gsize i = 0; i < LAMBDOCK_STAT_COUNT; i++) {
    if (i > 0)
      g_string_append_c(text, ' ');
    g_string_append_printf(text, "%s=%" G_GUINT64_FORMAT, stat_names[i],
                           counters[i]);
  }

  return g_string_free(text, FALSE);
}
