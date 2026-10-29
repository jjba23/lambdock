// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef TOPLEVEL_TRACKER_H
#define TOPLEVEL_TRACKER_H

#include "dock_window.h"

/* =========================================================================
 * WINDOW TRACKING
 * =========================================================================
 * Binds the wlr-foreign-toplevel manager and broadcasts each window event to
 * every registered dock.
 *
 * Owns every Wayland proxy it creates; docks hold borrowed handles. It makes no
 * decisions: those are in core/tracker_rules.h, and the dock-side effect of an
 * event is in ui/dock_item.h.
 */

/**
 * init_toplevel_tracker:
 * @error: (nullable): return location for a GError.
 *
 * Connects to the low-level Wayland display registry and binds the listeners
 * that track open application windows. Call it once, before
 * sync_window_trackers(); calling it again is a no-op rather than a second
 * registry listener.
 *
 * Window tracking is optional: a dock that cannot track windows still shows its
 * pinned items, so a FALSE return is a reason to log, not to abort startup.
 *
 * Returns: TRUE when the tracker is listening (or already was), FALSE with
 * @error set when the display cannot provide a Wayland registry.
 */
gboolean init_toplevel_tracker(GError **error);

/**
 * activate_toplevel_window:
 * @handle: Pointer to target Wayland foreign toplevel handle struct.
 *
 * Requests compositor window activation to bring target window to user focus.
 * A minimized window is un-minimized first, so this is also how a dock click
 * restores a window that was tucked away.
 */
void activate_toplevel_window(struct zwlr_foreign_toplevel_handle_v1 *handle);

/**
 * minimize_toplevel_window:
 * @handle: Pointer to target Wayland foreign toplevel handle struct.
 *
 * Asks the compositor to minimize the window, which is what "minimize to dock"
 * is: the window stays running, the dock item keeps its running indicator, and
 * clicking that item brings it back through activate_toplevel_window(). Like
 * every request in this protocol it is advisory: the compositor may ignore it.
 */
void minimize_toplevel_window(struct zwlr_foreign_toplevel_handle_v1 *handle);

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
