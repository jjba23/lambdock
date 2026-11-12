// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAMBDOCK_X11_RULES_H
#define LAMBDOCK_X11_RULES_H

#include <glib.h>
#include <gtk4-layer-shell.h> /* GtkLayerShellEdge: the dock's edge, shared */

/*
 * =========================================================================
 * X11 / EWMH RULES
 * =========================================================================
 * What an X11 window means to a dock, as functions of plain values: no Xlib,
 * no GtkWidget, no global state. Reading `_NET_CLIENT_LIST` and the properties
 * of each window is `x11_tracker.c`'s job; deciding which windows the dock
 * lists, what their state is, and where the dock puts itself is here, where it
 * can be tested without a display.
 *
 * The X11 backend exists because the two things a dock needs -- a list of
 * windows, and a way to reserve screen space -- are *not* part of the X11 core
 * protocol: they are conventions in the EWMH specification, which every
 * desktop on X11 (Xfce, KDE, Cinnamon, MATE, GNOME on Xorg, the tiling window
 * managers) implements. tests/test_x11_rules.c links this file alone.
 */

/** A rectangle in root (whole-desktop) coordinates, in pixels. */
typedef struct {
  int x;
  int y;
  int width;
  int height;
} LambdockX11Rect;

/** The two flags a dock reads off a window: the same pair `AppWindow` holds. */
typedef struct {
  gboolean minimized;
  gboolean activated;
} LambdockX11WindowState;

/**
 * LambdockX11Placement:
 * @x: root x of the dock window's top-left corner.
 * @y: root y of the dock window's top-left corner.
 * @width: the window's width, in pixels.
 * @height: the window's height, in pixels.
 * @strut_*: the four `_NET_WM_STRUT_PARTIAL` distances, measured from the
 *  corresponding edge of the desktop; 0 means "reserve nothing there".
 * @strut_*_start/end: the span along that edge the reservation covers, in
 *  absolute root coordinates, so a window manager can shrink the work area of
 *  the monitor the dock is on and leave the others alone.
 *
 * Where the dock's window goes and how much desktop it reserves, as one value.
 */
typedef struct {
  int x;
  int y;
  int width;
  int height;
  int strut_left;
  int strut_right;
  int strut_top;
  int strut_bottom;
  int strut_left_start_y;
  int strut_left_end_y;
  int strut_right_start_y;
  int strut_right_end_y;
  int strut_top_start_x;
  int strut_top_end_x;
  int strut_bottom_start_x;
  int strut_bottom_end_x;
} LambdockX11Placement;

/**
 * lambdock_x11_type_is_desktop_chrome:
 * @window_type: (nullable): a `_NET_WM_WINDOW_TYPE_*` atom name, as EWMH
 *  spells it.
 *
 * Returns: TRUE for the window types the desktop owns -- the desktop itself,
 * docks and panels, toolbars, menus, utilities, splash screens, notifications,
 * tooltips, open combo boxes and drag icons. A dock that listed them would show
 * icons for its own shell.
 */
gboolean lambdock_x11_type_is_desktop_chrome(const char *window_type);

/**
 * lambdock_x11_shows_window:
 * @desktop_chrome: whether the window's type says the desktop owns it; see
 *  lambdock_x11_type_is_desktop_chrome().
 * @skip_taskbar: whether it carries `_NET_WM_STATE_SKIP_TASKBAR`, which is the
 *  window asking not to be listed.
 * @own_window: whether the window belongs to this process.
 * @has_wm_class: whether it reports a `WM_CLASS`, which is the app id a dock
 *  matches a window to an icon with.
 *
 * Returns: TRUE when the dock should list the window.
 */
gboolean lambdock_x11_shows_window(gboolean desktop_chrome,
                                   gboolean skip_taskbar, gboolean own_window,
                                   gboolean has_wm_class);

/**
 * lambdock_x11_window_state:
 * @hidden: whether the window carries `_NET_WM_STATE_HIDDEN`, which is how EWMH
 *  spells "minimized".
 * @window: the window the state is read for.
 * @active_window: the value of `_NET_ACTIVE_WINDOW` (0 when nothing is focused).
 *
 * Returns: the two flags the dock's click policy reads.
 */
LambdockX11WindowState lambdock_x11_window_state(gboolean hidden, gulong window,
                                                 gulong active_window);

/**
 * lambdock_x11_placement_compute:
 * @edge: the screen edge the dock is docked to.
 * @root: the whole desktop, in root coordinates.
 * @monitor: the monitor the dock belongs to, in root coordinates.
 * @desired_main_px: the dock's content along its main axis, or 0/negative when
 *  there is nothing measured yet (a taskbar then spans its monitor).
 * @window_cross_px: the window's thickness, which is the capsule plus the room
 *  the icons magnify into.
 * @reserve_px: the thickness the dock reserves on the desktop, which is the
 *  capsule alone: the magnification room is not part of the dock.
 *
 * Returns: where the window goes and what it reserves. The window hugs the
 * monitor's edge on the dock's side and is centred along the other axis; a size
 * larger than the monitor is clamped to it, so the window is always on screen.
 */
LambdockX11Placement lambdock_x11_placement_compute(
    GtkLayerShellEdge edge, LambdockX11Rect root, LambdockX11Rect monitor,
    int desired_main_px, int window_cross_px, int reserve_px);

#endif // LAMBDOCK_X11_RULES_H
