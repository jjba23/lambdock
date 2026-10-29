// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAMBDOCK_DOCK_CLICK_H
#define LAMBDOCK_DOCK_CLICK_H

#include <glib.h>

/*
 * =========================================================================
 * WHAT A DOCK CLICK DOES
 * =========================================================================
 * The click policy of an application item, as a function of plain values: how
 * many windows the app has, what state the compositor last reported for the one
 * window, and whether there is a command to launch.
 *
 * It lives here, away from the widgets, because it is the one part of the dock
 * that behaves differently on every desktop: taskbar people expect the icon of
 * the focused app to tuck it away, launcher people expect it to stay put. As a
 * value it is unit tested without a compositor, and `ui/dock_item.c` only
 * applies the answer.
 */

/**
 * LambdockClickAction:
 * @LAMBDOCK_CLICK_NOTHING: nothing is running and there is no command to run.
 * @LAMBDOCK_CLICK_LAUNCH: nothing is running; start the launcher command.
 * @LAMBDOCK_CLICK_MINIMIZE: the app's only window is focused; minimize it.
 * @LAMBDOCK_CLICK_ACTIVATE: the app's only window is minimized or in the
 *   background; bring it forward.
 * @LAMBDOCK_CLICK_MENU: several windows are open; let the user pick one.
 *
 * What a left click on an application item should do.
 */
typedef enum {
  LAMBDOCK_CLICK_NOTHING = 0,
  LAMBDOCK_CLICK_LAUNCH,
  LAMBDOCK_CLICK_MINIMIZE,
  LAMBDOCK_CLICK_ACTIVATE,
  LAMBDOCK_CLICK_MENU,
} LambdockClickAction;

/**
 * lambdock_click_action_for:
 * @window_count: how many windows the item is tracking.
 * @window_minimized: whether the compositor reports the (only) window as
 *   minimized. Ignored when @window_count is not 1.
 * @window_active: whether the (only) window currently holds the focus. Ignored
 *   when @window_count is not 1.
 * @launcher_command: (nullable): the command to run when nothing is open.
 *
 * Pure: same arguments, same action. With exactly one window the click
 * toggles it -- an active window is minimized, anything else is brought
 * forward -- which is what makes "minimize to dock" work without a separate
 * minimize button.
 *
 * Returns: the action the click should perform.
 */
LambdockClickAction lambdock_click_action_for(guint window_count,
                                              gboolean window_minimized,
                                              gboolean window_active,
                                              const char *launcher_command);

#endif // LAMBDOCK_DOCK_CLICK_H
