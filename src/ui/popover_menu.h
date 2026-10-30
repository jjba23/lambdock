// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef POPOVER_MENU_H
#define POPOVER_MENU_H

#include "../lambdock.h"

/* =========================================================================
 * CONTEXT MENUS
 * =========================================================================
 * Build the right-click and multi-window popovers, and own their keyboard,
 * focus and close behaviour.
 *
 * Owns no item state: it reads the item it is given and calls back into
 * dock_item.h / app_actions.h for the effects.
 */

/**
 * populate_popover_menu:
 * @item: Target DockItem containing popover context.
 *
 * Builds dynamic GTK popover menu contents for launching application instances
 * or selecting/closing specific Wayland foreign-toplevel windows.
 */
void populate_popover_menu(DockItem *item);

/**
 * lambdock_popover_menu_keys:
 * @popover: A popover whose child is an item menu.
 *
 * Makes Up/Down walk the menu's rows and Home/End jump to the ends. Safe to call
 * more than once, and inert on a popover that is showing something else (a
 * stack panel reuses the same popover and handles its own arrow keys).
 */
void lambdock_popover_menu_keys(GtkWidget *popover);

/**
 * lambdock_popover_menu_focus_first:
 * @popover: A popover whose child is an item menu.
 *
 * Puts the keyboard focus on the menu's first row, so the arrow keys work from
 * the moment it opens instead of only after a Tab. Call it after showing the
 * popover; it does nothing when the child is not a menu.
 */
void lambdock_popover_menu_focus_first(GtkWidget *popover);

/**
 * lambdock_popover_close_on_escape:
 * @popover: A GtkPopover owned by a dock item.
 *
 * Makes Escape dismiss @popover. A popover only gets that behaviour from GTK
 * while one of its own children holds the keyboard focus; when the click came
 * from the dock, focus can stay on the dock button and Escape reaches the
 * window instead. A capture-phase key controller on the popover closes it in
 * both cases, and is safe to call more than once.
 */
void lambdock_popover_close_on_escape(GtkWidget *popover);

#endif // POPOVER_MENU_H
