// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef TOPLEVEL_TRACKER_H
#define TOPLEVEL_TRACKER_H

#include "dock_window.h"

/**
 * init_toplevel_tracker:
 * @state: Pointer to LambdockState context.
 *
 * Connects to low-level Wayland display server registry and binds listeners to
 * track open application window instances dynamically.
 */
void init_toplevel_tracker(LambdockState *state);

/**
 * activate_toplevel_window:
 * @handle: Pointer to target Wayland foreign toplevel handle struct.
 *
 * Requests compositor window activation to bring target window to user focus.
 */
void activate_toplevel_window(struct zwlr_foreign_toplevel_handle_v1 *handle);

/**
 * close_toplevel_window:
 * @handle: Pointer to target Wayland foreign toplevel handle struct.
 *
 * Requests graceful window closure via Wayland foreign toplevel protocols.
 */
void close_toplevel_window(struct zwlr_foreign_toplevel_handle_v1 *handle);

/**
 * sync_window_trackers:
 * @state: Pointer to LambdockState context.
 *
 * Re-binds active Wayland window trackers to current dock items after
 * configuration reloading or reset.
 */
void sync_window_trackers(LambdockState *state);

void clear_window_tracker_dock_references(LambdockState *state);

/**
 * free_window_tracker:
 * @tracker_ptr: Pointer to WindowTracker structure.
 *
 * Safely destroys the foreign toplevel Wayland handle proxy and frees
 * allocated tracker memory.
 */
void free_window_tracker(void *tracker_ptr);

#endif // TOPLEVEL_TRACKER_H
