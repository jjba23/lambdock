// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef DOCK_ITEM_H
#define DOCK_ITEM_H

#include "../lambdock.h"
#include "dock_item_windows.h"

/* =========================================================================
 * THE DOCK ICON
 * =========================================================================
 * Build one item per kind, mount it in the dock box, attach and detach the
 * windows it tracks, and free it.
 *
 * Kind-specific behaviour is a row of the ITEM_KIND_OPS table in the .c file,
 * not a switch here: adding a kind never edits this header.
 */

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
 *
 * @poll_fn is the bridge's opaque handle for the Scheme procedure that produces
 * the reading (NULL for an item that never polls).
 */
DockItem *create_dynamic_dock_item(LambdockState *state, const char *name,
                                   const char *icon, const char *exec,
                                   void *poll_fn, guint interval_ms,
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
 * dock_item_enable_clock:
 * @item: A dynamic dock item.
 *
 * Replaces the item's icon+text with an analogue clock face. The item's poll
 * function is still called: its string becomes the face's tooltip and accessible
 * name, while the hands are set from the system clock on every poll.
 */
void dock_item_enable_clock(DockItem *item);

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
 * dock_items_refresh_polls:
 * @state: (nullable): the dock whose polling items should be updated now.
 *
 * Polls every dynamic and trash item immediately. The poll timers skip their
 * work while the dock is off screen (nothing to see), so this is what makes the
 * first frame after a reveal show current values instead of the ones from when
 * the dock hid.
 */
void dock_items_refresh_polls(LambdockState *state);

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
 * @view: Initial view mode ("grid" or "list").
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
 * create_trash_dock_item:
 * @state: Dock state the item belongs to.
 * @name: Display name, also the popover header and tooltip ("Trash").
 * @icon: Icon shown while the trash is empty. NULL or empty uses the icon
 * theme's `user-trash`.
 * @hover_animate: Whether the fisheye magnification applies.
 *
 * Creates an icon for the desktop trash. It shows the theme's `user-trash` while
 * the trash is empty and `user-trash-full` once it is not, and it re-counts the
 * trash every TRASH_REFRESH_INTERVAL_MS so a deletion made anywhere else shows
 * up on the dock. Clicking it opens the trash in the file manager; its context
 * menu offers opening and emptying it.
 */
DockItem *create_trash_dock_item(LambdockState *state, const char *name,
                                 const char *icon, gboolean hover_animate);

/**
 * dock_item_refresh_trash:
 * @item: A trash dock item (any other kind is ignored).
 *
 * Re-counts the trash and, when the count changed, swaps the icon between its
 * empty and full artwork and updates the tooltip. Called on a timer, and right
 * after the context menu empties the trash.
 */
void dock_item_refresh_trash(DockItem *item);

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
 * lambdock_item_title:
 * @item: (nullable): a dock item.
 * @fallback: (nullable): what to return when the item has no name at all.
 *
 * The text a user should see for @item: its name, else its app id, else
 * @fallback. Every menu header and tooltip used to spell this chain out again,
 * with slightly different fallbacks.
 *
 * Returns: (transfer none) (nullable): a borrowed string owned by @item.
 */
const char *lambdock_item_title(const DockItem *item, const char *fallback);


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
 * dock_item_update_accessible:
 * @item: a dock item (separators and NULL are ignored).
 *
 * Rebuilds the accessible label and description of the item's button: the
 * application's name plus whether it is running, the stack's folder, the
 * trash's contents or a dynamic item's current reading. Called whenever that
 * state changes, so a screen reader always announces the truth.
 */
void dock_item_update_accessible(DockItem *item);

/**
 * lambdock_dock_item_count:
 * Number of items the keyboard can reach: everything except separators.
 */
int lambdock_dock_item_count(const LambdockState *state);

/**
 * lambdock_dock_item_at_index / lambdock_dock_item_index_of:
 * Translate between a dock's keyboard order (separators skipped) and its items.
 */
DockItem *lambdock_dock_item_at_index(const LambdockState *state, int index);
int lambdock_dock_item_index_of(const LambdockState *state,
                                const DockItem *item);

/**
 * lambdock_dock_item_focused:
 * Returns: (transfer none) (nullable): the item holding the dock window's
 * keyboard focus, if any.
 */
DockItem *lambdock_dock_item_focused(LambdockState *state);

/**
 * lambdock_dock_item_focus_step:
 * Moves the keyboard focus @delta items along the dock, wrapping nothing: the
 * first and last items are the ends of the row. With no item focused, a positive
 * step focuses the first item and a negative one the last.
 *
 * Returns: TRUE when the focus moved.
 */
gboolean lambdock_dock_item_focus_step(LambdockState *state, int delta);

/**
 * lambdock_dock_item_focus_edge:
 * Focuses the first (or, with @last, the last) item of the dock.
 */
gboolean lambdock_dock_item_focus_edge(LambdockState *state, gboolean last);

/**
 * lambdock_dock_item_move:
 * @item: the focused item.
 * @delta: -1 to move it one place towards the dock's start, +1 towards its end.
 *
 * Reorders the dock, its widgets and the stored layout, so icons can be
 * rearranged with Ctrl+Arrow instead of a drag (which is hard for people with
 * tremor or without a pointer).
 *
 * Returns: TRUE when the item moved.
 */
gboolean lambdock_dock_item_move(LambdockState *state, DockItem *item,
                                 int delta);

/**
 * lambdock_dock_item_show_menu:
 * Opens the item's context menu, or closes it when it is already open. This is
 * the keyboard's right click (Menu key or Shift+F10).
 */
void lambdock_dock_item_show_menu(DockItem *item);

/**
 * lambdock_dock_items_close_popovers:
 * Dismisses every open dock menu (including the background menu and the
 * launchpad).
 *
 * Returns: TRUE when something was open and is now closed.
 */
gboolean lambdock_dock_items_close_popovers(LambdockState *state);

/**
 * lambdock_dock_items_handle_key:
 * @state: the dock.
 * @keyval: the pressed key.
 * @modifiers: its modifier state.
 *
 * The dock's keyboard behaviour: Left/Right (Up/Down on a vertical dock) walk
 * the icons, Home/End jump to the ends, Ctrl+Arrow rearranges them, Menu or
 * Shift+F10 opens the focused item's context menu, and Escape closes whatever
 * the dock has open. Enter and Space are handled by the focused button itself.
 *
 * Returns: TRUE when the key was used.
 */
gboolean lambdock_dock_items_handle_key(LambdockState *state, guint keyval,
                                        GdkModifierType modifiers);

/**
 * lambdock_dock_attach_key_navigation:
 * @root: the dock's window (or any widget whose focus should navigate the dock).
 * @state: the dock the keys belong to.
 *
 * Installs the key controller once; calling it again is harmless.
 */
void lambdock_dock_attach_key_navigation(GtkWidget *root, LambdockState *state);

/**
 * reload_dock_instance:
 * Hot-reloads layout, CSS themes, and pinned items on a running dock instance.
 */
void reload_dock_instance(LambdockState *state);

#endif // DOCK_ITEM_H
