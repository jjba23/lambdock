// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef DOCK_DND_H
#define DOCK_DND_H

#include "../lambdock.h"

/* =========================================================================
 * DRAG AND DROP
 * =========================================================================
 * Reorder items within the row, add a dropped path as a favourite, and hand
 * drops to stack and trash items.
 *
 * Owns the drag payload format (DOCK_DND_ITEM_PREFIX); the drop targets
 * themselves belong to the item modules.
 */

/**
 * dock_dnd_setup:
 * @state: Dock state.
 *
 * Installs the drop target on the dock window. Dropping a dock icon inside the
 * dock reorders it, dropping a .desktop file adds that application and dropping
 * a folder adds it as a stack.
 */
void dock_dnd_setup(LambdockState *state);

/**
 * dock_dnd_setup_item:
 * @item: A freshly created dock item.
 *
 * Turns the item's button into a drag source. Dragging an icon out of the dock
 * (dropping it where nothing accepts it) removes it.
 */
void dock_dnd_setup_item(DockItem *item);

/**
 * dock_drop_handle_item:
 * @state: Dock state.
 * @key: Key (see dock_item_key()) of the dragged item.
 * @x: Drop position, in window coordinates.
 * @y: Drop position, in window coordinates.
 *
 * Moves the item to the drop position and persists the new order.
 *
 * Returns: TRUE when the dock accepted the drop.
 */
gboolean dock_drop_handle_item(LambdockState *state, const char *key, double x,
                               double y);

/**
 * remove_dock_item:
 * @state: Dock state (may be NULL, the item knows its own state).
 * @item: Item to remove.
 *
 * Unparents the item's widgets, forgets it in the dock layout overrides and
 * refreshes the dock geometry. Used by drag-out removal and by the context
 * menu's "Remove from Dock" entry.
 */
void remove_dock_item(LambdockState *state, DockItem *item);

/**
 * schedule_remove_dock_item:
 * @item: Item to remove.
 *
 * Removes the item from an idle callback. Destroying an item from inside a
 * handler of one of its own widgets (the context menu's "Remove from Dock") is
 * unsafe while that widget is still emitting, so the work is deferred and
 * re-resolved through the item's widget data.
 */
void schedule_remove_dock_item(DockItem *item);

/**
 * dock_dnd_persist_order:
 * @state: Dock state.
 *
 * Stores the current left-to-right item order in the layout overrides.
 * Separators are skipped: they have no stable identity, so they are re-placed
 * between their neighbours when the layout is replayed.
 */
void dock_dnd_persist_order(LambdockState *state);

/**
 * dock_list_move:
 * @list: A GList.
 * @data: Element to move (must be in the list).
 * @index: Destination index, clamped to the list bounds.
 *
 * Returns: (transfer none): the list with @data moved to @index.
 */
GList *dock_list_move(GList *list, gpointer data, int index);

/**
 * dock_drop_index_for_position:
 * @centers: Centre coordinates of the dock items along the dock axis.
 * @n: Number of entries in @centers.
 * @position: Pointer position along the same axis.
 *
 * Returns the insertion index (0..n) for a pointer at @position: the first item
 * whose centre lies past the pointer wins, so dropping on the left half of an
 * icon inserts before it and on the right half inserts after it.
 */
int dock_drop_index_for_position(const double *centers, int n, double position);

#endif // DOCK_DND_H
