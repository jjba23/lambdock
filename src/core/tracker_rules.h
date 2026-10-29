// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAMBDOCK_TRACKER_RULES_H
#define LAMBDOCK_TRACKER_RULES_H

#include <glib.h>

/*
 * =========================================================================
 * TOPLEVEL TRACKER RULES
 * =========================================================================
 * The decisions the foreign-toplevel tracker makes, expressed as functions of
 * plain values: no Wayland proxy, no GtkWidget, no global state. They used to
 * be conditions buried inside the protocol listeners (`if (state->auto_hide &&
 * state->show_when_clear && state->window_trackers != NULL)`), where the only
 * way to test them was to fake a compositor.
 *
 * `toplevel_tracker.c` reads the state, calls these, and then does the widget
 * work. tests/test_tracker_rules.c links this file alone.
 */

/**
 * lambdock_tracker_ignores_app_id:
 * @app_id: (nullable): app id reported by the compositor.
 *
 * Returns: TRUE when @app_id carries no tracking information: the compositor
 * sends NULL/"" before it knows the app, and lambdock's own windows are in the
 * same toplevel list as everyone else's, so tracking them would put an icon for
 * the dock inside the dock.
 */
gboolean lambdock_tracker_ignores_app_id(const char *app_id);

/**
 * lambdock_tracker_item_should_be_dropped:
 * @has_windows: whether any window is still attached to the item.
 * @is_favorite: whether the item is pinned by the configuration.
 *
 * Returns: TRUE when the item exists only because a window did, and that window
 * is gone. A pinned item stays even when nothing is running.
 */
gboolean lambdock_tracker_item_should_be_dropped(gboolean has_windows,
                                                 gboolean is_favorite);

/**
 * lambdock_tracker_should_hide:
 * @auto_hide: the dock's "hide when idle" setting.
 * @show_when_clear: the dock's "only while something runs" setting.
 * @has_tracked_windows: whether the dock tracks any window right now.
 *
 * Returns: TRUE when a window just appeared and the dock should slide away.
 */
gboolean lambdock_tracker_should_hide(gboolean auto_hide,
                                      gboolean show_when_clear,
                                      gboolean has_tracked_windows);

/**
 * lambdock_tracker_should_reveal:
 * @show_when_clear: the dock's "only while something runs" setting.
 * @has_tracked_windows: whether the dock still tracks a window.
 *
 * Returns: TRUE when the last tracked window closed and the dock should come
 * back into view.
 */
gboolean lambdock_tracker_should_reveal(gboolean show_when_clear,
                                        gboolean has_tracked_windows);

#endif // LAMBDOCK_TRACKER_RULES_H
