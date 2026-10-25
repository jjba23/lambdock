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
 * dock_item_enable_meter:
 * @item: A dynamic dock item.
 * @unit: Reading suffix ("%" or "°C"), may be NULL.
 * @max_value: Reading that fills the gauge (0 falls back to 100).
 *
 * Replaces the item's icon+text with a single gauge widget, so the reading is
 * drawn as an icon that fills up. The poll function must then return a number;
 * strings still go to the (now absent) label, which is simply ignored.
 */
void dock_item_enable_meter(DockItem *item, const char *unit, double max_value);

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
 * create_stack_dock_item:
 * @state: Dock state the item belongs to.
 * @name: Display name (usually the folder's base name).
 * @icon: Icon name or path.
 * @path: Folder whose contents the stack shows.
 * @view: Initial view mode ("grid", "list" or "fan").
 * @hover_animate: Whether the fisheye magnification applies.
 *
 * Creates a dock icon that opens a popover listing @path.
 */
DockItem *create_stack_dock_item(LambdockState *state, const char *name,
                                 const char *icon, const char *path,
                                 const char *view, gboolean hover_animate);

/**
 * create_launchpad_dock_item:
 * Creates a dock icon that opens the full screen application grid.
 */
DockItem *create_launchpad_dock_item(LambdockState *state, const char *name,
                                     const char *icon,
                                     gboolean hover_animate);

/**
 * dock_item_key:
 * @item: A dock item (may be NULL).
 *
 * Stable identity of an item, "kind:name", matching `(dock-item-key item)` on
 * the Scheme side. The dock layout overrides use it to remember order, removals
 * and additions across restarts.
 *
 * Returns: (transfer full) (nullable): the key, or NULL for a NULL item.
 */
char *dock_item_key(const DockItem *item);

/**
 * dock_item_kind_to_string:
 * The Scheme-side name of @kind ("app", "dynamic", "separator", "stack",
 * "launchpad").
 */
const char *dock_item_kind_to_string(LambdockItemKind kind);

/**
 * dock_item_icon_size:
 * @item: A dock item (may be NULL).
 *
 * The size the item's icon is drawn at right now, in pixels: the magnified size
 * while the fisheye is scaling it, the resting size otherwise.
 *
 * The glass-shelf reflection needs this to mirror the icon at the size it is
 * actually shown, and it must not be read from the GtkImage's pixel size -- that
 * one is whatever raster the icon is currently held at, not the size on screen
 * (see icon_zoom.h).
 *
 * Returns: the icon size in pixels, or 0 when the item has no icon.
 */
int dock_item_icon_size(DockItem *item);

/**
 * dock_item_from_widget:
 * @widget: (nullable): any widget of a dock item (button or container).
 *
 * Looks the owning DockItem up from the widget's data, which is NULL once the
 * item has been destroyed. Deferred callbacks (drag cancel, idle removal) use
 * this instead of holding a raw DockItem pointer that a config reload may have
 * freed in the meantime.
 *
 * Returns: (transfer none) (nullable): the live DockItem, if any.
 */
DockItem *dock_item_from_widget(GtkWidget *widget);

/**
 * reload_dock_instance:
 * Hot-reloads layout, CSS themes, and pinned items on a running dock instance.
 */
void reload_dock_instance(LambdockState *state);

#endif // DOCK_ITEM_H
