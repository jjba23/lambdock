#ifndef POPOVER_MENU_H
#define POPOVER_MENU_H

#include "lambdock.h"

/**
 * populate_popover_menu:
 * @item: Target DockItem containing popover context.
 *
 * Builds dynamic GTK popover menu contents for launching application instances
 * or selecting/closing specific Wayland foreign-toplevel windows.
 */
void populate_popover_menu(DockItem *item);

#endif // POPOVER_MENU_H
