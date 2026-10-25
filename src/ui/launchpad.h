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

/* Icon size of a launchpad tile, in pixels. */
#define LAUNCHPAD_ICON_SIZE 64

/**
 * lambdock_launchpad_show:
 * @state: Any dock state; its window provides the GtkApplication and monitor.
 *
 * Shows the launchpad: a full screen layer-shell overlay with a search field and
 * a grid of every installed application, macOS Launchpad style. The window is
 * created on first use and reused afterwards; the application list is rescanned
 * on every show so freshly installed apps appear.
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

#endif // LAUNCHPAD_H
