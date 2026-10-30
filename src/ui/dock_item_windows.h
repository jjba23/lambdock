// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef DOCK_ITEM_WINDOWS_H
#define DOCK_ITEM_WINDOWS_H

#include "../lambdock.h"

/* =========================================================================
 * THE DOCK ITEM'S WINDOWS
 * =========================================================================
 * The window model behind a dock item: the AppWindows the compositor reported
 * for it, the running/focused/minimized styling that follows, and the lookup
 * from a reported app id to the item that owns it.
 *
 * `dock_item.h` includes this header, so callers keep one include for the item
 * API; the split is of the implementation (ui/dock_item_windows.c), where a file
 * about building an icon no longer also owns the window model.
 */

/**
 * clear_tracked_windows:
 * Drops all tracked AppWindows/handles and removes non-favorite (window-tracked)
 * items. Used when the Wayland foreign-toplevel manager is withdrawn.
 */
void clear_tracked_windows(LambdockState *state);

/**
 * dock_item_add_window / dock_item_remove_window:
 * Manages active Wayland window handle attachments on a DockItem.
 */
AppWindow *dock_item_add_window(DockItem *item,
                                struct zwlr_foreign_toplevel_handle_v1 *handle);
void dock_item_remove_window(DockItem *item,
                             struct zwlr_foreign_toplevel_handle_v1 *handle);

/**
 * dock_item_has_window:
 * @item: (nullable): the item to ask.
 * @handle: (nullable): a Wayland toplevel handle captured earlier.
 *
 * Returns: TRUE when @item still tracks @handle.
 *
 * The tracker owns and frees the proxies, so a handle stored elsewhere -- a
 * popover row's action data, built when the menu opened -- can be freed while
 * that menu is still on screen. Callers that act on a captured handle must ask
 * this first, or they would call a Wayland request on a destroyed proxy.
 */
gboolean dock_item_has_window(
    const DockItem *item,
    const struct zwlr_foreign_toplevel_handle_v1 *handle);

/**
 * update_dock_item_status:
 * Toggles 'running-app' CSS styling depending on active window counts.
 */
void update_dock_item_status(DockItem *item);

/**
 * dock_item_refresh_window_state:
 * @item: (nullable): the item whose windows' state changed.
 *
 * Re-applies the styling that depends on what the compositor reports for the
 * item's windows -- currently the `window-minimized` class that dims the icon of
 * an app whose windows are all minimized. Cheap by design: two CSS-class toggles
 * and one scan of a short list, so it can run on every state event.
 */
void dock_item_refresh_window_state(DockItem *item);

/**
 * find_dock_item_by_app_id:
 * @state: the dock to search.
 * @app_id: (nullable): the application id a window reported.
 *
 * The item a window belongs to: matched by the spellings a launcher can be
 * written with (app id, icon, name, desktop id, exec), then through the desktop
 * entry the id resolves to.
 *
 * Returns: (transfer none) (nullable): the matching item, or NULL.
 */
DockItem *find_dock_item_by_app_id(LambdockState *state, const char *app_id);

#endif // DOCK_ITEM_WINDOWS_H
