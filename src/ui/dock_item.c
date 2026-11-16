// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "dock_item.h"
#include "../dock_window.h"
#include "../guile_bridge.h"
#include "../logger.h"
#include "../toplevel_tracker.h"
#include "theme_engine.h"
#include <gtk4-layer-shell.h>
#include <string.h>

/* ============================================================================
 * WHAT IS LEFT HERE
 * ============================================================================
 * Everything about one item -- its identity, what a click or a key means, its
 * alignment, its poller, its artwork, its construction and its teardown -- is in
 * rust/src/dock_item.rs. What is still C is the reload, below: it is a sequence
 * of stages over a whole *dock*, and every stage calls into dock_window.c, the
 * bridge and the tracker.
 */

/* ============================================================================
 * HOT RELOAD
 * ============================================================================
 * reload_dock_instance() is a sequence of named stages rather than one long
 * body: what a reload *is* is "re-read the settings, move the surface, rebuild
 * the row, re-apply the derived state, put the dock where it belongs". Each
 * stage below is one of those sentences.
 */

/** Mode-relevant settings, captured before a reload so it can be compared
 * afterwards. */
typedef struct DockModeSnapshot {
  gboolean taskbar;
  LambdockShelfParams shelf;
} DockModeSnapshot;

/** Re-reads the settings CSS depends on, and reloads the stylesheets. */
static DockModeSnapshot reload_appearance(LambdockState *state) {
  DockModeSnapshot before = {
      .taskbar = state->taskbar,
      .shelf = state->shelf,
  };

  /* One reader, before the CSS: it fills the mode flags *and* the icon size the
   * shelf's band is computed from, so the stylesheet is never built from the
   * size the previous reload happened to leave behind. */
  dock_read_settings(state);
  load_custom_css(state);

  return before;
}

/** Applies the settings a widget takes once it exists. */
static void apply_settings(LambdockState *state) {
  /* A pending auto-hide timeout would fire against the layout being replaced. */
  if (state->hide_timer_id != 0) {
    g_source_remove(state->hide_timer_id);
    state->hide_timer_id = 0;
  }

  if (state->box)
    gtk_box_set_spacing(GTK_BOX(state->box), state->item_spacing);
}

/** Moves the surface to a new screen edge, re-orienting the dock with it. */
static void apply_edge_change(LambdockState *state, GtkLayerShellEdge edge) {
  if (edge == state->edge)
    return;

  if (state->tick_cb_id != 0) {
    gtk_widget_remove_tick_callback(state->window, state->tick_cb_id);
    state->tick_cb_id = 0;
  }

  /* The edge lives in layer-shell anchors; without a layer surface (X11, GNOME)
   * there is nothing to re-anchor, and every call would log a critical. */
  if (gtk_layer_is_layer_window(GTK_WINDOW(state->window))) {
    gtk_layer_set_margin(GTK_WINDOW(state->window), state->edge, 0);
    gtk_layer_set_anchor(GTK_WINDOW(state->window), state->edge, FALSE);

    state->edge = edge;
    gtk_layer_set_anchor(GTK_WINDOW(state->window), state->edge, TRUE);
  } else {
    state->edge = edge;
  }

  state->orientation = (edge == GTK_LAYER_SHELL_EDGE_LEFT ||
                        edge == GTK_LAYER_SHELL_EDGE_RIGHT)
                           ? GTK_ORIENTATION_VERTICAL
                           : GTK_ORIENTATION_HORIZONTAL;
  gtk_orientable_set_orientation(GTK_ORIENTABLE(state->box),
                                 state->orientation);
  if (state->bg_capsule) {
    gtk_orientable_set_orientation(GTK_ORIENTABLE(state->bg_capsule),
                                   state->orientation);
  }

  /* A dock that just changed edges is on screen, at rest. */
  state->current_margin = 0;
  state->target_margin = 0;
  state->start_margin = 0;
  state->is_hidden = FALSE;
}

/** Rebuilds the row from the configuration and re-syncs the tracked windows. */
static void reload_items(LambdockState *state) {
  clear_favorite_dock_items(state);
  populate_dock_items_for(state);

  /* Re-sync Wayland foreign window handles back to the new launchers. */
  sync_window_trackers(state);

  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    if (!item || !item->button)
      continue;

    apply_dock_item_alignment(item);
    update_dock_item_status(item);
  }
}

/** Reserves space for the dock (or not) and puts it where the setting wants it. */
static void apply_auto_hide_state(LambdockState *state) {
  dock_apply_exclusive_zone(state);

  gboolean stay_visible =
      !state->auto_hide ||
      (state->show_when_clear && state->window_trackers == NULL);

  if (stay_visible)
    reveal_dock(state);
  else
    hide_dock(state);
}


/**
 * reload_dock_instance:
 * @state: Global Lambdock application state.
 *
 * Hot-reloads dock UI appearance, Scheme/Guile configuration rules for
 * state->config_path, custom CSS themes, autohide behavior, screen edge
 * positioning, and rebuilds dock launcher layout without restarting
 * application.
 */
void reload_dock_instance(LambdockState *state) {
  if (!state)
    return;

  LOG_C_INFO("UI", "Hot-reloading UI layout for config: %s",
             state->config_path ? state->config_path : "default");

  DockModeSnapshot before = reload_appearance(state);
  apply_settings(state);
  apply_edge_change(state, get_position_config_for(state->config_path));
  reload_items(state);

  /* Mode classes and dimensions depend on both the new flags and the new row. */
  apply_dock_mode_classes(state);
  if (before.taskbar != state->taskbar ||
      memcmp(&before.shelf, &state->shelf, sizeof(LambdockShelfParams)) != 0) {
    refresh_dock_mode(state);
  }

  update_dock_dimensions(state);

  /* A toggle from the preferences window takes effect on the running dock. */
  dock_apply_motion_preference(state);

  apply_auto_hide_state(state);
}

