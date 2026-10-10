// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock. If not, see <https://www.gnu.org/licenses/>.

#ifndef DOCK_ITEM_H
#define DOCK_ITEM_H

#include "../lambdock.h"

/**
 * apply_dock_item_alignment:
 * Applies edge-anchored alignment rules (valign/halign) to DockItem widgets,
 * ensuring non-animated items and separators remain anchored to the dock edge.
 */
void apply_dock_item_alignment(DockItem *item);

/**
 * create_dock_item:
 * Constructs a DockItem, instantiates its GTK button/popover widgets,
 * and appends it to the dock container layout.
 */
DockItem *create_dock_item(LambdockState *state, const char *name,
                           const char *icon, const char *exec,
                           const char *app_id, gboolean is_favorite,
                           gboolean hover_animate);

/**
 * create_dynamic_dock_item:
 * Constructs a dynamic polling DockItem with text label and interval timer.
 */
DockItem *create_dynamic_dock_item(LambdockState *state, const char *name,
                                   const char *icon, const char *exec,
                                   SCM poll_fn, guint interval_ms,
                                   gboolean hover_animate);

/**
 * free_dock_item:
 * Unparents widgets, cleans child window trackers, cancels timers,
 * unprotects Scheme procedure objects, and deallocates DockItem memory.
 */
void free_dock_item(DockItem *item);

/**
 * clear_favorite_dock_items:
 * Frees pinned favorite launchers while preserving active running window
 * trackers.
 */
void clear_favorite_dock_items(LambdockState *state);

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
 * update_dock_item_status:
 * Toggles 'running-app' CSS styling depending on active window counts.
 */
void update_dock_item_status(DockItem *item);

/**
 * find_dock_item_by_app_id:
 * Searches active DockItem structs matching app_id, exec, or icon name.
 */
DockItem *find_dock_item_by_app_id(LambdockState *state, const char *app_id);

/**
 * create_separator_dock_item:
 * Instantiates layout divider bar items.
 */
DockItem *create_separator_dock_item(LambdockState *state,
                                     GtkOrientation orientation);

/**
 * reload_dock_instance:
 * Hot-reloads layout, CSS themes, and pinned items on a running dock instance.
 */
void reload_dock_instance(LambdockState *state);

#endif // DOCK_ITEM_H
