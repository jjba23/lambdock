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
