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

#ifndef UI_HELPERS_H
#define UI_HELPERS_H

#include "lambdock.h"

/**
 * setup_icon_theme:
 *
 * Configures search paths for icon themes across user home directories and XDG
 * locations, automatically loading GNOME system icon preferences.
 */
void setup_icon_theme(void);

/**
 * load_custom_css:
 * @state: Pointer to LambdockState structure.
 *
 * Instantiates a GtkCssProvider, loads dock stylesheet rules from file paths,
 * and registers the CSS provider globally across the active GdkDisplay.
 */
void load_custom_css(LambdockState *state);

/**
 * create_dock_item:
 * @state: Pointer to LambdockState structure.
 * @name: Human-readable display name.
 * @icon: Icon identifier or path.
 * @exec: Execution command line string.
 * @app_id: Application tracking ID.
 * @is_favorite: TRUE if item is a pinned launcher.
 *
 * Constructs a DockItem, instantiates its GTK button and popover widgets, and
 * appends it to the dock container layout.
 *
 * Returns: Pointer to newly allocated DockItem structure.
 */
DockItem *create_dock_item(LambdockState *state, const char *name,
                           const char *icon, const char *exec,
                           const char *app_id, gboolean is_favorite);

/**
 * free_dock_item:
 * @item: Pointer to DockItem to deallocate.
 *
 * Frees dynamic string allocations, child windows, and destroys associated GTK
 * popover bindings.
 */
void free_dock_item(DockItem *item);

/**
 * dock_item_add_window:
 * @item: Target DockItem structure.
 * @handle: Low-level Wayland window handle pointer.
 *
 * Appends a newly opened window instance to the DockItem's internal window
 * list.
 *
 * Returns: Pointer to newly allocated AppWindow tracking struct.
 */
AppWindow *dock_item_add_window(DockItem *item,
                                struct zwlr_foreign_toplevel_handle_v1 *handle);

/**
 * dock_item_remove_window:
 * @item: Target DockItem structure.
 * @handle: Low-level Wayland window handle pointer.
 *
 * Removes a closed window handle instance from the DockItem's internal window
 * list.
 */
void dock_item_remove_window(DockItem *item,
                             struct zwlr_foreign_toplevel_handle_v1 *handle);

/**
 * update_dock_item_status:
 * @item: Target DockItem structure.
 *
 * Dynamically toggles the "running-app" CSS styling class depending on whether
 * active windows exist in the item's list.
 */
void update_dock_item_status(DockItem *item);

/**
 * find_dock_item_by_app_id:
 * @state: Pointer to LambdockState structure.
 * @app_id: Application identifier string.
 *
 * Searches active DockItem structs in state->dock_items matching app_id, exec,
 * or icon name.
 *
 * Returns: Matching DockItem pointer, or NULL if unmatched.
 */
DockItem *find_dock_item_by_app_id(LambdockState *state, const char *app_id);

DockItem *create_separator_dock_item(LambdockState *state,
                                     GtkOrientation orientation);

/**
 * clear_favorite_dock_items:
 * @state: Pointer to LambdockState structure.
 *
 * Removes and frees pinned favorite launchers from the dock box container
 * while preserving active running application window trackers.
 */
void clear_favorite_dock_items(LambdockState *state);

/**
 * reload_dock_instance:
 * @state: Pointer to LambdockState structure.
 *
 * Re-applies active theme styles, auto-hide settings, and repopulates pinned
 * launchers dynamically on an existing dock window instance.
 */
void reload_dock_instance(LambdockState *state);

#endif // UI_HELPERS_H
