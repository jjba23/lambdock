// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef X11_TRACKER_H
#define X11_TRACKER_H

#include "dock_window.h"
#include "toplevel_tracker.h"

/* =========================================================================
 * THE X11 BACKEND
 * =========================================================================
 * Two services for the dock on an X11 session, both defined by EWMH (the
 * Extended Window Manager Hints every desktop on X11 implements):
 *
 *   - the window list: which windows are open, what they are called, which
 *     application they belong to, and whether they are minimized or focused;
 *   - the dock's own surface: a window the window manager treats as a dock, the
 *     screen space it reserves, and the geometry that puts it on its edge.
 *
 * The decisions these calls make are in core/x11_rules.h; this module only
 * reads properties and sends requests. It keeps one Xlib connection of its own
 * -- GDK owns its connection and does not hand out the events it reads -- and
 * everything below is a no-op in a build without X11 support.
 *
 * If you are reading this because the dock does not line up on X11: the window
 * manager, not the dock, decides whether a reserve is honoured, and only an
 * EWMH window manager (`_NET_SUPPORTING_WM_CHECK`) does. Without one the dock
 * still shows and launches; it just cannot reserve space.
 */

/**
 * lambdock_x11_session:
 * Returns: TRUE when this session is an X11 one and this build can drive it.
 *
 * The one question a module with something to say about X11 has to ask, and the
 * reason no other file needs GDK's X11 headers.
 */
gboolean lambdock_x11_session(void);

/**
 * x11_tracker_start:
 * @error: (nullable): return location for a GError.
 *
 * Opens the private Xlib connection, reads the window list once and starts
 * watching the root window for the property changes that mean "the list
 * changed".
 *
 * Returns: TRUE when the tracker is listening, FALSE with @error set when there
 * is no X server to talk to.
 */
gboolean x11_tracker_start(GError **error);

/**
 * x11_tracker_replay:
 *
 * Attaches every window the tracker knows to every dock. The counterpart of the
 * Wayland half's replay in sync_window_trackers(): same records, same broadcast.
 */
void x11_tracker_replay(void);

/**
 * x11_tracker_activate:
 * @window: the window to bring forward.
 *
 * Sends the `_NET_ACTIVE_WINDOW` request (source indication: pager), which is
 * how a panel raises a window without stealing focus from the keyboard's owner.
 * A minimized window is restored first.
 */
void x11_tracker_activate(LambdockToplevel *window);

/**
 * x11_tracker_minimize:
 * @window: the window to tuck away.
 *
 * Iconifies the window with `XIconifyWindow`, falling back to the
 * `_NET_WM_STATE_HIDDEN` request a reparenting window manager understands.
 */
void x11_tracker_minimize(LambdockToplevel *window);

/**
 * x11_tracker_close:
 * @window: the window to close.
 *
 * Sends the `_NET_CLOSE_WINDOW` request: the same thing the window's own close
 * button asks for, which lets the application save its work.
 */
void x11_tracker_close(LambdockToplevel *window);

/**
 * lambdock_x11_dock_apply:
 * @state: the dock whose window should be docked.
 *
 * Makes the dock's window an X11 dock: it carries the dock window type and the
 * "above / sticky / skip taskbar" state, it is moved to its edge, and it
 * reserves its thickness on the desktop through `_NET_WM_STRUT_PARTIAL`, so
 * maximized windows stop at the dock instead of covering it.
 *
 * Idempotent: the window is only moved and re-stated when the rectangle it
 * should have changed, so this can be called on every reload and every frame
 * without asking the window manager to re-place the dock each time.
 */
void lambdock_x11_dock_apply(LambdockState *state);

/**
 * lambdock_x11_dock_forget:
 * @state: the dock going away.
 *
 * Drops the per-window bookkeeping (@state->x11_window) so a dock that is
 * reloaded with a new window is placed again instead of being told its own
 * geometry is already current.
 */
void lambdock_x11_dock_forget(LambdockState *state);

#endif // X11_TRACKER_H
