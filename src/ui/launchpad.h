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

#ifndef LAUNCHPAD_H
#define LAUNCHPAD_H

#include "../lambdock.h"
#include "desktop_entry.h"

/* Icon size of a launchpad tile, in pixels. */
#define LAUNCHPAD_ICON_SIZE 64
#define LAUNCHPAD_ICON_SIZE_MIN 32
#define LAUNCHPAD_ICON_SIZE_MAX 160

/* Tiles per row. 0 lets the launcher choose from the screen width, which is
 * what a display of any size wants; a fixed number is for people who want the
 * same grid everywhere. */
#define LAUNCHPAD_COLUMNS_AUTO 0
#define LAUNCHPAD_COLUMNS_MIN 3
#define LAUNCHPAD_COLUMNS_MAX 14

/**
 * lambdock_launchpad_show:
 * @state: Any dock state; its window provides the GtkApplication and monitor.
 *
 * Shows the launchpad: a full screen layer-shell overlay with a search field and
 * a grid of every installed application, macOS Launchpad style. The window is
 * created on first use and reused afterwards; the application list is rescanned
 * on every show so freshly installed apps appear. The tile size, column count
 * and frosted background are resolved from @state's dock settings on each show.
 *
 * Escape, the close button and a click on the backdrop all hide it.
 */
void lambdock_launchpad_show(LambdockState *state);

/**
 * lambdock_launchpad_hide:
 * Hides the launchpad overlay if it is currently shown.
 */
void lambdock_launchpad_hide(void);

/**
 * lambdock_launchpad_is_open:
 * Returns: TRUE when the launchpad overlay is mapped.
 */
gboolean lambdock_launchpad_is_open(void);

/**
 * lambdock_launchpad_shutdown:
 * Destroys the launchpad window (used when the last dock goes away).
 */
void lambdock_launchpad_shutdown(void);

/**
 * lambdock_launchpad_grid_new:
 * @entries: Applications to show (borrowed).
 *
 * Builds the searchable application grid widget. Exposed so the layout can be
 * exercised without a display server.
 *
 * Returns: (transfer none): a GtkFlowBox.
 */
GtkWidget *lambdock_launchpad_grid_new(GPtrArray *entries);

/**
 * lambdock_launchpad_tile_new:
 * @entry: the installed application to show.
 * @icon_size: pixel size of its icon.
 *
 * Builds one tile of the grid: the icon, the name, the accessibility text and
 * the tooltip. An application that is installed more than once (see
 * lambdock_desktop_entries_scan()) also gets a context menu, opened with the
 * right mouse button, the Menu key or Shift+F10, that lists the copies so the
 * user picks the one to start instead of the scan picking for them. Tiles of an
 * application with a single copy have no menu. Exposed so the tiles can be
 * exercised without a display server.
 *
 * Returns: (transfer none): a GtkButton.
 */
GtkWidget *lambdock_launchpad_tile_new(LambdockDesktopEntry *entry,
                                       int icon_size);

#endif // LAUNCHPAD_H
