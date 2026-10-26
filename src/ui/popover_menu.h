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

#ifndef POPOVER_MENU_H
#define POPOVER_MENU_H

#include "../lambdock.h"

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
