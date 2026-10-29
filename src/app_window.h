// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAMBDOCK_APP_WINDOW_H
#define LAMBDOCK_APP_WINDOW_H

#include "lambdock.h"

/*
 * =========================================================================
 * AppWindow OWNERSHIP
 * =========================================================================
 * An `AppWindow` is one window of a tracked application: a Wayland toplevel
 * handle plus its title. It is created in two places and freed in five, and the
 * five used to disagree about NULL checks. These helpers are the ownership
 * rules in one place, so a list of windows can be freed with one call and a
 * NULL window is always a no-op instead of a crash.
 */

/**
 * app_window_new:
 * Creates a window record for @handle. @title may be NULL.
 *
 * Returns: (transfer full): the new record.
 */
AppWindow *app_window_new(struct zwlr_foreign_toplevel_handle_v1 *handle,
                          const char *title);

/**
 * app_window_set_title:
 * Replaces @win's title. NULL @win or @title is ignored, so a late compositor
 * event cannot blank or crash a window that is already gone.
 */
void app_window_set_title(AppWindow *win, const char *title);

/**
 * app_window_set_state:
 * @win: (nullable): the window record.
 * @minimized: whether the compositor reports the window as minimized.
 * @activated: whether it reports the window as focused.
 *
 * Records the last known state of @win. NULL is a no-op, like every other
 * helper here, so a late state event cannot crash a window that is already
 * gone. The dock's click policy reads these flags.
 */
void app_window_set_state(AppWindow *win, gboolean minimized,
                          gboolean activated);

/**
 * app_window_matches:
 * Returns: TRUE when @win is non-NULL and tracks @handle.
 */
gboolean app_window_matches(const AppWindow *win,
                            const struct zwlr_foreign_toplevel_handle_v1 *handle);

/**
 * app_window_free:
 * Frees @win (NULL is a no-op).
 */
void app_window_free(AppWindow *win);

/**
 * app_windows_free:
 * Frees every AppWindow in @windows and the list itself. The caller keeps
 * ownership of the list pointer; pass NULL after calling this.
 */
void app_windows_free(GList *windows);

#endif // LAMBDOCK_APP_WINDOW_H
