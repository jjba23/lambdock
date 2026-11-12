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
 * One record per window the session has open, and the broadcast of every
 * change to it to every registered dock.
 *
 * Two backends fill that record: the Wayland foreign-toplevel manager
 * (toplevel_tracker.c) and the X11/EWMH window list (x11_tracker.c). They agree
 * on nothing but the value below, which is the whole point: the docks, the
 * popover, the click policy and the item wording are written once, against
 * `LambdockToplevel`, and neither backend knows a dock exists.
 *
 * The tracker owns every record; docks hold borrowed pointers, which is why a
 * record is only freed after it has been withdrawn from every dock.
 */

/**
 * LambdockToplevel:
 * @handle: the Wayland foreign-toplevel proxy, or NULL on X11.
 * @x11_window: the X11 window id, or 0 on Wayland.
 * @title: the window's title as last reported, or NULL.
 * @app_id: the window's application id (`app_id` on Wayland, `WM_CLASS` on X11)
 *  or NULL while the backend has not read it yet.
 * @minimized: whether the session reports the window tucked away.
 * @activated: whether it reports the window focused.
 *
 * One window, in terms both backends can describe. Only the trackers write it;
 * only `ui/dock_item_windows.c` reads the flags, through `AppWindow`.
 */
struct LambdockToplevel {
  struct zwlr_foreign_toplevel_handle_v1 *handle;
  gulong x11_window;
  char *title;
  char *app_id;
  gboolean minimized;
  gboolean activated;
};

/**
 * init_toplevel_tracker:
 * @error: (nullable): return location for a GError.
 *
 * Starts the tracker for this session: the foreign-toplevel manager on Wayland,
 * the EWMH window list on X11. Call it once, before sync_window_trackers();
 * calling it again is a no-op rather than a second set of listeners.
 *
 * Window tracking is optional: a dock that cannot track windows still shows its
 * pinned items, so a FALSE return is a reason to log, not to abort startup.
 *
 * Returns: TRUE when the tracker is listening (or already was), FALSE with
 * @error set when this session can list no windows.
 */
gboolean init_toplevel_tracker(GError **error);

/**
 * activate_toplevel_window:
 * @window: the window to bring forward.
 *
 * Requests window activation, un-minimizing it first, so this is also how a dock
 * click restores a window that was tucked away. Advisory on both backends: a
 * compositor with no minimize concept ignores it.
 */
void activate_toplevel_window(LambdockToplevel *window);

/**
 * minimize_toplevel_window:
 * @window: the window to tuck away.
 *
 * Asks the session to minimize the window, which is what "minimize to dock" is:
 * the window stays running, the dock item keeps its running indicator, and
 * clicking that item brings it back through activate_toplevel_window().
 */
void minimize_toplevel_window(LambdockToplevel *window);

/**
 * close_toplevel_window:
 * @window: the window to close.
 *
 * Requests a graceful close, exactly as the window's own close button does.
 */
void close_toplevel_window(LambdockToplevel *window);

/**
 * sync_window_trackers:
 * @state: Pointer to LambdockState context.
 *
 * Replays every window the tracker knows about into one dock, and warns once
 * when this session cannot report any. It is called when a dock is created and
 * after every reload, which is what makes a reloaded dock as complete as a
 * fresh one.
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

/* ---- what the backends use to publish into the docks ---- */

/**
 * lambdock_toplevel_new:
 *
 * Returns: (transfer full): a blank window record for a backend to fill in.
 */
LambdockToplevel *lambdock_toplevel_new(void);

/**
 * lambdock_toplevel_set_app_id:
 * @window: the record the session reported on.
 * @app_id: (nullable): the id it reported.
 *
 * Records the id and attaches the window to every dock's item for it, detaching
 * it from the item of the id it used to have. Applications that load a profile
 * late (browsers, Electron apps) change their id after their window exists, and
 * without the detach the old item keeps a stale window and a permanent running
 * indicator.
 */
void lambdock_toplevel_set_app_id(LambdockToplevel *window, const char *app_id);

/**
 * lambdock_toplevel_set_title:
 * @window: the record the session reported on.
 * @title: (nullable): the title it reported.
 *
 * Records the title and renames the window in every dock, unless the text did
 * not actually change.
 */
void lambdock_toplevel_set_title(LambdockToplevel *window, const char *title);

/**
 * lambdock_toplevel_set_state:
 * @window: the record.
 * @minimized: whether the session reports it minimized.
 * @activated: whether it reports it focused.
 *
 * Records the flags and mirrors them onto every dock's copy.
 */
void lambdock_toplevel_set_state(LambdockToplevel *window, gboolean minimized,
                                 gboolean activated);

/**
 * lambdock_toplevel_withdraw:
 * @window: the window the session closed.
 *
 * Forgets the window in every dock and drops the items that existed only
 * because of it. The record itself is left for the backend to free.
 */
void lambdock_toplevel_withdraw(LambdockToplevel *window);

/**
 * lambdock_toplevel_free:
 * @window: (nullable): the record to free.
 *
 * Frees the title, the id and the record. The backend destroys its own handle
 * first: only it knows what the handle means.
 */
void lambdock_toplevel_free(LambdockToplevel *window);

/**
 * lambdock_toplevel_deliver:
 * @window: a window one backend just learned about.
 *
 * Replays one window into every dock: attachment, title and state. A backend
 * calls it once per window when a dock asks for a refresh, which is what makes
 * `sync_window_trackers()` backend-independent.
 */
void lambdock_toplevel_deliver(LambdockToplevel *window);

#endif // TOPLEVEL_TRACKER_H
